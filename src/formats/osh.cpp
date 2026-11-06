#include "formats/osh.hpp"

#include "core/error.hpp"

namespace spud::osh {

namespace {

constexpr auto k_info = container::fourcc("INFO");
constexpr auto k_mesh = container::fourcc("MESH");
constexpr auto k_impo = container::fourcc("IMPO");
constexpr auto k_tree = container::fourcc("TREE");
constexpr auto k_tcel = container::fourcc("TCEL");
constexpr auto k_regn = container::fourcc("REGN");
constexpr auto k_prox = container::fourcc("PROX");
constexpr auto k_strs = container::fourcc("STRS");

struct InfoRecord {
    int32_t cell_min[2];
    int32_t cell_max[2];
    uint32_t colour_per_cell;
    uint32_t normal_per_cell;
    uint32_t colour;
    uint32_t normal;
};

struct ImpostorRecord {
    uint32_t colour;
    uint32_t normal;
    uint32_t frames;
    float centre[3];
    float radius;
    float lateral;
};

} // namespace

std::vector<std::byte> write(const FarView& view) {
    container::Writer w(k_kind, k_version);
    container::StringTableBuilder strings;
    InfoRecord info{};
    info.cell_min[0] = view.cell_min[0];
    info.cell_min[1] = view.cell_min[1];
    info.cell_max[0] = view.cell_max[0];
    info.cell_max[1] = view.cell_max[1];
    info.colour_per_cell = view.colour_per_cell;
    info.normal_per_cell = view.normal_per_cell;
    info.colour = strings.add(view.colour);
    info.normal = strings.add(view.normal);
    w.add_struct(k_info, info);
    std::vector<uint32_t> meshes;
    meshes.reserve(view.meshes.size());
    for (const auto& m : view.meshes) {
        meshes.push_back(strings.add(m));
    }
    w.add_array(k_mesh, meshes);
    std::vector<ImpostorRecord> impostors;
    impostors.reserve(view.impostors.size());
    for (const auto& i : view.impostors) {
        impostors.push_back({strings.add(i.colour), strings.add(i.normal), i.frames, {i.centre[0], i.centre[1], i.centre[2]},
                             i.radius, i.lateral});
    }
    w.add_array(k_impo, impostors);
    w.add_array(k_tree, view.trees);
    w.add_array(k_tcel, view.tree_cells);
    w.add_array(k_regn, view.regions);
    w.add_array(k_prox, view.proxies);
    w.add(k_strs, strings.bytes());
    return w.finish();
}

FarView read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() != k_version) {
        throw ParseError("unsupported .osh version");
    }
    container::StringTable strings(r.require(k_strs));
    FarView view;
    const auto info = r.read_struct<InfoRecord>(k_info);
    view.cell_min[0] = info.cell_min[0];
    view.cell_min[1] = info.cell_min[1];
    view.cell_max[0] = info.cell_max[0];
    view.cell_max[1] = info.cell_max[1];
    view.colour_per_cell = info.colour_per_cell;
    view.normal_per_cell = info.normal_per_cell;
    view.colour = strings.get(info.colour);
    view.normal = strings.get(info.normal);
    for (uint32_t offset : r.read_array<uint32_t>(k_mesh)) {
        view.meshes.emplace_back(strings.get(offset));
    }
    for (const auto& rec : r.read_array<ImpostorRecord>(k_impo)) {
        TreeImpostor i;
        i.colour = strings.get(rec.colour);
        i.normal = strings.get(rec.normal);
        i.frames = rec.frames;
        for (int c = 0; c < 3; ++c) {
            i.centre[c] = rec.centre[c];
        }
        i.radius = rec.radius;
        i.lateral = rec.lateral;
        view.impostors.push_back(std::move(i));
    }
    view.trees = r.read_array<TreeInstance>(k_tree);
    view.tree_cells = r.read_array<TreeCell>(k_tcel);
    view.regions = r.read_array<Region>(k_regn);
    view.proxies = r.read_array<Proxy>(k_prox);
    return view;
}

} // namespace spud::osh
