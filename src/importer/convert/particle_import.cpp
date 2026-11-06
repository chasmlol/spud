#include "importer/convert/particle_import.hpp"

#include "importer/convert/mesh_import.hpp"

#include "core/binary.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace spud::importer {

using namespace spud::legacy;

namespace {

constexpr size_t k_max_warnings = 32;
constexpr uint32_t k_none = ose::k_none;

ose::Vec3 to_vec3(const glm::vec3& v) {
    return {v.x, v.y, v.z};
}

class EffectBuilder {
public:
    EffectBuilder(const NifFile& nif, std::span<const NifRef> submesh_shapes, ParticleImportResult& out,
                  bool animation = false)
        : m_nif(nif), m_submesh_shapes(submesh_shapes), m_out(out), m_e(out.effect),
          m_node_of(nif.blocks.size(), k_none), m_parent(nif.blocks.size(), -1),
          m_system_of(nif.blocks.size(), k_none), m_animation(animation) {}

    void build() {
        collect_nodes();
        for (size_t i = 0; i < m_nif.blocks.size(); ++i) {
            if (const auto* ps = std::get_if<NiParticleSystem>(&m_nif.blocks[i].data)) {
                add_system(static_cast<NifRef>(i), *ps);
            }
        }
        if (m_e.systems.empty() && !m_animation) {
            return;
        }
        std::unordered_set<NifRef> visited;
        for (auto root : m_nif.roots) {
            visit(root, visited, 0);
        }
        resolve_age_death_spawn();
        add_sequences();
        std::set<std::string> seen;
        for (const auto& m : m_e.materials) {
            for (auto t : {m.source_texture, m.greyscale_texture}) {
                std::string path(m_e.string(t));
                if (!path.empty() && seen.insert(path).second) {
                    m_out.textures.push_back(path);
                }
            }
        }
    }

private:
    void warn(std::string message) {
        if (m_out.warnings.size() < k_max_warnings &&
            std::ranges::find(m_out.warnings, message) == m_out.warnings.end()) {
            m_out.warnings.push_back(std::move(message));
        }
    }

    // ---- Scene nodes: every NiAVObject, with the parent that lists it as a child.

    // A block's transform in the file's model space at rest: world = parent.R (s local) + parent.t.
    struct Xform {
        glm::mat3 r{1.0f};
        glm::vec3 t{0.0f};
        float s = 1.0f;
        Xform operator*(const Xform& o) const { return {r * o.r, t + s * (r * o.t), s * o.s}; }
        Xform inverse() const {
            const glm::mat3 ri = glm::transpose(r);
            const float si = s != 0.0f ? 1.0f / s : 0.0f;
            return {ri, -(si * (ri * t)), si};
        }
    };
    Xform rest_world(NifRef ref) const {
        Xform w;
        for (NifRef b = ref; m_nif.valid(b); b = m_parent[static_cast<size_t>(b)]) {
            const NiAVObject* av = m_nif.av_object(b);
            if (!av) {
                break;
            }
            w = Xform{av->rotation, av->translation, av->scale} * w;
        }
        return w;
    }
    static void merge(glm::vec3& c, float& r, const glm::vec3& oc, float orad) {
        // The game, as ui::merge_bound.
        const glm::vec3 d = c - oc;
        const float d2 = glm::dot(d, d);
        const float diff = orad - r;
        if (diff * diff >= d2) { // one holds the other: keep the larger
            if (diff >= 0.0f) {
                c = oc;
                r = orad;
            }
            return;
        }
        const float len = std::sqrt(d2);
        if (len > 1e-6f) {
            c = oc + d * ((len - diff) * (0.5f / len));
        }
        r = ((orad + len) + r) * 0.5f;
    }

    // A skinned shape's bound in its own space as the game computes it with the bones at the file's rest pose:
    // The game merges each bone's bounding sphere (NiSkinData bone data)
    // placed by the bone's world transform (the first copied, the rest merged in order), then takes it through the
    // skin data's root-parent-to-skin transform after the inverse of the skeleton root's world transform;
    // The game places that with the shape's world transform. The stored bound of an
    // SE skinned BSTriShape is empty, so without this a skinned model (bows, the books) would have none.
    void skinned_bound(const BSTriShape& t, glm::vec3& center, float& radius) const {
        const auto* si = m_nif.valid(t.skin) ? std::get_if<NiSkinInstance>(&m_nif.blocks[size_t(t.skin)].data) : nullptr;
        const auto* sd = si && m_nif.valid(si->data) ? std::get_if<NiSkinData>(&m_nif.blocks[size_t(si->data)].data)
                                                     : nullptr;
        if (!sd || sd->bones.empty() || sd->bones.size() != si->bones.size() || !m_nif.valid(si->skeleton_root)) {
            return;
        }
        glm::vec3 c(0.0f);
        float r = 0.0f;
        for (size_t i = 0; i < sd->bones.size(); ++i) {
            if (!m_nif.valid(si->bones[i])) {
                continue;
            }
            const Xform w = rest_world(si->bones[i]);
            const glm::vec3 bc = w.t + w.s * (w.r * sd->bones[i].bound_center);
            const float br = sd->bones[i].bound_radius * w.s;
            if (i == 0) {
                c = bc;
                r = br;
            } else {
                merge(c, r, bc, br);
            }
        }
        const NiTransform& k = sd->skin_transform;
        const Xform to_skin = Xform{k.rotation, k.translation, k.scale} * rest_world(si->skeleton_root).inverse();
        center = to_skin.t + to_skin.s * (to_skin.r * c);
        radius = r * to_skin.s;
    }

