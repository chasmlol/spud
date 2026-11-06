#include "importer/convert/mesh_import.hpp"

#include "core/error.hpp"
#include "core/path.hpp"
#include "core/units.hpp"

#include <glm/glm.hpp>
#include <meshoptimizer.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <set>

namespace spud::importer {

using namespace spud::legacy;

namespace {

constexpr int k_max_depth = 256;
constexpr size_t k_max_warnings = 32;

// Skyrim shader property flags (SkyrimShaderPropertyFlags1/2 in nif.xml).
constexpr uint32_t k_f1_vertex_alpha = 1u << 3;
constexpr uint32_t k_f1_env_map = 1u << 7;
constexpr uint32_t k_f1_model_space_normals = 1u << 12;
constexpr uint32_t k_f1_decal = 1u << 26;
constexpr uint32_t k_f1_dynamic_decal = 1u << 27;
constexpr uint32_t k_f1_z_test = 1u << 31;
constexpr uint32_t k_f2_z_write = 1u << 0;
constexpr uint32_t k_f2_double_sided = 1u << 4;
constexpr uint32_t k_f2_vertex_colors = 1u << 5;
constexpr uint32_t k_f2_glow_map = 1u << 6;
constexpr uint32_t k_f2_tree_anim = 1u << 29;

// NiAlphaProperty flags.
constexpr uint16_t k_alpha_blend = 1u << 0;
constexpr uint16_t k_alpha_test = 1u << 9;

// world = linear * local + translation (Skyrim units).
struct Transform {
    glm::mat3 linear{1.0f};
    glm::vec3 translation{0.0f};
};

Transform compose(const Transform& parent, const NiAVObject& o) {
    return {parent.linear * (o.rotation * o.scale), parent.linear * o.translation + parent.translation};
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

// The top level of meshes/ holds the Creation Kit's own visualisation meshes: markers (XMarker, map, light,
// sound, teleport, ...) and transform gizmos. Their geometry is ordinary, with nothing in the file marking
// it editor-only; the game never draws it because marker references are hidden, so we drop it outright.
bool is_editor_asset(std::string_view source_path) {
    constexpr std::string_view k_root = "meshes/";
    if (!source_path.starts_with(k_root)) {
        return false;
    }
    auto name = source_path.substr(k_root.size());
    if (name.find('/') != std::string_view::npos) {
        return false;
    }
    return starts_with_ci(name, "marker") || name.ends_with("gizmo.nif");
}

std::string texture_path(std::string_view raw) {
    auto p = path::normalize(raw);
    return p.empty() ? p : path::with_root(p, "textures");
}

// Any unit vector perpendicular to n.
glm::vec3 perpendicular(const glm::vec3& n) {
    glm::vec3 axis = std::abs(n.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    return glm::normalize(glm::cross(axis, n));
}

using Triangle = std::array<uint16_t, 3>;

// One shape's geometry in its own space, before transforming.
struct ShapeSource {
    std::span<const glm::vec3> positions;
    std::span<const glm::vec3> normals;
    std::span<const glm::vec2> uvs;
    std::span<const glm::u8vec4> colors;
    std::span<const Triangle> triangles;
    // The game's packed tangent frame (BSTriShape), or float tangents/bitangents (NiTriShapeData).
    std::span<const glm::u8vec4> packed_normals;
    std::span<const glm::u8vec4> packed_tangents;
    std::span<const float> bitangent_x;
    std::span<const glm::vec3> tangents;
    std::span<const glm::vec3> bitangents;
};

// Skyrim's packing of a unit-vector component into a byte, decoded by its shaders as 2v - 1. The engine's own
// rounding for converted (non-packed) data is not known yet; round to nearest.
uint8_t pack_unorm8(float v) {
    return static_cast<uint8_t>(std::lround(std::clamp(v * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f));
}

float unpack_unorm8(uint8_t b) { return float(b) / 255.0f * 2.0f - 1.0f; }

class MeshBuilder {
public:
    bool harvested = false;
    MeshBuilder(const NifFile& nif, MeshImportResult& out, bool skinned = false, int damage_stage = 0,
                bool keep_vertices = false, bool object = false)
        : m_nif(nif), m_out(out), m_on_path(nif.blocks.size(), 0), m_parent(nif.blocks.size(), -1),
          m_world(nif.blocks.size()), m_skinned(skinned), m_damage_stage(damage_stage), m_keep_vertices(keep_vertices), m_object(object) {
        for (size_t i = 0; i < nif.blocks.size(); ++i) {
            if (const auto* node = std::get_if<NiNode>(&nif.blocks[i].data)) {
                for (auto c : node->children) {
                    if (nif.valid(c) && m_parent[static_cast<size_t>(c)] == -1) {
                        m_parent[static_cast<size_t>(c)] = static_cast<NifRef>(i);
                    }
                }
            }
        }
    }

    void visit(NifRef ref, const Transform& parent, int depth) {
        if (!m_nif.valid(ref)) {
            if (ref != -1) {
                warn(std::format("invalid block reference {}", ref));
            }
            return;
        }
        auto index = static_cast<size_t>(ref);
        const auto& block = m_nif.blocks[index];
        auto type = m_nif.type_name(ref);
        if (block.status == NifBlockStatus::mismatch) {
            warn(std::format("block {} ({}) could not be decoded: {}", ref, type, block.error));
            return;
        }
        const NiAVObject* av = m_nif.av_object(ref);
        if (!av) {
            if (!is_ignored_child(type)) {
                warn(std::format("block {} ({}) not drawn: unsupported type", ref, type));
            }
            return;
        }
        if (starts_with_ci(av->name, "EditorMarker") || type == "RootCollisionNode") {
            return;
        }
        if (type == "BSDamageStage") {
            // Destructible objects store every stage hidden and the game shows the one covering the current
            // destruction stage; an intact object shows the stage whose range starts at 0.
            const auto* stage = std::get_if<NiNode>(&block.data);
            if (!stage || m_damage_stage < stage->range[0] || m_damage_stage > stage->range[1]) {
                return;
            }
        } else if (av->hidden() && !blood_shape(block)) {
            return;
        }
        if (m_on_path[index] || depth > k_max_depth) {
            warn(std::format("block {} ({}) forms a cycle or is nested too deeply", ref, type));
            return;
        }
        // The reference's transform takes the place of the root's own: a root's rotation, translation and scale never
        // reach the screen (frozenmarshicefloechunks01.nif, whose root BSFadeNode is
        // rotated by 97.7 degrees, draws with the reference's rotation alone).
        Transform world = depth == 0 ? parent : compose(parent, *av);
        if (const auto* node = std::get_if<NiNode>(&block.data)) {
            m_on_path[index] = 1;
            const bool scabbard = m_skinned && av->name == "Scb";
            m_in_scabbard = m_in_scabbard || scabbard;
            // An NiBillboardNode is re-oriented by the engine every cull (section 16.1): its children's shapes keep
            // their transform below it, and the node's own frame goes on the submesh. Nested billboards: the inner
            // one is taken with its static frame inside the outer.
            const auto saved = m_billboard;
            Transform below = world;
            if (node->billboard_mode) {
                m_billboard = Billboard{*node->billboard_mode, world};
                below = Transform{};
            }
            if (auto child = drawn_child(*node)) {
                if (*child < node->children.size()) {
                    visit(node->children[*child], below, depth + 1);
                }
            } else {
                for (auto c : node->children) {
                    visit(c, below, depth + 1);
                }
            }
            m_billboard = saved;
            m_on_path[index] = 0;
            m_in_scabbard = m_in_scabbard && !scabbard;
        } else if (const auto* shape = std::get_if<BSTriShape>(&block.data)) {
            add_tri_shape(ref, *shape, world);
        } else if (const auto* geom = std::get_if<NiGeometry>(&block.data)) {
            add_geometry(ref, *geom, world);
        }
    }

    // The root's world bound in the reference's frame, as the original engine's update pass merges it
    // in Skyrim units. Unlike drawing, hidden children count.
    void fade_bound(NifRef root) {
        const Sphere s = bound_of(root, Transform{}, 1.0f, 0);
        if (std::bit_cast<uint32_t>(s.radius) != 0) {
            for (int i = 0; i < 3; ++i) {
                m_out.mesh.fade_bound[i] = s.centre[i] * units::k_skyrim_unit_m;
            }
            m_out.mesh.fade_bound[3] = s.radius * units::k_skyrim_unit_m;
        }
    }

    // Vertices and triangles go in the order the GPU's caches like: the same surface, fewer vertices shaded and fetched.
    // Unused vertices stay, last: they count towards the bounds. Triangles keep the source's order wherever it shows,
    // that is wherever depth doesn't settle which one wins.
    void order_for_gpu() {
        auto& mesh = m_out.mesh;
        constexpr uint32_t depth = osm::material_z_write | osm::material_z_test;
        constexpr uint32_t ordered = osm::material_alpha_blend | osm::material_decal;
        std::vector<uint32_t> remap;
        std::vector<osm::Vertex> vertices;
        std::vector<osm::SkinVertex> skin;
        for (size_t si = 0; si < mesh.submeshes.size(); ++si) {
            const auto& sub = mesh.submeshes[si];
            uint32_t* indices = mesh.indices.data() + sub.first_index;
            const uint32_t flags = mesh.materials[sub.material].params.flags;
            if ((flags & depth) == depth && (flags & ordered) == 0) {
                // A skinned shape's partitions show and hide on their own: each keeps its run of indices.
                uint32_t at = 0;
                if (!mesh.skin.shapes.empty()) {
                    const auto& shape = mesh.skin.shapes[si];
                    uint32_t total = 0;
                    for (uint32_t i = 0; i < shape.part_count; ++i) {
                        total += mesh.skin.part_indices[shape.first_part + i];
                    }
                    if (total == sub.index_count) {
                        for (uint32_t i = 0; i < shape.part_count; ++i) {
                            const uint32_t n = mesh.skin.part_indices[shape.first_part + i];
                            meshopt_optimizeVertexCache(indices + at, indices + at, n, sub.vertex_count);
                            at += n;
                        }
                    }
                }
                if (at == 0) {
                    meshopt_optimizeVertexCache(indices, indices, sub.index_count, sub.vertex_count);
                }
            }
            if (m_keep_vertices) {
                continue;
            }
            remap.resize(sub.vertex_count);
            uint32_t used = static_cast<uint32_t>(
                meshopt_optimizeVertexFetchRemap(remap.data(), indices, sub.index_count, sub.vertex_count));
            const uint32_t first_unused = used;
            osm::Vertex* source = mesh.vertices.data() + sub.base_vertex;
            vertices.resize(sub.vertex_count);
            for (uint32_t v = 0; v < sub.vertex_count; ++v) {
                vertices[remap[v] == ~0u ? used++ : remap[v]] = source[v];
            }
            std::copy(vertices.begin(), vertices.end(), source);
            if (!mesh.skin.vertices.empty()) {
                osm::SkinVertex* from = mesh.skin.vertices.data() + sub.base_vertex;
                skin.resize(sub.vertex_count);
                uint32_t unused = first_unused;
                for (uint32_t v = 0; v < sub.vertex_count; ++v) {
                    skin[remap[v] == ~0u ? unused++ : remap[v]] = from[v];
                }
                std::copy(skin.begin(), skin.end(), from);
            }
            for (uint32_t i = 0; i < sub.index_count; ++i) {
                indices[i] = remap[indices[i]];
            }
        }
    }

    // The chain of simplified levels: over the same vertices, in the same materials.
    void build_lods() {
        osm::Mesh& mesh = m_out.mesh;
        mesh.lods.assign(mesh.submeshes.size() * osm::k_lod_levels, {});
        constexpr float k_ratios[osm::k_lod_levels] = {0.5f, 0.25f, 0.1f};
        constexpr uint32_t k_min_triangles = 96;
        constexpr float k_leaf_m = 0.1f;
        // Normal, uv, colour. The colour counts: it's the art's baked shading, and without it a simplified level smears
        // the dark of a recess across whatever is left, which reads as the object changing as the level swaps.
        constexpr uint32_t k_attributes = 9;
        constexpr float k_attribute_weights[k_attributes] = {0.5f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        std::vector<float> attributes;
        std::vector<uint32_t> source, level;
        for (size_t si = 0; si < mesh.submeshes.size(); ++si) {
            const osm::Submesh s = mesh.submeshes[si];
            const auto& p = mesh.materials[s.material].params;
            // Alpha-tested shapes have their outline in the texture, so a level can lose all of it for an error that reads
            // as nothing. Cut-outs (a door's ironwork) stay whole. Foliage keeps its levels for far trees, but a lost leaf
            // strays by a leaf, whatever the simplifier says.
            const bool alpha_tested = p.flags & osm::material_alpha_test;
            const bool cut_out = alpha_tested && !(p.flags & osm::material_tree_anim);
            if (s.index_count / 3 < k_min_triangles || p.model != osm::ShaderModel::lighting || cut_out ||
                s.billboard_mode != osm::k_no_billboard || (p.flags & (osm::material_alpha_blend | osm::material_decal))) {
                continue;
            }
            const float scale = glm::length(glm::vec3(s.transform[0][0], s.transform[1][0], s.transform[2][0]));
            const osm::Vertex* v = mesh.vertices.data() + s.base_vertex;
            attributes.resize(size_t(s.vertex_count) * k_attributes);
            for (uint32_t i = 0; i < s.vertex_count; ++i) {
                for (int c = 0; c < 3; ++c) {
                    attributes[i * k_attributes + c] = float(v[i].normal[c]) * (2.0f / 255.0f) - 1.0f;
                }
                attributes[i * k_attributes + 3] = v[i].uv[0];
                attributes[i * k_attributes + 4] = v[i].uv[1];
                for (int c = 0; c < 4; ++c) {
                    attributes[i * k_attributes + 5 + c] = float(v[i].color[c]) * (1.0f / 255.0f);
                }
            }
            source.assign(mesh.indices.begin() + s.first_index, mesh.indices.begin() + s.first_index + s.index_count);
            uint32_t previous = s.index_count;
            for (uint32_t l = 0; l < osm::k_lod_levels; ++l) {
                const size_t target = size_t(float(s.index_count) * k_ratios[l]) / 3 * 3;
                level.resize(source.size());
                float error = 0.0f;
                const size_t count = meshopt_simplifyWithAttributes(
                    level.data(), source.data(), source.size(), v[0].position, s.vertex_count, sizeof(osm::Vertex),
                    attributes.data(), k_attributes * sizeof(float), k_attribute_weights, k_attributes, nullptr, target, 1e9f,
                    meshopt_SimplifyErrorAbsolute, &error);
                if (count == 0 || float(count) > 0.85f * float(previous)) {
                    continue; // locked by seams: this level would save nothing
                }
                osm::SubmeshLod& lod = mesh.lods[si * osm::k_lod_levels + l];
                lod.first_index = uint32_t(mesh.indices.size());
                lod.index_count = uint32_t(count);
                lod.error = std::max(error * scale, alpha_tested ? k_leaf_m : 0.0f);
                mesh.indices.insert(mesh.indices.end(), level.begin(), level.begin() + count);
                previous = uint32_t(count);
            }
        }
    }

    void finish() {
        osm::compute_bounds(m_out.mesh);
        order_for_gpu();
        if (!m_skinned) { // an actor's parts draw whole, near the camera
            build_lods();
        }
        std::set<std::string> seen;
        for (const auto& m : m_out.mesh.materials) {
            for (const auto& t : m.textures) {
                if (!t.empty() && seen.insert(t).second) {
                    m_out.textures.push_back(t);
                }
            }
        }
    }

private:
    struct Sphere {
        glm::vec3 centre{0.0f};
        float radius = 0.0f; // a radius whose bits are 0 is "no bound": merges skip it
    };

    static bool has_bound(const Sphere& s) { return std::bit_cast<uint32_t>(s.radius) != 0; }

    static Sphere transformed(const Transform& world, float scale, glm::vec3 centre, float radius) {
        return {world.linear * centre + world.translation, radius * scale};
    }

    // a grows to hold b (2.2.10).
    static void merge(Sphere& a, const Sphere& b) {
        const glm::vec3 d = a.centre - b.centre;
        const float dist2 = d.x * d.x + d.y * d.y + d.z * d.z;
        const float grow = b.radius - a.radius;
        if (grow * grow >= dist2) {
            if (grow >= 0.0f) {
                a = b; // a lies inside b
            }
            return; // otherwise b lies inside a
        }
        const float dist = std::sqrt(dist2);
        if (dist > 1e-6f) {
            a.centre = b.centre + ((dist - grow) * (0.5f / dist)) * d;
        }
        a.radius = (b.radius + dist + a.radius) * 0.5f;
    }

    Sphere bound_of(NifRef ref, const Transform& parent, float parent_scale, int depth) {
        if (!m_nif.valid(ref) || depth > k_max_depth) {
            return {};
        }
        const auto& block = m_nif.blocks[static_cast<size_t>(ref)];
        const NiAVObject* av = m_nif.av_object(ref);
        if (!av || block.status != NifBlockStatus::parsed) {
            return {}; // particle systems and other undecoded objects: their bound is the simulation's
        }
        const Transform world = depth == 0 ? parent : compose(parent, *av);
        const float scale = depth == 0 ? parent_scale : parent_scale * av->scale;
        if (const auto* shape = std::get_if<BSTriShape>(&block.data)) {
            return transformed(world, scale, shape->bound_center, shape->bound_radius);
        }
        if (const auto* geom = std::get_if<NiGeometry>(&block.data)) {
            const auto* data = m_nif.get<NiGeometryData>(geom->data);
            return data ? transformed(world, scale, data->bound_center, data->bound_radius) : Sphere{};
        }
        const auto* node = std::get_if<NiNode>(&block.data);
        if (!node) {
            return {};
        }
        const auto type = m_nif.type_name(ref);
        if (type == "BSMultiBoundNode") {
            const auto* mb = m_nif.get<BSMultiBound>(node->multi_bound);
            if (const auto* shape = mb ? m_nif.get<BSMultiBoundShape>(mb->data) : nullptr) {
                return transformed(world, scale, shape->center,
                                   shape->sphere ? shape->radius : glm::length(shape->extent));
            }
        } else if (type == "BSOrderedNode") {
            if (node->static_bound && node->alpha_sort_bound.w != 0.0f) {
                return transformed(world, scale, glm::vec3(node->alpha_sort_bound), node->alpha_sort_bound.w);
            }
        } else if (type == "BSDamageStage" && av->hidden() && m_damage_stage == 0) {
            return {}; // a hidden stage skips its update
        } else if ((type == "NiSwitchNode" || type == "NiLODNode") && (node->switch_flags & 1)) {
            const uint32_t i = node->switch_index.value_or(~0u);
            return i < node->children.size() ? bound_of(node->children[i], world, scale, depth + 1) : Sphere{};
        } else if ((av->flags & k_fixed_bound) != 0) {
            return {}; // never recomputed from its children
        }
        Sphere s;
        for (auto c : node->children) {
            const Sphere b = bound_of(c, world, scale, depth + 1);
            if (!has_bound(b)) {
                continue;
            }
            if (has_bound(s)) {
                merge(s, b);
            } else {
                s = b;
            }
        }
        return s;
    }

    static constexpr uint32_t k_fixed_bound = 0x2000; // NiAVObject flag: the update pass leaves the bound alone

    // Types that legitimately appear in a scene graph but are never part of static render geometry.
    static bool is_ignored_child(std::string_view type) {
        static constexpr std::string_view k_prefixes[] = {
            "bhk",      "NiParticle",   "NiMeshParticle", "NiPSys",         "BSStripParticle",
            "BSPSys",   "NiPointLight", "NiSpotLight",    "NiAmbientLight", "NiDirectionalLight",
            "NiCamera",
        };
        return std::ranges::any_of(k_prefixes, [&](std::string_view p) { return type.starts_with(p); });
    }

    // NiSwitchNode draws only its active child; NiLODNode its highest-detail child.
    std::optional<size_t> drawn_child(const NiNode& node) const {
        if (!node.switch_index) {
            return std::nullopt;
        }
        if (const auto* lod = m_nif.get<NiRangeLODData>(node.lod_data); lod && !lod->ranges.empty()) {
            size_t best = 0;
            for (size_t i = 1; i < lod->ranges.size(); ++i) {
                if (lod->ranges[i][0] < lod->ranges[best][0]) {
                    best = i;
                }
            }
            return best;
        }
        return harvested && node.children.size() > 1 ? size_t(1) : *node.switch_index;
    }

    void warn(std::string message) {
        if (m_out.warnings.size() >= k_max_warnings ||
            std::ranges::find(m_out.warnings, message) != m_out.warnings.end()) {
            return;
        }
        m_out.warnings.push_back(std::move(message));
    }

    // Shapes the engine draws itself or that are not static geometry.
    bool skip_shader(NifRef shader, NifRef shape) {
        auto type = m_nif.type_name(shader);
        if (type == "BSWaterShaderProperty" || type == "BSSkyShaderProperty") {
            return true;
        }
        if (type.empty()) {
            return true; // helper geometry (particle emitter meshes, collision proxies): never drawn
        }
        if (type != "BSLightingShaderProperty" && type != "BSEffectShaderProperty") {
            warn(std::format("shape {} uses unsupported shader {}", shape, type));
            return true;
        }
        if (m_nif.blocks[static_cast<size_t>(shader)].status != NifBlockStatus::parsed) {
            warn(std::format("shape {}: shader block {} could not be decoded", shape, shader));
            return true;
        }
        return false;
    }

    // A held weapon's blood: a hidden shape with the Decal flag, which a hit turns on.
    template <typename Block> bool blood_shape(const Block& block) const {
        const auto* shape = std::get_if<BSTriShape>(&block.data);
        if (!m_skinned || !shape || !m_attach_bone.starts_with("Weapon")) {
            return false;
        }
        if (const auto* lit = m_nif.get<BSLightingShaderProperty>(shape->shader_property)) {
            return (lit->flags1 & k_f1_decal) != 0;
        }
        const auto* effect = m_nif.get<BSEffectShaderProperty>(shape->shader_property);
        return effect && (effect->flags1 & k_f1_decal) != 0;
    }

    void add_tri_shape(NifRef ref, const BSTriShape& shape, const Transform& world) {
        if (skip_shader(shape.shader_property, ref)) {
            return;
        }
        const NifGeometry* attributes = &shape.geometry;
        std::vector<Triangle> triangles = shape.geometry.triangles;
        m_partition_indices.clear();
        if ((triangles.empty() || m_skinned) && shape.skin != -1) {
            // SE skinned shapes keep vertices and triangles in the skin partition. A shape may hold its triangles
            // itself as well; a skinned model takes the partitions', which show and hide one by one.
            const auto* skin = m_nif.get<NiSkinInstance>(shape.skin);
            const auto* partition = skin ? m_nif.get<NiSkinPartition>(skin->skin_partition) : nullptr;
            if (!partition) {
                warn(std::format("skinned shape {} has no decodable skin partition", ref));
                return;
            }
            if (triangles.empty()) {
                attributes = &partition->geometry;
            }
            triangles.clear();
            for (const auto& p : partition->partitions) {
                triangles.insert(triangles.end(), p.triangles.begin(), p.triangles.end());
                m_partition_indices.push_back(static_cast<uint32_t>(p.triangles.size() * 3));
            }
        }
        if (shape.has_lods) {
            check_lods(ref, triangles.size(), shape.lod_sizes);
        }
        ShapeSource src;
        // BSDynamicTriShape stores positions (and bitangent x) separately from the packed vertices.
        const NifGeometry& placed = shape.dynamic ? shape.geometry : *attributes;
        src.positions = placed.positions;
        src.normals = attributes->normals;
        src.packed_normals = attributes->packed_normals;
        src.packed_tangents = attributes->packed_tangents;
        src.bitangent_x = placed.bitangent_x;
        src.uvs = attributes->uvs;
        src.colors = attributes->colors;
        src.triangles = triangles;
        if (m_skinned) {
            emit_skinned(ref, src, shape.skin, attributes, world, shape.shader_property, shape.alpha_property);
            return;
        }
        std::vector<Transform> skinned;
        if (shape.skin != -1) {
            skinned = skin_transforms(ref, shape.skin, src.positions.size(), attributes);
        }
        emit(ref, src, skinned.empty() ? std::span(&world, 1) : std::span<const Transform>(skinned),
             shape.shader_property, shape.alpha_property);
    }

    void add_geometry(NifRef ref, const NiGeometry& geom, const Transform& world) {
        if (skip_shader(geom.shader_property, ref)) {
            return;
        }
        const auto* data = m_nif.get<NiGeometryData>(geom.data);
        if (!data) {
            warn(std::format("shape {} has no decodable geometry data (block {})", ref, geom.data));
            return;
        }
        const auto& g = data->geometry;
        if (geom.has_lods) {
            check_lods(ref, g.triangles.size(), geom.lod_sizes);
        }
        ShapeSource src{g.positions, g.normals, g.uvs, g.colors, g.triangles, {}, {}, {}, g.tangents, g.bitangents};
        if (m_skinned) {
            emit_skinned(ref, src, geom.skin, nullptr, world, geom.shader_property, geom.alpha_property);
            return;
        }
        std::vector<Transform> skinned;
        if (geom.skin != -1) {
            skinned = skin_transforms(ref, geom.skin, src.positions.size(), nullptr);
        }
        emit(ref, src, skinned.empty() ? std::span(&world, 1) : std::span<const Transform>(skinned),
             geom.shader_property, geom.alpha_property);
    }

    // BSLODTriShape / BSMeshLODTriShape: the three sizes are consecutive triangle runs that together make
    // the full-detail mesh (lower LODs draw fewer runs), so every triangle is kept.
    void check_lods(NifRef ref, size_t triangles, const std::array<uint32_t, 3>& sizes) {
        uint64_t sum = uint64_t{sizes[0]} + sizes[1] + sizes[2];
        if (sum != triangles) {
            warn(std::format("shape {}: LOD sizes {}+{}+{} do not add up to its {} triangles", ref, sizes[0],
                             sizes[1], sizes[2], triangles));
        }
    }

    // World transform of any node, following parents (bones need not be on the visited path).
    Transform world_of(NifRef ref, int depth = 0) {
        if (!m_nif.valid(ref)) {
            return {};
        }
        auto index = static_cast<size_t>(ref);
        if (m_world[index]) {
            return *m_world[index];
        }
        NifRef parent = m_parent[index];
        Transform up = parent != -1 && depth < k_max_depth ? world_of(parent, depth + 1) : Transform{};
        const NiAVObject* av = m_nif.av_object(ref);
        Transform t = av ? compose(up, *av) : up;
        m_world[index] = t;
        return t;
    }

    // Rest-pose linear blend skinning, as the game draws a skinned shape with its skeleton in the pose the
    // file stores: v' = sum_i w_i * (bone_i world * skin-to-bone_i) * v. The shape's own transform does
    // not apply. A skin may list the same bone twice with different binds (merged meshes, e.g. trees), so
    // this is per vertex rather than one rigid transform. Empty result: draw the shape unskinned.
    std::vector<Transform> skin_transforms(NifRef ref, NifRef skin_ref, size_t n, const NifGeometry* packed) {
        const auto* skin = m_nif.get<NiSkinInstance>(skin_ref);
        const auto* data = skin ? m_nif.get<NiSkinData>(skin->data) : nullptr;
        if (!data || data->bones.size() != skin->bones.size()) {
            warn(std::format("skinned shape {} has no matching skin data; drawn unskinned", ref));
            return {};
        }
        std::vector<Transform> bones(skin->bones.size());
        for (size_t i = 0; i < bones.size(); ++i) {
            Transform b = world_of(skin->bones[i]);
            const auto& s = data->bones[i].skin_to_bone;
            bones[i] = {b.linear * (s.rotation * s.scale), b.linear * s.translation + b.translation};
        }
        std::vector<Transform> out(n, Transform{glm::mat3(0.0f), glm::vec3(0.0f)});
        std::vector<float> total(n, 0.0f);
        size_t bad = 0;
        auto add = [&](size_t v, size_t bone, float w) {
            if (!(w > 0.0f)) {
                return;
            }
            if (v >= n || bone >= bones.size()) {
                ++bad;
                return;
            }
            out[v].linear += bones[bone].linear * w;
            out[v].translation += bones[bone].translation * w;
            total[v] += w;
        };
        bool weighted = false;
        // LE keeps weights per bone in NiSkinData; SE packs them into the vertices, indexing the skin's
        // bones.
        for (size_t b = 0; b < data->bones.size(); ++b) {
            for (auto [v, w] : data->bones[b].weights) {
                weighted = true;
                add(v, b, w);
            }
        }
        if (!weighted && packed && packed->bone_weights.size() == n) {
            weighted = true;
            for (size_t v = 0; v < n; ++v) {
                for (int k = 0; k < 4; ++k) {
                    add(v, packed->bone_indices[v][k], packed->bone_weights[v][k]);
                }
            }
        }
        if (!weighted) {
            warn(std::format("skinned shape {} has no vertex weights; drawn unskinned", ref));
            return {};
        }
        if (bad > 0) {
            warn(std::format("skinned shape {}: {} weights reference missing vertices or bones", ref, bad));
        }
        size_t unweighted = 0;
        for (size_t v = 0; v < n; ++v) {
            if (total[v] > 1e-6f) {
                out[v].linear /= total[v];
                out[v].translation /= total[v];
            } else {
                out[v] = bones.empty() ? Transform{} : bones[0];
                ++unweighted;
            }
        }
        if (unweighted > 0) {
            warn(std::format("skinned shape {}: {} vertices without weights follow the first bone", ref,
                             unweighted));
        }
        return out;
    }

    // A skinned model's shape: its vertices stay in skin space and its bones go with it, for the engine to pose. A
    // rigid shape in such a model (a part fixed to one bone) becomes a shape skinned wholly to one bone with no name:
    // the node the actor holds the model at (osm::Skin::attach, its Prn, while it is sheathed).
    void emit_skinned(NifRef ref, const ShapeSource& src, NifRef skin_ref, const NifGeometry* packed,
                      const Transform& world, NifRef shader, NifRef alpha) {
        const size_t n = src.positions.size();
        m_pending = {};
        m_pending.vertices.assign(n, osm::SkinVertex{});
        const auto* skin = skin_ref != -1 ? m_nif.get<NiSkinInstance>(skin_ref) : nullptr;
        const auto* data = skin ? m_nif.get<NiSkinData>(skin->data) : nullptr;
        if (skin && data && data->bones.size() == skin->bones.size()) {
            for (size_t i = 0; i < skin->bones.size(); ++i) {
                const NiAVObject* av = m_nif.av_object(skin->bones[i]);
                const auto& t = data->bones[i].skin_to_bone;
                Transform to_bone{t.rotation * t.scale, t.translation};
                if (!m_attach_bone.empty()) {
                    // A model held at a node (a bow): its bones are its own, the skeleton has none of them, and they ride
                    // that node as the file places them. The vertices stay in skin space, so a bow whose shape stands 25
                    // units off its root needs the whole way to the root.
                    const Transform bone = world_of(skin->bones[i]);
                    to_bone = {bone.linear * to_bone.linear, bone.linear * to_bone.translation + bone.translation};
                }
                m_pending.bones.push_back(skin_bone(av ? av->name : "", to_bone));
            }
            m_pending.parts = skin->body_parts;
            m_pending.part_indices.assign(skin->body_parts.size(), 0);
            if (m_partition_indices.size() == skin->body_parts.size()) {
                m_pending.part_indices = m_partition_indices;
            }
            if (packed && packed->bone_weights.size() == n) {
                for (size_t v = 0; v < n; ++v) {
                    for (int k = 0; k < 4; ++k) {
                        m_pending.vertices[v].weight[k] =
                            static_cast<uint16_t>(glm::packHalf1x16(packed->bone_weights[v][k]));
                        m_pending.vertices[v].bone[k] = packed->bone_indices[v][k];
                    }
                }
            } else {
                // NiSkinData lists weights by bone: each vertex keeps its four largest, as the game's partitions do.
                std::vector<std::array<std::pair<float, uint8_t>, 4>> top(n);
                for (size_t b = 0; b < data->bones.size() && b < 256; ++b) {
                    for (auto [v, w] : data->bones[b].weights) {
                        if (v >= n) {
                            continue;
                        }
                        auto& slots = top[v];
                        auto low = std::ranges::min_element(slots, {}, [](const auto& s) { return s.first; });
                        if (w > low->first) {
                            *low = {w, static_cast<uint8_t>(b)};
                        }
                    }
                }
                for (size_t v = 0; v < n; ++v) {
                    float total = 0.0f;
                    for (const auto& [w, b] : top[v]) {
                        total += w;
                    }
                    for (int k = 0; k < 4 && total > 0.0f; ++k) {
                        m_pending.vertices[v].weight[k] = static_cast<uint16_t>(glm::packHalf1x16(top[v][k].first / total));
                        m_pending.vertices[v].bone[k] = top[v][k].second;
                    }
                }
            }
        } else {
            // A weapon's own shapes name no bone: they follow it from sheath to hand. Its scabbard stays on the sheath.
            // An animated object's ride on their parent nodes.
            const NifRef parent = m_object ? m_parent[static_cast<size_t>(ref)] : -1;
            const NiAVObject* node = parent != -1 ? m_nif.av_object(parent) : nullptr;
            if (node) {
                const Transform up = world_of(parent);
                const glm::mat3 back = glm::inverse(up.linear);
                m_pending.bones.push_back(skin_bone(node->name, {back * world.linear, back * (world.translation - up.translation)}));
            } else {
                // `Scb` is a shape on the root as often as a node: the scabbard stays on the attach node when the weapon is drawn.
                const NiAVObject* self = m_nif.av_object(ref);
                const bool scabbard = m_in_scabbard || (self && self->name == "Scb");
                m_pending.bones.push_back(skin_bone(scabbard ? m_attach_bone : std::string(), world));
            }
            for (auto& v : m_pending.vertices) {
                v.weight[0] = static_cast<uint16_t>(glm::packHalf1x16(1.0f));
            }
        }
        const Transform identity;
        emit(ref, src, std::span(&identity, 1), shader, alpha);
    }

    static osm::SkinBone skin_bone(std::string name, const Transform& t) {
        osm::SkinBone b;
        b.name = std::move(name);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                b.skin_to_bone[r][c] = t.linear[c][r]; // glm is column-major
            }
            b.skin_to_bone[r][3] = t.translation[r] * units::k_skyrim_unit_m;
        }
        return b;
    }

    // `xf` is one transform for the whole shape, or one per vertex (skinned).
    void emit(NifRef ref, const ShapeSource& src, std::span<const Transform> xf, NifRef shader,
              NifRef alpha) {
        size_t n = src.positions.size();
        if (n == 0 || src.triangles.empty()) {
            return;
        }
        bool rigid = xf.size() == 1;
        auto at = [&](size_t i) -> const Transform& { return rigid ? xf[0] : xf[i]; };
        float rigid_det = glm::determinant(xf[0].linear);
        if (rigid && (!std::isfinite(rigid_det) || std::abs(rigid_det) < 1e-12f)) {
            return; // collapsed by a zero scale: never visible
        }
        auto mirrored = [&](size_t i) { return (rigid ? rigid_det : glm::determinant(xf[i].linear)) < 0.0f; };
        auto attribute = [&](auto span, const char* what) {
            if (!span.empty() && span.size() != n) {
                warn(std::format("shape {}: {} {} for {} vertices ignored", ref, span.size(), what, n));
                return decltype(span){};
            }
            return span;
        };
        auto normals_in = attribute(src.normals, "normals");
        auto uvs = attribute(src.uvs, "uvs");
        auto colors = attribute(src.colors, "colours");

        // Drop triangles that index past the vertices; a mirroring transform flips the winding back.
        std::vector<uint32_t> indices;
        indices.reserve(src.triangles.size() * 3);
        size_t bad = 0;
        for (const auto& t : src.triangles) {
            if (t[0] >= n || t[1] >= n || t[2] >= n) {
                ++bad;
                continue;
            }
            if (mirrored(t[0])) {
                indices.insert(indices.end(), {t[0], t[2], t[1]});
            } else {
                indices.insert(indices.end(), {t[0], t[1], t[2]});
            }
        }
        if (bad > 0) {
            warn(std::format("shape {}: {} triangles reference missing vertices", ref, bad));
        }
        if (indices.empty()) {
            return;
        }

        bool has_colors = !colors.empty();
        auto material = material_for(shader, alpha, has_colors);
        if (!material) {
            return;
        }

        // Positions stay in shape space (metres) for rigid shapes, with the shape's transform kept on the
        // submesh, so the packed tangent frame keeps the game's bytes. Skinned shapes are posed per vertex and
        // their frame is rotated and re-packed.
        std::vector<glm::vec3> positions(n);
        for (size_t i = 0; i < n; ++i) {
            positions[i] = rigid ? src.positions[i] * units::k_skyrim_unit_m
                                 : (at(i).linear * src.positions[i] + at(i).translation) * units::k_skyrim_unit_m;
        }
        auto packed_normals = attribute(src.packed_normals, "packed normals");
        auto packed_tangents = attribute(src.packed_tangents, "packed tangents");
        auto bitangent_x = attribute(src.bitangent_x, "bitangents");
        auto tangents_in = attribute(src.tangents, "tangents");
        auto bitangents_in = attribute(src.bitangents, "bitangents");
        const bool packed = !packed_normals.empty() && !packed_tangents.empty() && !bitangent_x.empty();

        // Frame vectors in the game's naming: N normal, T "tangent", B "bitangent" (normal-map x follows B).
        std::vector<glm::vec3> nrm(n, glm::vec3(0.0f)), tan(n, glm::vec3(0.0f)), bit(n, glm::vec3(0.0f));
        if (packed) {
            for (size_t i = 0; i < n; ++i) {
                const auto& pn = packed_normals[i];
                const auto& pt = packed_tangents[i];
                nrm[i] = {unpack_unorm8(pn.x), unpack_unorm8(pn.y), unpack_unorm8(pn.z)};
                tan[i] = {unpack_unorm8(pt.x), unpack_unorm8(pt.y), unpack_unorm8(pt.z)};
                bit[i] = {bitangent_x[i], unpack_unorm8(pn.w), unpack_unorm8(pt.w)};
            }
        } else {
            if (!normals_in.empty()) {
                std::copy(normals_in.begin(), normals_in.end(), nrm.begin());
            } else {
                for (size_t i = 0; i < indices.size(); i += 3) {
                    auto a = src.positions[indices[i]];
                    auto face = glm::cross(src.positions[indices[i + 1]] - a, src.positions[indices[i + 2]] - a);
                    for (size_t k = 0; k < 3; ++k) {
                        nrm[indices[i + k]] += face; // area weighted
                    }
                }
                for (auto& v : nrm) {
                    float len = glm::length(v);
                    v = len > 1e-12f && std::isfinite(len) ? v / len : glm::vec3(0, 0, 1);
                }
            }
            if (!tangents_in.empty() && !bitangents_in.empty()) {
                std::copy(tangents_in.begin(), tangents_in.end(), tan.begin());
                std::copy(bitangents_in.begin(), bitangents_in.end(), bit.begin());
            } else if (!uvs.empty()) {
                // No stored frame: B along +u and T along +v (dP/du, dP/dv).
                for (size_t i = 0; i < indices.size(); i += 3) {
                    uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
                    glm::vec3 e1 = src.positions[i1] - src.positions[i0];
                    glm::vec3 e2 = src.positions[i2] - src.positions[i0];
                    glm::vec2 d1 = uvs[i1] - uvs[i0];
                    glm::vec2 d2 = uvs[i2] - uvs[i0];
                    float r = d1.x * d2.y - d2.x * d1.y;
                    if (std::abs(r) < 1e-20f || !std::isfinite(r)) {
                        continue;
                    }
                    glm::vec3 du = (e1 * d2.y - e2 * d1.y) / r;
                    glm::vec3 dv = (e2 * d1.x - e1 * d2.x) / r;
                    for (uint32_t k : {i0, i1, i2}) {
                        bit[k] += du;
                        tan[k] += dv;
                    }
                }
                for (size_t i = 0; i < n; ++i) {
                    auto ortho = [&](glm::vec3 v, const glm::vec3& fallback) {
                        v -= nrm[i] * glm::dot(nrm[i], v);
                        float len = glm::length(v);
                        return len > 1e-12f && std::isfinite(len) ? v / len : fallback;
                    };
                    bit[i] = ortho(bit[i], perpendicular(nrm[i]));
                    tan[i] = ortho(tan[i], glm::cross(nrm[i], bit[i]));
                }
            }
        }
        if (!rigid) {
            for (size_t i = 0; i < n; ++i) {
                const glm::mat3& m = at(i).linear; // blended skinning matrices are near-orthogonal
                auto rot = [&](const glm::vec3& v) {
                    glm::vec3 r = m * v;
                    float len = glm::length(r);
                    return len > 1e-12f && std::isfinite(len) ? r / len : v;
                };
                nrm[i] = rot(nrm[i]);
                tan[i] = rot(tan[i]);
                bit[i] = rot(bit[i]);
            }
        }

        auto& mesh = m_out.mesh;
        osm::Submesh sub{};
        sub.first_index = static_cast<uint32_t>(mesh.indices.size());
        sub.index_count = static_cast<uint32_t>(indices.size());
        sub.base_vertex = static_cast<uint32_t>(mesh.vertices.size());
        sub.vertex_count = static_cast<uint32_t>(n);
        sub.material = *material;
        if (rigid) {
            const auto& t = xf[0];
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    sub.transform[r][c] = t.linear[c][r]; // glm is column-major
                }
                sub.transform[r][3] = t.translation[r] * units::k_skyrim_unit_m;
            }
            if (m_billboard) {
                sub.billboard_mode = m_billboard->mode;
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        sub.billboard[r][c] = m_billboard->frame.linear[c][r];
                    }
                    sub.billboard[r][3] = m_billboard->frame.translation[r] * units::k_skyrim_unit_m;
                }
            }
        }
        mesh.vertices.reserve(mesh.vertices.size() + n);
        const bool keep_bytes = packed && rigid;
        for (size_t i = 0; i < n; ++i) {
            osm::Vertex v{};
            v.position[0] = positions[i].x;
            v.position[1] = positions[i].y;
            v.position[2] = positions[i].z;
            if (!uvs.empty()) {
                v.uv[0] = uvs[i].x;
                v.uv[1] = uvs[i].y;
            }
            if (keep_bytes) {
                std::memcpy(v.normal, &packed_normals[i], 4);
                std::memcpy(v.tangent, &packed_tangents[i], 4);
                v.bitangent_x = bitangent_x[i];
            } else {
                v.normal[0] = pack_unorm8(nrm[i].x);
                v.normal[1] = pack_unorm8(nrm[i].y);
                v.normal[2] = pack_unorm8(nrm[i].z);
                v.normal[3] = pack_unorm8(bit[i].y);
                v.tangent[0] = pack_unorm8(tan[i].x);
                v.tangent[1] = pack_unorm8(tan[i].y);
                v.tangent[2] = pack_unorm8(tan[i].z);
                v.tangent[3] = pack_unorm8(bit[i].z);
                v.bitangent_x = bit[i].x;
            }
            glm::u8vec4 c = has_colors ? colors[i] : glm::u8vec4(255);
            v.color[0] = c.r;
            v.color[1] = c.g;
            v.color[2] = c.b;
            v.color[3] = c.a;
            mesh.vertices.push_back(v);
        }
        mesh.indices.insert(mesh.indices.end(), indices.begin(), indices.end());
        mesh.submeshes.push_back(sub);
        m_out.submesh_shapes.push_back(ref);
        if (m_skinned) {
            auto& skin = mesh.skin;
            const NiAVObject* named = m_nif.av_object(ref);
            skin.shape_names.push_back(named ? named->name : std::string());
            skin.shapes.push_back({static_cast<uint32_t>(skin.bones.size()), static_cast<uint32_t>(m_pending.bones.size()),
                                   static_cast<uint32_t>(skin.parts.size()), static_cast<uint32_t>(m_pending.parts.size())});
            skin.vertices.insert(skin.vertices.end(), m_pending.vertices.begin(), m_pending.vertices.end());
            skin.bones.insert(skin.bones.end(), m_pending.bones.begin(), m_pending.bones.end());
            skin.parts.insert(skin.parts.end(), m_pending.parts.begin(), m_pending.parts.end());
            skin.part_indices.insert(skin.part_indices.end(), m_pending.part_indices.begin(), m_pending.part_indices.end());
        }
    }

