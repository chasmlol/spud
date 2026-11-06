#include "formats/osw.hpp"

#include <algorithm>

namespace spud::osw {

namespace {

constexpr auto k_winf = container::fourcc("WINF");
constexpr auto k_mesh = container::fourcc("MESH");
constexpr auto k_gras = container::fourcc("GRAS");
constexpr auto k_lgrs = container::fourcc("LGRS");
constexpr auto k_ltex = container::fourcc("LTEX");
constexpr auto k_cell = container::fourcc("CELL");
constexpr auto k_land = container::fourcc("LAND");
constexpr auto k_llyr = container::fourcc("LLYR");
constexpr auto k_refs = container::fourcc("REFS");
constexpr auto k_lght = container::fourcc("LGHT");
constexpr auto k_dmat = container::fourcc("DMAT");
constexpr auto k_tree = container::fourcc("TREE");
constexpr auto k_emit = container::fourcc("EMIT");
constexpr auto k_dpas = container::fourcc("DPAS");
constexpr auto k_curc = container::fourcc("CURC");
constexpr auto k_curq = container::fourcc("CURQ");
constexpr auto k_prim = container::fourcc("PRIM");
constexpr auto k_rgns = container::fourcc("RGNS");
constexpr auto k_crgn = container::fourcc("CRGN");
constexpr auto k_cwat = container::fourcc("CWAT");
constexpr auto k_flwc = container::fourcc("FLWC");
constexpr auto k_flwt = container::fourcc("FLWT");
constexpr auto k_wsrf = container::fourcc("WSRF");
constexpr auto k_wvtx = container::fourcc("WVTX");
constexpr auto k_ilgt = container::fourcc("ILGT");
constexpr auto k_door = container::fourcc("DOOR");
constexpr auto k_room = container::fourcc("ROOM");
constexpr auto k_rlnk = container::fourcc("RLNK");
constexpr auto k_rlgt = container::fourcc("RLGT");
constexpr auto k_port = container::fourcc("PORT");
constexpr auto k_occl = container::fourcc("OCCL");
constexpr auto k_strs = container::fourcc("STRS");

struct WorldInfo {
    uint32_t edid;
    uint32_t name;
    uint32_t parent_edid;
    uint32_t default_land_texture;
    uint32_t form_id;
    uint32_t flags;
    int32_t cell_min[2];
    int32_t cell_max[2];
    float cell_size;
    float default_land_height;
    float default_water_height;
    uint32_t reserved[5];
};

struct DirectionalPassRecord {
    uint32_t shader_type;
    uint32_t flags1;
    uint32_t flags2;
    float uv_offset[2];
    float uv_scale[2];
    float emissive[3];
    float alpha;
    float specular[3];
    float glossiness;
    float sparkle[4];
    uint32_t diffuse;
    uint32_t back_light;
};
static_assert(sizeof(DirectionalPassRecord) == 84);

struct RegionRecord {
    uint32_t form_id;
    uint32_t edid;
};

struct DoorRecord {
    uint32_t form_id;
    uint32_t base_form_id;
    uint32_t destination;
    uint32_t flags;
    uint32_t teleport_flags;
    uint32_t destination_space;
    float position[3];
    float rotation[4];
    float scale;
    float bounds_min[3];
    float bounds_max[3];
    float arrival_position[3];
    float arrival_rotation[3];
    float dest_position[3];
    float dest_rotation[4];
    float dest_scale;
    float dest_bounds_min[3];
    float dest_bounds_max[3];
    float marker_position[3];
};
static_assert(sizeof(DoorRecord) == 172);

struct LandTextureRecord {
    uint32_t form_id;
    uint32_t diffuse;
    uint32_t normal;
    uint8_t specular_exponent;
    uint8_t flags;
    uint8_t pad[2];
    uint32_t first_grass;
    uint32_t grass_count;
};

} // namespace

std::vector<std::byte> write(const World& world) {
    container::StringTableBuilder strings;

    WorldInfo info{};
    info.edid = strings.add(world.edid);
    info.name = strings.add(world.name);
    info.parent_edid = strings.add(world.parent_edid);
    info.default_land_texture = strings.add(world.default_land_texture);
    info.form_id = world.form_id;
    info.flags = world.flags;
    std::copy_n(world.cell_min, 2, info.cell_min);
    std::copy_n(world.cell_max, 2, info.cell_max);
    info.cell_size = world.cell_size;
    info.default_land_height = world.default_land_height;
    info.default_water_height = world.default_water_height;

    std::vector<uint32_t> meshes;
    meshes.reserve(world.meshes.size());
    for (const auto& m : world.meshes) {
        meshes.push_back(strings.add(m));
    }

    std::vector<LandTextureRecord> ltex;
    ltex.reserve(world.land_textures.size());
    for (const auto& t : world.land_textures) {
        ltex.push_back({t.form_id, strings.add(t.diffuse), strings.add(t.normal), t.specular_exponent, t.flags, {},
                        t.first_grass, t.grass_count});
    }

    std::vector<DirectionalPassRecord> passes;
    for (const auto& p : world.directional_passes) {
        DirectionalPassRecord r{};
        r.shader_type = p.shader_type;
        r.flags1 = p.flags1;
        r.flags2 = p.flags2;
        std::copy_n(p.uv_offset, 2, r.uv_offset);
        std::copy_n(p.uv_scale, 2, r.uv_scale);
        std::copy_n(p.emissive, 3, r.emissive);
        r.alpha = p.alpha;
        std::copy_n(p.specular, 3, r.specular);
        r.glossiness = p.glossiness;
        std::copy_n(p.sparkle, 4, r.sparkle);
        r.diffuse = strings.add(p.diffuse);
        r.back_light = strings.add(p.back_light);
        passes.push_back(r);
    }

    std::vector<RegionRecord> regions;
    for (const auto& reg : world.regions) {
        regions.push_back({reg.form_id, strings.add(reg.edid)});
    }
    std::vector<DoorRecord> doors;
    for (const auto& d : world.doors) {
        DoorRecord r{};
        r.form_id = d.form_id;
        r.base_form_id = d.base_form_id;
        r.destination = d.destination;
        r.flags = d.flags;
        r.teleport_flags = d.teleport_flags;
        r.destination_space = strings.add(d.destination_space);
        std::copy_n(d.position, 3, r.position);
        std::copy_n(d.rotation, 4, r.rotation);
        r.scale = d.scale;
        std::copy_n(d.bounds_min, 3, r.bounds_min);
        std::copy_n(d.bounds_max, 3, r.bounds_max);
        std::copy_n(d.arrival_position, 3, r.arrival_position);
        std::copy_n(d.arrival_rotation, 3, r.arrival_rotation);
        std::copy_n(d.dest_position, 3, r.dest_position);
        std::copy_n(d.dest_rotation, 4, r.dest_rotation);
        r.dest_scale = d.dest_scale;
        std::copy_n(d.dest_bounds_min, 3, r.dest_bounds_min);
        std::copy_n(d.dest_bounds_max, 3, r.dest_bounds_max);
        std::copy_n(d.marker_position, 3, r.marker_position);
        doors.push_back(r);
    }

    container::Writer w(k_kind, k_version);
    w.add_struct(k_winf, info);
    w.add_array(k_mesh, meshes);
    w.add_array(k_ltex, ltex);
    w.add_array(k_gras, world.grasses);
    w.add_array(k_lgrs, world.land_grass);
    w.add_array(k_cell, world.cells);
    w.add_array(k_land, world.lands);
    w.add_array(k_llyr, world.land_layers);
    w.add_array(k_refs, world.refs);
    w.add_array(k_lght, world.lights);
    w.add_array(k_dmat, world.directional_materials);
    w.add_array(k_tree, world.trees);
    w.add_array(k_emit, world.emittances);
    w.add_array(k_dpas, passes);
    w.add_array(k_curc, world.currents);
    w.add_array(k_curq, world.current_quads);
    w.add_array(k_prim, world.primitives);
    w.add_array(k_rgns, regions);
    w.add_array(k_crgn, world.cell_regions);
    w.add_array(k_cwat, world.cell_waters);
    w.add_array(k_flwc, world.flow_cells);
    w.add_array(k_flwt, world.flow_texels);
    w.add_array(k_wsrf, world.water_surfaces);
    w.add_array(k_wvtx, world.water_vertices);
    w.add_struct(k_ilgt, world.lighting);
    w.add_array(k_door, doors);
    w.add_array(k_room, world.rooms);
    w.add_array(k_rlnk, world.room_links);
    w.add_array(k_rlgt, world.room_lighting);
    w.add_array(k_port, world.portals);
    w.add_array(k_occl, world.occluders);
    w.add(k_strs, strings.bytes());
    return w.finish();
}

World read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    // Earlier versions have other Ref and LandTexture layouts: re-import (docs/formats/osw.md, "Versions").
    if (r.version() != k_version) {
        throw ParseError("unsupported .osw version " + std::to_string(r.version()));
    }
    container::StringTable strings(r.require(k_strs));
    auto info = r.read_struct<WorldInfo>(k_winf);