    void collect_nodes() {
        for (size_t i = 0; i < m_nif.blocks.size(); ++i) {
            if (const auto* n = std::get_if<NiNode>(&m_nif.blocks[i].data)) {
                for (auto c : n->children) {
                    if (m_nif.valid(c) && m_parent[static_cast<size_t>(c)] == -1) {
                        m_parent[static_cast<size_t>(c)] = static_cast<NifRef>(i);
                    }
                }
            }
        }
        for (size_t i = 0; i < m_nif.blocks.size(); ++i) {
            const NiAVObject* av = m_nif.av_object(static_cast<NifRef>(i));
            if (!av) {
                continue;
            }
            ose::Node n{};
            n.name = m_e.add_string(av->name);
            n.parent = k_none;
            n.flags = av->flags;
            n.scale = av->scale;
            n.translation[0] = av->translation.x;
            n.translation[1] = av->translation.y;
            n.translation[2] = av->translation.z;
            std::memcpy(n.rotation, &av->rotation, sizeof n.rotation);
            m_node_of[i] = static_cast<uint32_t>(m_e.nodes.size());
            m_e.nodes.push_back(n);
        }
        for (size_t i = 0; i < m_nif.blocks.size(); ++i) {
            if (m_node_of[i] != k_none && m_parent[i] != -1) {
                m_e.nodes[m_node_of[i]].parent = m_node_of[static_cast<size_t>(m_parent[i])];
            }
        }
        if (!m_nif.roots.empty() && m_nif.valid(m_nif.roots[0])) {
            m_e.root = m_node_of[static_cast<size_t>(m_nif.roots[0])];
        }
        // Version 3: each node's class and a shape's stored bound, and the node each .osm submesh came from.
        m_e.node_extras.assign(m_e.nodes.size(), ose::NodeExtra{});
        for (size_t i = 0; i < m_nif.blocks.size(); ++i) {
            if (m_node_of[i] == k_none) {
                continue;
            }
            ose::NodeExtra& x = m_e.node_extras[m_node_of[i]];
            x.uv_rect[0] = 0.0f;
            x.uv_rect[1] = 1.0f;
            x.uv_rect[2] = 0.0f;
            x.uv_rect[3] = 1.0f;
            const auto& data = m_nif.blocks[i].data;
            if (const auto* n = std::get_if<NiNode>(&data)) {
                x.kind = n->billboard_mode ? ose::NodeKind::billboard : ose::NodeKind::node;
                x.billboard_mode = n->billboard_mode.value_or(0);
            } else if (const auto* t = std::get_if<BSTriShape>(&data)) {
                x.kind = ose::NodeKind::shape;
                glm::vec3 c = t->bound_center;
                float radius = t->bound_radius;
                skinned_bound(*t, c, radius);
                x.bound_center[0] = c.x;
                x.bound_center[1] = c.y;
                x.bound_center[2] = c.z;
                x.bound_radius = radius;
                const auto& uvs = t->geometry.uvs;
                if (uvs.size() >= 4 && uvs.size() == t->geometry.positions.size()) {
                    glm::vec2 lo = uvs[0], hi = uvs[0];
                    for (const auto& uv : uvs) {
                        lo = glm::min(lo, uv);
                        hi = glm::max(hi, uv);
                    }
                    x.uv_rect[0] = std::max(lo.x, 0.0f);
                    x.uv_rect[1] = std::min(hi.x, 1.0f);
                    x.uv_rect[2] = std::max(lo.y, 0.0f);
                    x.uv_rect[3] = std::min(hi.y, 1.0f);
                }
            } else if (std::get_if<NiGeometry>(&data)) {
                x.kind = ose::NodeKind::shape;
            }
        }
        m_e.submesh_nodes.clear();
        for (NifRef ref : m_submesh_shapes) {
            m_e.submesh_nodes.push_back(node(ref));
        }
    }

    uint32_t node(NifRef ref) const {
        return m_nif.valid(ref) ? m_node_of[static_cast<size_t>(ref)] : k_none;
    }

    // ---- Materials.

    uint32_t add_material(NifRef shader, NifRef alpha, bool has_colors, ose::MaterialOwner kind,
                          uint32_t owner, uint32_t owner_node) {
        ose::EffectMaterial m{};
        m.owner_kind = kind;
        m.owner = owner;
        m.node = owner_node;
        m.source_texture = k_none;
        m.greyscale_texture = k_none;
        if (const auto* converted = m_nif.get<BSEffectShaderProperty>(shader)) {
            auto mat = convert_material(m_nif, shader, alpha, has_colors);
            const auto& p = mat->params;
            m.skyrim_flags1 = p.skyrim_flags1;
            m.skyrim_flags2 = p.skyrim_flags2;
            m.flags = p.flags;
            m.blend_src = p.blend_src;
            m.blend_dst = p.blend_dst;
            m.alpha_threshold = p.alpha_threshold;
            m.alpha_func = p.alpha_func;
            m.texture_clamp = p.texture_clamp;
            m.lighting_influence = converted->lighting_influence;
            m.env_map_min_lod = converted->env_map_min_lod;
            std::memcpy(m.uv_offset, p.uv_offset, sizeof m.uv_offset);
            std::memcpy(m.uv_scale, p.uv_scale, sizeof m.uv_scale);
            std::memcpy(m.base_color, p.base_color, sizeof m.base_color);
            m.base_color_scale = p.base_color_scale;
            std::memcpy(m.falloff, p.falloff, sizeof m.falloff);
            m.soft_falloff_depth = p.soft_falloff_depth;
            if (!mat->textures[osm::texture_diffuse].empty()) {
                m.source_texture = m_e.add_string(mat->textures[osm::texture_diffuse]);
            }
            if (!mat->textures[osm::texture_normal].empty()) {
                m.greyscale_texture = m_e.add_string(mat->textures[osm::texture_normal]);
            }
        } else {
            // Effect defaults (the game constructor, see the design): base colour 1, scale 1.
            m.uv_scale[0] = m.uv_scale[1] = 1.0f;
            std::ranges::fill(m.base_color, 1.0f);
            m.base_color_scale = 1.0f;
            m.texture_clamp = 3;
            warn(std::format("shader block {} ({}) is not a BSEffectShaderProperty; default material used",
                             shader, m_nif.type_name(shader)));
        }
        if (const auto* a = m_nif.get<NiAlphaProperty>(alpha)) {
            m.alpha_property_flags = a->flags;
        }
        auto index = static_cast<uint32_t>(m_e.materials.size());
        m_e.materials.push_back(m);
        if (m_nif.valid(shader)) {
            m_material_of.emplace(shader, index);
        }
        return index;
    }