    std::optional<uint32_t> material_for(NifRef shader, NifRef alpha, bool has_colors) {
        auto converted = convert_material(m_nif, shader, alpha, has_colors);
        if (!converted) {
            return std::nullopt;
        }
        osm::Material m = std::move(*converted);
        const auto& p = m.params;

        auto& materials = m_out.mesh.materials;
        for (size_t i = 0; i < materials.size(); ++i) {
            if (std::memcmp(&materials[i].params, &p, sizeof p) == 0 && materials[i].textures == m.textures &&
                materials[i].controllers.empty() && m.controllers.empty()) {
                return static_cast<uint32_t>(i);
            }
        }
        materials.push_back(std::move(m));
        return static_cast<uint32_t>(materials.size() - 1);
    }

    const NifFile& m_nif;
    MeshImportResult& m_out;
    std::vector<uint8_t> m_on_path;                // cycle guard: blocks on the current root-to-node path
    std::vector<NifRef> m_parent;                  // node that lists each block as a child
    std::vector<std::optional<Transform>> m_world; // world_of cache
    struct Billboard {
        uint32_t mode;
        Transform frame; // the billboard node in mesh space
    };
    std::optional<Billboard> m_billboard; // the innermost billboard above the shapes being visited
    bool m_skinned = false;               // keep skin data (emit_skinned) instead of posing the shapes
    int m_damage_stage = 0;
    bool m_keep_vertices = false;
    bool m_object = false;
    struct PendingSkin {
        std::vector<osm::SkinVertex> vertices;
        std::vector<osm::SkinBone> bones;
        std::vector<uint16_t> parts;
        std::vector<uint32_t> part_indices;
    };
    PendingSkin m_pending; // the shape emit is adding
    std::vector<uint32_t> m_partition_indices; // the shape being added: indices of each skin partition
    std::string m_attach_bone;
    bool m_in_scabbard = false; // below the model's Scb node

public:
    void set_attach_bone(std::string name) { m_attach_bone = std::move(name); }
};

