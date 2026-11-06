#include "formats/ofm.hpp"

#include "core/error.hpp"

#include <lz4.h>

#include <cstring>
#include <format>

namespace spud::ofm {

namespace {

constexpr auto k_head = container::fourcc("HEAD");
constexpr auto k_name = container::fourcc("NAME");
constexpr auto k_scal = container::fourcc("SCAL");
constexpr auto k_data = container::fourcc("DATA");

struct Head {
    uint32_t vertex_count;
    uint32_t morph_count;
    uint32_t raw_size; // bytes of the uncompressed deltas
    uint32_t reserved;
};
static_assert(sizeof(Head) == 16);

} // namespace

std::vector<std::byte> write(const MorphSet& set) {
    const Head head{set.vertex_count, static_cast<uint32_t>(set.names.size()),
                    static_cast<uint32_t>(set.deltas.size() * sizeof(int16_t)), 0};
    std::vector<std::byte> packed(static_cast<size_t>(LZ4_compressBound(static_cast<int>(head.raw_size))));
    const int n = LZ4_compress_default(reinterpret_cast<const char*>(set.deltas.data()),
                                       reinterpret_cast<char*>(packed.data()), static_cast<int>(head.raw_size),
                                       static_cast<int>(packed.size()));
    if (n <= 0) {
        throw Error("ofm: LZ4 compression failed");
    }
    packed.resize(static_cast<size_t>(n));
    std::vector<std::byte> names;
    for (const std::string& name : set.names) {
        const auto* bytes = reinterpret_cast<const std::byte*>(name.c_str());
        names.insert(names.end(), bytes, bytes + name.size() + 1);
    }
    container::Writer w(k_kind, k_version);
    w.add_struct(k_head, head);
    w.add(k_name, names);
    w.add_array(k_scal, set.scales);
    w.add(k_data, packed);
    return w.finish();
}

MorphSet read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() != k_version) {
        throw ParseError(std::format("unsupported .ofm version {}", r.version()));
    }
    const auto head = r.read_struct<Head>(k_head);
    MorphSet set;
    set.vertex_count = head.vertex_count;
    set.scales = r.read_array<float>(k_scal);
    const auto names = r.require(k_name);
    for (size_t at = 0; at < names.size();) {
        const char* text = reinterpret_cast<const char*>(names.data()) + at;
        const size_t length = strnlen(text, names.size() - at);
        set.names.emplace_back(text, length);
        at += length + 1;
    }
    if (set.names.size() != head.morph_count || set.scales.size() != head.morph_count ||
        head.raw_size != size_t(head.morph_count) * head.vertex_count * 4 * sizeof(int16_t)) {
        throw ParseError("ofm: sizes don't match the header");
    }
    const auto data = r.require(k_data);
    set.deltas.resize(head.raw_size / sizeof(int16_t));
    const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(data.data()),
                                      reinterpret_cast<char*>(set.deltas.data()), static_cast<int>(data.size()),
                                      static_cast<int>(head.raw_size));
    if (n != static_cast<int>(head.raw_size)) {
        throw ParseError("ofm: corrupt deltas");
    }
    return set;
}

} // namespace spud::ofm