    // The material for a shader property an effect controller animates: the particle system's own, or a new
    // entry for the mesh shape that uses it.
    uint32_t material_for_shader(NifRef shader) {
        if (auto it = m_material_of.find(shader); it != m_material_of.end()) {
            return it->second;
        }
        for (size_t i = 0; i < m_nif.blocks.size(); ++i) {
            const auto& data = m_nif.blocks[i].data;
            NifRef shape_shader = -1;
            NifRef shape_alpha = -1;
            bool colors = false;
            if (const auto* s = std::get_if<BSTriShape>(&data)) {
                shape_shader = s->shader_property;
                shape_alpha = s->alpha_property;
                colors = !s->geometry.colors.empty();
            } else if (const auto* g = std::get_if<NiGeometry>(&data)) {
                shape_shader = g->shader_property;
                shape_alpha = g->alpha_property;
            }
            if (shape_shader != shader) {
                continue;
            }
            auto ref = static_cast<NifRef>(i);
            uint32_t submesh = k_none;
            for (size_t k = 0; k < m_submesh_shapes.size(); ++k) {
                if (m_submesh_shapes[k] == ref) {
                    submesh = static_cast<uint32_t>(k);
                    break;
                }
            }
            return add_material(shader, shape_alpha, colors, ose::MaterialOwner::mesh_shape, submesh,
                                node(ref));
        }
        warn(std::format("effect controller target {} is not the shader of any shape", shader));
        return k_none;
    }

    // ---- Particle systems and their modifiers.

    void add_system(NifRef ref, const NiParticleSystem& ps) {
        const auto* data = m_nif.get<NiPSysData>(ps.data);
        if (!data) {
            warn(std::format("particle system {} has no decodable NiPSysData (block {})", ref, ps.data));
            return;
        }
        auto index = static_cast<uint32_t>(m_e.systems.size());
        m_system_of[static_cast<size_t>(ref)] = index;
        ose::System s{};
        s.node = node(ref);
        s.max_particles = data->max_particles;
        uint16_t flags = 0;
        auto set = [&](bool on, uint16_t f) { flags = static_cast<uint16_t>(flags | (on ? f : 0)); };
        set(ps.world_space, ose::system_world_space);
        set(data->has_colors, ose::system_colors);
        set(data->has_radii, ose::system_radii);
        set(data->has_sizes, ose::system_sizes);
        set(data->has_rotations, ose::system_rotations);
        set(data->has_rotation_angles, ose::system_rotation_angles);
        set(data->has_rotation_axes, ose::system_rotation_axes);
        set(data->has_texture_indices, ose::system_texture_indices);
        set(data->has_rotation_speeds, ose::system_rotation_speeds);
        s.flags = flags;
        s.first_subtexture = static_cast<uint32_t>(m_e.subtextures.size());
        s.subtexture_count = static_cast<uint32_t>(data->subtexture_offsets.size());
        for (const auto& o : data->subtexture_offsets) {
            m_e.subtextures.push_back({o.x, o.y, o.z, o.w});
        }
        s.aspect_ratio = data->aspect_ratio;
        s.aspect_flags = data->aspect_flags;
        std::copy(data->speed_to_aspect.begin(), data->speed_to_aspect.end(), s.speed_to_aspect);
        s.bound[0] = data->bound_center.x;
        s.bound[1] = data->bound_center.y;
        s.bound[2] = data->bound_center.z;
        s.bound[3] = data->bound_radius;
        m_e.systems.push_back(s);
        m_e.systems[index].material = add_material(ps.shader_property, ps.alpha_property, data->has_colors,
                                                   ose::MaterialOwner::system, index, node(ref));

        // The game inserts each modifier before the first one with a higher order.
        std::vector<std::pair<NifRef, ose::Modifier>> mods;
        for (auto m : ps.modifiers) {
            ose::Modifier rec{};
            if (convert_modifier(m, index, rec)) {
                mods.emplace_back(m, rec);
            }
        }
        std::ranges::stable_sort(mods, {}, [](const auto& p) { return p.second.order; });
        m_e.systems[index].first_modifier = static_cast<uint32_t>(m_e.modifiers.size());
        m_e.systems[index].modifier_count = static_cast<uint32_t>(mods.size());
        for (auto& [m, rec] : mods) {
            m_modifier_of.emplace(m, static_cast<uint32_t>(m_e.modifiers.size()));
            m_e.modifiers.push_back(rec);
        }
    }

    void base(const NiPSysModifierBase& b, uint32_t system, ose::ModifierType type, ose::Modifier& m) {
        m.type = type;
        m.name = m_e.add_string(b.name);
        m.order = b.order;
        m.system = system;
        m.active = b.active ? 1 : 0;
        m.ref = k_none;
        m.first = 0;
        m.count = 0;
    }

    uint32_t emitter_mesh(NifRef shape_ref) {
        if (auto it = m_emitter_mesh_of.find(shape_ref); it != m_emitter_mesh_of.end()) {
            return it->second;
        }
        ose::EmitterMesh em{};
        em.node = node(shape_ref);
        em.first_vertex = static_cast<uint32_t>(m_e.mesh_positions.size());
        em.first_triangle = static_cast<uint32_t>(m_e.mesh_triangles.size());
        if (const auto* shape = m_nif.get<BSTriShape>(shape_ref)) {
            em.vertex_count = static_cast<uint32_t>(shape->particle_positions.size());
            em.triangle_count = static_cast<uint32_t>(shape->particle_triangles.size());
            for (size_t i = 0; i < shape->particle_positions.size(); ++i) {
                m_e.mesh_positions.push_back(to_vec3(shape->particle_positions[i]));
                m_e.mesh_normals.push_back(to_vec3(shape->particle_normals[i]));
            }
            for (const auto& t : shape->particle_triangles) {
                if (t[0] >= em.vertex_count || t[1] >= em.vertex_count || t[2] >= em.vertex_count) {
                    warn(std::format("emitter mesh {}: triangle indexes past its {} vertices", shape_ref,
                                     em.vertex_count));
                    em.triangle_count = 0;
                    m_e.mesh_triangles.resize(em.first_triangle);
                    break;
                }
                m_e.mesh_triangles.push_back({{t[0], t[1], t[2]}});
            }
            if (em.triangle_count == 0) {
                warn(std::format("emitter mesh {} has no particle data: the game emits nothing from it",
                                 shape_ref));
            }
        } else {
            warn(std::format("emitter mesh {} ({}) is not a decodable BSTriShape", shape_ref,
                             m_nif.type_name(shape_ref)));
        }
        if (em.node == k_none) {
            em.node = m_e.root;
        }
        auto index = static_cast<uint32_t>(m_e.emitter_meshes.size());
        m_e.emitter_meshes.push_back(em);
        m_emitter_mesh_of.emplace(shape_ref, index);
        return index;
    }