void decode_shader_flags(osm::MaterialParams& p, bool has_colors) {
    uint32_t f1 = p.skyrim_flags1;
    uint32_t f2 = p.skyrim_flags2;
    uint32_t flags = 0;
    auto set = [&](bool on, uint32_t flag) {
        if (on) {
            flags |= flag;
        }
    };
    set(f1 & k_f1_z_test, osm::material_z_test);
    set(f2 & k_f2_z_write, osm::material_z_write);
    set(f2 & k_f2_double_sided, osm::material_two_sided);
    set(f1 & (k_f1_decal | k_f1_dynamic_decal), osm::material_decal);
    set((f2 & k_f2_vertex_colors) && has_colors, osm::material_vertex_colors);
    set(f1 & k_f1_vertex_alpha, osm::material_vertex_alpha);
    set(f2 & k_f2_tree_anim, osm::material_tree_anim);
    set(f2 & k_f2_glow_map, osm::material_glow);
    set(f1 & k_f1_env_map, osm::material_env_map);
    set(f1 & k_f1_model_space_normals, osm::material_model_space_normals);
    p.flags = flags;
}

// The game (16.3): TBC keys become Hermite keys with the computed tangents, incoming in
// [2] and outgoing in [3] like quadratic keys. The file's floats are tension, continuity, bias in that order.
void tbc_to_hermite(std::vector<std::array<float, 5>>& keys) {
    const size_t n = keys.size();
    std::vector<std::array<float, 2>> tangents(n);
    for (size_t i = 0; i < n; ++i) {
        const float p = keys[i][1];
        const float prev = i > 0 ? keys[i - 1][1] : (n > 1 ? 2.0f * p - keys[1][1] : p);
        const float next = i + 1 < n ? keys[i + 1][1] : (n > 1 ? 2.0f * p - keys[n - 2][1] : p);
        const float d0 = p - prev, d1 = next - p;
        const float span0 = i > 0 && i + 1 < n ? keys[i][0] - keys[i - 1][0] : 1.0f;
        const float span1 = i > 0 && i + 1 < n ? keys[i + 1][0] - keys[i][0] : 1.0f;
        const float t = keys[i][2], c = keys[i][3], b = keys[i][4];
        const float sum = span0 + span1 > 0.0f ? span0 + span1 : 1.0f;
        tangents[i][0] = (1 - t) * 0.5f * ((1 - c) * (1 + b) * d0 + (1 + c) * (1 - b) * d1) * 2 * span0 / sum;
        tangents[i][1] = (1 - t) * 0.5f * ((1 + c) * (1 + b) * d0 + (1 - c) * (1 - b) * d1) * 2 * span1 / sum;
    }
    for (size_t i = 0; i < n; ++i) {
        keys[i][2] = tangents[i][0];
        keys[i][3] = tangents[i][1];
        keys[i][4] = 0.0f;
    }
}

