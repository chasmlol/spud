#include "formats/okc.hpp"

#include "core/binary.hpp"

namespace spud::okc {

namespace {

constexpr container::FourCC k_chunk_models = container::fourcc("MODS");
constexpr container::FourCC k_chunk_frames = container::fourcc("FRMS");
constexpr container::FourCC k_chunk_strings = container::fourcc("STRS");

struct ModelRecord {
    uint32_t path;  // string offset
    float frustum_right;
    uint32_t rotates;
    uint32_t first_frame;
    uint32_t frame_count;
};
static_assert(sizeof(ModelRecord) == 20);

struct FrameRecord {
    float t[3], a[3], right;
};
static_assert(sizeof(FrameRecord) == 28);

} // namespace

std::vector<std::byte> write(const Cameras& cameras) {
    container::StringTableBuilder strings;
    std::vector<ModelRecord> records;
    std::vector<FrameRecord> frames;
    for (const Model& m : cameras.models) {
        records.push_back({strings.add(m.path), m.frustum_right, m.rotates ? 1u : 0u, uint32_t(frames.size()),
                           uint32_t(m.frames.size())});
        for (const Frame& f : m.frames) {
            frames.push_back({{f.translation[0], f.translation[1], f.translation[2]}, {f.angles[0], f.angles[1], f.angles[2]}, f.right});
        }
    }
    container::Writer w(k_kind, k_version);
    w.add_array(k_chunk_models, records);
    w.add_array(k_chunk_frames, frames);
    w.add(k_chunk_strings, strings.bytes());
    return w.finish();
}

Cameras read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() != k_version) {
        throw ParseError("unsupported .okc version " + std::to_string(r.version()));
    }
    const auto records = r.read_array<ModelRecord>(k_chunk_models);
    const auto frames = r.read_array<FrameRecord>(k_chunk_frames);
    container::StringTable strings(r.require(k_chunk_strings));
    Cameras out;
    for (const ModelRecord& m : records) {
        if (uint64_t(m.first_frame) + m.frame_count > frames.size()) {
            throw ParseError(".okc frames out of range");
        }
        Model model;
        model.path = std::string(strings.get(m.path));
        model.frustum_right = m.frustum_right;
        model.rotates = m.rotates != 0;
        for (uint32_t i = 0; i < m.frame_count; ++i) {
            const FrameRecord& f = frames[m.first_frame + i];
            model.frames.push_back({{f.t[0], f.t[1], f.t[2]}, {f.a[0], f.a[1], f.a[2]}, f.right});
        }
        out.models.push_back(std::move(model));
    }
    return out;
}

} // namespace spud::okc