    bool convert_modifier(NifRef ref, uint32_t system, ose::Modifier& m) {
        const auto& data = m_nif.valid(ref) ? m_nif.blocks[static_cast<size_t>(ref)].data : NifBlockData{};
        using T = ose::ModifierType;
        if (const auto* a = std::get_if<NiPSysAgeDeathModifier>(&data)) {
            base(*a, system, T::age_death, m);
            m.flag0 = a->spawn_on_death ? 1 : 0;
            m_age_death_spawn.emplace_back(ref, a->spawn_modifier);
        } else if (const auto* l = std::get_if<BSPSysLODModifier>(&data)) {
            base(*l, system, T::lod, m);
            // The game scales both distances by 1/3 and forces the order to 1.
            m.order = 1;
            m.f[0] = l->begin_distance * 0.33333334f;
            m.f[1] = l->end_distance * 0.33333334f;
            m.f[2] = l->end_emit_scale;
            m.f[3] = l->end_size;
        } else if (const auto* e = std::get_if<NiPSysMeshEmitter>(&data)) {
            base(*e, system, T::mesh_emitter, m);
            float f[] = {
                e->speed,           e->speed_variation,        e->declination,     e->declination_variation,
                e->planar_angle,    e->planar_angle_variation, e->initial_color.r, e->initial_color.g,
                e->initial_color.b, e->initial_color.a,        e->initial_radius,  e->radius_variation,
                e->life_span,       e->life_span_variation,    e->emission_axis.x, e->emission_axis.y,
                e->emission_axis.z};
            std::copy(std::begin(f), std::end(f), m.f);
            m.u0 = e->velocity_type;
            m.u1 = e->emission_type;
            m.first = static_cast<uint32_t>(m_e.emitter_meshes.size());
            std::vector<uint32_t> meshes;
            for (auto mesh : e->meshes) {
                meshes.push_back(emitter_mesh(mesh));
            }
            // The records of one emitter must be contiguous: re-add shared meshes at the end if needed.
            bool contiguous = true;
            for (size_t i = 0; i < meshes.size(); ++i) {
                contiguous = contiguous && meshes[i] == m.first + i;
            }
            if (!contiguous) {
                m.first = static_cast<uint32_t>(m_e.emitter_meshes.size());
                for (auto idx : meshes) {
                    m_e.emitter_meshes.push_back(m_e.emitter_meshes[idx]);
                }
            }
            m.count = static_cast<uint32_t>(meshes.size());
            if (e->emission_type != 1 && e->emission_type != 3) {
                warn(std::format(
                    "mesh emitter {} uses emission type {}: the game only emits from face centres "
                    "(1) and face surfaces (3), so it emits nothing",
                    ref, e->emission_type));
            }
        } else if (const auto* e = std::get_if<NiPSysVolumeEmitter>(&data)) {
            base(*e, system, T::volume_emitter, m);
            float f[] = {
                e->speed,           e->speed_variation,        e->declination,     e->declination_variation,
                e->planar_angle,    e->planar_angle_variation, e->initial_color.r, e->initial_color.g,
                e->initial_color.b, e->initial_color.a,        e->initial_radius,  e->radius_variation,
                e->life_span,       e->life_span_variation,    e->extents[0],      e->extents[1],
                e->extents[2]};
            std::copy(std::begin(f), std::end(f), m.f);
            m.u0 = static_cast<uint32_t>(e->shape);
            m.ref = node(e->emitter_object);
        } else if (const auto* s = std::get_if<NiPSysSpawnModifier>(&data)) {
            base(*s, system, T::spawn, m);
            m.u0 = s->num_generations;
            m.u1 = uint32_t{s->min_spawned} | uint32_t{s->max_spawned} << 16;
            float f[] = {s->percentage_spawned, s->speed_variation, s->direction_variation, s->life_span,
                         s->life_span_variation};
            std::copy(std::begin(f), std::end(f), m.f);
        } else if (const auto* c = std::get_if<BSPSysSimpleColorModifier>(&data)) {
            base(*c, system, T::simple_color, m);
            float f[] = {c->fade_in,    c->fade_out,     c->color1_start,
                         c->color1_end, c->color2_start, c->color2_end};
            std::copy(std::begin(f), std::end(f), m.f);
            for (size_t i = 0; i < 3; ++i) {
                for (int k = 0; k < 4; ++k) {
                    m.f[6 + i * 4 + k] = c->colors[i][k];
                }
            }
        } else if (const auto* r = std::get_if<NiPSysRotationModifier>(&data)) {
            base(*r, system, T::rotation, m);
            float f[] = {r->speed,  r->speed_variation, r->angle, r->angle_variation,
                         r->axis.x, r->axis.y,          r->axis.z};
            std::copy(std::begin(f), std::end(f), m.f);
            m.flag0 = r->random_speed_sign ? 1 : 0;
            m.flag1 = r->random_axis ? 1 : 0;
        } else if (const auto* t = std::get_if<BSPSysSubTexModifier>(&data)) {
            base(*t, system, T::subtex, m);
            float f[] = {t->start_frame,      t->start_frame_fudge,      t->end_frame,
                         t->loop_start_frame, t->loop_start_frame_fudge, t->frame_count,
                         t->frame_count_fudge};
            std::copy(std::begin(f), std::end(f), m.f);
        } else if (const auto* sc = std::get_if<BSPSysScaleModifier>(&data)) {
            base(*sc, system, T::scale, m);
            m.first = static_cast<uint32_t>(m_e.floats.size());
            m.count = static_cast<uint32_t>(sc->scales.size());
            m_e.floats.insert(m_e.floats.end(), sc->scales.begin(), sc->scales.end());
            if (sc->scales.empty()) {
                warn(std::format("scale modifier {} has an empty curve", ref));
            }
        } else if (const auto* g = std::get_if<NiPSysGravityModifier>(&data)) {
            base(*g, system, T::gravity, m);
            m.ref = node(g->gravity_object);
            float f[] = {g->axis.x,   g->axis.y,     g->axis.z,          g->decay,
                         g->strength, g->turbulence, g->turbulence_scale};
            std::copy(std::begin(f), std::end(f), m.f);
            m.u0 = g->force_type;
            m.flag0 = g->world_aligned ? 1 : 0;
        } else if (const auto* d = std::get_if<NiPSysDragModifier>(&data)) {
            base(*d, system, T::drag, m);
            m.ref = node(d->parent);
            float f[] = {d->axis.x, d->axis.y, d->axis.z, d->percentage, d->range, d->range_falloff};
            std::copy(std::begin(f), std::end(f), m.f);
        } else if (const auto* p = std::get_if<NiPSysPositionModifier>(&data)) {
            base(*p, system, T::position, m);
        } else if (const auto* b = std::get_if<NiPSysBoundUpdateModifier>(&data)) {
            base(*b, system, T::bound_update, m);
            m.u0 = b->update_skip;
        } else {
            warn(std::format("particle modifier {} ({}) not converted: unsupported or undecodable", ref,
                             m_nif.type_name(ref)));
            return false;
        }
        return true;
    }