// The kinds of material controllers (nif.xml BSLightingShaderPropertyFloatController / ColorController,
// BSEffectShaderPropertyFloatController / ColorController, BSNiAlphaPropertyTestRefController).
enum class PropertyControllerKind : uint8_t { lighting_float, lighting_color, effect_float, effect_color, alpha_test_ref };

std::optional<PropertyControllerKind> property_controller_kind(std::string_view type) {
    using K = PropertyControllerKind;
    if (type == "BSLightingShaderPropertyFloatController") return K::lighting_float;
    if (type == "BSLightingShaderPropertyColorController") return K::lighting_color;
    if (type == "BSEffectShaderPropertyFloatController") return K::effect_float;
    if (type == "BSEffectShaderPropertyColorController") return K::effect_color;
    if (type == "BSNiAlphaPropertyTestRefController") return K::alpha_test_ref;
    return std::nullopt;
}

// The variable a property controller animates (16.5), or -1 for variables vanilla never animates (and whose
// targets are not mapped). nif.xml LightingShaderControlledVariable, EffectShaderControlledVariable and the
// ...ControlledColor enums; colour controllers return the red component's variable.
int controller_variable(PropertyControllerKind kind, uint32_t variable) {
    using K = PropertyControllerKind;
    switch (kind) {
    case K::lighting_float:
        switch (variable) {
        case 0: return osm::var_refraction_strength;
        case 8: return osm::var_env_map_scale;
        case 9: return osm::var_glossiness;
        case 10: return osm::var_specular_strength;
        case 11: return osm::var_emissive_multiple;
        case 12: return osm::var_alpha;
        case 20: return osm::uv_offset_u;
        case 21: return osm::uv_scale_u;
        case 22: return osm::uv_offset_v;
        case 23: return osm::uv_scale_v;
        default: return -1;
        }
    case K::lighting_color: return variable == 0 ? osm::var_specular_r : variable == 1 ? osm::var_emissive_r : -1;
    case K::effect_float:
        switch (variable) {
        case 0: return osm::var_base_color_scale;
        case 1: return osm::var_falloff_start;
        case 2: return osm::var_falloff_stop;
        case 3: return osm::var_falloff_start_opacity;
        case 4: return osm::var_falloff_stop_opacity;
        case 5: return osm::var_base_alpha;
        case 6: return osm::uv_offset_u;
        case 7: return osm::uv_scale_u;
        case 8: return osm::uv_offset_v;
        case 9: return osm::uv_scale_v;
        default: return -1;
        }
    case K::effect_color: return variable == 0 ? osm::var_base_r : -1;
    case K::alpha_test_ref: return osm::var_alpha_test_ref;
    }
    return -1;
}

