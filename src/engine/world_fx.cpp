#include "engine/world_fx.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/gtc/quaternion.hpp>

namespace spud {

namespace {

constexpr float k_reach_m = 150.0f;   // how near a reference's effect runs: about the loaded cells round the camera
constexpr float k_warm_seconds = 2.0f; // a new effect starts as if it had been running: the game's cells load far ahead
constexpr float k_warm_step = 1.0f / 30.0f;
constexpr uint32_t k_starts_per_frame = 2;

// The effect shader's technique bits for a particle system's material (meshes.cpp effect_technique, section 10).
uint32_t technique(const ose::EffectMaterial& m, bool greyscale) {
    const uint32_t f1 = m.skyrim_flags1, f2 = m.skyrim_flags2;
    uint32_t t = 0x1 | 0x42; // vertex colours: the particle's; a texture
    if (m.blend_dst == 0) t |= 0x400;
    if (m.blend_dst == 2) t |= 0x800;
    if (m.blend_src == 4 && m.blend_dst == 7) t |= 0x400000;
    if (f2 >> 30 & 1) t |= 0x10000;
    if (f1 >> 30 & 1) t |= 0x40000;
    if ((f1 >> 4 & 1) && greyscale) t |= 0x80000;
    if ((f1 >> 5 & 1) && greyscale) t |= 0x100000;
    return t;
}

// The game's remap.
void game_blend(uint32_t src, uint32_t dst, uint32_t& out_src, uint32_t& out_dst) {
    if ((src == 6 && dst == 0) || (src == 0 && dst == 0) || (src == 6 && dst == 9)) {
        out_src = 6, out_dst = 0;
    } else if ((src == 1 && dst == 2) || (src == 4 && dst == 1)) {
        out_src = 4, out_dst = 1;
    } else if (src == 4 && dst == 7) {
        out_src = 4, out_dst = 7;
    } else {
        out_src = 6, out_dst = 7;
    }
}

// A node's world transform after the instance's last update, in game units.
glm::mat4 node_matrix(const fx::EffectInstance& e, uint32_t node) {
    glm::mat4 m(e.node_world_rotation(node) * e.node_world_scale(node));
    m[3] = glm::vec4(e.node_world_translation_units(node), 1.0f);
    return m;
}

} // namespace

WorldFx::WorldFx(std::filesystem::path cache, TextureResolver textures)
    : m_cache(std::move(cache)), m_textures(std::move(textures)) {}

namespace {

std::shared_ptr<const fx::Effect> read_effect(const std::filesystem::path& file) {
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        try {
            auto e = fx::load_effect(read_file(file));
            if (!e->data.systems.empty() || !e->data.addon_nodes.empty() || !e->data.sequences.empty()) {
                return e;
            }
        } catch (const std::exception& e) {
            log::warn("{}: {}", file.string(), e.what());
        }
    }
    return nullptr;
}

// The longest stop - start of the model's time controllers, at least 0.1; none keeps `duration` ().
float temp_lifetime(const ose::Effect& e, float duration) {
    float longest = -1.0f;
    for (const ose::Controller& c : e.controllers) {
        longest = std::max(longest, c.stop_time - c.start_time);
    }
    return longest < 0.0f ? duration : std::max(longest, 0.1f);
}

} // namespace

std::shared_ptr<const fx::Effect> WorldFx::effect(const std::string& osm_path) {
    auto [it, fresh] = m_effects.try_emplace(osm_path);
    if (fresh) {
        it->second = read_effect(m_cache / path::replace_extension(osm_path, ".ose"));
    }
    return it->second;
}

// Off the frame: nothing that fires in play reads a file on it.
void WorldFx::load(const std::string& osm_path) {
    if (!m_effects.contains(osm_path) && !m_loading.contains(osm_path)) {
        m_loading.emplace(osm_path, std::async(std::launch::async, read_effect, m_cache / path::replace_extension(osm_path, ".ose")));
    }
}

bool WorldFx::has_effect(std::string_view model) {
    std::error_code ec;
    const std::string m(model);
    if (const auto it = m_effects.find(m); it != m_effects.end()) {
        return it->second != nullptr;
    }
    return std::filesystem::exists(m_cache / path::replace_extension(m, ".ose"), ec);
}

