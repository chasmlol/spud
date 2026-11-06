// The world side of projectiles: sweeps over physics::World with actors as upright cylinders
// until bodies have bones, a spawned model each, sounds and impact effects.

#include "app_internal.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/units.hpp"
#include "game/game.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace spud::engine {

namespace {

constexpr uint8_t k_layer_biped = 8, k_layer_ground = 17, k_layer_dead_biped = 32;
constexpr uint64_t k_actor_layers = uint64_t(1) << 8 | uint64_t(1) << 30 | uint64_t(1) << 32 | uint64_t(1) << 33;
constexpr float k_min_radius_m = 0.001f;

using game::Units3;

glm::vec3 metres(const Units3& p) { return glm::vec3(p[0], p[1], p[2]) * units::k_skyrim_unit_m; }
Units3 game_units(const glm::vec3& m) {
    const glm::vec3 u = m / units::k_skyrim_unit_m;
    return {u.x, u.y, u.z};
}

// The heading about z, then the pitch about the reference's x; positive pitch looks down.
glm::mat3 rotation_of(float heading, float pitch, float scale) {
    const float ch = std::cos(heading), sh = std::sin(heading), cp = std::cos(pitch), sp = std::sin(pitch);
    const glm::mat3 z(glm::vec3(ch, -sh, 0), glm::vec3(sh, ch, 0), glm::vec3(0, 0, 1));
    const glm::mat3 x(glm::vec3(1, 0, 0), glm::vec3(0, cp, -sp), glm::vec3(0, sp, cp));
    return z * x * scale;
}

// Where a segment first enters an upright cylinder, as a fraction of it.
std::optional<float> enter_cylinder(const Units3& a, const Units3& b, const Units3& foot, float radius, float height) {
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    const float ox = a[0] - foot[0], oy = a[1] - foot[1];
    float enter = 0.0f, leave = 1.0f;
    const float qa = dx * dx + dy * dy, qb = 2.0f * (ox * dx + oy * dy), qc = ox * ox + oy * oy - radius * radius;
    if (qa > 0.0f) {
        const float disc = qb * qb - 4.0f * qa * qc;
        if (disc < 0.0f) {
            return std::nullopt;
        }
        const float root = std::sqrt(disc);
        enter = std::max(enter, (-qb - root) / (2.0f * qa));
        leave = std::min(leave, (-qb + root) / (2.0f * qa));
    } else if (qc > 0.0f) {
        return std::nullopt;
    }
    if (std::abs(dz) > 0.0f) {
        const float t0 = (foot[2] - a[2]) / dz, t1 = (foot[2] + height - a[2]) / dz;
        enter = std::max(enter, std::min(t0, t1));
        leave = std::min(leave, std::max(t0, t1));
    } else if (a[2] < foot[2] || a[2] > foot[2] + height) {
        return std::nullopt;
    }
    return enter <= leave ? std::optional(enter) : std::nullopt;
}

} // namespace

class EngineProjectiles final : public game::ProjectileWorld {
public:
    explicit EngineProjectiles(App& app) : m_app(app) {}

    std::vector<game::ProjectileContact> sweep(const Units3& a, const Units3& b, float radius, uint64_t layers) override {
        std::vector<game::ProjectileContact> out;
        const float r = std::max(radius * units::k_skyrim_unit_m, k_min_radius_m);
        if (m_app.m_physics) {
            for (const physics::World::SweepContact& c : m_app.m_physics->sphere_sweep(metres(a), metres(b), r, layers)) {
                game::ProjectileContact o = contact(c);
                o.position = game_units(c.point);
                o.normal = {c.normal.x, c.normal.y, c.normal.z};
                o.fraction = c.fraction;
                out.push_back(o);
            }
        }
        if (layers & k_actor_layers) {
            cylinders(a, b, radius, out);
        }
        std::ranges::sort(out, {}, &game::ProjectileContact::fraction);
        return out;
    }

    std::vector<game::ProjectileContact> pick(const Units3& a, const Units3& b, uint64_t layers) override {
        return sweep(a, b, 0.0f, layers);
    }

