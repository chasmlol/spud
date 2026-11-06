#include "formats/osv.hpp"

#include <lz4.h>

#include <format>

namespace spud::osv {

namespace {

constexpr auto k_head = container::fourcc("HEAD");
constexpr auto k_data = container::fourcc("DATA");

struct Head {
    uint32_t frame_count;
    int32_t start_frame;
    uint32_t phoneme_count;
    uint32_t modifier_count;
    float frame_rate;
    uint32_t compression; // 0 raw float32, 1 LZ4 block
    uint32_t raw_size;    // bytes of the uncompressed frames
    uint32_t reserved;
};
static_assert(sizeof(Head) == 32);

} // namespace

std::vector<std::byte> write(const LipTrack& track) {
    Head head{track.frame_count(),
              track.start_frame,
              track.phoneme_count,
              track.modifier_count,
              k_frame_rate,
              1,
              static_cast<uint32_t>(track.frames.size() * sizeof(float)),
              0};
    std::vector<std::byte> packed(static_cast<size_t>(LZ4_compressBound(static_cast<int>(head.raw_size))));
    const int n = LZ4_compress_default(reinterpret_cast<const char*>(track.frames.data()),
                                       reinterpret_cast<char*>(packed.data()),
                                       static_cast<int>(head.raw_size), static_cast<int>(packed.size()));
    if (n <= 0) {
        throw Error("osv: LZ4 compression failed");
    }
    packed.resize(static_cast<size_t>(n));
    container::Writer w(k_kind, k_version);
    w.add_struct(k_head, head);
    w.add(k_data, packed);
    return w.finish();
}

LipTrack read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() < 1 || r.version() > k_version) {
        throw ParseError(std::format("unsupported .osv version {}", r.version()));
    }
    const auto head = r.read_struct<Head>(k_head);
    const auto data = r.require(k_data);
    LipTrack t;
    t.start_frame = head.start_frame;
    t.phoneme_count = head.phoneme_count;
    t.modifier_count = head.modifier_count;
    const size_t row = size_t(head.phoneme_count) + head.modifier_count;
    if (head.raw_size != size_t(head.frame_count) * row * sizeof(float)) {
        throw ParseError("osv: frame data size does not match the header");
    }
    t.frames.resize(head.raw_size / sizeof(float));
    if (head.compression == 0) {
        if (data.size() < head.raw_size) {
            throw ParseError("osv: frame data truncated");
        }
        std::memcpy(t.frames.data(), data.data(), head.raw_size);
    } else {
        const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(data.data()),
                                          reinterpret_cast<char*>(t.frames.data()),
                                          static_cast<int>(data.size()), static_cast<int>(head.raw_size));
        if (n != static_cast<int>(head.raw_size)) {
            throw ParseError("osv: corrupt frame data");
        }
    }
    return t;
}

} // namespace spud::osv