void WorldFx::set_world(const osw::World& world) {
    m_world = &world;
    m_world_edid = world.edid;
    m_emitters.clear();
    m_live.clear();
    m_one_shots.clear();
    m_requests.clear();
    m_tracked.clear();
    // A file per model: never on the frame. The emitters join when they are read, as everything else streams in.
    std::vector<std::string> meshes = world.meshes;
    m_world_effects = std::async(std::launch::async, [cache = m_cache, meshes = std::move(meshes)] {
        std::vector<std::shared_ptr<const fx::Effect>> effects(meshes.size());
        for (size_t i = 0; i < meshes.size(); ++i) {
            effects[i] = read_effect(cache / path::replace_extension(meshes[i], ".ose"));
        }
        return effects;
    });
}

void WorldFx::settle(const osw::World& world) {
    if (m_world == &world && m_world_edid == world.edid) {
        finish_world();
    } else if (m_world_effects.valid() && m_world_effects.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_world_effects = {}; // read for a world that has gone, which may be freed: update() starts on this one
    }
}

void WorldFx::finish_world() {
    if (!m_world_effects.valid() || m_world_effects.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    const auto effects = m_world_effects.get();
    const osw::World& world = *m_world;
    for (size_t i = 0; i < effects.size(); ++i) {
        m_effects.try_emplace(world.meshes[i], effects[i]);
    }
    for (uint32_t i = 0; i < world.refs.size(); ++i) {
        const osw::Ref& r = world.refs[i];
        if (r.mesh < effects.size() && effects[r.mesh] && !effects[r.mesh]->data.systems.empty()) {
            m_emitters.push_back({i, effects[r.mesh]});
        }
    }
    log::info("{}: {} references with particle systems", world.edid, m_emitters.size());
}

WorldFx::Live WorldFx::start(const std::shared_ptr<const fx::Effect>& e, uint32_t seed, const glm::mat3& rotation,
                             const glm::vec3& position, float scale) {
    Live l;
    l.instance = std::make_unique<fx::EffectInstance>(e, seed);
    l.scale = scale;
    const auto& d = l.instance->data();
    l.instance->set_root_transform(rotation, position, scale * (d.root != ose::k_none ? d.nodes[d.root].scale : 1.0f));
    for (const std::string& name : l.instance->sequence_names()) {
        l.instance->activate_sequence(name);
        break; // the default sequence: the first, as the controller manager's auto-play
    }
    return l;
}

// An effect that shows its shapes as well as its particles: the rest pose is taken at scale 1, so what moves or scales the root
// afterwards reaches the shapes through the nodes.
WorldFx::Live WorldFx::place(const std::shared_ptr<const fx::Effect>& e, const std::string& model, const glm::mat3& rotation,
                             const glm::vec3& position, float scale) {
    fx::EffectInstance rest(e, 1u);
    rest.set_root_transform(rotation, position, e->data.root != ose::k_none ? e->data.nodes[e->data.root].scale : 1.0f);
    rest.update(0.0f);
    Live l = start(e, uint32_t(m_one_shots.size() + m_tracked.size()) + 1u, rotation, position, scale);
    l.one_shot = true;
    l.model = model;
    l.root_rotation = rotation;
    l.root_position = position;
    for (uint32_t node : e->data.submesh_nodes) {
        l.rest_inverse.push_back(node == ose::k_none ? glm::mat4(1.0f) : glm::inverse(node_matrix(rest, node)));
    }
    return l;
}

void WorldFx::spawn(std::string_view model, const glm::vec3& position, float heading) {
    if (auto e = effect(std::string(model)); e && !e->data.systems.empty()) {
        const glm::mat3 r = glm::mat3_cast(glm::angleAxis(heading, glm::vec3(0.0f, 0.0f, 1.0f)));
        m_one_shots.push_back(place(e, std::string(model), r, position, 1.0f));
    }
}

void WorldFx::spawn_temp(std::string_view model, const glm::mat3& rotation, const glm::vec3& position, float scale, float duration) {
    const std::string m(model);
    load(m);
    m_requests.push_back({m, rotation, position, scale, duration, 0, false});
}

uint32_t WorldFx::track(std::string_view model, const glm::mat3& rotation, const glm::vec3& position, float scale) {
    const std::string m(model);
    load(m);
    const uint32_t id = m_next_tracked++;
    m_requests.push_back({m, rotation, position, scale, 0.0f, id, false});
    return id;
}

bool WorldFx::tracked_ready(uint32_t id) const {
    return std::ranges::none_of(m_requests, [id](const Request& r) { return r.tracked == id; });
}

void WorldFx::move(uint32_t id, const glm::mat3& rotation, const glm::vec3& position) {
    for (Request& r : m_requests) {
        if (r.tracked == id) {
            r.rotation = rotation;
            r.position = position;
        }
    }
    for (size_t i = 0; i < m_tracked.size(); ++i) {
        if (m_tracked[i].id == id) {
            const auto& d = m_tracked[i].live.instance->data();
            m_tracked[i].live.instance->set_root_transform(rotation, position, m_tracked[i].live.scale * (d.root != ose::k_none ? d.nodes[d.root].scale : 1.0f));
            const float scale = m_tracked[i].scale;
            for (const Child c : std::vector<Child>(m_tracked[i].children)) {
                move(c.id, rotation * c.rotation, position + rotation * c.position * scale);
            }
            return;
        }
    }
}

void WorldFx::hide(uint32_t id, bool hidden) {
    for (Request& r : m_requests) {
        if (r.tracked == id) {
            r.hidden = hidden;
        }
    }
    for (Tracked& t : m_tracked) {
        if (t.id == id) {
            t.hidden = hidden;
            for (const Child& c : std::vector<Child>(t.children)) {
                hide(c.id, hidden);
            }
            return;
        }
    }
}

void WorldFx::stop(uint32_t id) {
    std::erase_if(m_requests, [id](const Request& r) { return r.tracked == id; });
    const auto t = std::ranges::find(m_tracked, id, &Tracked::id);
    if (t != m_tracked.end()) {
        const std::vector<Child> children = std::move(t->children);
        m_tracked.erase(t);
        for (const Child& c : children) {
            stop(c.id);
        }
    }
}

// Each addon node asks for its ADDN's model: a temp effect emits for the node's life, a tracked one follows its parent.
void WorldFx::place_addons(const fx::Effect& e, const Request& r, std::vector<Child>* children) {
    for (const ose::AddonNode& a : e.data.addon_nodes) {
        const auto model = m_addons.find(a.index);
        if (model == m_addons.end()) {
            continue;
        }
        glm::mat3 node;
        std::copy_n(a.rotation, 9, &node[0][0]);
        const glm::vec3 at = glm::vec3(a.translation[0], a.translation[1], a.translation[2]) * a.scale * units::k_skyrim_unit_m;
        const glm::mat3 rotation = r.rotation * node;
        const glm::vec3 position = r.position + r.rotation * at * r.scale;
        if (children) {
            // Not track(): this runs inside the walk of m_requests.
            load(model->second);
            const uint32_t id = m_next_tracked++;
            m_addon_requests.push_back({model->second, rotation, position, r.scale * a.scale, 0.0f, id, false});
            // AddOnNode<index>: the sequences move it, as FireSprayFX01's movers do, and what it emits follows.
            // Repeats are AddOnNode34@#1 and so on, in file order, as the addon list has them.
            uint32_t on = ose::k_none;
            const std::string name = std::format("AddOnNode{}", a.index);
            size_t skip = uint32_t(std::ranges::count_if(*children, [&](const Child& c) { return c.addon == a.index; }));
            for (uint32_t i = 0; i < e.data.nodes.size() && on == ose::k_none; ++i) {
                const std::string_view n = e.data.string(e.data.nodes[i].name);
                if ((n == name || n.starts_with(name + "@#")) && skip-- == 0) {
                    on = i;
                }
            }
            children->push_back({id, node, at, on, a.index});
        } else {
            load(model->second);
            Request q{model->second, rotation, position, r.scale * a.scale, r.duration, 0, false};
            q.emit_for = a.lifetime >= 0.0f ? std::max(a.lifetime, 0.1f) : r.duration;
            m_addon_requests.push_back(std::move(q));
        }
    }
}

fx::EffectInstance* WorldFx::tracked(uint32_t id) {
    const auto t = std::ranges::find(m_tracked, id, &Tracked::id);
    return t != m_tracked.end() ? t->live.instance.get() : nullptr;
}

void WorldFx::start_requests() {
    for (auto it = m_loading.begin(); it != m_loading.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            m_effects[it->first] = it->second.get();
            it = m_loading.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(m_requests, [this](const Request& r) {
        const auto it = m_effects.find(r.model);
        if (it == m_effects.end()) {
            return false;
        }
        if (it->second) {
            Live l = place(it->second, r.model, r.rotation, r.position, r.scale);
            if (r.tracked) {
                std::vector<Child> children;
                place_addons(*it->second, r, &children);
                m_tracked.push_back({r.tracked, std::move(l), r.hidden, r.scale, std::move(children)});
            } else {
                place_addons(*it->second, r, nullptr);
                if (!it->second->data.systems.empty()) {
                    l.lifetime = temp_lifetime(it->second->data, r.duration);
                    l.emit_for = r.emit_for;
                    m_one_shots.push_back(std::move(l));
                }
            }
        }
        return true;
    });
    m_requests.insert(m_requests.end(), std::make_move_iterator(m_addon_requests.begin()), std::make_move_iterator(m_addon_requests.end()));
    m_addon_requests.clear();
}

float WorldFx::text_key_time(std::string_view model, std::string_view sequence, std::string_view key) {
    const auto e = effect(std::string(model));
    if (!e) {
        return 0.0f;
    }
    const ose::Effect& d = e->data;
    for (const ose::Sequence& s : d.sequences) {
        if (d.string(s.name) == sequence) {
            for (uint32_t i = 0; i < s.text_key_count; ++i) {
                const ose::TextKey& k = d.text_keys[s.first_text_key + i];
                if (d.string(k.text) == key) {
                    return k.time;
                }
            }
        }
    }
    return 0.0f;
}

std::string WorldFx::sequence_named(std::string_view model, std::string_view sequence) {
    if (const auto e = effect(std::string(model))) {
        for (const ose::Sequence& s : e->data.sequences) {
            if (path::to_lower(e->data.string(s.name)) == path::to_lower(sequence)) {
                return std::string(e->data.string(s.name));
            }
        }
    }
    return {};
}

float WorldFx::sequence_length(std::string_view model, std::string_view sequence) {
    const auto e = effect(std::string(model));
    if (!e) {
        return 0.0f;
    }
    for (const ose::Sequence& s : e->data.sequences) {
        if (e->data.string(s.name) == sequence) {
            return (s.stop_time - s.start_time) / std::max(s.frequency, 1e-3f);
        }
    }
    return 0.0f;
}

// A shape goes from its rest pose to where the effect's animation has it: the vertices are in the model's space, so
// the draw transform is now x rest^-1 x the root's.
void WorldFx::collect_shapes(const Live& l) {
    const auto& nodes = l.instance->data().submesh_nodes;
    glm::mat4 root(l.root_rotation);
    root[3] = glm::vec4(l.root_position / units::k_skyrim_unit_m, 1.0f);
    for (uint32_t s = 0; s < nodes.size(); ++s) {
        if (nodes[s] == ose::k_none) {
            continue;
        }
        glm::vec4 color(1.0f), uv(0.0f, 0.0f, 1.0f, 1.0f);
        bool animated = false; // a material the effect doesn't run stays as the model has it
        for (size_t k = 0; k < l.instance->material_count(); ++k) {
            const fx::MaterialState& m = l.instance->material(k);
            if (m.source && m.source->owner_kind == ose::MaterialOwner::mesh_shape && m.source->owner == s) {
                const bool palette = technique(*m.source, m.source->greyscale_texture != ose::k_none) & 0x80000u;
                color = glm::vec4(glm::vec3(m.base_color) * (palette ? 1.0f : m.base_color_scale), m.base_color.a);
                uv = glm::vec4(m.uv_offset, m.uv_scale);
                animated = true;
            }
        }
        const glm::mat4 draw = node_matrix(*l.instance, nodes[s]) * l.rest_inverse[s] * root;
        m_shapes.push_back({l.model, s, glm::mat3(draw), glm::vec3(draw[3]) * units::k_skyrim_unit_m, color, uv, animated});
    }
}

void WorldFx::collect(const fx::EffectInstance& instance, const render::View& view) {
    for (size_t s = 0; s < instance.system_count(); ++s) {
        const auto particles = instance.particles(s);
        if (particles.empty()) {
            continue;
        }
        const fx::MaterialState& state = instance.system_material(s);
        if (!state.source) {
            continue;
        }
        const ose::EffectMaterial& m = *state.source;
        render::GpuParticleSystem g{};
        auto [slots, fresh] = m_material_textures.try_emplace(&m);
        if (fresh) {
            const std::string_view texture = instance.data().string(m.source_texture);
            const std::string_view palette = instance.data().string(m.greyscale_texture);
            slots->second = glm::uvec2(texture.empty() ? 0u : m_textures(texture), palette.empty() ? 0u : m_textures(palette));
        }
        g.info = glm::uvec4(slots->second.x, technique(m, slots->second.y != 0u) & (m_soft ? ~0u : ~0x40000u), slots->second.y, 0u);
        // In palette colour the base's red picks the row and the scale, in params, multiplies the result.
        const float scale = g.info.y & 0x80000u ? 1.0f : state.base_color_scale;
        g.base_color = glm::vec4(glm::vec3(state.base_color) * scale, state.base_color.a);
        g.uv = glm::vec4(state.uv_offset, state.uv_scale);
        g.params = glm::vec4(m.soft_falloff_depth, state.base_color_scale, float(m.lighting_influence) / 255.0f, 0.0f);
        const uint32_t system = uint32_t(m_systems.size());
        m_systems.push_back(g);
        render::ParticleDraw draw{};
        game_blend(m.blend_src, m.blend_dst, draw.blend_src, draw.blend_dst);
        draw.first = uint32_t(m_particles.size());
        draw.count = uint32_t(particles.size());
        const fx::Bound b = instance.bound(s);
        draw.distance = glm::length(b.center - view.position);
        for (const fx::Particle& p : particles) {
            render::GpuParticle q{};
            q.position = glm::vec4(p.position, p.rotation);
            float system_bits;
            std::memcpy(&system_bits, &system, 4);
            q.size_system = glm::vec4(p.radius * p.size, system_bits, 0.0f, 0.0f);
            q.color = p.color;
            q.uv = glm::vec4(p.uv_rect.x, p.uv_rect.y, p.uv_rect.z, p.uv_rect.w);
            m_particles.push_back(q);
        }
        m_draws.push_back(draw);
    }
}

void WorldFx::update(const osw::World& world, const render::View& view, float dt) {
    // By name too: a new space can land at the old one's address.
    if (m_world != &world || m_world_edid != world.edid) {
        set_world(world);
    }
    {
        finish_world();
    }
    // Which references run: near the camera. A new one is warmed up the same way whatever the route here; a few a
    // frame while the world runs, so walking into a town costs no hitch. With the clock stopped, all at once.
    uint32_t starts = dt > 0.0f ? k_starts_per_frame : ~0u;
    for (uint32_t i = 0; i < m_emitters.size(); ++i) {
        const osw::Ref& r = world.refs[m_emitters[i].ref];
        const glm::vec3 p(r.position[0], r.position[1], r.position[2]);
        bool near = glm::distance(p, view.position) < k_reach_m;
        // What the game holds disabled has no 3D: the siege fires around Whiterun, and scripts turn such effects on and off.
        if (near) {
            near = !(ref_disabled ? ref_disabled(r.form_id) : (r.flags & osw::ref_initially_disabled) != 0);
        }
        auto it = m_live.find(i);
        if (near && it == m_live.end() && starts > 0) {
            --starts;
            const glm::quat q(r.rotation[3], r.rotation[0], r.rotation[1], r.rotation[2]);
            Live l = start(m_emitters[i].effect, r.form_id | 1u, glm::mat3_cast(q), p, r.scale);
            for (float t = k_warm_step; t <= k_warm_seconds; t += k_warm_step) {
                l.instance->update(t);
            }
            l.seconds = k_warm_seconds;
            m_live.emplace(i, std::move(l));
        } else if (!near && it != m_live.end()) {
            m_live.erase(it);
        }
    }
    m_particles.clear();
    m_systems.clear();
    m_draws.clear();
    m_shapes.clear();
    start_requests();
    for (auto& [index, l] : m_live) {
        if (dt > 0.0f) {
            l.seconds += dt;
            l.instance->update(l.seconds);
        }
        collect(*l.instance, view);
    }
    for (Live& l : m_one_shots) {
        l.seconds += dt;
        l.life += dt;
        if (l.emit_for >= 0.0f && l.life >= l.emit_for) {
            l.instance->set_density_scale(0.0f);
        }
        l.instance->update(l.seconds);
        collect(*l.instance, view);
        collect_shapes(l);
    }
    std::erase_if(m_one_shots, [](const Live& l) {
        if (l.emit_for < 0.0f) {
            return l.life > l.lifetime;
        }
        if (l.life < l.emit_for) {
            return false;
        }
        for (size_t s = 0; s < l.instance->system_count(); ++s) {
            if (l.instance->particle_count(s) != 0) {
                return false;
            }
        }
        return true;
    });
    for (Tracked& t : m_tracked) {
        t.live.seconds += dt;
        t.live.instance->update(t.live.seconds);
        if (!t.hidden) {
            collect(*t.live.instance, view);
            collect_shapes(t.live);
        }
    }
    for (size_t i = 0; i < m_tracked.size(); ++i) {
        for (const Child c : std::vector<Child>(m_tracked[i].children)) {
            if (c.node != ose::k_none) {
                const fx::EffectInstance& on = *m_tracked[i].live.instance;
                move(c.id, on.node_world_rotation(c.node), on.node_world_translation_units(c.node) * units::k_skyrim_unit_m);
            }
        }
    }
    // Back to front, as blended draws are; the same order however the map was filled.
    for (Batch& b : m_batches) {
        const auto texture = [this](const std::string& path) {
            if (path.empty()) {
                return 0u;
            }
            auto [it, fresh] = m_batch_textures.try_emplace(path, 0u);
            if (fresh) {
                it->second = m_textures(path);
            }
            return it->second;
        };
        render::GpuParticleSystem g{};
        const uint32_t palette = texture(b.palette);
        g.info = glm::uvec4(texture(b.texture), palette ? b.technique : b.technique & ~0x180000u, palette, 0u);
        // Palette colour: the particle's red picks the row, and the scale multiplies what it gives (the Particle shaders).
        const float scale = g.info.y & 0x80000u ? 1.0f : b.colour_scale;
        g.base_color = glm::vec4(glm::vec3(scale), 1.0f);
        g.uv = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        g.params = glm::vec4(0.0f, b.colour_scale, 0.0f, 0.0f);
        const uint32_t system = uint32_t(m_systems.size());
        m_systems.push_back(g);
        render::ParticleDraw draw{};
        game_blend(b.blend_src, b.blend_dst, draw.blend_src, draw.blend_dst);
        draw.first = uint32_t(m_particles.size());
        draw.count = uint32_t(b.particles.size());
        draw.distance = glm::length(b.centre - view.position);
        for (render::GpuParticle& p : b.particles) {
            std::memcpy(&p.size_system.y, &system, 4);
            m_particles.push_back(p);
        }
        m_draws.push_back(draw);
    }
    m_batches.clear();
    std::sort(m_draws.begin(), m_draws.end(), [](const render::ParticleDraw& a, const render::ParticleDraw& b) {
        return a.distance != b.distance ? a.distance > b.distance : a.first < b.first;
    });
    m_frame.particles = m_particles;
    m_frame.systems = m_systems;
    m_frame.draws = m_draws;
}

} // namespace spud