    void resolve_age_death_spawn() {
        for (auto [age, spawn] : m_age_death_spawn) {
            auto a = m_modifier_of.find(age);
            if (a == m_modifier_of.end()) {
                continue;
            }
            auto s = m_modifier_of.find(spawn);
            m_e.modifiers[a->second].ref = s == m_modifier_of.end() ? k_none : s->second;
        }
    }

    // ---- Interpolators and keys.

    uint32_t key_group(NifRef ref) {
        if (!m_nif.valid(ref)) {
            return k_none;
        }
        if (auto it = m_keys_of.find(ref); it != m_keys_of.end()) {
            return it->second;
        }
        ose::KeyGroup g{};
        if (const auto* f = m_nif.get<NiFloatData>(ref)) {
            g.kind = ose::KeyKind::float_keys;
            g.key_type = f->key_type;
            g.first = static_cast<uint32_t>(m_e.float_keys.size());
            g.count = static_cast<uint32_t>(f->keys.size());
            for (const auto& k : f->keys) {
                m_e.float_keys.push_back(
                    {k.time, k.value, k.forward, k.backward, k.tbc[0], k.tbc[1], k.tbc[2], 0});
            }
            if (f->key_type == 3 || f->key_type == 4) {
                warn(std::format("float keys {} use key type {}: simulated as linear", ref, f->key_type));
            }
        } else if (const auto* b = m_nif.get<NiBoolData>(ref)) {
            g.kind = ose::KeyKind::bool_keys;
            g.key_type = b->key_type;
            g.first = static_cast<uint32_t>(m_e.bool_keys.size());
            g.count = static_cast<uint32_t>(b->keys.size());
            for (auto [t, v] : b->keys) {
                m_e.bool_keys.push_back({t, v, {}});
            }
        } else {
            warn(std::format("key data {} ({}) not decoded", ref, m_nif.type_name(ref)));
            return k_none;
        }
        auto index = static_cast<uint32_t>(m_e.key_groups.size());
        m_e.key_groups.push_back(g);
        m_keys_of.emplace(ref, index);
        return index;
    }

    uint32_t float_group(const NiFloatData& f, std::string_view what) {
        ose::KeyGroup g{};
        g.kind = ose::KeyKind::float_keys;
        g.key_type = f.key_type;
        g.first = static_cast<uint32_t>(m_e.float_keys.size());
        g.count = static_cast<uint32_t>(f.keys.size());
        for (const auto& k : f.keys) {
            m_e.float_keys.push_back({k.time, k.value, k.forward, k.backward, k.tbc[0], k.tbc[1], k.tbc[2], 0});
        }
        if (f.key_type == 3 || f.key_type == 4) {
            warn(std::format("{} use key type {}: simulated as linear", what, f.key_type));
        }
        auto index = static_cast<uint32_t>(m_e.key_groups.size());
        m_e.key_groups.push_back(g);
        return index;
    }

    // NiTransformInterpolator with its NiTransformData.
    uint32_t transform_interpolator(NifRef ref, const NiTransformInterpolator& ti) {
        ose::TransformInterp t{};
        t.translation[0] = ti.translation.x;
        t.translation[1] = ti.translation.y;
        t.translation[2] = ti.translation.z;
        for (int i = 0; i < 4; ++i) {
            t.rotation[i] = ti.rotation[i];
        }
        t.scale = ti.scale;
        t.rotation_first = t.translation_first = 0;
        t.scale_group = k_none;
        if (const auto* d = m_nif.get<NiTransformData>(ti.data)) {
            t.rotation_type = d->rotation_type;
            if (d->rotation_type == 4) {
                t.rotation_first = float_group(d->xyz[0], "XYZ rotation keys");
                float_group(d->xyz[1], "XYZ rotation keys");
                float_group(d->xyz[2], "XYZ rotation keys");
                t.rotation_count = 3;
            } else {
                t.rotation_first = static_cast<uint32_t>(m_e.quat_keys.size());
                t.rotation_count = static_cast<uint32_t>(d->rotation_keys.size());
                for (const auto& k : d->rotation_keys) {
                    m_e.quat_keys.push_back(
                        {k.time, {k.value[0], k.value[1], k.value[2], k.value[3]}, k.tbc[0], k.tbc[1], k.tbc[2]});
                }
            }
            t.translation_type = d->translation_type;
            t.translation_first = static_cast<uint32_t>(m_e.vec3_keys.size());
            t.translation_count = static_cast<uint32_t>(d->translation_keys.size());
            for (const auto& k : d->translation_keys) {
                m_e.vec3_keys.push_back({k.time,
                                         {k.value.x, k.value.y, k.value.z},
                                         {k.forward.x, k.forward.y, k.forward.z},
                                         {k.backward.x, k.backward.y, k.backward.z},
                                         k.tbc[0],
                                         k.tbc[1],
                                         k.tbc[2],
                                         0.0f});
            }
            if (!d->translation_keys.empty() && d->translation_type != 1) {
                warn(std::format("transform data {}: translation key type {} is not simulated", ti.data,
                                 d->translation_type));
            }
            if (!d->scales.keys.empty()) {
                t.scale_group = float_group(d->scales, "scale keys");
            }
        } else if (m_nif.valid(ti.data)) {
            warn(std::format("transform data {} ({}) not decoded", ti.data, m_nif.type_name(ti.data)));
        }
        ose::Interpolator rec{};
        rec.type = ose::InterpolatorType::transform;
        rec.float_value = -3.402823466e+38f;
        rec.bool_value = 2;
        rec.key_group = static_cast<uint32_t>(m_e.transforms.size());
        m_e.transforms.push_back(t);
        auto index = static_cast<uint32_t>(m_e.interpolators.size());
        m_e.interpolators.push_back(rec);
        m_interp_of.emplace(ref, index);
        return index;
    }

