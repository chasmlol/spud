#include "formats/ots.hpp"

namespace spud::ots {

namespace {

constexpr auto k_swap = container::fourcc("SWAP");
constexpr auto k_shap = container::fourcc("SHAP");
constexpr auto k_strs = container::fourcc("STRS");

struct SwapRecord {
    uint32_t base_form_id;
    uint32_t variant;
    uint32_t first_shape;
    uint32_t shape_count;
};

struct ShapeRecord {
    uint32_t submesh;
    uint32_t name; // string offset
    uint32_t textures[8];
};

} // namespace

std::vector<std::byte> write(const File& file) {
    container::StringTableBuilder strings;
    std::vector<SwapRecord> swaps;
    std::vector<ShapeRecord> shapes;
    for (const Swap& s : file.swaps) {
        swaps.push_back({s.base_form_id, s.variant, uint32_t(shapes.size()), uint32_t(s.shapes.size())});
        for (const Shape& shape : s.shapes) {
            ShapeRecord r{shape.submesh, strings.add(shape.name), {}};
            for (size_t i = 0; i < shape.textures.size(); ++i) {
                r.textures[i] = strings.add(shape.textures[i]);
            }
            shapes.push_back(r);
        }
    }
    container::Writer w(k_kind, k_version);
    w.add_array(k_swap, swaps);
    w.add_array(k_shap, shapes);
    w.add(k_strs, strings.bytes());
    return w.finish();
}

File read(std::span<const std::byte> bytes) {
    container::Reader r(bytes, k_kind);
    if (r.version() != k_version) {
        return {}; // an older file: the next import writes it again
    }
    container::StringTable strings(r.require(k_strs));
    const auto swaps = r.read_array<SwapRecord>(k_swap);
    const auto shapes = r.read_array<ShapeRecord>(k_shap);
    File f;
    for (const SwapRecord& s : swaps) {
        Swap out;
        out.base_form_id = s.base_form_id;
        out.variant = s.variant;
        for (uint32_t i = 0; i < s.shape_count; ++i) {
            const ShapeRecord& q = shapes[s.first_shape + i];
            Shape shape;
            shape.submesh = q.submesh;
            shape.name = std::string(strings.get(q.name));
            for (size_t t = 0; t < shape.textures.size(); ++t) {
                shape.textures[t] = std::string(strings.get(q.textures[t]));
            }
            out.shapes.push_back(std::move(shape));
        }
        f.swaps.push_back(std::move(out));
    }
    return f;
}

} // namespace spud::ots