    std::vector<game::ProjectileContact> overlap(const Units3& centre, float radius, uint64_t layers) override {
        std::vector<game::ProjectileContact> out;
        if (m_app.m_physics) {
            const float r = radius * units::k_skyrim_unit_m;
            for (const physics::World::SweepContact& c : m_app.m_physics->sphere_overlap(metres(centre), r, layers)) {
                game::ProjectileContact o = contact(c);
                o.position = game_units(c.point);
                out.push_back(o);
            }
        }
        game::CombatWorld& combat = m_app.m_game->combat().world();
        for (uint32_t ref : everyone()) {
            game::CombatBody body;
            if (combat.body(ref, body) && actor_near(body, centre, radius)) {
                game::ProjectileContact o;
                o.ref = ref;
                o.actor = true;
                o.position = body.position;
                o.layer = actor_layer(ref);
                out.push_back(o);
            }
        }
        return out;
    }

    uint64_t layer_row(uint8_t layer) override {
        const osr::CollisionLayer* row = m_app.m_rules ? m_app.m_rules->collision_layer(layer) : nullptr;
        return row ? row->collides : ~uint64_t(0);
    }

    bool body(uint32_t ref, game::ProjectileBody& out) override {
        game::CombatBody body;
        if (m_app.m_game->combat().world().body(ref, body)) {
            out.position = body.position;
            out.radius = body.radius;
            out.height = body.height;
            if (ref == game::k_player_ref && m_app.m_player) {
                out.velocity = game_units(m_app.m_player->state().velocity);
            }
            return true;
        }
        if (const auto at = m_app.m_physics ? m_app.m_physics->ref_position(ref) : std::nullopt) {
            out.position = game_units(*at);
            return true;
        }
        return false;
    }

    bool water_height(const Units3& p, float& out) override {
        if (!m_app.m_space) {
            return false;
        }
        const osw::World& w = m_app.m_space->world();
        const glm::vec3 at = metres(p);
        const int cx = int(std::floor(at.x / w.cell_size)), cy = int(std::floor(at.y / w.cell_size));
        const auto it = std::ranges::lower_bound(w.cells, std::pair(cy, cx), {},
                                                 [](const osw::Cell& c) { return std::pair(c.y, c.x); });
        if (it == w.cells.end() || it->x != cx || it->y != cy || !(it->flags & osw::cell_has_water)) {
            return false;
        }
        out = it->water_height / units::k_skyrim_unit_m;
        return true;
    }

    uint32_t space() override { return m_app.player_space(); }
    bool exterior() override { return m_app.m_space && !m_app.m_space->interior(); }
    // An interior is one cell, whatever squares its references happen to fill.
    bool loaded(const Units3& p) override {
        return !m_app.m_physics || !exterior() || m_app.m_physics->cell_loaded(metres(p));
    }
    bool present(uint32_t ref) override { return m_app.m_physics && m_app.m_physics->ref_position(ref).has_value(); }

    // The first-person origin is the view's; third person and NPCs take the body's fallback of 3.5 when no node answers.
    bool aim(uint32_t shooter, bool, game::ShotAim& out) override {
        if (shooter == game::k_player_ref) {
            const bool first = !m_app.m_player || m_app.m_player->view() == View::first_person;
            out.origin = game_units(m_app.m_camera.position);
            out.heading = m_app.m_camera.yaw;
            out.pitch = -m_app.m_camera.pitch;
            out.node = first;
            out.first_person = first;
            out.third_person = !first;
            return true;
        }
        const game::Agent* a = m_app.m_game->ai().agent(shooter);
        if (a) {
            out.heading = a->heading;
        }
        return a != nullptr;
    }

    bool magic_node(uint32_t shooter, int source, Units3& out) override {
        glm::vec3 at;
        if (m_app.magic_node(shooter, uint8_t(source), at)) {
            out = game_units(at);
            return true;
        }
        if (shooter != game::k_player_ref || (m_app.m_player && m_app.m_player->view() != View::first_person)) {
            return false;
        }
        out = game_units(m_app.m_camera.position);
        return true;
    }

    bool combat_aim(uint32_t shooter, const game::Item*, Units3& out) override {
        game::Combat& combat = m_app.m_game->combat();
        const uint32_t target = combat.fighter(shooter).combat_target;
        game::CombatBody b;
        if (!target || !combat.world().body(target, b)) {
            return false;
        }
        out = {b.position[0], b.position[1], b.position[2] + b.height * 0.5f};
        return true;
    }