    uint32_t interpolator(NifRef ref) {
        if (!m_nif.valid(ref)) {
            return k_none;
        }
        if (auto it = m_interp_of.find(ref); it != m_interp_of.end()) {
            return it->second;
        }
        if (const auto* ti = m_nif.get<NiTransformInterpolator>(ref)) {
            return transform_interpolator(ref, *ti);
        }
        const auto* i = m_nif.get<NiInterpolator>(ref);
        if (!i) {
            warn(std::format("interpolator {} ({}) not decoded", ref, m_nif.type_name(ref)));
            return k_none;
        }
        auto type = m_nif.type_name(ref);
        ose::Interpolator rec{};
        using T = ose::InterpolatorType;
        rec.type = type == "NiFloatInterpolator"          ? T::float_value
                   : type == "NiBoolInterpolator"         ? T::bool_value
                   : type == "NiBoolTimelineInterpolator" ? T::bool_timeline
                   : type == "NiBlendFloatInterpolator"   ? T::blend_float
                                                          : T::blend_bool;
        rec.float_value = i->float_value;
        rec.bool_value = i->bool_value;
        rec.blend_flags = i->blend_flags;
        rec.array_size = i->array_size;
        rec.weight_threshold = i->weight_threshold;
        rec.key_group = key_group(i->data);
        auto index = static_cast<uint32_t>(m_e.interpolators.size());
        m_e.interpolators.push_back(rec);
        m_interp_of.emplace(ref, index);
        return index;
    }

    // ---- Controllers, in the order the game's scene update reaches them.

    void visit(NifRef ref, std::unordered_set<NifRef>& visited, int depth) {
        if (!m_nif.valid(ref) || depth > 256 || !visited.insert(ref).second) {
            return;
        }
        const NiAVObject* av = m_nif.av_object(ref);
        if (!av) {
            return;
        }
        add_controller_chain(av->controller);
        const auto& data = m_nif.blocks[static_cast<size_t>(ref)].data;
        NifRef shader = -1;
        NifRef alpha = -1;
        if (const auto* s = std::get_if<BSTriShape>(&data)) {
            shader = s->shader_property;
            alpha = s->alpha_property;
        } else if (const auto* g = std::get_if<NiGeometry>(&data)) {
            shader = g->shader_property;
            alpha = g->alpha_property;
        } else if (const auto* p = std::get_if<NiParticleSystem>(&data)) {
            shader = p->shader_property;
            alpha = p->alpha_property;
        }
        for (auto prop : {shader, alpha}) {
            if (const auto* e = m_nif.get<BSEffectShaderProperty>(prop)) {
                add_controller_chain(e->controller);
            } else if (const auto* l = m_nif.get<BSLightingShaderProperty>(prop)) {
                add_controller_chain(l->controller);
            } else if (const auto* a = m_nif.get<NiAlphaProperty>(prop)) {
                add_controller_chain(a->controller);
            }
        }
        if (const auto* n = std::get_if<NiNode>(&data)) {
            for (auto c : n->children) {
                visit(c, visited, depth + 1);
            }
        }
    }

    void add_controller_chain(NifRef ref) {
        for (int guard = 0; m_nif.valid(ref) && guard < 1024; ++guard) {
            if (m_controller_of.contains(ref)) {
                return;
            }
            if (const auto* manager = m_nif.get<NiControllerManager>(ref)) {
                if (m_manager == -1) {
                    m_manager = ref;
                } else if (m_manager != ref) {
                    warn(std::format("second NiControllerManager {} ignored", ref));
                }
                m_controller_of.emplace(ref, k_none);
                ref = manager->base.next;
                continue;
            }
            const auto* c = m_nif.get<NiTimeController>(ref);
            if (!c) {
                // Not decoded: skipped, but the chain goes on through its "next controller" link.
                warn(std::format("controller {} ({}) not decoded", ref, m_nif.type_name(ref)));
                m_controller_of.emplace(ref, k_none);
                ref = m_nif.blocks[static_cast<size_t>(ref)].next_controller;
                continue;
            }
            m_controller_of.emplace(ref, add_controller(ref, *c));
            ref = c->next;
        }
    }

    uint32_t modifier_by_name(uint32_t system, std::string_view name) const {
        const auto& s = m_e.systems[system];
        for (uint32_t i = 0; i < s.modifier_count; ++i) {
            const auto& m = m_e.modifiers[s.first_modifier + i];
            if (m_e.string(m.name) == name) {
                return s.first_modifier + i;
            }
        }
        return k_none;
    }

