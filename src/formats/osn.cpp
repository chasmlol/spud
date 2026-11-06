#include "formats/osn.hpp"

namespace spud::osn {

namespace {

constexpr auto k_info = container::fourcc("INFO");
constexpr auto k_blks = container::fourcc("BLKS");
constexpr auto k_flbk = container::fourcc("FLBK");
constexpr auto k_data = container::fourcc("DATA");
constexpr auto k_trbn = container::fourcc("TRBN");
constexpr auto k_flsl = container::fourcc("FLSL");
constexpr auto k_anno = container::fourcc("ANNO");
constexpr auto k_trnm = container::fourcc("TRNM");
constexpr auto k_strs = container::fourcc("STRS");
constexpr auto k_rawt = container::fourcc("RAWT");

struct Info {
    float duration;
    uint32_t transform_tracks;
    uint32_t float_tracks;
    uint32_t frames;
    uint32_t blocks;
    uint32_t max_frames_per_block;
    uint32_t mask_size;
    float block_duration;
    float block_inverse_duration;
    float frame_duration;
    uint32_t additive;
};

struct AnnotationRecord {
    float time;
    uint32_t text;
};

} // namespace

std::vector<std::byte> write(const Clip& c) {
    container::StringTableBuilder strings;
    std::vector<AnnotationRecord> annotations;
    annotations.reserve(c.annotations.size());
    for (const auto& a : c.annotations) {
        annotations.push_back({a.time, strings.add(a.text)});
    }
    std::vector<uint32_t> names;
    names.reserve(c.track_names.size());
    for (const auto& n : c.track_names) {
        names.push_back(strings.add(n));
    }
    container::Writer w(k_kind, c.raw.empty() ? k_version : k_version_raw);
    w.add_struct(k_info, Info{c.duration, c.transform_tracks, c.float_tracks, c.frames, c.blocks, c.max_frames_per_block,
                              c.mask_size, c.block_duration, c.block_inverse_duration, c.frame_duration,
                              c.additive ? 1u : 0u});
    w.add_array(k_blks, c.block_offsets);
    w.add_array(k_flbk, c.float_block_offsets);
    w.add_array(k_data, c.data);
    w.add_array(k_trbn, c.track_bones);
    w.add_array(k_flsl, c.float_slots);
    w.add_array(k_anno, annotations);
    w.add_array(k_trnm, names);
    if (!c.raw.empty()) {
        w.add_array(k_rawt, c.raw);
    }
    w.add(k_strs, strings.bytes());
    return w.finish();
}

Clip read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() != k_version && r.version() != k_version_raw) {
        throw ParseError("unsupported .osn version " + std::to_string(r.version()));
    }
    const auto info = r.read_struct<Info>(k_info);
    container::StringTable strings(r.require(k_strs));
    Clip c;
    c.duration = info.duration;
    c.transform_tracks = info.transform_tracks;
    c.float_tracks = info.float_tracks;
    c.frames = info.frames;
    c.blocks = info.blocks;
    c.max_frames_per_block = info.max_frames_per_block;
    c.mask_size = info.mask_size;
    c.block_duration = info.block_duration;
    c.block_inverse_duration = info.block_inverse_duration;
    c.frame_duration = info.frame_duration;
    c.additive = info.additive != 0;
    c.block_offsets = r.read_array<uint32_t>(k_blks);
    c.float_block_offsets = r.read_array<uint32_t>(k_flbk);
    c.data = r.read_array<uint8_t>(k_data);
    c.track_bones = r.read_array<int16_t>(k_trbn);
    c.float_slots = r.read_array<int16_t>(k_flsl);
    for (const auto& a : r.read_array<AnnotationRecord>(k_anno)) {
        c.annotations.push_back({a.time, std::string(strings.get(a.text))});
    }
    for (uint32_t n : r.read_array<uint32_t>(k_trnm)) {
        c.track_names.emplace_back(strings.get(n));
    }
    if (r.version() == k_version_raw) {
        c.raw = r.read_array<float>(k_rawt);
    }
    if (c.block_offsets.size() != c.blocks || c.float_block_offsets.size() != c.blocks) {
        throw ParseError("clip blocks out of range");
    }
    return c;
}

} // namespace spud::osn