    bool on_screen(const Units3& p, float& x, float& y) override {
        const FreeCamera& camera = m_app.m_camera;
        const glm::vec3 d = metres(p) - camera.position;
        const glm::vec3 forward = camera.forward();
        const float z = glm::dot(d, forward);
        if (z <= 0.0f) {
            return false;
        }
        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 0, 1)));
        const glm::vec3 up = glm::cross(right, forward);
        const float half = std::tan(camera.fov_y * 0.5f);
        x = glm::dot(d, right) / z / (2.0f * half * m_app.aspect());
        y = glm::dot(d, up) / z / (2.0f * half);
        return true;
    }

    void shake_camera(const Units3& at, float strength) override {
        m_app.shake_camera(glm::vec3(at[0], at[1], at[2]), strength, 0.0f);
    }

    std::vector<uint32_t> actors() override {
        std::vector<uint32_t> out = m_app.m_game->combat().world().high_actors();
        std::erase(out, game::k_player_ref);
        return out;
    }

    // A model with particle systems runs as a tracked effect, which draws its shapes too; the rest are meshes.
    void create_visual(uint32_t id, std::string_view model, const Units3& at, float heading, float pitch,
                       float scale) override {
        if (model.empty()) {
            return;
        }
        if (m_app.m_world_fx && m_app.m_world_fx->has_effect(model)) {
            m_tracked[id] = m_app.m_world_fx->track(model, rotation_of(heading, pitch, 1.0f), metres(at), scale);
        } else if (m_app.m_space) {
            m_app.m_space->spawn(std::string(model), metres(at), heading, id);
            m_app.m_space->move_spawned(id, metres(at), rotation_of(heading, pitch, scale));
            m_scales[id] = scale;
        }
    }
    bool visual_ready(uint32_t id) override {
        if (const auto it = m_tracked.find(id); it != m_tracked.end()) {
            return m_app.m_world_fx->tracked_ready(it->second);
        }
        return !m_app.m_space || m_app.m_space->spawned_ready(id);
    }
    void flame_length(uint32_t id, float fraction) override { m_flames[id].length = fraction; }
    // SprayRangeBehavior: mSprayStart to its end, then mSprayLong and mSprayShort together, each sequence's weight the
    // fFlameProjectileLength or one minus it (BMF_PERCENT, BMF_ONE_MINUS_PERCENT).
    void drive_flame(uint32_t id, uint32_t tracked) {
        const auto it = m_flames.find(id);
        fx::EffectInstance* fx = it != m_flames.end() ? m_app.m_world_fx->tracked(tracked) : nullptr;
        if (!fx) {
            return;
        }
        Flame& f = it->second;
        if (f.stage == 0) {
            for (const std::string& s : fx->sequence_names()) {
                fx->deactivate_sequence(s);
            }
            fx->activate_sequence("mSprayStart");
            f.stage = 1;
        } else if (f.stage == 1 && (fx->sequence_time("mSprayStart") < 0.0f ||
                                    fx->sequence_time("mSprayStart") >= fx->sequence_duration("mSprayStart"))) {
            fx->deactivate_sequence("mSprayStart");
            fx->activate_sequence("mSprayLong", f.length);
            fx->activate_sequence("mSprayShort", 1.0f - f.length);
            f.stage = 2;
        } else if (f.stage == 2) {
            fx->set_sequence_weight("mSprayLong", f.length);
            fx->set_sequence_weight("mSprayShort", 1.0f - f.length);
        }
    }
    void move_visual(uint32_t id, const Units3& at, float heading, float pitch) override {
        if (const auto it = m_tracked.find(id); it != m_tracked.end()) {
            m_app.m_world_fx->move(it->second, rotation_of(heading, pitch, 1.0f), metres(at));
            drive_flame(id, it->second);
        } else if (const auto it = m_scales.find(id); m_app.m_space && it != m_scales.end()) {
            m_app.m_space->move_spawned(id, metres(at), rotation_of(heading, pitch, it->second));
        }
    }
    void hide_visual(uint32_t id, bool hidden) override {
        if (const auto it = m_tracked.find(id); it != m_tracked.end()) {
            m_app.m_world_fx->hide(it->second, hidden);
        } else if (m_app.m_space) {
            m_app.m_space->set_spawned_hidden(id, hidden);
        }
    }
    void destroy_visual(uint32_t id) override {
        if (const auto it = m_tracked.find(id); it != m_tracked.end()) {
            m_app.m_world_fx->stop(it->second);
            m_tracked.erase(it);
            m_flames.erase(id);
        } else if (m_app.m_space) {
            m_app.m_space->despawn(id);
        }
        m_scales.erase(id);
    }
    void set_light(uint32_t id, uint32_t light, const Units3& at, float alpha) override { m_app.m_vfx->set_light(id, light, at, alpha); }
    void remove_light(uint32_t id) override { m_app.m_vfx->remove_light(id); }
    // The "Hit" key of the model's SpecialIdle_AreaEffect, and that sequence's length.
    float hit_time(std::string_view model) override {
        return m_app.m_world_fx ? m_app.m_world_fx->text_key_time(model, "SpecialIdle_AreaEffect", "Hit") : 0.0f;
    }
    float sequence_length(std::string_view model) override {
        return m_app.m_world_fx ? m_app.m_world_fx->sequence_length(model, "SpecialIdle_AreaEffect") : 0.0f;
    }

    uint32_t play_sound(uint32_t form, const Units3* at) override {
        audio::AudioSystem* audio = m_app.m_audio.get();
        const auto descriptor = audio && form ? audio->find_descriptor(std::format("{:08X}", form)) : std::nullopt;
        if (!descriptor) {
            return 0;
        }
        return audio->play(*descriptor, at ? std::optional(metres(*at)) : std::nullopt);
    }
    void move_sound(uint32_t sound, const Units3& at) override {
        if (m_app.m_audio) {
            m_app.m_audio->set_position(sound, metres(at));
        }
    }
    void stop_sound(uint32_t sound) override {
        if (m_app.m_audio) {
            m_app.m_audio->stop(sound);
        }
    }

    float camera_distance(const Units3& at) override { return glm::distance(metres(at), m_app.m_camera.position) / units::k_skyrim_unit_m; }
    void temp_particle(std::string_view model, const Units3& at, const game::DecalAxes& frame, float scale, float lifetime) override {
        m_app.m_vfx->temp_particle(model, at, frame, scale, lifetime);
    }
    void add_decal(const game::DecalRequest& d) override { m_app.m_vfx->add_decal(d); }
    void skin_decal(const game::SkinDecalRequest& d) override { m_app.m_vfx->add_skin_decal(d); }
    void screen_blood(int count) override { m_app.m_vfx->screen_blood(count); }
    void weapon_blood(uint32_t attacker, float amount) override {
        if (ActorSet* actors = m_app.m_space ? m_app.m_space->actors() : nullptr) {
            const auto setting = [this](const char* name, float fallback) { return m_app.m_game->settings().number(name, fallback); };
            actors->add_weapon_blood(attacker, {amount, setting("fWeaponBloodLifetime", 10.0f), setting("fWeaponBloodFadeDuration", 2.0f),
                                                setting("fWeaponBloodFalloffThreshold", 0.3f),
                                                setting("fWeaponBloodAlphaToRGBScale", 0.85f)});
        }
    }
    void flash(uint32_t imad, float strength) override { m_app.m_vfx->modifier(imad, strength, false); }
    // Effects\\ExplosionSplash.NIF (sExplosionSplashParticles), at the water's height with a random spin.
    void ripple(const Units3& at, float) override {
        if (m_app.m_water_system) {
            m_app.m_water_system->add_ripple(glm::vec2(at[0], at[1]));
        }
    }
    void splash(const Units3& at, float scale) override {
        ripple(at, 0.01f);
        m_app.m_vfx->temp_particle("meshes/effects/explosionsplash.osm", at, {Units3{1, 0, 0}, Units3{0, 1, 0}, Units3{0, 0, 1}}, scale, 0.25f);
    }
    void muzzle_flash(const game::ProjectileRecord& r, const Units3& at) override {
        m_app.m_vfx->temp_particle(r.muzzle_model, at, {Units3{1, 0, 0}, Units3{0, 1, 0}, Units3{0, 0, 1}}, 1.0f, r.muzzle_duration);
    }

    void push(uint32_t body, const Units3& linear, const Units3& angular, const Units3& at) override {
        if (m_app.m_physics) {
            m_app.m_physics->push_body(body, glm::vec3(linear[0], linear[1], linear[2]),
                                       glm::vec3(angular[0], angular[1], angular[2]), metres(at));
        }
    }

    void knock(uint32_t actor, const Units3& from) override { m_app.m_game->combat().world().knockdown(actor, from, 0.0f); }

    void place_object(uint32_t base, const Units3& at, float heading, uint32_t) override {
        const game::Item* item = m_app.m_game->items().find(base);
        if (!item || !m_app.m_space) {
            return;
        }
        game::Stack stack;
        stack.form = base;
        stack.count = 1;
        const glm::vec3 m = metres(at);
        m_app.m_space->spawn(item->model, m, heading, m_app.m_game->place(stack, {m.x, m.y, m.z}));
    }