    uint32_t add_controller(NifRef ref, const NiTimeController& c) {
        auto type = m_nif.type_name(ref);
        ose::Controller rec{};
        using T = ose::ControllerType;
        rec.type = type == "NiPSysModifierActiveCtlr"                ? T::modifier_active
                   : type == "NiPSysEmitterCtlr"                     ? T::emitter
                   : type == "BSPSysMultiTargetEmitterCtlr"          ? T::emitter
                   : type == "NiPSysUpdateCtlr"                      ? T::update
                   : type == "BSEffectShaderPropertyFloatController" ? T::effect_float
                   : type == "BSEffectShaderPropertyColorController" ? T::effect_color
                   : type == "NiMultiTargetTransformController"      ? T::transform
                                                                     : T::other;
        // The game clears "force update" (0x80) and sets "compute scaled time" (0x40).
        rec.flags = static_cast<uint16_t>((c.flags & 0xFF3F) | 0x40);
        rec.frequency = c.frequency;
        rec.phase = c.phase;
        rec.start_time = c.start_time;
        rec.stop_time = c.stop_time;
        rec.target_kind = ose::TargetKind::none;
        rec.target = k_none;
        rec.interpolator = interpolator(c.interpolator);
        rec.interpolator2 = interpolator(c.visibility_interpolator);
        rec.modifier = k_none;
        rec.first_target = 0;
        rec.target_count = 0;
        rec.variable = c.controlled_variable;
        rec.name = m_e.add_string(rec.type == T::other ? std::string(type) : c.modifier_name);
        switch (rec.type) {
        case T::modifier_active:
        case T::emitter:
        case T::update:
        case T::other:
            if (!type.starts_with("NiPSys") && !type.starts_with("BSPSys")) {
                // Not a particle controller (lighting-shader, transform, visibility, ...): stored by name
                // with the scene node it animates.
                rec.target = node(c.target);
                rec.target_kind = rec.target == k_none ? ose::TargetKind::none : ose::TargetKind::node;
            } else if (m_nif.valid(c.target) && m_system_of[static_cast<size_t>(c.target)] != k_none) {
                rec.target_kind = ose::TargetKind::system;
                rec.target = m_system_of[static_cast<size_t>(c.target)];
                if (!c.modifier_name.empty()) {
                    rec.modifier = modifier_by_name(rec.target, c.modifier_name);
                    if (rec.modifier == k_none) {
                        warn(std::format("controller {}: no modifier named '{}'", ref, c.modifier_name));
                    }
                }
            } else {
                warn(std::format("controller {} ({}) targets block {}, not a particle system", ref, type,
                                 c.target));
            }
            break;
        case T::effect_float:
        case T::effect_color:
            rec.target = material_for_shader(c.target);
            rec.target_kind = rec.target == k_none ? ose::TargetKind::none : ose::TargetKind::material;
            break;
        case T::transform:
            rec.target_kind = ose::TargetKind::node;
            rec.target = node(c.target);
            if (rec.target == k_none) {
                rec.target_kind = ose::TargetKind::none;
            }
            break;
        }
        if (rec.type == T::other || rec.type == T::effect_color) {
            warn(std::format("controller {} ({}) is stored but not simulated", ref, type));
        }
        if (rec.type == T::transform) {
            // Its targets, in file order: the controller drives one blend interpolator per target.
            rec.first_target = static_cast<uint32_t>(m_e.controller_targets.size());
            rec.target_count = static_cast<uint32_t>(c.extra_targets.size());
            for (NifRef t : c.extra_targets) {
                m_e.controller_targets.push_back(node(t));
            }
        }
        auto index = static_cast<uint32_t>(m_e.controllers.size());
        m_e.controllers.push_back(rec);
        return index;
    }

    // ---- Controller sequences.

    void add_sequences() {
        const auto* manager = m_nif.get<NiControllerManager>(m_manager);
        if (!manager) {
            return;
        }
        for (auto ref : manager->sequences) {
            const auto* s = m_nif.get<NiControllerSequence>(ref);
            if (!s) {
                warn(std::format("sequence {} ({}) not decoded", ref, m_nif.type_name(ref)));
                continue;
            }
            ose::Sequence rec{};
            rec.name = m_e.add_string(s->name);
            rec.weight = s->weight;
            rec.cycle_type = s->cycle_type;
            rec.frequency = s->frequency;
            rec.start_time = s->start_time;
            rec.stop_time = s->stop_time;
            rec.accum_root = m_e.add_string(s->accum_root_name);
            rec.first_block = static_cast<uint32_t>(m_e.blocks.size());
            for (const auto& b : s->blocks) {
                ose::Block blk{};
                blk.interpolator = interpolator(b.interpolator);
                auto it = m_controller_of.find(b.controller);
                blk.controller = it == m_controller_of.end() ? k_none : it->second;
                if (m_nif.valid(b.controller) && it == m_controller_of.end()) {
                    warn(std::format("sequence '{}': controller {} is not on the scene graph", s->name,
                                     b.controller));
                }
                blk.priority = b.priority;
                blk.node_name = m_e.add_string(b.node_name);
                blk.property_type = m_e.add_string(b.property_type);
                blk.controller_type = m_e.add_string(b.controller_type);
                blk.controller_id = m_e.add_string(b.controller_id);
                blk.interpolator_id = m_e.add_string(b.interpolator_id);
                m_e.blocks.push_back(blk);
            }
            rec.block_count = static_cast<uint32_t>(s->blocks.size());
            rec.first_text_key = static_cast<uint32_t>(m_e.text_keys.size());
            if (const auto* t = m_nif.get<NiTextKeyExtraData>(s->text_keys)) {
                for (const auto& [time, text] : t->keys) {
                    m_e.text_keys.push_back({time, m_e.add_string(text)});
                }
                rec.text_key_count = static_cast<uint32_t>(t->keys.size());
            }
            m_e.sequences.push_back(rec);
        }
    }

    const NifFile& m_nif;
    std::span<const NifRef> m_submesh_shapes;
    ParticleImportResult& m_out;
    ose::Effect& m_e;
    std::vector<uint32_t> m_node_of;
    std::vector<NifRef> m_parent;
    std::vector<uint32_t> m_system_of;
    std::unordered_map<NifRef, uint32_t> m_modifier_of;
    std::unordered_map<NifRef, uint32_t> m_interp_of;
    std::unordered_map<NifRef, uint32_t> m_keys_of;
    std::unordered_map<NifRef, uint32_t> m_controller_of;
    std::unordered_map<NifRef, uint32_t> m_material_of;
    std::unordered_map<NifRef, uint32_t> m_emitter_mesh_of;
    std::vector<std::pair<NifRef, NifRef>> m_age_death_spawn;
    NifRef m_manager = -1;
    bool m_animation = false; // keep the controllers and sequences of a NIF without particle systems
};

} // namespace

bool has_node_animation(const NifFile& nif) {
    for (size_t i = 0; i < nif.blocks.size(); ++i) {
        if (nif.type_name(static_cast<NifRef>(i)) == "NiMultiTargetTransformController") {
            return true;
        }
    }
    return false;
}

ParticleImportResult import_node_animation(const NifFile& nif, std::string_view source_path) {
    ParticleImportResult result;
    for (size_t i = 0; i < nif.blocks.size(); ++i) {
        const auto& b = nif.blocks[i];
        const auto type = nif.type_name(static_cast<NifRef>(i));
        if (b.status == NifBlockStatus::mismatch &&
            (type == "NiTransformData" || type == "NiTransformInterpolator" || type.starts_with("NiController"))) {
            result.warnings.push_back(
                std::format("{}: block {} ({}) could not be decoded: {}", source_path, i, type, b.error));
        }
    }
    EffectBuilder builder(nif, {}, result, true);
    builder.build();
    return result;
}

