#include "formats/osm.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace spud::osm {

namespace {

constexpr auto k_info = container::fourcc("INFO");
constexpr auto k_vert = container::fourcc("VERT");
constexpr auto k_indx = container::fourcc("INDX");
constexpr auto k_subm = container::fourcc("SUBM");
constexpr auto k_lods = container::fourcc("LODS");
constexpr auto k_matl = container::fourcc("MATL");
constexpr auto k_strs = container::fourcc("STRS");
constexpr auto k_uvct = container::fourcc("UVCT");
constexpr auto k_keys = container::fourcc("KEYS");
constexpr auto k_skvt = container::fourcc("SKVT");
constexpr auto k_skbn = container::fourcc("SKBN");
constexpr auto k_sksh = container::fourcc("SKSH");
constexpr auto k_sknm = container::fourcc("SKNM");
constexpr auto k_skpt = container::fourcc("SKPT");
constexpr auto k_skpi = container::fourcc("SKPI");
constexpr auto k_skmo = container::fourcc("SKMO");
constexpr auto k_skat = container::fourcc("SKAT");

struct Info {
    float bounds_min[3];
    float bounds_max[3];
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t submesh_count;
    uint32_t material_count;
    float fade_bound[4];
};

struct MaterialRecord {
    MaterialParams params;
    uint32_t textures[k_texture_slots];
};

struct ControllerRecord {
    uint32_t material;
    uint8_t variable;
    uint8_t key_type;
    uint16_t flags;
    float frequency;
    float phase;
    float start;
    float stop;
    float value;
    uint32_t first_key;
    uint32_t key_count;
};
static_assert(sizeof(ControllerRecord) == 36);

struct SkinBoneRecord {
    uint32_t name;
    float skin_to_bone[3][4];
};
static_assert(sizeof(SkinBoneRecord) == 52);

} // namespace

void compute_bounds(Mesh& mesh) {
    for (int i = 0; i < 3; ++i) {
        mesh.bounds_min[i] = std::numeric_limits<float>::max();
        mesh.bounds_max[i] = std::numeric_limits<float>::lowest();
    }
    bool any = false;
    for (const auto& sm : mesh.submeshes) {
        const auto& m = sm.transform;
        const size_t end = std::min<size_t>(size_t{sm.base_vertex} + sm.vertex_count, mesh.vertices.size());
        for (size_t k = sm.base_vertex; k < end; ++k) {
            const float* p = mesh.vertices[k].position;
            for (int i = 0; i < 3; ++i) {
                const float c = m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3];
                mesh.bounds_min[i] = std::min(mesh.bounds_min[i], c);
                mesh.bounds_max[i] = std::max(mesh.bounds_max[i], c);
            }
            any = true;
        }
    }
    if (!any) {
        std::fill(std::begin(mesh.bounds_min), std::end(mesh.bounds_min), 0.0f);
        std::fill(std::begin(mesh.bounds_max), std::end(mesh.bounds_max), 0.0f);
    }
}

std::vector<std::byte> write(const Mesh& mesh) {
    container::StringTableBuilder strings;
    std::vector<MaterialRecord> materials;
    materials.reserve(mesh.materials.size());
    for (const auto& m : mesh.materials) {
        MaterialRecord rec{};
        rec.params = m.params;
        for (uint32_t i = 0; i < k_texture_slots; ++i) {
            rec.textures[i] = m.textures[i].empty() ? container::k_no_string : strings.add(m.textures[i]);
        }
        materials.push_back(rec);
    }
    std::vector<SkinBoneRecord> bones;
    bones.reserve(mesh.skin.bones.size());
    for (const auto& b : mesh.skin.bones) {
        SkinBoneRecord rec{};
        rec.name = strings.add(b.name);
        std::memcpy(rec.skin_to_bone, b.skin_to_bone, sizeof rec.skin_to_bone);
        bones.push_back(rec);
    }
    std::vector<ControllerRecord> controllers;
    std::vector<std::array<float, 5>> keys;
    for (uint32_t mi = 0; mi < mesh.materials.size(); ++mi) {
        for (const auto& c : mesh.materials[mi].controllers) {
            controllers.push_back({mi, c.variable, c.key_type, c.flags, c.frequency, c.phase, c.start, c.stop,
                                   c.value, static_cast<uint32_t>(keys.size()),
                                   static_cast<uint32_t>(c.keys.size())});
            keys.insert(keys.end(), c.keys.begin(), c.keys.end());
        }
    }

    Info info{};
    std::copy(std::begin(mesh.bounds_min), std::end(mesh.bounds_min), info.bounds_min);
    std::copy(std::begin(mesh.bounds_max), std::end(mesh.bounds_max), info.bounds_max);
    info.vertex_count = static_cast<uint32_t>(mesh.vertices.size());
    info.index_count = static_cast<uint32_t>(mesh.indices.size());
    info.submesh_count = static_cast<uint32_t>(mesh.submeshes.size());
    info.material_count = static_cast<uint32_t>(mesh.materials.size());
    std::copy(std::begin(mesh.fade_bound), std::end(mesh.fade_bound), info.fade_bound);

    container::Writer w(k_kind, k_version);
    w.add_struct(k_info, info);
    w.add_array(k_vert, mesh.vertices);
    w.add_array(k_indx, mesh.indices);
    w.add_array(k_subm, mesh.submeshes);
    w.add_array(k_lods, mesh.lods);
    w.add_array(k_matl, materials);
    w.add_array(k_uvct, controllers);
    w.add_array(k_keys, keys);
    if (!mesh.skin.shapes.empty()) {
        w.add_array(k_skvt, mesh.skin.vertices);
        w.add_array(k_skbn, bones);
        w.add_array(k_sksh, mesh.skin.shapes);
        w.add_array(k_skpt, mesh.skin.parts);
        w.add_array(k_skpi, mesh.skin.part_indices);
        w.add_array(k_skmo, mesh.skin.morph);
        w.add_struct(k_skat, strings.add(mesh.skin.attach));
        std::vector<uint32_t> names;
        for (const std::string& name : mesh.skin.shape_names) {
            names.push_back(strings.add(name));
        }
        w.add_array(k_sknm, names);
    }
    w.add(k_strs, strings.bytes());
    return w.finish();
}