private:
    std::vector<uint32_t> everyone() {
        std::vector<uint32_t> out = actors();
        if (m_app.m_player) {
            out.push_back(game::k_player_ref);
        }
        return out;
    }

    uint8_t actor_layer(uint32_t ref) {
        return m_app.m_game->combat().fighter(ref).dead() ? k_layer_dead_biped : k_layer_biped;
    }

    static bool actor_near(const game::CombatBody& b, const Units3& centre, float radius) {
        const float x = b.position[0] - centre[0], y = b.position[1] - centre[1];
        const float z = std::clamp(centre[2], b.position[2], b.position[2] + b.height) - centre[2];
        return std::sqrt(x * x + y * y + z * z) <= radius + b.radius;
    }

    game::ProjectileContact contact(const physics::World::SweepContact& c) {
        game::ProjectileContact o;
        o.ref = c.ref;
        o.layer = c.layer;
        o.body = c.body;
        o.mass = c.mass;
        o.material = m_app.m_game->projectiles().material_of_havok(c.material);
        if (!o.material && c.layer == k_layer_ground && m_app.m_space && m_app.m_actor_data) {
            const glm::vec3 p = c.point;
            o.material = m_app.m_actor_data->land_material(m_app.m_space->dominant_land_texture(p.x, p.y));
        }
        return o;
    }

    // While an actor's ward holds, WardBodyFX's shell stands before it: HavokGeometry puts the art's body in the world, layer
    // 29, at MagicEffectsNode. The 22-point hull goes by its measured extent: the front half of an ellipsoid.
    void ward_contact(uint32_t ref, float heading, const Units3& a, const Units3& b, float radius,
                      std::vector<game::ProjectileContact>& out) {
        constexpr int k_ward_power = 63;
        constexpr uint8_t k_layer_ward = 29;
        constexpr glm::vec3 k_centre(0.5f, 6.4f, 14.5f), k_size(60.7f, 71.4f, 72.8f); // units, x right, y ahead, z up
        constexpr float k_shell = 3.5f;                                               // its Havok radius, 0.05
        ActorSet* actors = m_app.m_space ? m_app.m_space->actors() : nullptr;
        if (!actors || m_app.m_game->actor_values(ref).current(k_ward_power) <= 0.0f) {
            return;
        }
        glm::mat3 turn;
        glm::vec3 node;
        if (!actors->node_world(ref, "magiceffectsnode", turn, node)) {
            return;
        }
        const glm::vec3 origin = node / units::k_skyrim_unit_m;
        const glm::vec3 right(std::cos(heading), -std::sin(heading), 0.0f), ahead(std::sin(heading), std::cos(heading), 0.0f);
        const auto local = [&](const Units3& p) {
            const glm::vec3 d = glm::vec3(p[0], p[1], p[2]) - origin;
            return glm::vec3(glm::dot(d, right), glm::dot(d, ahead), d.z);
        };
        const glm::vec3 size = k_size + glm::vec3(k_shell + radius);
        const glm::vec3 from = (local(a) - k_centre) / size, to = (local(b) - k_centre) / size, d = to - from;
        const float qa = glm::dot(d, d), qb = 2.0f * glm::dot(from, d), qc = glm::dot(from, from) - 1.0f;
        const float root = qb * qb - 4.0f * qa * qc;
        if (qa <= 0.0f || root < 0.0f || qc <= 0.0f) {
            return; // what starts inside it, as the caster's own bolts do, goes out
        }
        const float t = (-qb - std::sqrt(root)) / (2.0f * qa);
        if (t < 0.0f || t > 1.0f || from.y + d.y * t < 0.0f) {
            return;
        }
        game::ProjectileContact o;
        o.ref = ref;
        o.actor = true;
        o.layer = k_layer_ward;
        o.fraction = t;
        for (int i = 0; i < 3; ++i) {
            o.position[i] = a[i] + (b[i] - a[i]) * t;
        }
        o.normal = {ahead.x, ahead.y, 0.0f};
        out.push_back(o);
    }

    void cylinders(const Units3& a, const Units3& b, float radius, std::vector<game::ProjectileContact>& out) {
        game::CombatWorld& combat = m_app.m_game->combat().world();
        for (uint32_t ref : everyone()) {
            game::CombatBody body;
            if (!combat.body(ref, body) || !body.loaded) {
                continue;
            }
            ward_contact(ref, glm::radians(body.heading), a, b, radius, out);
            const float r = body.radius + radius;
            const auto t = enter_cylinder(a, b, body.position, r, body.height);
            if (!t) {
                continue;
            }
            game::ProjectileContact o;
            o.ref = ref;
            o.actor = true;
            o.layer = actor_layer(ref);
            o.fraction = *t;
            for (int i = 0; i < 3; ++i) {
                o.position[i] = a[i] + (b[i] - a[i]) * *t;
            }
            const float nx = o.position[0] - body.position[0], ny = o.position[1] - body.position[1];
            const float len = std::max(std::hypot(nx, ny), 1e-4f);
            o.normal = {nx / len, ny / len, 0.0f};
            out.push_back(o);
        }
    }

    App& m_app;
    std::unordered_map<uint32_t, float> m_scales;
    std::unordered_map<uint32_t, uint32_t> m_tracked; // by the visual's id
    struct Flame {
        float length = 1.0f;
        int stage = 0; // 1 mSprayStart, 2 the blend
    };
    std::unordered_map<uint32_t, Flame> m_flames;
};