// The controllers of a shader property and of its alpha property that animate on their own (16.4, 16.5): active
// (flag 0x8), with a float or point3 interpolator that has keys. Controllers driven by blend interpolators
// belong to the model's animation sequences (another workstream); keyless ones write nothing.
void append_controllers(const NifFile& nif, NifRef first, std::vector<osm::MaterialController>& out) {
    NifRef c = first;
    for (int guard = 0; guard < 64 && nif.valid(c); ++guard) {
        const auto kind = property_controller_kind(nif.type_name(c));
        const NiTimeController* ctrl = nif.get<NiTimeController>(c);
        if (const auto* a = nif.get<BSNiAlphaPropertyTestRefController>(c)) {
            ctrl = &a->base;
        }
        if (!kind || !ctrl) {
            break;
        }
        const int variable = controller_variable(*kind, ctrl->controlled_variable);
        osm::MaterialController u;
        u.flags = ctrl->flags;
        u.frequency = ctrl->frequency;
        u.phase = ctrl->phase;
        u.start = ctrl->start_time;
        u.stop = ctrl->stop_time;
        std::array<std::vector<std::array<float, 5>>, 3> channels;
        int components = 0;
        uint32_t key_type = 0;
        const auto* fi = nif.type_name(ctrl->interpolator) == "NiFloatInterpolator"
                             ? nif.get<NiInterpolator>(ctrl->interpolator)
                             : nullptr;
        if (fi) {
            if (const auto* data = nif.get<NiFloatData>(fi->data)) {
                // Keys as time, value, then the tangents (quadratic) or tension, continuity, bias (TBC).
                key_type = data->key_type;
                for (const auto& k : data->keys) {
                    channels[0].push_back(key_type == 3
                                              ? std::array<float, 5>{k.time, k.value, k.tbc[0], k.tbc[1], k.tbc[2]}
                                              : std::array<float, 5>{k.time, k.value, k.forward, k.backward, 0.0f});
                }
            }
            components = 1;
        } else if (const auto* pi = nif.get<NiPoint3Interpolator>(ctrl->interpolator)) {
            if (const auto* data = nif.get<NiPosData>(pi->data)) {
                key_type = data->key_type;
                channels = data->keys;
            }
            components = 3;
        }
        const bool colour =
            *kind == PropertyControllerKind::lighting_color || *kind == PropertyControllerKind::effect_color;
        if (variable >= 0 && (ctrl->flags & 0x8) && components == (colour ? 3 : 1) && !channels[0].empty()) {
            for (int k = 0; k < components; ++k) {
                osm::MaterialController v = u;
                v.variable = static_cast<uint8_t>(variable + k);
                v.key_type = static_cast<uint8_t>(key_type);
                v.keys = channels[k];
                if (v.key_type == 3) {
                    tbc_to_hermite(v.keys);
                    v.key_type = 2;
                }
                out.push_back(std::move(v));
            }
        }
        c = ctrl->next;
    }
}

} // namespace