std::optional<ose::InvMarker> read_inv_marker(const NifFile& nif, std::span<const std::byte> bytes) {
    if (nif.roots.empty()) {
        return std::nullopt;
    }
    const NiAVObject* root = nif.av_object(nif.roots[0]);
    if (!root) {
        return std::nullopt;
    }
    for (NifRef ref : root->extra_data) {
        if (!nif.valid(ref) || nif.type_name(ref) != "BSInvMarker") {
            continue;
        }
        const auto& b = nif.blocks[static_cast<size_t>(ref)];
        if (uint64_t{b.offset} + b.size > bytes.size()) {
            continue;
        }
        BinaryReader r(bytes.subspan(b.offset, b.size));
        const int32_t name = r.read<int32_t>();
        if (name < 0 || static_cast<size_t>(name) >= nif.header.strings.size() ||
            nif.header.strings[static_cast<size_t>(name)] != "INV") {
            continue;
        }
        ose::InvMarker m{};
        for (uint16_t& v : m.rotation) {
            v = r.read<uint16_t>();
        }
        m.zoom = r.read<float>();
        return m;
    }
    return std::nullopt;
}

std::vector<std::string> read_attach_techniques(const NifFile& nif, std::span<const std::byte> bytes) {
    std::vector<std::string> out;
    const NiAVObject* root = nif.roots.empty() ? nullptr : nif.av_object(nif.roots[0]);
    if (!root) {
        return out;
    }
    for (NifRef ref : root->extra_data) {
        if (!nif.valid(ref) || nif.type_name(ref) != "NiStringsExtraData") {
            continue;
        }
        const auto& b = nif.blocks[static_cast<size_t>(ref)];
        if (uint64_t{b.offset} + b.size > bytes.size()) {
            continue;
        }
        BinaryReader r(bytes.subspan(b.offset, b.size));
        const int32_t name = r.read<int32_t>();
        if (name < 0 || static_cast<size_t>(name) >= nif.header.strings.size() ||
            nif.header.strings[static_cast<size_t>(name)] != "AttachT") {
            continue;
        }
        const uint32_t count = r.read<uint32_t>();
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t length = r.read<uint32_t>();
            std::string text(length, '\0');
            for (char& c : text) {
                c = static_cast<char>(r.read<uint8_t>());
            }
            out.push_back(std::move(text));
        }
    }
    return out;
}

ParticleImportResult import_scene_model(const NifFile& nif, std::span<const std::byte> bytes,
                                        std::string_view source_path, std::span<const NifRef> submesh_shapes) {
    ParticleImportResult result;
    for (size_t i = 0; i < nif.blocks.size(); ++i) {
        const auto& b = nif.blocks[i];
        const auto type = nif.type_name(static_cast<NifRef>(i));
        if (b.status == NifBlockStatus::mismatch &&
            (type.starts_with("NiPSys") || type == "NiTransformData" || type == "NiTransformInterpolator" ||
             type.starts_with("NiController"))) {
            result.warnings.push_back(
                std::format("{}: block {} ({}) could not be decoded: {}", source_path, i, type, b.error));
        }
    }
    EffectBuilder builder(nif, submesh_shapes, result, true);
    builder.build();
    if (auto m = read_inv_marker(nif, bytes)) {
        result.effect.inv_marker.push_back(*m);
    }
    return result;
}

std::vector<ose::AddonNode> read_addon_nodes(const NifFile& nif, std::span<const std::byte> bytes) {
    std::vector<ose::AddonNode> out;
    if (nif.roots.empty()) {
        return out;
    }
    // NiTimeController: next, flags, frequency, phase, then the key range. The file's transform controllers aren't decoded.
    float lifetime = -1.0f;
    for (size_t i = 0; i < nif.blocks.size(); ++i) {
        const auto& b = nif.blocks[i];
        if (nif.type_name(static_cast<NifRef>(i)).ends_with("Controller") && b.size >= 22 && uint64_t{b.offset} + b.size <= bytes.size()) {
            BinaryReader r(bytes.subspan(b.offset + 14, 8));
            const float start = r.read<float>(), stop = r.read<float>();
            lifetime = std::max(lifetime, stop - start);
        }
    }
    constexpr std::string_view k_prefix = "AddOnNode";
    const auto walk = [&](const auto& self, NifRef ref, const glm::mat3& rotation, const glm::vec3& translation, float scale) -> void {
        const NiAVObject* o = nif.av_object(ref);
        if (!o) {
            return;
        }
        const glm::mat3 r = rotation * o->rotation;
        const glm::vec3 t = translation + scale * (rotation * o->translation);
        const float s = scale * o->scale;
        if (nif.type_name(ref) == "BSValueNode" && o->name.starts_with(k_prefix)) {
            ose::AddonNode a{};
            a.index = uint32_t(std::atoi(o->name.c_str() + k_prefix.size()));
            a.lifetime = lifetime;
            std::copy_n(&r[0][0], 9, a.rotation);
            std::copy_n(&t[0], 3, a.translation);
            a.scale = s;
            out.push_back(a);
        }
        if (const auto* n = nif.get<NiNode>(ref)) {
            for (NifRef child : n->children) {
                self(self, child, r, t, s);
            }
        }
    };
    walk(walk, nif.roots[0], glm::mat3(1.0f), glm::vec3(0.0f), 1.0f);
    return out;
}

bool has_particle_systems(const NifFile& nif) {
    for (size_t i = 0; i < nif.blocks.size(); ++i) {
        if (nif.type_name(static_cast<NifRef>(i)) == "NiParticleSystem") {
            return true;
        }
    }
    return false;
}

bool has_controller_manager(const NifFile& nif) {
    for (size_t i = 0; i < nif.blocks.size(); ++i) {
        if (nif.type_name(static_cast<NifRef>(i)) == "NiControllerManager") {
            return true;
        }
    }
    return false;
}

ParticleImportResult import_particles(const NifFile& nif, std::string_view source_path,
                                      std::span<const NifRef> submesh_shapes, bool animation) {
    ParticleImportResult result;
    for (size_t i = 0; i < nif.blocks.size(); ++i) {
        const auto& b = nif.blocks[i];
        if (b.status == NifBlockStatus::mismatch &&
            nif.type_name(static_cast<NifRef>(i)).starts_with("NiPSys")) {
            result.warnings.push_back(std::format("{}: block {} ({}) could not be decoded: {}", source_path,
                                                  i, nif.type_name(static_cast<NifRef>(i)), b.error));
        }
    }
    EffectBuilder builder(nif, submesh_shapes, result, animation);
    builder.build();
    return result;
}

} // namespace spud::importer
