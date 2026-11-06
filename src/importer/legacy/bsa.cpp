#include "importer/legacy/bsa.hpp"

#include "core/binary.hpp"
#include "core/error.hpp"
#include "core/path.hpp"

#include <lz4frame.h>
#include <zlib.h>

#include <format>
#include <memory>

namespace spud::legacy {

namespace {

constexpr uint32_t k_bsa_magic = 0x00415342; // "BSA\0"

enum ArchiveFlag : uint32_t {
    archive_dir_names = 0x1,
    archive_file_names = 0x2,
    archive_compressed = 0x4,
    archive_embed_names = 0x100,
};

constexpr uint32_t k_size_compression_toggle = 0x40000000;
constexpr uint32_t k_size_mask = 0x3FFFFFFF;

void inflate_zlib(std::span<const std::byte> in, std::span<std::byte> out) {
    uLongf out_size = static_cast<uLongf>(out.size());
    int rc = uncompress(reinterpret_cast<Bytef*>(out.data()), &out_size, reinterpret_cast<const Bytef*>(in.data()),
                        static_cast<uLong>(in.size()));
    if (rc != Z_OK || out_size != out.size()) {
        throw ParseError(std::format("zlib inflate failed ({})", rc));
    }
}

void inflate_lz4_frame(std::span<const std::byte> in, std::span<std::byte> out) {
    LZ4F_dctx* ctx = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION))) {
        throw Error("cannot create LZ4 context");
    }
    std::unique_ptr<LZ4F_dctx, decltype(&LZ4F_freeDecompressionContext)> guard(ctx, LZ4F_freeDecompressionContext);
    size_t in_pos = 0;
    size_t out_pos = 0;
    while (in_pos < in.size() && out_pos < out.size()) {
        size_t in_size = in.size() - in_pos;
        size_t out_size = out.size() - out_pos;
        size_t rc = LZ4F_decompress(ctx, out.data() + out_pos, &out_size, in.data() + in_pos, &in_size, nullptr);
        if (LZ4F_isError(rc)) {
            throw ParseError(std::format("LZ4 decompression failed: {}", LZ4F_getErrorName(rc)));
        }
        in_pos += in_size;
        out_pos += out_size;
        if (rc == 0) {
            break;
        }
        if (in_size == 0 && out_size == 0) {
            throw ParseError("LZ4 decompression made no progress");
        }
    }
    if (out_pos != out.size()) {
        throw ParseError(std::format("LZ4 produced {} bytes, expected {}", out_pos, out.size()));
    }
}

} // namespace

BsaArchive::BsaArchive(const std::filesystem::path& path) : m_path(path), m_file(path) {
    BinaryReader r(m_file.bytes());
    if (r.read<uint32_t>() != k_bsa_magic) {
        throw ParseError(std::format("{} is not a BSA archive", path.string()));
    }
    m_version = r.read<uint32_t>();
    if (m_version != 104 && m_version != 105) {
        throw ParseError(std::format("{}: unsupported BSA version {}", path.string(), m_version));
    }
    auto folder_offset = r.read<uint32_t>();
    m_flags = r.read<uint32_t>();
    auto folder_count = r.read<uint32_t>();
    auto file_count = r.read<uint32_t>();
    r.skip(4 + 4 + 2 + 2); // total folder/file name lengths, file flags, padding

    std::vector<uint32_t> folder_file_counts(folder_count);
    r.seek(folder_offset);
    for (uint32_t i = 0; i < folder_count; ++i) {
        r.skip(8); // name hash
        folder_file_counts[i] = r.read<uint32_t>();
        r.skip(m_version == 105 ? 4 + 8 : 4);
    }

    // File record blocks follow the folder records, one block per folder.
    std::vector<std::string> folder_names(folder_count);
    std::vector<size_t> folder_of_file;
    folder_of_file.reserve(file_count);
    m_entries.reserve(file_count);
    for (uint32_t i = 0; i < folder_count; ++i) {
        if (m_flags & archive_dir_names) {
            auto len = r.read<uint8_t>();
            folder_names[i] = path::normalize(r.read_string(len));
        }
        for (uint32_t j = 0; j < folder_file_counts[i]; ++j) {
            r.skip(8); // name hash
            auto raw_size = r.read<uint32_t>();
            auto offset = r.read<uint32_t>();
            bool compressed = (m_flags & archive_compressed) != 0;
            if (raw_size & k_size_compression_toggle) {
                compressed = !compressed;
            }
            m_entries.push_back({{}, offset, raw_size & k_size_mask, compressed});
            folder_of_file.push_back(i);
        }
    }

    if (!(m_flags & archive_file_names)) {
        throw ParseError(std::format("{}: archives without file names are not supported", path.string()));
    }
    for (size_t i = 0; i < m_entries.size(); ++i) {
        std::string name;
        for (char c; (c = static_cast<char>(r.read<uint8_t>())) != '\0';) {
            name.push_back(c);
        }
        const auto& folder = folder_names[folder_of_file[i]];
        m_entries[i].path = folder.empty() ? path::normalize(name) : folder + "/" + path::normalize(name);
        m_index.emplace(m_entries[i].path, i);
    }
}

std::optional<size_t> BsaArchive::find(std::string_view normalized_path) const {
    if (auto it = m_index.find(std::string(normalized_path)); it != m_index.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<std::byte> BsaArchive::read(size_t index) const {
    const auto& e = m_entries.at(index);
    BinaryReader r(m_file.bytes());
    r.seek(static_cast<size_t>(e.offset));
    auto data = r.take(e.size);
    BinaryReader d(data);
    if (m_flags & archive_embed_names) {
        auto len = d.read<uint8_t>();
        d.skip(len);
    }
    if (!e.compressed) {
        auto rest = d.rest();
        return {rest.begin(), rest.end()};
    }
    auto original_size = d.read<uint32_t>();
    std::vector<std::byte> out(original_size);
    try {
        if (m_version == 105) {
            inflate_lz4_frame(d.rest(), out);
        } else {
            inflate_zlib(d.rest(), out);
        }
    } catch (const ParseError& err) {
        throw ParseError(std::format("{}: {}: {}", m_path.filename().string(), e.path, err.what()));
    }
    return out;
}

} // namespace spud::legacy