MeshImportResult import_nif(const NifFile& nif, std::string_view source_path, int damage_stage, bool harvested) {
    MeshImportResult result;
    if (is_editor_asset(source_path)) {
        return result;
    }
    MeshBuilder builder(nif, result, false, damage_stage);
    builder.harvested = harvested;
    for (auto root : nif.roots) {
        builder.visit(root, Transform{}, 0);
    }
    if (!nif.roots.empty()) {
        builder.fade_bound(nif.roots.front());
    }
    builder.finish();
    return result;
}

MeshImportResult import_skinned_nif(const NifFile& nif, std::string_view source_path, bool keep_vertices, bool object) {
    MeshImportResult result;
    MeshBuilder builder(nif, result, true, 0, keep_vertices, object);
    if (!nif.roots.empty()) {
        if (const NiAVObject* root = nif.av_object(nif.roots.front())) {
            for (NifRef e : root->extra_data) {
                const auto* text = nif.get<NiStringExtraData>(e);
                if (text && text->name == "Prn") {
                    builder.set_attach_bone(text->value);
                    result.mesh.skin.attach = text->value;
                }
            }
        }
    }
    for (auto root : nif.roots) {
        builder.visit(root, Transform{}, 0);
    }
    if (!nif.roots.empty()) {
        builder.fade_bound(nif.roots.front());
    }
    builder.finish();
    return result;
}