    World world;
    world.edid = strings.get(info.edid);
    world.name = strings.get(info.name);
    world.parent_edid = strings.get(info.parent_edid);
    world.default_land_texture = strings.get(info.default_land_texture);
    world.form_id = info.form_id;
    world.flags = info.flags;
    std::copy_n(info.cell_min, 2, world.cell_min);
    std::copy_n(info.cell_max, 2, world.cell_max);
    world.cell_size = info.cell_size;
    world.default_land_height = info.default_land_height;
    world.default_water_height = info.default_water_height;

    for (auto offset : r.read_array<uint32_t>(k_mesh)) {
        world.meshes.emplace_back(strings.get(offset));
    }
    for (const auto& t : r.read_array<LandTextureRecord>(k_ltex)) {
        world.land_textures.push_back(
            {t.form_id, std::string(strings.get(t.diffuse)), std::string(strings.get(t.normal)), t.specular_exponent,
             t.flags, t.first_grass, t.grass_count});
    }
    world.grasses = r.read_array<Grass>(k_gras);
    world.land_grass = r.read_array<uint32_t>(k_lgrs);
    world.cells = r.read_array<Cell>(k_cell);
    world.lands = r.read_array<Land>(k_land);
    world.land_layers = r.read_array<LandLayer>(k_llyr);
    world.refs = r.read_array<Ref>(k_refs);
    world.lights = r.read_array<Light>(k_lght);
    world.directional_materials = r.read_array<DirectionalMaterial>(k_dmat);
    world.trees = r.read_array<Tree>(k_tree);
    world.emittances = r.read_array<Emittance>(k_emit);
    for (const auto& rec : r.read_array<DirectionalPassRecord>(k_dpas)) {
        DirectionalPass p;
        p.shader_type = rec.shader_type;
        p.flags1 = rec.flags1;
        p.flags2 = rec.flags2;
        std::copy_n(rec.uv_offset, 2, p.uv_offset);
        std::copy_n(rec.uv_scale, 2, p.uv_scale);
        std::copy_n(rec.emissive, 3, p.emissive);
        p.alpha = rec.alpha;
        std::copy_n(rec.specular, 3, p.specular);
        p.glossiness = rec.glossiness;
        std::copy_n(rec.sparkle, 4, p.sparkle);
        p.diffuse = strings.get(rec.diffuse);
        p.back_light = strings.get(rec.back_light);
        world.directional_passes.push_back(std::move(p));
    }
    world.currents = r.read_array<CellCurrent>(k_curc);
    world.current_quads = r.read_array<CurrentQuad>(k_curq);
    world.primitives = r.read_array<Primitive>(k_prim);
    for (const auto& reg : r.read_array<RegionRecord>(k_rgns)) {
        world.regions.push_back({reg.form_id, std::string(strings.get(reg.edid))});
    }
    world.cell_regions = r.read_array<uint16_t>(k_crgn);
    world.cell_waters = r.read_array<uint32_t>(k_cwat);
    world.flow_cells = r.read_array<FlowCell>(k_flwc);
    world.flow_texels = r.read_array<uint8_t>(k_flwt);
    world.water_surfaces = r.read_array<WaterSurface>(k_wsrf);
    world.water_vertices = r.read_array<WaterVertex>(k_wvtx);
    world.lighting = r.read_struct<CellLighting>(k_ilgt);
    for (const auto& rec : r.read_array<DoorRecord>(k_door)) {
        Door d;
        d.form_id = rec.form_id;
        d.base_form_id = rec.base_form_id;
        d.destination = rec.destination;
        d.flags = rec.flags;
        d.teleport_flags = rec.teleport_flags;
        d.destination_space = strings.get(rec.destination_space);
        std::copy_n(rec.position, 3, d.position);
        std::copy_n(rec.rotation, 4, d.rotation);
        d.scale = rec.scale;
        std::copy_n(rec.bounds_min, 3, d.bounds_min);
        std::copy_n(rec.bounds_max, 3, d.bounds_max);
        std::copy_n(rec.arrival_position, 3, d.arrival_position);
        std::copy_n(rec.arrival_rotation, 3, d.arrival_rotation);
        std::copy_n(rec.dest_position, 3, d.dest_position);
        std::copy_n(rec.dest_rotation, 4, d.dest_rotation);
        d.dest_scale = rec.dest_scale;
        std::copy_n(rec.dest_bounds_min, 3, d.dest_bounds_min);
        std::copy_n(rec.dest_bounds_max, 3, d.dest_bounds_max);
        std::copy_n(rec.marker_position, 3, d.marker_position);
        world.doors.push_back(std::move(d));
    }
    world.rooms = r.read_array<Room>(k_room);
    world.room_links = r.read_array<uint32_t>(k_rlnk);
    world.room_lighting = r.read_array<CellLighting>(k_rlgt);
    world.portals = r.read_array<Portal>(k_port);
    world.occluders = r.read_array<Occluder>(k_occl);
    return world;
}

} // namespace spud::osw