void App::update_projectiles(float dt) {
    if (!m_game) {
        return;
    }
    if (!m_projectile_world) {
        m_projectile_world = std::make_unique<EngineProjectiles>(*this);
        m_game->projectiles().set_world(m_projectile_world.get());
        m_vfx = std::make_unique<Vfx>(*this);
    }
    if (!(m_ui && m_ui->pausing_menus() > 0)) {
        m_game->projectiles().update(dt);
    }
}

// `projectile <PROJ editor id or form (hex)> <heading> <pitch> [SPEL editor id]`: degrees, from the camera, by the player
// when on foot.
void App::projectile_command(const std::string& line) {
    std::istringstream in(line);
    std::string which, spell;
    float heading = 0.0f, pitch = 0.0f;
    in >> which >> heading >> pitch >> spell;
    if (!m_game || !m_space) {
        throw Error("projectile needs a game and a world");
    }
    const forms::Database& db = m_game->db();
    constexpr forms::Sig proj = 'P' | 'R' << 8 | 'O' << 16 | uint32_t('J') << 24;
    constexpr forms::Sig ammo = 'A' | 'M' << 8 | 'M' << 16 | uint32_t('O') << 24;
    constexpr forms::Sig spel = 'S' | 'P' << 8 | 'E' << 16 | uint32_t('L') << 24;
    game::LaunchData data;
    for (const forms::FormEntry* e : db.of_type(proj)) {
        if (db.editor_id(*e) == which) {
            data.projectile = e->form_id;
        }
    }
    if (!data.projectile) {
        data.projectile = static_cast<uint32_t>(std::stoul(which, nullptr, 16));
    }
    // An arrow's mesh and what is picked up are the ammo's.
    for (const forms::FormEntry* e : db.of_type(ammo)) {
        const game::Item* item = m_game->items().find(e->form_id);
        if (item && item->projectile == data.projectile) {
            data.ammo = item;
            break;
        }
    }
    update_projectiles(0.0f);
    data.origin = game_units(m_camera.position);
    data.shooter = m_player ? game::k_player_ref : 0;
    data.heading = glm::radians(heading);
    data.pitch = glm::radians(pitch);
    for (const forms::FormEntry* e : db.of_type(spel)) {
        if (db.editor_id(*e) == spell) {
            data.spell = e->form_id;
            const game::Spell* s = m_game->magic().spell(data.spell);
            data.area = s && !s->effects.empty() ? float(s->effects[0].area) : 0.0f;
        }
    }
    game::Projectiles& projectiles = m_game->projectiles();
    projectiles.trace = true;
    if (!(data.spell ? projectiles.launch_spell(data) : projectiles.launch(data))) {
        throw Error(std::format("projectile: {} is not a PROJ", which));
    }
    log::info("projectile {} from {:.0f} {:.0f} {:.0f}", which, data.origin[0], data.origin[1], data.origin[2]);
}

} // namespace spud::engine
