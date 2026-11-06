#include "app_internal.hpp"

#include "core/path.hpp"
#include "core/units.hpp"
#include "game/game.hpp"

#include <algorithm>
#include <cmath>

namespace spud::engine {

namespace {

constexpr uint32_t k_decals_per_frame = 100; // iMaxDecalsPerFrame:Display
constexpr float k_decal_lod = 800.0f;        // fDecalLOD0:Display, units from the player
constexpr float k_decal_lifetime = 30.0f;    // fDecalLifetime:Display
constexpr float k_decal_fade = 1.0f;         // then alpha 1 - (age - lifetime)
constexpr float k_min_cos_angle = 0.01f;
constexpr size_t k_cell_checks = 8;          // decals a frame asked whether their cell is still there
constexpr float k_splat_rise = 2.5f;         // the global strength, per second

glm::vec3 metres(const game::Units3& p) { return glm::vec3(p[0], p[1], p[2]) * units::k_skyrim_unit_m; }
glm::vec3 vector(const game::Units3& p) { return glm::vec3(p[0], p[1], p[2]); }

} // namespace

Vfx::Vfx(App& app) : m_app(app) {
    if (m_app.m_world_fx) {
        const forms::Database& db = m_app.m_game->db();
        std::unordered_map<uint32_t, std::string> addons;
        for (const forms::FormEntry* e : db.of_type(forms::sig("ADDN"))) {
            const forms::Form f(db, *e);
            const forms::Value* index = f.get("DATA");
            const forms::Value* model = f.get("MODL");
            if (index && model) {
                addons[uint32_t(index->integer)] = game::model_path(model->text);
            }
        }
        m_app.m_world_fx->set_addons(std::move(addons));
    }
}

float Vfx::setting(std::string_view name, float fallback) const { return m_app.m_game->settings().number(name, fallback); }
float Vfx::roll(float low, float high) { return low + (high - low) * float(m_random() >> 8) * (1.0f / 16777216.0f); }

// A model with particle systems runs in WorldFx; the rest are a mesh for the IPCT's duration.
void Vfx::temp_particle(std::string_view model, const game::Units3& at, const game::DecalAxes& frame, float scale, float duration) {
    const glm::mat3 rotation(vector(frame[0]), vector(frame[1]), vector(frame[2]));
    if (m_app.m_world_fx && m_app.m_world_fx->has_effect(model)) {
        m_app.m_world_fx->spawn_temp(model, rotation, metres(at), scale, duration);
    } else if (m_app.m_space && !model.empty()) {
        const uint32_t id = m_next_mesh++;
        m_app.m_space->spawn(std::string(model), metres(at), 0.0f, id);
        m_app.m_space->move_spawned(id, metres(at), rotation * scale);
        m_temp_meshes.push_back({id, duration});
    }
}

const Vfx::TextureSet& Vfx::texture_set(uint32_t form) {
    const auto [it, added] = m_texture_sets.try_emplace(form);
    if (added) {
        const forms::Database& db = m_app.m_game->db();
        if (const forms::FormEntry* e = db.find(form)) {
            const forms::Form f(db, *e);
            render::TextureManager& textures = m_app.m_renderer->textures();
            const auto acquire = [&](std::string_view field, render::TextureKind kind) -> uint32_t {
                const forms::Value* v = f.get(field);
                return v && !v->text.empty() ? textures.acquire(path::with_root(path::normalize(v->text), "textures"), kind, 1.0f) : 0u;
            };
            it->second.diffuse = acquire("TX00", render::TextureKind::color);
            it->second.normal = acquire("TX01", render::TextureKind::normal);
            it->second.glow = acquire("TX03", render::TextureKind::color);
            const forms::Value* flags = f.get("DNAM");
            it->second.no_specular = flags && (flags->integer & 1);
        }
    }
    return it->second;
}

void Vfx::add_skin_decal(const game::SkinDecalRequest& r) {
    ActorSet* actors = m_app.m_space ? m_app.m_space->actors() : nullptr;
    const glm::vec3 at = metres(r.position);
    const glm::vec3 player = m_app.m_player ? m_app.m_player->feet() : m_app.m_camera.position;
    if (!actors || setting("bSkinnedDecals", 1.0f) == 0.0f ||
        (!r.forced && (m_skin_decals_this_frame >= int(setting("iMaxSkinDecalsPerFrame", 25.0f)) ||
                       glm::distance(at, player) > k_decal_lod * units::k_skyrim_unit_m))) {
        return;
    }
    SkinDecalLook look;
    look.position = at;
    look.axis_x = vector(r.axes[0]);
    look.axis_y = vector(r.axes[1]);
    look.direction = vector(r.direction);
    look.size = r.size * units::k_skyrim_unit_m;
    look.texture = texture_set(r.impact->texture_set).diffuse;
    look.limit = uint32_t(setting("uMaxSkinDecalPerActor", 25.0f));
    look.lifetime = setting("fDecalLifetime", 30.0f);
    if (look.texture && actors->add_skin_decal(r.victim, look)) {
        ++m_skin_decals_this_frame;
    }
}

void Vfx::add_decal(const game::DecalRequest& r) {
    const game::ImpactRecord& i = *r.impact;
    const glm::vec3 at = metres(r.position);
    const glm::vec3 player = m_app.m_player ? m_app.m_player->feet() : m_app.m_camera.position;
    if (!m_app.m_space || m_decals_this_frame >= k_decals_per_frame ||
        glm::distance(at, player) > k_decal_lod * units::k_skyrim_unit_m) {
        return;
    }
    // Spacing against the decals on the same thing, newest first.
    const float size = std::max(r.width, r.height);
    for (auto d = m_decals.rbegin(); r.placement > 0.0f && d != m_decals.rend(); ++d) {
        const float reach = (size + d->width) * units::k_skyrim_unit_m;
        const glm::vec3 apart = d->position - at;
        if (d->ref == r.ref && glm::dot(apart, apart) <= reach * reach / r.placement) {
            return;
        }
    }
    Decal d;
    d.ref = r.ref;
    d.textures = texture_set(i.texture_set);
    d.position = at;
    for (int k = 0; k < 3; ++k) {
        d.axes[k] = vector(r.axes[k]);
    }
    d.half_width = r.width * 0.5f * units::k_skyrim_unit_m;
    d.half_height = r.height * 0.5f * units::k_skyrim_unit_m;
    d.depth = r.depth * units::k_skyrim_unit_m;
    d.width = r.width;
    d.cos_angle = std::max(std::cos(glm::radians(i.angle)), k_min_cos_angle);
    d.shininess = i.shininess;
    d.color = glm::vec3(i.color[0], i.color[1], i.color[2]);
    d.flags = (i.decal_flags & game::decal_alpha_blending ? render::k_decal_alpha_blend : 0u) |
              (i.decal_flags & game::decal_alpha_testing ? render::k_decal_alpha_test : 0u) |
              (!d.textures.no_specular && i.shininess > 1.0f ? render::k_decal_specular : 0u);
    if (r.subtexture < 0) {
        d.uv_scale = 0.5f; // doubled: the whole texture
    } else {
        d.uv_offset = glm::vec2(r.subtexture & 1 ? 0.5f : 0.0f, r.subtexture & 2 ? 0.5f : 0.0f);
    }
    uint32_t list[4];
    m_app.m_space->light_list(at, std::max({d.half_width, d.half_height, d.depth}), list);
    d.lights = glm::uvec4(list[0], list[1], list[2], list[3]);
    m_decals.push_back(d);
    ++m_decals_this_frame;
    // Past the cap the oldest goes to the process list, and only then ages (7).
    if (m_decals.size() > m_app.m_max_decals) {
        m_aging.push_back(m_decals.front());
        m_decals.pop_front();
    }
}

void Vfx::put(const Decal& d, const render::View& view, float fade) {
    const glm::vec3 rel = d.position - view.position;
    const float radius = std::sqrt(d.half_width * d.half_width + d.half_height * d.half_height + d.depth * d.depth);
    if (m_app.m_renderer->textures().view(d.textures.diffuse) == VK_NULL_HANDLE || !view.frustum.sphere(rel, radius)) {
        return; // not yet streamed in, or out of sight
    }
    render::GpuDecal g{};
    g.position = glm::vec4(rel, d.cos_angle);
    const float half[3] = {d.half_width, d.half_height, d.depth};
    for (int k = 0; k < 3; ++k) {
        g.axes[k] = glm::vec4(d.axes[k] * half[k], 1.0f / (half[k] * half[k]));
    }
    g.color = glm::vec4(d.color, fade);
    g.textures = glm::uvec4(d.textures.diffuse, d.textures.normal, d.textures.glow, d.flags);
    g.lights = d.lights;
    g.params = glm::vec4(d.shininess, d.uv_scale, d.uv_offset);
    m_gpu_decals.push_back(g);
}

void Vfx::update_decals(const render::View& view, float dt) {
    // Cell unload clears them [I: base slot].
    if (m_app.m_physics && !m_app.m_space->interior()) {
        for (size_t i = 0; i < k_cell_checks && !m_decals.empty(); ++i) {
            m_cell_check = (m_cell_check + 1) % m_decals.size();
            if (!m_app.m_physics->cell_loaded(m_decals[m_cell_check].position)) {
                m_decals.erase(m_decals.begin() + std::ptrdiff_t(m_cell_check));
            }
        }
    }
    for (Decal& d : m_aging) {
        d.age += dt;
    }
    std::erase_if(m_aging, [](const Decal& d) { return d.age >= k_decal_lifetime + k_decal_fade; });
    m_gpu_decals.clear();
    for (const Decal& d : m_aging) {
        put(d, view, std::clamp(1.0f - (d.age - k_decal_lifetime), 0.0f, 1.0f));
    }
    for (const Decal& d : m_decals) {
        put(d, view, 1.0f);
    }
}

const Vfx::LightBase& Vfx::light_base(uint32_t form) {
    const auto [it, added] = m_light_bases.try_emplace(form);
    if (added) {
        const forms::Database& db = m_app.m_game->db();
        if (const forms::FormEntry* e = db.find(form)) {
            const forms::Form f(db, *e);
            const auto number = [&](std::string_view p) {
                const forms::Value* v = f.get(p);
                return !v ? 0.0f : v->kind == forms::Value::Kind::real ? float(v->real) : float(v->integer);
            };
            it->second.radius = units::to_metres(number("DATA.radius"));
            it->second.color = glm::vec3(number("DATA.color.r"), number("DATA.color.g"), number("DATA.color.b")) / 255.0f * number("FNAM");
        }
    }
    return it->second;
}

void Vfx::set_light(uint32_t id, uint32_t light, const game::Units3& at, float alpha) {
    const LightBase& base = light_base(light);
    m_lights[id] = {metres(at), base.color, base.radius, std::max(alpha, 0.0f)};
}

void Vfx::remove_light(uint32_t id) { m_lights.erase(id); }

void Vfx::follow_light(uint32_t ref, uint32_t light) {
    if (light) {
        m_followers[ref] = light;
    } else {
        m_followers.erase(ref);
        remove_light(ref);
    }
}

// The slots go to what is nearest the camera.
void Vfx::update_lights(const render::View& view) {
    // The game hangs the light on the hit art's AttachLight node; an art with no such node here leaves it at the head.
    ActorSet* actors = m_app.m_space ? m_app.m_space->actors() : nullptr;
    for (const auto& [ref, light] : m_followers) {
        std::optional<glm::vec3> at;
        if (const auto attached = m_attached_lights.find(ref); attached != m_attached_lights.end()) {
            set_light(ref, light, {attached->second.x, attached->second.y, attached->second.z}, 1.0f);
            continue;
        }
        if (ref == game::k_player_ref && m_app.m_player) {
            at = m_app.m_player->camera();
        } else if (actors) {
            at = actors->head_position(ref);
        }
        if (at) {
            const glm::vec3 u = *at / units::k_skyrim_unit_m;
            set_light(ref, light, {u.x, u.y, u.z}, 1.0f);
        }
    }
    std::vector<const Light*> near;
    for (const auto& [id, l] : m_lights) {
        near.push_back(&l);
    }
    std::ranges::sort(near, [&](const Light* a, const Light* b) {
        const float da = glm::distance(a->position, view.position), db = glm::distance(b->position, view.position);
        return da != db ? da < db : a->radius > b->radius;
    });
    m_attached_lights.clear();
    std::array<render::PointLight, render::k_moving_lights> out{};
    const size_t count = std::min(near.size(), out.size());
    for (size_t i = 0; i < count; ++i) {
        out[i].position = near[i]->position;
        out[i].radius = near[i]->radius;
        out[i].color = near[i]->color * near[i]->alpha;
        out[i].flags = render::point_light_on;
    }
    m_app.m_space->set_moving_lights(std::span(out).first(count));
}

void Vfx::screen_blood(int count) {
    if (setting("bBloodSplatterEnabled", 1.0f) == 0.0f) {
        return;
    }
    if (!m_splat_textures[0]) {
        render::TextureManager& textures = m_app.m_renderer->textures();
        m_splat_textures[0] = textures.acquire("textures/blood/screenbloodalpha01opt.dds", render::TextureKind::color, 1.0f);
        m_splat_textures[1] = textures.acquire("textures/blood/screenbloodcolor01opt.dds", render::TextureKind::color, 1.0f);
        m_splat_textures[2] = textures.acquire("textures/blood/fxbloodflare.dds", render::TextureKind::color, 1.0f);
    }
    int live = 0;
    for (const SplatCall& c : m_splat_calls) {
        live += int(c.splats.size());
    }
    count = std::min(count, int(setting("iBloodSplatterMaxCount", 10.0f)) - live);
    if (count <= 0) {
        return;
    }
    SplatCall call;
    const float aspect = m_app.aspect();
    for (int i = 0; i < count; ++i) {
        const glm::vec2 at(roll(0.0f, 1.0f), roll(0.0f, 1.0f));
        const float half = roll(setting("fBloodSplatterMinSize", 0.025f), setting("fBloodSplatterMaxSize", 0.125f));
        float opacity = roll(setting("fBloodSplatterMinOpacity", 0.1f), setting("fBloodSplatterMaxOpacity", 1.0f));
        if (roll(0.0f, 1.0f) <= setting("fBloodSplatterOpacityChance", 0.8f)) {
            opacity = roll(setting("fBloodSplatterMinOpacity2", 0.1f), setting("fBloodSplatterMaxOpacity2", 1.0f));
        }
        const float flare = setting("fBloodSplatterFlareSize", 0.065f);
        Splat s;
        s.look.rect = glm::vec4(at, half / aspect, half);
        s.look.flare = glm::vec4(at, flare / aspect, flare);
        s.look.uv = glm::vec4(m_random() & 1 ? 0.5f : 0.0f, m_random() & 1 ? 0.5f : 0.0f, 0.0f, 0.0f);
        s.opacity = opacity;
        call.splats.push_back(s);
    }
    m_splat_calls.push_back(std::move(call));
}

void Vfx::clear_screen_blood() {
    m_splat_calls.clear();
    m_gpu_splats.clear();
    m_splat_strength = 0;
}

void Vfx::update_screen_blood(float dt) {
    m_gpu_splats.clear();
    const float duration = setting("fBloodSplatterDuration", 5.0f), fade_start = setting("fBloodSplatterFadeStart", 0.8f);
    for (SplatCall& c : m_splat_calls) {
        c.seconds += dt;
    }
    std::erase_if(m_splat_calls, [&](const SplatCall& c) { return c.seconds >= duration; });
    if (m_splat_calls.empty()) {
        m_splat_strength = 0;
        return;
    }
    m_splat_strength = std::min(m_splat_strength + k_splat_rise * dt, 1.0f);
    const float flare_mult = setting("fBloodSplatterFlareMult", 1.0f);
    for (const SplatCall& c : m_splat_calls) {
        const float t = c.seconds / duration;
        const float alpha = (t < fade_start ? 1.0f : (1.0f - t) / (1.0f - fade_start)) * m_splat_strength;
        for (const Splat& s : c.splats) {
            render::GpuSplat g = s.look;
            g.uv.z = s.opacity * alpha;
            g.uv.w = alpha * flare_mult;
            m_gpu_splats.push_back(g);
        }
    }
}

void Vfx::clear() {
    m_decals.clear();
    m_aging.clear();
    m_lights.clear();
    for (const TempMesh& t : m_temp_meshes) {
        m_app.m_space->despawn(t.id);
    }
    m_temp_meshes.clear();
}

void Vfx::modifier(uint32_t imad, float strength, bool unique) {
    const forms::Database& db = m_app.m_game->db();
    const forms::FormEntry* e = db.find(imad);
    if (!e || !m_app.m_sky) {
        return; // 0x166, the explosion's flash, isn't in the load order
    }
    const std::string_view edid = db.editor_id(*e);
    if (unique) {
        m_app.m_sky->apply_modifier_unique(edid, strength);
    } else {
        m_app.m_sky->apply_modifier(edid, strength);
    }
}

void Vfx::update(const render::View& view, float dt) {
    if (const void* world = &m_app.m_space->world(); world != m_world) {
        m_world = world;
        clear();
    }
    m_decals_this_frame = 0;
    m_skin_decals_this_frame = 0;
    std::erase_if(m_temp_meshes, [&](TempMesh& t) {
        t.left -= dt;
        if (t.left > 0.0f) {
            return false;
        }
        m_app.m_space->despawn(t.id);
        return true;
    });
    update_decals(view, dt);
    update_lights(view);
    update_screen_blood(dt);
}

} // namespace spud::engine
