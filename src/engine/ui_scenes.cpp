#include "engine/ui_scenes.hpp"

#include "core/units.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace spud::engine {

namespace {

constexpr uint32_t k_instances = 4096; // the starfield: ~500 draws, each re-placed at most once a frame

glm::vec3 metres(const float (&units)[3]) {
    return {units::to_metres(units[0]), units::to_metres(units[1]), units::to_metres(units[2])};
}

// A row-major NiMatrix3 (column vectors) as glm's column-major matrix.
glm::mat3 matrix(const float (&m)[9]) {
    glm::mat3 r;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            r[col][row] = m[row * 3 + col];
        }
    }
    return r;
}

// The renderer's view of an NiCamera: camera-relative, reverse-Z, infinite far plane, Vulkan clip space, with
// the Gamebryo frustum's slopes (asymmetric in general: x_ndc = (right/forward - (l + r)/2) / ((r - l)/2),
// likewise up).
render::View camera_view(const ui::SceneCamera& c) {
    const glm::mat3 r = matrix(c.rotation);
    const glm::vec3 forward = glm::normalize(r[0]);
    const glm::vec3 up = glm::normalize(r[1]);
    const float l = c.frustum[0], rt = c.frustum[1], t = c.frustum[2], b = c.frustum[3];
    const float near_m = units::to_metres(c.frustum[4]);
    render::View v;
    v.position = metres(c.position);
    v.near_plane = near_m;
    v.aspect = (rt - l) / std::max(t - b, 1e-6f);
    v.fov_y = 2.0f * std::atan((t - b) * 0.5f);
    v.rotation = glm::lookAtRH(glm::vec3(0), forward, up);
    glm::mat4 p(0.0f);
    p[0][0] = 2.0f / (rt - l);
    p[2][0] = (rt + l) / (rt - l);
    p[1][1] = -2.0f / (t - b);
    p[2][1] = -(t + b) / (t - b);
    p[2][3] = -1.0f;
    p[3][2] = near_m;
    v.projection = p;
    v.view_proj = p * v.rotation;
    v.frustum = render::Frustum::from_view_proj(v.view_proj);
    return v;
}

} // namespace

UiScenes::View::View(render::Device& device) : instances(device, k_instances) {
    // Nothing but the scene: no sky, no fog.
    environment.sky_zenith = environment.sky_horizon = environment.sky_ground = glm::vec3(0.0f);
    environment.sun_disc = 0.0f;
    environment.fog_max = 0.0f;
    environment.ambient_sky = environment.ambient_ground = glm::vec3(0.0f);
}

UiScenes::UiScenes(render::Renderer& renderer) : m_renderer(renderer) {
    // The slots the menus use (1 the item menus, 2 the Magic Menu, 3 the book, 6 the Stats Menu, 7 the lock),
    // made at start-up: their instance tables are device memory, which must not be allocated while frames
    // run.
    m_views.resize(8);
    for (int slot : {1, 2, 3, 6, 7}) {
        m_views[size_t(slot)] = std::make_unique<View>(renderer.device());
    }
}

UiScenes::~UiScenes() {
    release();
    m_renderer.drain(); // deferred frees refer to the instance tables
}

render::MeshId UiScenes::mesh(const std::string& path) {
    if (auto it = m_meshes.find(path); it != m_meshes.end()) {
        return it->second;
    }
    const render::MeshId id = m_renderer.meshes().acquire(path, 1.0f);
    m_meshes.emplace(path, id);
    return id;
}

uint32_t UiScenes::group_index(render::MeshId mesh, int submesh) const {
    const auto groups = m_renderer.meshes().groups(mesh);
    for (uint32_t k = 0; k < groups.size(); ++k) {
        if (int(m_renderer.meshes().group(groups[k]).submesh) == submesh) {
            return k;
        }
    }
    return ~0u;
}

void UiScenes::release() {
    for (auto& v : m_views) {
        if (!v) {
            continue;
        }
        std::vector<uint32_t> slots, materials;
        for (Draw& d : v->cache) {
            if (d.slot != ~0u) {
                slots.push_back(d.slot);
            }
            if (d.material != ~0u) {
                materials.push_back(d.material);
            }
        }
        v->cache.clear();
        v->draws.clear();
        if (!slots.empty() || !materials.empty()) {
            m_renderer.device().defer(
                [this, table = &v->instances, slots = std::move(slots), materials = std::move(materials)] {
                    for (uint32_t s : slots) {
                        table->free(s);
                    }
                    for (uint32_t m : materials) {
                        m_renderer.meshes().free_material(m);
                    }
                });
        }
    }
    for (auto& [path, id] : m_meshes) {
        m_renderer.meshes().release(id);
    }
    m_meshes.clear();
    m_inputs.clear();
}