Mesh read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() < k_min_version || r.version() > k_version) {
        throw ParseError("unsupported .osm version " + std::to_string(r.version()));
    }
    auto info = r.read_struct<Info>(k_info);
    container::StringTable strings(r.require(k_strs));

    Mesh mesh;
    std::copy(std::begin(info.bounds_min), std::end(info.bounds_min), mesh.bounds_min);
    std::copy(std::begin(info.bounds_max), std::end(info.bounds_max), mesh.bounds_max);
    std::copy(std::begin(info.fade_bound), std::end(info.fade_bound), mesh.fade_bound);
    mesh.vertices = r.read_array<Vertex>(k_vert);
    mesh.indices = r.read_array<uint32_t>(k_indx);
    mesh.submeshes = r.read_array<Submesh>(k_subm);
    mesh.lods = r.read_array<SubmeshLod>(k_lods);
    for (const auto& rec : r.read_array<MaterialRecord>(k_matl)) {
        Material m;
        m.params = rec.params;
        for (uint32_t i = 0; i < k_texture_slots; ++i) {
            m.textures[i] = std::string(strings.get(rec.textures[i]));
        }
        mesh.materials.push_back(std::move(m));
    }
    const auto keys = r.read_array<std::array<float, 5>>(k_keys);
    for (const auto& rec : r.read_array<ControllerRecord>(k_uvct)) {
        if (rec.material >= mesh.materials.size() || size_t{rec.first_key} + rec.key_count > keys.size()) {
            throw ParseError("UV controller out of range");
        }
        MaterialController c;
        c.variable = rec.variable;
        c.key_type = rec.key_type;
        c.flags = rec.flags;
        c.frequency = rec.frequency;
        c.phase = rec.phase;
        c.start = rec.start;
        c.stop = rec.stop;
        c.value = rec.value;
        c.keys.assign(keys.begin() + rec.first_key, keys.begin() + rec.first_key + rec.key_count);
        if (rec.variable >= k_material_variable_count) {
            throw ParseError("controller variable out of range");
        }
        mesh.materials[rec.material].controllers.push_back(std::move(c));
    }
    if (r.find(k_sksh)) {
        mesh.skin.vertices = r.read_array<SkinVertex>(k_skvt);
        mesh.skin.shapes = r.read_array<SkinShape>(k_sksh);
        mesh.skin.parts = r.read_array<uint16_t>(k_skpt);
        mesh.skin.part_indices = r.read_array<uint32_t>(k_skpi);
        mesh.skin.morph = r.read_array<Vertex>(k_skmo);
        mesh.skin.attach = std::string(strings.get(r.read_struct<uint32_t>(k_skat)));
        if (r.find(k_sknm)) {
            for (const uint32_t name : r.read_array<uint32_t>(k_sknm)) {
                mesh.skin.shape_names.emplace_back(strings.get(name));
            }
        }
        for (const auto& rec : r.read_array<SkinBoneRecord>(k_skbn)) {
            SkinBone b;
            b.name = std::string(strings.get(rec.name));
            std::memcpy(b.skin_to_bone, rec.skin_to_bone, sizeof b.skin_to_bone);
            mesh.skin.bones.push_back(std::move(b));
        }
        if (mesh.skin.vertices.size() != mesh.vertices.size() || mesh.skin.shapes.size() != mesh.submeshes.size() ||
            mesh.skin.part_indices.size() != mesh.skin.parts.size() ||
            (!mesh.skin.morph.empty() && mesh.skin.morph.size() != mesh.vertices.size())) {
            throw ParseError("skin data doesn't match the mesh");
        }
        for (const auto& shape : mesh.skin.shapes) {
            if (size_t{shape.first_bone} + shape.bone_count > mesh.skin.bones.size() ||
                size_t{shape.first_part} + shape.part_count > mesh.skin.parts.size()) {
                throw ParseError("skin shape out of range");
            }
        }
    }
    return mesh;
}

} // namespace spud::osm