bool add_weight_morph(osm::Mesh& heavy, osm::Mesh&& light) {
    if (heavy.vertices.size() != light.vertices.size() || heavy.indices != light.indices) {
        return false;
    }
    heavy.skin.morph = std::move(light.vertices);
    for (const auto& v : heavy.skin.morph) { // the bounds hold both ends
        for (int i = 0; i < 3; ++i) {
            heavy.bounds_min[i] = std::min(heavy.bounds_min[i], v.position[i]);
            heavy.bounds_max[i] = std::max(heavy.bounds_max[i], v.position[i]);
        }
    }
    return true;
}

MeshImportResult import_nif(std::span<const std::byte> nif, std::string_view source_path) {
    try {
        return import_nif(read_nif(nif), source_path);
    } catch (const ParseError& e) {
        throw ParseError(std::format("{}: {}", source_path, e.what()));
    }
}

std::optional<osm::Material> convert_material(const NifFile& nif, NifRef shader, NifRef alpha, bool has_colors) {
    osm::Material m;
    auto& p = m.params;
    if (const auto* s = nif.get<BSLightingShaderProperty>(shader)) {
        p.model = osm::ShaderModel::lighting;
        p.skyrim_shader_type = s->shader_type;
        p.skyrim_flags1 = s->flags1;
        p.skyrim_flags2 = s->flags2;
        p.texture_clamp = static_cast<uint8_t>(s->texture_clamp & 3);
        p.alpha = s->alpha;
        p.uv_offset[0] = s->uv_offset.x;
        p.uv_offset[1] = s->uv_offset.y;
        p.uv_scale[0] = s->uv_scale.x;
        p.uv_scale[1] = s->uv_scale.y;
        std::memcpy(p.emissive_color, &s->emissive_color, sizeof p.emissive_color);
        p.emissive_multiple = s->emissive_multiple;
        std::memcpy(p.specular_color, &s->specular_color, sizeof p.specular_color);
        p.specular_strength = s->specular_strength;
        p.glossiness = s->glossiness;
        p.refraction_strength = s->refraction_strength;
        p.lighting_effect1 = s->lighting_effect1;
        p.lighting_effect2 = s->lighting_effect2;
        p.env_map_scale = s->shader_type == 1 ? s->type_params[0] : 1.0f;
        std::copy(s->type_params.begin(), s->type_params.end(), p.type_params);
        if (const auto* set = nif.get<BSShaderTextureSet>(s->texture_set)) {
            for (size_t i = 0; i < osm::k_texture_slots && i < set->textures.size(); ++i) {
                m.textures[i] = texture_path(set->textures[i]);
            }
        }
    } else if (const auto* e = nif.get<BSEffectShaderProperty>(shader)) {
        p.model = osm::ShaderModel::effect;
        p.skyrim_flags1 = e->flags1;
        p.skyrim_flags2 = e->flags2;
        p.texture_clamp = static_cast<uint8_t>(e->texture_clamp & 3);
        p.uv_offset[0] = e->uv_offset.x;
        p.uv_offset[1] = e->uv_offset.y;
        p.uv_scale[0] = e->uv_scale.x;
        p.uv_scale[1] = e->uv_scale.y;
        std::memcpy(p.base_color, &e->base_color, sizeof p.base_color);
        p.base_color_scale = e->base_color_scale;
        std::memcpy(p.falloff, &e->falloff, sizeof p.falloff);
        p.soft_falloff_depth = e->soft_falloff_depth;
        // The game keeps the constructor's 255 when the byte is 0.
        p.lighting_influence = e->lighting_influence != 0 ? e->lighting_influence : 255;
        m.textures[osm::texture_diffuse] = texture_path(e->source_texture);
        m.textures[osm::texture_normal] = texture_path(e->greyscale_texture);
    } else {
        return std::nullopt;
    }
    decode_shader_flags(p, has_colors);
    if (const auto* net = nif.get<BSLightingShaderProperty>(shader)) {
        append_controllers(nif, net->controller, m.controllers);
    } else if (const auto* e = nif.get<BSEffectShaderProperty>(shader)) {
        append_controllers(nif, e->controller, m.controllers);
    }
    if (const auto* a = nif.get<NiAlphaProperty>(alpha)) {
        append_controllers(nif, a->controller, m.controllers);
        if (a->flags & k_alpha_blend) {
            p.flags |= osm::material_alpha_blend;
        }
        if (a->flags & k_alpha_test) {
            p.flags |= osm::material_alpha_test;
        }
        p.blend_src = static_cast<uint16_t>((a->flags >> 1) & 0xF);
        p.blend_dst = static_cast<uint16_t>((a->flags >> 5) & 0xF);
        p.alpha_func = static_cast<uint8_t>((a->flags >> 10) & 0x7);
        p.alpha_threshold = a->threshold;
    }
    return m;
}

} // namespace spud::importer