void UiScenes::update(std::span<const ui::UiScene> scenes, ui::Ui& ui) {
    if (scenes.empty()) {
        if (!m_meshes.empty() || !m_inputs.empty()) {
            release();
        }
        return;
    }
    m_inputs.clear();
    for (const ui::UiScene& scene : scenes) {
        const size_t slot = size_t(std::clamp(scene.slot, 0, 7));
        if (m_views.size() <= slot) {
            m_views.resize(slot + 1);
        }
        if (!m_views[slot]) {
            m_views[slot] = std::make_unique<View>(m_renderer.device());
        }
        View& v = *m_views[slot];
        update_view(v, scene, ui);
        render::UiSceneInput in;
        in.view = &v.view;
        in.space.draws = &v.draws;
        in.space.instance_table = v.instances.address();
        in.space.environment = v.environment;
        m_inputs.push_back(in);
    }
}

void UiScenes::update_view(View& v, const ui::UiScene& scene, ui::Ui& ui) {
    v.view = camera_view(scene.camera);
    v.draws.clear();
    std::vector<uint32_t> free_slots, free_materials;
    if (v.cache.size() > scene.draws.size()) {
        for (size_t i = scene.draws.size(); i < v.cache.size(); ++i) {
            if (v.cache[i].slot != ~0u) {
                free_slots.push_back(v.cache[i].slot);
            }
            if (v.cache[i].material != ~0u) {
                free_materials.push_back(v.cache[i].material);
            }
        }
    }
    v.cache.resize(scene.draws.size());
    glm::vec3 lit_center(0.0f);
    bool lit_any = false;
    for (size_t i = 0; i < scene.draws.size(); ++i) {
        const ui::SceneDraw& sd = scene.draws[i];
        Draw& d = v.cache[i];
        const render::MeshId id =
            sd.model < scene.model_count ? mesh(scene.models[sd.model]) : render::k_invalid_mesh;
        if (id == render::k_invalid_mesh || !m_renderer.meshes().ready(id)) {
            continue; // still loading: the scene shows it when it is in
        }
        // The draw's instance: rows of [R * s | t] in metres, written into a fresh slot when it changes (a
        // slot a frame in flight may read is never rewritten).
        float t[12];
        for (int r = 0; r < 3; ++r) {
            t[r * 4 + 0] = sd.rotation[r * 3 + 0];
            t[r * 4 + 1] = sd.rotation[r * 3 + 1];
            t[r * 4 + 2] = sd.rotation[r * 3 + 2];
            t[r * 4 + 3] = units::to_metres(sd.translation[r]);
        }
        const bool moved = d.slot == ~0u || d.mesh != id || std::memcmp(t, d.transform, sizeof t) != 0;
        if (moved) {
            const uint32_t s = v.instances.allocate();
            if (s == ~0u && d.slot == ~0u) {
                continue;
            }
            if (s != ~0u) {
                const glm::vec4 local = m_renderer.meshes().sphere(id);
                render::GpuInstanceData g;
                for (int r = 0; r < 3; ++r) {
                    g.rows[r] = glm::vec4(t[r * 4 + 0], t[r * 4 + 1], t[r * 4 + 2], t[r * 4 + 3]);
                }
                const glm::mat3 m(glm::vec3(t[0], t[4], t[8]), glm::vec3(t[1], t[5], t[9]),
                                  glm::vec3(t[2], t[6], t[10]));
                const glm::vec3 c = m * glm::vec3(local) + glm::vec3(t[3], t[7], t[11]);
                const float scale = std::cbrt(std::abs(glm::determinant(m)));
                g.sphere = glm::vec4(c, local.w * scale);
                v.instances.write(s, g);
                if (d.slot != ~0u) {
                    free_slots.push_back(d.slot);
                }
                d.slot = s;
                std::memcpy(d.transform, t, sizeof t);
            }
        }
        // A shape of the model, and its material when the menu changes it.
        uint32_t group = render::k_all_groups;
        if (sd.submesh >= 0) {
            if (d.mesh != id || d.submesh != sd.submesh || d.group == ~0u) {
                d.group = group_index(id, sd.submesh);
            }
            group = d.group;
            if (group == ~0u) {
                d.mesh = id;
                d.submesh = sd.submesh;
                continue; // a shape the import dropped
            }
        }
        const uint32_t texture = sd.page >= 0 ? ui.page_texture(sd.page) : ~0u;
        const float effect[8] = {sd.base_color[0], sd.base_color[1], sd.base_color[2], sd.base_color[3],
                                 sd.uv_offset[0],  sd.uv_offset[1],  sd.uv_scale[0],   sd.uv_scale[1]};
        const bool overrides =
            group != render::k_all_groups && (sd.alpha >= 0.0f || texture != ~0u || sd.effect || sd.turned);
        if (!overrides) {
            if (d.material != ~0u) {
                free_materials.push_back(d.material);
                d.material = ~0u;
            }
        } else if (d.material == ~0u || d.alpha != sd.alpha || d.texture != texture || d.mesh != id ||
                   d.has_effect != sd.effect ||
                   (sd.effect && std::memcmp(d.effect, effect, sizeof effect) != 0)) {
            // A fresh slot whenever it changes (a slot a frame in flight may read is never rewritten).
            const uint32_t g = m_renderer.meshes().groups(id)[group];
            const uint32_t m = m_renderer.meshes().copy_material(g, [&](render::GpuMaterial& mat) {
                if (sd.turned) {
                    mat.flags &= ~render::k_material_billboard;
                }
                if (sd.effect && (mat.flags & render::k_material_effect_bit)) {
                    for (int c = 0; c < 4; ++c) {
                        mat.base_color[c] = sd.base_color[c];
                    }
                    for (int c = 0; c < 2; ++c) {
                        mat.uv[c] = sd.uv_offset[c]; // TexcoordOffset: offset xy, scale zw
                        mat.uv[2 + c] = sd.uv_scale[c];
                    }
                }
                if (sd.alpha >= 0.0f) {
                    mat.base_color[3] = sd.alpha; // The game base colour alpha
                }
                if ((sd.effect && (mat.flags & render::k_material_effect_bit)) || sd.alpha >= 0.0f) {
                    // The scene's values stand: the material's own controllers would replace them.
                    mat.uv_anim = ~0u;
                }
                if (texture != ~0u) {
                    // The game binds the render target in slot 0
                    mat.tex[render::gtex_diffuse] = texture;
                }
            });
            if (m != ~0u) {
                if (d.material != ~0u) {
                    free_materials.push_back(d.material);
                }
                d.material = m;
                d.alpha = sd.alpha;
                d.texture = texture;
                d.has_effect = sd.effect;
                std::memcpy(d.effect, effect, sizeof effect);
            }
        }
        d.mesh = id;
        d.submesh = sd.submesh;
        const glm::vec3 centre(t[3], t[7], t[11]);
        const glm::vec3 rel = centre - v.view.position;
        const float dist = sd.sort >= 0.0f ? sd.sort : glm::dot(rel, rel);
        if (group == render::k_all_groups) {
            v.draws.add(id, d.slot, dist);
        } else {
            v.draws.add_group(id, group, d.slot, dist,
                              d.material == ~0u ? render::k_group_material : d.material);
        }
        if (!lit_any) {
            lit_center = centre;
            lit_any = true;
        }
    }
    if (!free_slots.empty() || !free_materials.empty()) {
        m_renderer.device().defer([this, table = &v.instances, slots = std::move(free_slots),
                                   materials = std::move(free_materials)] {
            for (uint32_t s : slots) {
                table->free(s);
            }
            for (uint32_t m : materials) {
                m_renderer.meshes().free_material(m);
            }
        });
    }

    // Stand-in lighting until the point-light path lands (see the header): the slot's first light with a
    // radius as the directional light, attenuated as Skyrim's lighting shader attenuates point lights, 1 -
    // saturate(d / r)^2.
    v.environment.sun_color = glm::vec3(0.0f);
    v.environment.sun_dir = glm::vec3(0, 0, 1);
    // No lockpicking code writes the directional ambient, so the lock has the sky's.
    v.environment.ambient_sky = scene.slot == 7 ? m_world_ambient_sky : glm::vec3(0.0f);
    v.environment.ambient_ground = scene.slot == 7 ? m_world_ambient_ground : glm::vec3(0.0f);
    for (const ui::SceneLight& l : scene.lights) {
        if (l.radius <= 0.0f) {
            continue;
        }
        const glm::vec3 to_light = metres(l.position) - lit_center;
        const float dist = glm::length(to_light);
        const float a = 1.0f - std::pow(std::clamp(dist / units::to_metres(l.radius), 0.0f, 1.0f), 2.0f);
        v.environment.sun_dir = dist > 0.0f ? to_light / dist : glm::vec3(0, 0, 1);
        v.environment.sun_color = glm::vec3(l.color[0], l.color[1], l.color[2]) * l.dimmer * a;
        break;
    }
}

} // namespace spud::engine
