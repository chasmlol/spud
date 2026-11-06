#include "importer/records/plugin_file.hpp"

#include "core/binary.hpp"
#include "core/error.hpp"

#include <zlib.h>

#include <cstring>
#include <format>

namespace spud::records {

namespace {

constexpr Sig k_grup = forms::sig("GRUP");
constexpr Sig k_tes4 = forms::sig("TES4");
constexpr Sig k_xxxx = forms::sig("XXXX");
constexpr size_t k_header_size = 24;
constexpr uint32_t k_compressed = 0x00040000;

struct RawHeader {
    uint32_t type;
    uint32_t size;
    uint32_t flags_or_label;
    uint32_t form_id_or_group_type;
    uint32_t stamp;
    uint16_t version;
    uint16_t unknown;
};
static_assert(sizeof(RawHeader) == k_header_size);

bool valid_type(Sig type) {
    for (int i = 0; i < 4; ++i) {
        auto c = static_cast<char>((type >> (8 * i)) & 0xFF);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

std::vector<RawSubrecord> split(std::span<const std::byte> payload) {
    std::vector<RawSubrecord> out;
    BinaryReader r(payload);
    uint32_t big_size = 0;
    bool has_big_size = false;
    while (r.remaining() >= 6) {
        auto type = r.read<uint32_t>();
        size_t size = r.read<uint16_t>();
        // XXXX carries the real 32-bit size of the next subrecord, whose own size field is 0.
        if (type == k_xxxx) {
            big_size = r.read<uint32_t>();
            has_big_size = true;
            continue;
        }
        if (has_big_size) {
            size = big_size;
            has_big_size = false;
        }
        out.push_back({type, r.take(size)});
    }
    if (r.remaining() != 0) {
        throw ParseError(std::format("{} stray bytes after the last subrecord", r.remaining()));
    }
    return out;
}

std::string zstring(std::span<const std::byte> data) {
    std::string_view s(reinterpret_cast<const char*>(data.data()), data.size());
    return std::string(s.substr(0, s.find('\0')));
}

} // namespace

std::vector<RawSubrecord> PluginFile::subrecords(const RawRecord& r, std::vector<std::byte>& storage) {
    auto payload = r.payload;
    if (r.flags & k_compressed) {
        BinaryReader br(payload);
        auto size = br.read<uint32_t>();
        storage.resize(size);
        auto in = br.rest();
        uLongf out_size = size;
        int rc = uncompress(reinterpret_cast<Bytef*>(storage.data()), &out_size,
                            reinterpret_cast<const Bytef*>(in.data()), static_cast<uLong>(in.size()));
        if (rc != Z_OK || out_size != size) {
            throw ParseError(std::format("zlib inflate failed ({})", rc));
        }
        payload = storage;
    }
    return split(payload);
}

PluginFile::PluginFile(const std::filesystem::path& path) : m_file(path) {
    auto file = m_file.bytes();
    BinaryReader r(file);
    auto h = r.read<RawHeader>();
    if (h.type != k_tes4) {
        throw ParseError("missing TES4 header record");
    }
    m_header.flags = h.flags_or_label;
    m_header.form_version = h.version;
    m_header.raw = r.take(h.size);
    for (const auto& sub : split(m_header.raw)) {
        BinaryReader s(sub.data);
        switch (sub.type) {
        case forms::sig("HEDR"):
            m_header.version = s.read<float>();
            m_header.record_count = s.read<uint32_t>();
            m_header.next_object_id = s.read<uint32_t>();
            break;
        case forms::sig("MAST"):
            m_header.masters.push_back(zstring(sub.data));
            break;
        case forms::sig("DATA"):
            m_header.master_sizes.push_back(sub.data.size() >= 8 ? s.read<uint64_t>() : 0);
            break;
        case forms::sig("ONAM"):
            m_header.overrides = s.read_array<uint32_t>(sub.data.size() / 4);
            break;
        case forms::sig("CNAM"):
            m_header.author = zstring(sub.data);
            break;
        case forms::sig("SNAM"):
            m_header.description = zstring(sub.data);
            break;
        case forms::sig("INTV"):
            m_header.intv = s.read<uint32_t>();
            m_header.has_intv = true;
            break;
        case forms::sig("INCC"):
            m_header.incc = s.read<uint32_t>();
            m_header.has_incc = true;
            break;
        default:
            break;
        }
    }

    struct Frame {
        size_t end;
        uint8_t type;
        uint32_t owner;
    };
    std::vector<Frame> stack;
    m_records.reserve(file.size() / 200);
    size_t pos = r.tell();
    while (pos < file.size()) {
        while (!stack.empty() && pos >= stack.back().end) {
            stack.pop_back();
        }
        size_t limit = stack.empty() ? file.size() : stack.back().end;
        if (limit - pos < k_header_size) {
            throw ParseError(std::format("truncated header at offset {:#x}", pos));
        }
        RawHeader rh;
        std::memcpy(&rh, file.data() + pos, sizeof rh);
        if (rh.type == k_grup) {
            if (rh.size < k_header_size || rh.size > limit - pos) {
                throw ParseError(std::format("group at {:#x} has bad size {}", pos, rh.size));
            }
            auto type = static_cast<uint8_t>(rh.form_id_or_group_type);
            uint32_t owner = stack.empty() ? 0 : stack.back().owner;
            // World, cell and topic children groups name their owner in the label.
            if (type == 1 || type == 6 || type == 7 || type == 8 || type == 9) {
                owner = rh.flags_or_label;
            }
            stack.push_back({pos + rh.size, type, owner});
            pos += k_header_size;
            ++m_groups;
            continue;
        }
        if (!valid_type(rh.type)) {
            throw ParseError(std::format("bad record type {:#010x} at offset {:#x}", rh.type, pos));
        }
        if (rh.size > limit - pos - k_header_size) {
            throw ParseError(std::format("{} record at {:#x} overruns its group", forms::sig_name(rh.type), pos));
        }
        RawRecord rec{};
        rec.type = rh.type;
        rec.flags = rh.flags_or_label;
        rec.form_id = rh.form_id_or_group_type;
        rec.vc_stamp = rh.stamp;
        rec.form_version = rh.version;
        rec.vc_unknown = rh.unknown;
        rec.parent = stack.empty() ? 0 : stack.back().owner;
        rec.group = stack.empty() ? 0xFF : stack.back().type;
        rec.offset = pos;
        rec.payload = file.subspan(pos + k_header_size, rh.size);
        m_records.push_back(rec);
        pos += k_header_size + rh.size;
    }
}

} // namespace spud::records
