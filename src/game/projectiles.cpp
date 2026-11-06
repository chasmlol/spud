#include "game/projectiles.hpp"

#include "core/log.hpp"
#include "core/units.hpp"
#include "game/game.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numbers>

namespace spud::game {

namespace {

using forms::Form;
using forms::Value;

constexpr float k_pi = std::numbers::pi_v<float>;
constexpr float k_gravity = 686.37866f;      // 9.80665 m/s2 in units
constexpr float k_havok_scale = 0.0142875f;
constexpr float k_max_node_pitch = 1.5533431f;

constexpr uint8_t k_bow = 7, k_crossbow = 9;
constexpr int k_shield_part = 18;
constexpr int k_weapon_speed_mult = 85;
constexpr int ep_mine_chance = 3, ep_recover_arrow = 21, ep_deflect = 40, ep_combat_hit_spell = 51;
constexpr uint32_t k_ammo_non_playable = 0x2, k_ammo_non_bolt = 0x4;
constexpr uint32_t k_material_arrows_stick = 0x2;

// Skyrim's collision layers.
enum Layer : uint8_t {
    layer_static = 1, layer_anim_static = 2, layer_clutter = 4, layer_projectile = 6, layer_spell = 7, layer_biped = 8,
    layer_trees = 9, layer_props = 10, layer_water = 11, layer_trigger = 12, layer_terrain = 13, layer_trap = 14,
    layer_ground = 17, layer_debris_large = 20, layer_projectile_zone = 23, layer_gas_trap = 24,
    layer_small_transparent = 26, layer_small_transparent_anim = 28, layer_ward = 29, layer_charcontroller = 30,
    layer_dead_biped = 32, layer_biped_no_cc = 33, layer_cone = 38, layer_explosion = 45,
};

constexpr uint64_t bit(int layer) { return uint64_t(1) << layer; }
constexpr uint64_t k_cone_los = bit(layer_static) | bit(layer_anim_static) | bit(layer_trees) | bit(layer_props) |
                                bit(layer_terrain) | bit(layer_ground);
constexpr uint64_t k_explosion_los = bit(layer_static) | bit(layer_anim_static) | bit(layer_trees) | bit(layer_terrain) |
                                     bit(layer_trap) | bit(layer_ground);
constexpr uint64_t k_impale_layers = bit(layer_static) | bit(layer_anim_static) | bit(layer_trees) | bit(layer_terrain) |
                                     bit(layer_ground) | bit(layer_debris_large);

constexpr forms::Sig sig(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

int64_t integer(const Form& f, std::string_view path) {
    const Value* v = f.get(path);
    return !v                                 ? 0
           : v->kind == Value::Kind::integer ? v->integer
           : v->kind == Value::Kind::real    ? int64_t(v->real)
                                             : int64_t(v->form);
}

float real(const Form& f, std::string_view path) {
    const Value* v = f.get(path);
    return !v ? 0.0f : v->kind == Value::Kind::real ? float(v->real) : float(v->integer);
}

uint32_t form_of(const Form& f, std::string_view path) {
    const Value* v = f.get(path);
    return v && v->kind == Value::Kind::form ? v->form : 0;
}

std::string text(const Form& f, std::string_view path) {
    const Value* v = f.get(path);
    return v && v->kind == Value::Kind::text ? v->text : std::string();
}

Units3 operator+(const Units3& a, const Units3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Units3 operator-(const Units3& a, const Units3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Units3 operator*(const Units3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const Units3& a, const Units3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float length(const Units3& a) { return std::sqrt(dot(a, a)); }
Units3 normalized(const Units3& a) {
    const float l = length(a);
    return l > 0.0f ? a * (1.0f / l) : Units3{0, 1, 0};
}
Units3 cross(const Units3& a, const Units3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Units3 direction_of(float heading, float pitch) {
    return {std::cos(-pitch) * std::sin(heading), std::cos(-pitch) * std::cos(heading), std::sin(-pitch)};
}

float heading_to(const Units3& d) { return std::atan2(d[0], d[1]); }
float pitch_to(const Units3& d) { return -std::atan2(d[2], std::hypot(d[0], d[1])); }
float radians(float degrees) { return degrees * k_pi / 180.0f; }
float wrapped(float angle) { return std::remainder(angle, 2.0f * k_pi); }

// 0 at or past the radius.
float falloff(float distance, float radius) {
    if (radius <= 0.0f || distance >= radius) {
        return 0.0f;
    }
    const float x = distance / radius;
    return 1.0f - x * x;
}

constexpr bool biped_layer(uint8_t layer) {
    return layer == layer_biped || layer == layer_dead_biped || layer == layer_biped_no_cc;
}

uint32_t crc_of_lower(std::string_view name) {
    uint32_t crc = 0;
    for (char c : name) {
        crc ^= uint8_t(std::tolower(static_cast<unsigned char>(c)));
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

} // namespace

Projectiles::Projectiles(Game& game) : m_game(game) {}
Projectiles::~Projectiles() = default;

float Projectiles::n(std::string_view name, float fallback) const { return m_game.settings().number(name, fallback); }

float Projectiles::perk(uint32_t owner, int point, float value, std::initializer_list<uint32_t> args) const {
    return owner == k_player_ref ? m_game.character().entry_point(point, value, args) : value;
}

float Projectiles::roll(float low, float high) {
    return low + (high - low) * (float(random() % 1000000u) / 1000000.0f);
}

bool Projectiles::is_actor(uint32_t ref) const {
    if (ref == k_player_ref) {
        return true;
    }
    const forms::FormEntry* e = ref ? m_game.db().find(m_game.base_of(ref)) : nullptr;
    return e && e->type == sig("NPC_");
}

// ---- records -------------------------------------------------------------------------------

const ProjectileRecord* Projectiles::record(uint32_t form) {
    const auto [it, added] = m_records.try_emplace(form);
    if (!added) {
        return it->second.get();
    }
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = db.find(form);
    if (!e || e->type != sig("PROJ")) {
        return nullptr;
    }
    const Form f(db, *e);
    auto r = std::make_unique<ProjectileRecord>();
    r->form = form;
    r->editor_id = std::string(f.editor_id());
    r->model = model_path(text(f, "MODL"));
    r->muzzle_model = model_path(text(f, "NAM1"));
    r->flags = uint16_t(integer(f, "DATA.flags"));
    r->type = ProjectileType(integer(f, "DATA.type"));
    r->gravity = real(f, "DATA.gravity");
    r->speed = real(f, "DATA.speed");
    r->range = real(f, "DATA.range");
    r->light = form_of(f, "DATA.light");
    r->muzzle_light = form_of(f, "DATA.muzzle_flash_light");
    r->tracer_chance = real(f, "DATA.tracer_chance");
    r->proximity = real(f, "DATA.explosion_proximity");
    r->timer = real(f, "DATA.explosion_timer");
    r->explosion = form_of(f, "DATA.explosion");
    r->sound = form_of(f, "DATA.sound");
    r->muzzle_duration = real(f, "DATA.muzzle_flash_duration");
    r->fade_duration = real(f, "DATA.fade_duration");
    r->impact_force = real(f, "DATA.impact_force");
    r->countdown_sound = form_of(f, "DATA.countdown_sound");
    r->disable_sound = form_of(f, "DATA.disable_sound");
    r->default_weapon = form_of(f, "DATA.default_weapon_source");
    r->cone_spread = real(f, "DATA.cone_spread");
    r->radius = real(f, "DATA.collision_radius");
    r->lifetime = real(f, "DATA.lifetime");
    r->relaunch = real(f, "DATA.relaunch_interval");
    r->decal = form_of(f, "DATA.decal_data");
    // No COLL: by the class.
    r->layer = r->is(ProjectileType::cone) ? layer_cone : r->is(ProjectileType::arrow) ? layer_projectile : layer_spell;
    if (const forms::FormEntry* coll = db.find(form_of(f, "DATA.collision_layer"))) {
        const Form c(db, *coll);
        r->layer = uint8_t(integer(c, "BNAM"));
        r->tells_triggers = (integer(c, "GNAM") & 0x1) != 0;
    }
    it->second = std::move(r);
    return it->second.get();
}

const ExplosionRecord* Projectiles::explosion(uint32_t form) {
    const auto [it, added] = m_explosion_records.try_emplace(form);
    if (!added) {
        return it->second.get();
    }
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = db.find(form);
    if (!e || e->type != sig("EXPL")) {
        return nullptr;
    }
    const Form f(db, *e);
    auto r = std::make_unique<ExplosionRecord>();
    r->form = form;
    r->editor_id = std::string(f.editor_id());
    r->model = model_path(text(f, "MODL"));
    r->enchantment = form_of(f, "EITM");
    r->image_space = form_of(f, "MNAM");
    r->light = form_of(f, "DATA.light");
    r->sound1 = form_of(f, "DATA.sound1");
    r->sound2 = form_of(f, "DATA.sound2");
    r->impact_set = form_of(f, "DATA.impact_data_set");
    r->placed_object = form_of(f, "DATA.placed_object");
    r->spawn_projectile = form_of(f, "DATA.spawn_projectile");
    r->force = real(f, "DATA.force");
    r->damage = real(f, "DATA.damage");
    r->radius = real(f, "DATA.radius");
    r->image_radius = real(f, "DATA.is_radius");
    r->flags = uint32_t(integer(f, "DATA.flags"));
    r->sound_level = int32_t(integer(f, "DATA.sound_level"));
    it->second = std::move(r);
    return it->second.get();
}

const Projectiles::SpellEffect* Projectiles::first_effect(uint32_t spell) {
    const Spell* s = spell ? m_game.magic().spell(spell) : nullptr;
    if (!s || s->effects.empty()) {
        return nullptr;
    }
    const uint32_t effect = s->effects[0].effect;
    const auto [it, added] = m_spell_effects.try_emplace(effect);
    if (added) {
        const forms::Database& db = m_game.db();
        if (const forms::FormEntry* e = db.find(effect)) {
            const Form f(db, *e);
            it->second.explosion = form_of(f, "DATA.explosion");
            it->second.projectile = form_of(f, "DATA.projectile");
            it->second.impact_set = form_of(f, "DATA.impact_data");
            it->second.dual_scale = real(f, "DATA.dual_casting_scale");
        }
    }
    return &it->second;
}

uint32_t Projectiles::material_of_havok(uint32_t havok) {
    if (m_havok_materials.empty()) {
        const forms::Database& db = m_game.db();
        m_havok_materials.emplace(0, 0);
        for (const forms::FormEntry* e : db.of_type(sig("MATT"))) {
            m_havok_materials.emplace(crc_of_lower(text(Form(db, *e), "MNAM")), e->form_id);
        }
    }
    const auto it = m_havok_materials.find(havok);
    return it == m_havok_materials.end() ? 0 : it->second;
}

// The race's blood material on an actor, else what the contact says.
uint32_t Projectiles::material_of(const ProjectileContact& c) {
    if (!c.actor) {
        return c.material;
    }
    const uint32_t race = m_game.combat().fighter(c.ref).race;
    const auto [it, added] = m_race_materials.try_emplace(race);
    if (added) {
        const forms::Database& db = m_game.db();
        if (const forms::FormEntry* e = db.find(race)) {
            it->second = form_of(Form(db, *e), "NAM4");
        }
    }
    return it->second;
}

uint32_t Projectiles::impact_set(const Projectile& p) {
    if (p.launch.weapon && p.launch.weapon->impact_set) {
        return p.launch.weapon->impact_set;
    }
    const SpellEffect* effect = first_effect(p.launch.spell);
    return effect ? effect->impact_set : 0;
}

uint32_t Projectiles::impact_of(uint32_t set, uint32_t material) { return m_game.impacts().impact_of(set, material); }

uint64_t Projectiles::layers_of(const Projectile& p) { return m_world->layer_row(p.record->layer); }

// ---- draw ----------------------------------------------------------------------------------

void Projectiles::bow_pressed() { m_draws.push_back({}); }

void Projectiles::bow_held(float seconds) {
    if (!m_draws.empty()) {
        m_draws.back().seconds = seconds;
    }
}

void Projectiles::bow_released() {
    if (!m_draws.empty()) {
        m_draws.back().released = m_time;
    }
}

float Projectiles::pop_draw() {
    const double keep =
        std::round((n("fBowDrawTime", 1.6667f) - n("fArrowBowMinTime", 0.8f)) * 1000.0) / 1000.0;
    while (!m_draws.empty()) {
        const DrawEntry e = m_draws.front();
        m_draws.pop_front();
        if (e.released < 0.0 || m_time - e.released <= keep) {
            return e.seconds;
        }
    }
    return 0.0f;
}

float Projectiles::draw_power(float seconds, const Item& weapon, uint32_t shooter) {
    const float min_time = n("fArrowBowMinTime", 0.8f), min_power = n("fArrowMinPower", 0.35f);
    float mult = m_game.actor_values(shooter).current(k_weapon_speed_mult);
    mult = mult > 0.0f ? mult : 1.0f;
    float speed = weapon.speed * mult;
    speed = speed > 0.0f ? speed : 1.0f;
    const float x = seconds - min_time;
    const float full = (n("fBowDrawTime", 1.6667f) - min_time) / speed;
    return x <= 0.0f ? min_power : x >= full ? 1.0f : min_power + x / full * (1.0f - min_power);
}

float Projectiles::power_speed_mult(float power) const {
    const float min_power = n("fArrowMinPower", 0.35f), min_velocity = n("fArrowMinVelocity", 0.2f);
    return min_velocity + (power - min_power) / (1.0f - min_power) * (1.0f - min_velocity);
}

float Projectiles::bow_speed_mult(const Item* weapon) const {
    if (!weapon) {
        return 1.0f;
    }
    const float slow = n("fArrowBowSlowMult", 0.5f), fast = n("fArrowBowFastMult", 1.0f);
    return 1.0f + (slow - std::clamp(weapon->speed, slow, fast)) * (1.0f - n("fArrowMinBowVelocity", 0.7f)) / (fast - slow);
}

float Projectiles::gravity_of(const Projectile& p) const {
    const ProjectileRecord& r = *p.record;
    if (r.is(ProjectileType::lobber)) {
        return 1.0f;
    }
    if (r.is(ProjectileType::arrow)) {
        const float weak = n("fArrowWeakGravity", 1.75f);
        return weak - (weak - r.gravity) * p.power;
    }
    return r.gravity;
}

// ---- launch --------------------------------------------------------------------------------

void Projectiles::restart(Projectile& p, const LaunchData& data) {
    p.impacts.clear();
    p.impacts_processed = false;
    p.effects_played = false;
    p.exploded = false;
    p.stage = 0;
    p.launch.power = data.power;
    if (p.record->is(ProjectileType::beam)) {
        p.age = 0;
        p.moved = 0;
        p.range = p.record->range;
        p.fading = false;
        p.alpha = 1;
        m_world->visual_alpha(p.id, 1.0f);
    }
    m_world->hide_visual(p.id, false);
}

const Projectile* Projectiles::find(uint32_t id) const {
    for (const auto& p : m_live) {
        if (p->id == id) {
            return p.get();
        }
    }
    return nullptr;
}

// at the target's detection point each frame; the heading snaps within 2.5 rad, the pitch only with no gravity.
void Projectiles::track(Projectile& p) {
    ProjectileBody b;
    if (!p.launch.desired_target || !m_world->body(p.launch.desired_target, b)) {
        return;
    }
    const Units3 to = b.position + Units3{0.0f, 0.0f, b.height * 0.75f} - p.position;
    const float flat = std::sqrt(to[0] * to[0] + to[1] * to[1]);
    if (flat < 1.0f) {
        return;
    }
    const float heading = std::atan2(to[0], to[1]);
    if (std::abs(std::remainder(heading - p.heading, 6.2831853f)) > 2.5f) {
        return;
    }
    p.heading = heading;
    if (gravity_of(p) == 0.0f) {
        p.pitch = std::atan2(to[2], flat);
    }
    const float speed = length(p.velocity);
    const float c = std::cos(p.pitch);
    p.direction = {std::sin(heading) * c, std::cos(heading) * c, std::sin(p.pitch)};
    p.velocity = {p.direction[0] * speed, p.direction[1] * speed, gravity_of(p) == 0.0f ? p.direction[2] * speed : p.velocity[2]};
}

uint32_t Projectiles::launch(const LaunchData& given) {
    LaunchData data = given;
    if (data.shooter == k_player_ref && before_player_shot) {
        before_player_shot(data);
    }
    const ProjectileRecord* r = record(data.projectile);
    if (!r) {
        return 0;
    }
    // One concentration stream is one object.
    if ((r->is(ProjectileType::beam) || r->is(ProjectileType::flame)) && !data.force_new) {
        for (const auto& live : m_live) {
            if (!live->dead && live->record == r && live->launch.shooter == data.shooter &&
                (live->launch.casting_source == data.casting_source || live->launch.casting_source == k_casting_source_none)) {
                restart(*live, data);
                if (data.shooter == k_player_ref && after_player_shot) {
                    after_player_shot(live->id);
                }
                return live->id;
            }
        }
    }
    auto owned = std::make_unique<Projectile>();
    Projectile& p = *owned;
    p.id = m_next_ref++;
    p.record = r;
    p.launch = data;
    p.space = m_world->space();
    p.position = p.origin = data.origin;
    p.heading = data.heading;
    p.pitch = data.pitch;
    p.direction = direction_of(data.heading, data.pitch);
    p.range = r->range;
    p.radius = r->radius;
    p.hitscan = r->has(proj_hitscan) || r->is(ProjectileType::beam);
    if (r->tracer_chance > 0.0f) {
        p.tracer = int(random() % 100u) < int(r->tracer_chance * 100.0f);
    }
    Combat& combat = m_game.combat();
    const uint32_t shooter = data.shooter;
    if (data.weapon) {
        // The game counts the ammo with the weapon's base, inside the skill factor.
        Item shot = *data.weapon;
        shot.damage += data.ammo ? data.ammo->damage : 0;
        const float base = is_actor(shooter) ? combat.weapon_base(shooter, Held{&shot, nullptr, data.weapon_health}, 1.0f)
                                             : float(shot.damage) * n("fDamageWeaponMult", 1.0f);
        p.weapon_damage = base / float(std::max<uint32_t>(data.weapon->projectile_count, 1));
        p.npc_in_combat = shooter != k_player_ref && is_actor(shooter) && combat.fighter(shooter).in_combat;
    }
    if (shooter == k_player_ref && data.weapon && data.weapon->animation_type == k_bow) {
        p.power = draw_power(pop_draw(), *data.weapon, shooter);
        p.weapon_damage *= p.power;
    }
    if (data.spell) {
        p.power = data.power;
    }
    p.explosion = r->explosion;
    if (const SpellEffect* effect = data.area > 0.0f ? first_effect(data.spell) : nullptr; effect && effect->explosion) {
        p.explosion = effect->explosion;
    }
    const uint32_t bound = m_game.keyword("WeapTypeBoundArrow");
    if (bound && data.ammo && data.ammo->has_keyword(bound) && !(data.weapon && data.weapon->has_keyword(bound))) {
        p.explosion = 0;
    }
    // An arrow's mesh is the ammo's (Clone3D).
    p.model = data.ammo && !r->is(ProjectileType::beam) ? data.ammo->model : r->model;
    m_world->create_visual(p.id, p.model, p.position, p.heading, p.pitch, data.scale);
    if (r->has(proj_muzzle_flash) && !r->muzzle_model.empty() && !(is_actor(shooter) && combat.fighter(shooter).dead())) {
        m_world->muzzle_flash(*r, data.origin);
    }
    m_live.push_back(std::move(owned));
    if (data.shooter == k_player_ref && after_player_shot) {
        after_player_shot(p.id);
    }
    return p.id;
}

uint32_t Projectiles::launch_spell(LaunchData data) {
    if (!data.projectile) {
        const SpellEffect* effect = first_effect(data.spell);
        data.projectile = effect ? effect->projectile : 0;
    }
    data.auto_aim = true;
    return launch(data);
}

bool Projectiles::arrow_release(uint32_t shooter) {
    Gear gear = m_game.combat().gear(shooter);
    Held& held = gear.right;
    if (!held.item || (held.item->animation_type != k_bow && held.item->animation_type != k_crossbow)) {
        return false;
    }
    uint32_t enchantment = 0, poison = 0;
    Stack* stack = held.stack;
    const uint32_t ench = stack && stack->extra.enchantment ? stack->extra.enchantment : held.item->enchantment;
    if (ench) {
        const float cost = m_game.magic().enchantment_cost(ench);
        if (!stack) {
            enchantment = ench;
        } else {
            const float max = stack->extra.max_charge ? float(stack->extra.max_charge) : float(held.item->enchantment_amount);
            const float charge = stack->extra.charge < 0.0f ? max : stack->extra.charge;
            if (charge >= std::floor(cost)) {
                enchantment = ench;
                stack->extra.charge = std::max(charge - cost, 0.0f);
            }
        }
    }
    if (stack && stack->extra.poison && stack->extra.poison_doses > 0) {
        poison = stack->extra.poison;
        if (--stack->extra.poison_doses <= 0) {
            stack->extra.poison = 0;
        }
    }
    if (!fire_weapon(shooter, held.item, nullptr, &held, enchantment, poison)) {
        return false;
    }
    m_game.combat().fighter(shooter).attack_state = AttackState::fired;
    return true;
}

bool Projectiles::fire_weapon(uint32_t shooter, const Item* weapon, const Item* ammo, const Held* held, uint32_t enchantment,
                              uint32_t poison) {
    if (!weapon) {
        return false;
    }
    const Items& items = m_game.items();
    Inventory& inventory = shooter == k_player_ref ? m_game.player().inventory : m_game.container(shooter);
    if (!ammo) {
        for (const Stack& s : inventory.stacks()) {
            const Item* item = items.find(s.form);
            if (item && item->kind == ItemKind::ammo && (s.worn != Worn::none || shooter != k_player_ref)) {
                ammo = item;
                break;
            }
        }
    }
    const ProjectileRecord* r = ammo ? record(ammo->projectile) : nullptr;
    if (!r) {
        return false;
    }
    ShotAim aim;
    m_world->aim(shooter, weapon->animation_type == k_crossbow, aim);
    if (!aim.node) {
        ProjectileBody b;
        if (!m_world->body(shooter, b)) {
            return false;
        }
        aim.origin = b.position + Units3{0, 0, 96};
    }
    const bool bolt = !(ammo->weapon_flags & k_ammo_non_bolt);
    if (shooter == k_player_ref) {
        const float tilt = aim.first_person ? (bolt ? n("f1PBoltTiltUpAngle", 1.0f) : n("f1PArrowTiltUpAngle", 2.0f))
                                            : n("f3PArrowTiltUpAngle", 2.5f);
        aim.pitch -= radians(tilt);
    } else {
        Units3 point;
        const bool aimed = m_world->combat_aim(shooter, weapon, point);
        if (!aimed && m_game.combat().fighter(shooter).in_combat) {
            return false;
        }
        if (aimed && !r->has(proj_no_combat_aim)) {
            const Units3 d = point - aim.origin;
            aim.heading = heading_to(d);
            aim.pitch = pitch_to(d);
        }
        const float archery = m_game.actor_values(shooter).current(av_marksman);
        const float spread = radians(n("fBowNPCSpreadAngle", 4.0f)) * std::clamp((100.0f - archery) / 100.0f, 0.0f, 1.0f);
        const float m = roll(0.0f, spread), a = roll(0.0f, 2.0f * k_pi);
        aim.pitch += std::sin(a) * m;
        aim.heading += std::cos(a) * m;
    }
    const int count = std::max<int>(weapon->projectile_count, 1);
    inventory.remove(ammo->form, count);
    LaunchData data;
    data.origin = aim.origin;
    data.projectile = ammo->projectile;
    data.shooter = shooter;
    data.weapon = weapon;
    data.ammo = ammo;
    data.heading = aim.heading;
    data.pitch = aim.pitch;
    data.enchantment = enchantment;
    data.poison = poison;
    data.auto_aim = true;
    data.weapon_health = held ? held->health : 1.0f;
    float power = 0;
    for (int i = 0; i < count; ++i) {
        launch(data);
        power = std::max(power, m_live.back()->power);
    }
    if (shooter == k_player_ref) {
        m_world->play_sound(weapon->attack_sound_2d, nullptr);
        if (on_bow_shot) {
            on_bow_shot(weapon, ammo, power);
        }
    } else {
        m_world->play_sound(weapon->attack_sound, &aim.origin);
    }
    m_world->noise(shooter, weapon->sound_level);
    return true;
}

bool Projectiles::activate(uint32_t projectile) {
    for (const auto& p : m_live) {
        if (p->id == projectile && p->record->has(proj_can_disable) && !p->disarmed) {
            p->disarmed = true;
            p->timer = std::numeric_limits<float>::max();
            m_world->play_sound(p->record->disable_sound, &p->position);
            return true;
        }
    }
    return false;
}

// ---- the first frame -----------------------------------------------------------------------

void Projectiles::auto_aim(Projectile& p) {
    ShotAim view;
    if (!m_world->aim(k_player_ref, false, view)) {
        return;
    }
    Combat& combat = m_game.combat();
    const float reach = n("fAutoAimMaxDistance", 1800.0f), screen = 0.005f * n("fAutoAimScreenPercentage", 6.0f);
    const bool ranged = p.launch.spell || p.launch.ammo;
    Units3 wanted{};
    bool have = false;
    float best = std::numeric_limits<float>::max();
    for (uint32_t ref : m_world->actors()) {
        ProjectileBody b;
        float x = 0, y = 0;
        if (!m_world->body(ref, b) || combat.fighter(ref).dead() || !combat.in_combat_with(ref, k_player_ref)) {
            continue;
        }
        const Units3 centre = b.position + Units3{0, 0, b.height * 0.5f};
        if (length(centre - view.origin) > reach || !m_world->on_screen(centre, x, y) || std::abs(x) > screen ||
            std::abs(y) > screen || std::hypot(x, y) >= best || !m_world->pick(view.origin, centre, k_explosion_los).empty()) {
            continue;
        }
        best = std::hypot(x, y);
        wanted = ranged ? centre : Units3{centre[0], centre[1], p.position[2]};
        have = true;
    }
    if (!have) {
        const Units3 end = view.origin + direction_of(view.heading, view.pitch) * p.range;
        const auto hits = m_world->pick(view.origin, end, layers_of(p));
        if (hits.empty()) {
            return;
        }
        wanted = hits.front().position;
    }
    const Units3 d = wanted - p.position;
    const float limit = radians(!ranged           ? n("fAutoAimMaxDegreesMelee", 90.0f)
                                : view.third_person ? n("fAutoAimMaxDegrees3rdPerson", 2.0f)
                                                    : n("fAutoAimMaxDegrees", 1.0f));
    float turn = std::clamp(wrapped(heading_to(d) - p.heading), -limit, limit);
    float tilt = std::clamp(pitch_to(d) - p.pitch, -limit, limit);
    if (p.launch.ammo && tilt > 0.0f) {
        tilt = 0.0f; // never pushes an arrow down
    }
    p.heading += turn;
    p.pitch += tilt;
}

void Projectiles::set_velocity(Projectile& p) {
    const ProjectileRecord& r = *p.record;
    const float power = p.launch.spell ? 1.0f : power_speed_mult(p.power);
    const float bow = r.is(ProjectileType::arrow) ? bow_speed_mult(p.launch.weapon) : 1.0f;
    p.speed = r.speed * power * bow;
    p.direction = direction_of(p.heading, p.pitch);
    p.velocity = p.direction * p.speed;
    if (p.launch.shooter == k_player_ref) {
        ProjectileBody b;
        if (m_world->body(k_player_ref, b)) {
            p.velocity[0] += b.velocity[0];
            p.velocity[1] += b.velocity[1];
        }
    }
}

void Projectiles::first_frame(Projectile& p) {
    const ProjectileRecord& r = *p.record;
    const LaunchData& l = p.launch;
    p.has_3d = true;
    if (r.light) {
        m_world->set_light(p.id, r.light, p.position, 1.0f);
    }
    if (l.spell && !l.dual_cast && !l.use_origin && !r.is(ProjectileType::lobber)) {
        Units3 hand;
        if (m_world->magic_node(l.shooter, l.casting_source, hand)) {
            p.position = p.origin = hand;
        }
    }
    if (!l.weapon && r.default_weapon) {
        p.launch.weapon = m_game.items().find(r.default_weapon);
    }
    if (r.has(proj_alt_trigger) && r.proximity <= 0.0f) {
        p.timer = r.timer;
    }
    if (l.auto_aim && l.shooter == k_player_ref && !l.actor_cause) {
        auto_aim(p);
    }
    set_velocity(p);
    if (r.tracer_chance > 0.0f) {
        m_world->tracer_nodes(p.id, p.tracer);
    }
    if (p.hitscan && !r.is(ProjectileType::beam)) {
        const float reach = p.range - p.moved;
        const float hit = run_pick(p, p.position, p.position + p.direction * reach);
        p.range = p.moved + reach * hit;
        p.fading = p.tracer;
    }
    if (r.sound) {
        p.sound = m_world->play_sound(r.sound, &p.position);
    }
    m_world->trail(p.id, false);
}

// ---- the step ------------------------------------------------------------------------------

Units3 Projectiles::integrate(Projectile& p, float dt, float gravity) {
    const Units3 next = p.position + p.velocity * dt;
    if (gravity != 0.0f) {
        p.velocity[2] -= k_gravity * gravity * dt;
    }
    return next;
}

void Projectiles::sweep(Projectile& p, const Units3& from, const Units3& to, float radius) {
    const Units3 d = to - from;
    const float distance = length(d);
    if (distance > 0.0f) {
        p.direction = d * (1.0f / distance);
    }
    p.moved += distance;
    const auto contacts = m_world->sweep(from, to, radius, layers_of(p));
    handle_hits(p, contacts);
    if (!p.dead) {
        p.position = to;
    }
}

void Projectiles::node(Projectile& p) {
    if (p.dead) {
        return;
    }
    if (gravity_of(p) > 0.0f && !p.hitscan && length(p.velocity) > 0.0f) {
        const float z = normalized(p.velocity)[2];
        p.pitch = std::clamp(-std::asin(std::clamp(z, -1.0f, 1.0f)), -k_max_node_pitch, k_max_node_pitch);
    }
    m_world->move_visual(p.id, p.position, p.heading, p.pitch);
    if (p.sound) {
        m_world->move_sound(p.sound, p.position);
    }
    if (p.record->light) {
        m_world->set_light(p.id, p.record->light, p.position, p.alpha);
    }
}

void Projectiles::fade(Projectile& p, float dt) {
    if (!p.fading) {
        return;
    }
    p.alpha -= p.record->fade_duration > 0.0f ? dt / p.record->fade_duration : 1.0f;
    m_world->visual_alpha(p.id, std::max(p.alpha, 0.0f));
    if (p.alpha <= 0.0f && (p.impacts.empty() || p.impacts_processed)) {
        kill(p);
    }
}

void Projectiles::land(Projectile& p, const char* how) {
    if (trace) {
        log::info("projectile {}: {} at {:.1f} {:.1f} {:.1f} after {:.2f} s, {:.0f} units flown, power {:.2f}",
                  p.record->editor_id, how, p.position[0], p.position[1], p.position[2], p.age, p.moved, p.power);
    }
}

void Projectiles::kill(Projectile& p) {
    if (p.dead) {
        return;
    }
    p.dead = true;
    if (p.sound) {
        m_world->stop_sound(p.sound);
        p.sound = 0;
    }
    if (p.record->light) {
        m_world->remove_light(p.id);
    }
    m_world->hide_visual(p.id, true);
    if (p.placed) {
        m_game.remove_placed(p.placed);
        p.placed = 0;
    }
}

void Projectiles::step(Projectile& p, float dt) {
    if (p.launch.shooter == k_player_ref) {
        dt *= player_rate;
    }
    if (p.id == tracked && p.flight == ArrowFlight::flying) {
        track(p);
    }
    if (p.flight == ArrowFlight::bounced) {
        step_bounced(p, dt);
        return;
    }
    if (p.flight == ArrowFlight::stuck) {
        step_stuck(p);
        return;
    }
    switch (p.record->type) {
    case ProjectileType::lobber: step_lobber(p, dt); break;
    case ProjectileType::beam: step_beam(p, dt); break;
    case ProjectileType::flame: step_flame(p, dt); break;
    case ProjectileType::cone: step_cone(p, dt); break;
    case ProjectileType::barrier: step_barrier(p); break;
    default: step_missile(p, dt); break;
    }
}

void Projectiles::step_missile(Projectile& p, float dt) {
    const bool pending = !p.impacts.empty() && !p.impacts_processed;
    if (p.moved > p.range && !pending) {
        land(p, "out of range");
        kill(p);
        return;
    }
    if (!p.impacts.empty() && p.impacts_processed) {
        act_on_result(p);
        return;
    }
    const uint32_t shooter = p.launch.shooter;
    ProjectileBody b;
    if (p.age == 0.0f && shooter && is_actor(shooter) && m_world->body(shooter, b) &&
        (!b.seated || m_game.combat().fighter(shooter).mounted)) {
        // What stands between the body and the muzzle.
        Units3 from = b.position;
        from[2] = std::max(from[2], p.position[2]);
        const auto contacts = m_world->sweep(from, p.position, p.radius, layers_of(p));
        handle_hits(p, contacts);
    } else if (p.hitscan) {
        const Units3 to = integrate(p, dt, gravity_of(p));
        p.moved += length(to - p.position);
        p.position = to;
    } else {
        const Units3 to = integrate(p, dt, gravity_of(p));
        sweep(p, p.position, to, p.radius);
    }
    if (!p.impacts.empty()) {
        const Impact& last = p.impacts.back();
        if (!last.resolved && (!p.hitscan || p.moved > p.range)) {
            p.position = last.position;
        }
    }
    if (p.record->is(ProjectileType::arrow) && p.speed >= n("fArrowMinSpeedForTrails", 1500.0f) &&
        p.moved >= n("fArrowMinDistanceForTrails", 300.0f)) {
        m_world->trail(p.id, true);
    }
    node(p);
    check_explode(p, dt);
    if (!p.dead && p.moved > p.range && p.impacts.empty()) {
        land(p, "out of range");
        kill(p);
    }
}

void Projectiles::follow_node(Projectile& p) {
    if (p.launch.use_origin) {
        return;
    }
    ShotAim aim;
    if (m_world->aim(p.launch.shooter, false, aim)) {
        p.heading = aim.heading;
        p.pitch = aim.pitch;
        p.position = aim.origin;
    }
    Units3 hand;
    if (m_world->magic_node(p.launch.shooter, p.launch.casting_source, hand)) {
        p.position = hand;
    }
    p.direction = direction_of(p.heading, p.pitch);
    m_world->move_visual(p.id, p.position, p.heading, p.pitch);
}

void Projectiles::step_beam(Projectile& p, float dt) {
    follow_node(p);
    if (p.stage == 0.0f) {
        p.stage = 1.0f; // once per (re)launch: the relaunch interval is the hit rate
        const float hit = run_pick(p, p.position, p.position + p.direction * p.range);
        p.range *= hit;
    }
    check_explode(p, dt);
    if (!p.launch.concentration || p.age > 2.0f * p.record->relaunch) {
        p.fading = true;
    }
    if (p.age >= 5.0f) {
        kill(p);
    }
}

void Projectiles::flame_pick(Projectile& p) {
    const ProjectileRecord& r = *p.record;
    Units3 from = p.position;
    if (is_actor(p.launch.shooter)) {
        from = from - p.direction * 64.0f;
    }
    // The ring's angle is 0.9 x a value of the model; no model gives one here, so 0.05 rad.
    const float ring = 0.05f;
    const Units3 side = normalized(cross(p.direction, std::abs(p.direction[2]) > 0.99f ? Units3{1, 0, 0} : Units3{0, 0, 1}));
    const Units3 up = cross(side, p.direction);
    const float turn = roll(0.0f, 2.0f * k_pi);
    float nearest = run_pick(p, from, from + p.direction * r.range);
    for (int i = 0; i < 3; ++i) {
        const float a = turn + float(i) * 2.0f * k_pi / 3.0f;
        const Units3 d = p.direction * std::cos(ring) + (side * std::cos(a) + up * std::sin(a)) * std::sin(ring);
        nearest = std::min(nearest, run_pick(p, from, from + d * r.range));
    }
    m_world->flame_length(p.id, nearest);
}

void Projectiles::step_flame(Projectile& p, float dt) {
    follow_node(p);
    if (p.stage == 0.0f) {
        flame_pick(p);
        check_explode(p, dt);
    }
    p.stage += dt;
    if (p.stage > 0.3f) {
        m_world->hide_visual(p.id, true);
    }
    if (p.stage > 1.0f && (p.impacts.empty() || p.impacts_processed)) {
        kill(p);
    }
}

void Projectiles::cone_ground(Projectile& p) {
    p.stage += 1.0f;
    const float reach = n("fConeProjectileEnvironmentDistance", 192.0f) + p.radius;
    const Units3 end = p.position - Units3{0, 0, reach};
    float water = 0;
    if (m_world->water_height(p.position, water) && water <= p.position[2] && water >= end[2]) {
        // Smaller with height; the exact curve is the effect's.
        const float height = 1.0f - (p.position[2] - water) / reach;
        m_world->splash({p.position[0], p.position[1], water}, n("fConeProjectileWaterScaleMult", 100.0f) * height);
        return;
    }
    const auto hits = m_world->pick(p.position, end, k_cone_los);
    if (hits.empty()) {
        return;
    }
    const ProjectileContact& c = hits.front();
    if (const uint32_t ipct = impact_of(impact_set(p), c.material)) {
        m_game.impacts().projectile(ipct, c.position, c.normal, Units3{0, 0, -1}, c.ref, c.mass <= 0.0f, p.decal_size);
    }
}

void Projectiles::step_cone(Projectile& p, float dt) {
    if (p.moved > p.range) {
        kill(p);
        return;
    }
    const Units3 to = integrate(p, dt, gravity_of(p));
    p.radius = length(to - p.origin) * std::tan(radians(p.record->cone_spread)) + p.record->radius;
    sweep(p, p.position, to, p.radius);
    const float every = n("fConeProjectileEnvironmentTimer", 0.125f);
    if (every > 0.0f && p.age >= p.stage * every) {
        cone_ground(p);
    }
    node(p);
    check_explode(p, dt);
}

void Projectiles::step_lobber(Projectile& p, float dt) {
    const ProjectileRecord& r = *p.record;
    const bool mine = r.has(proj_alt_trigger) && r.proximity > 0.0f;
    const float life = r.lifetime > 0.0f ? r.lifetime : mine ? n("fMineAgeMax", 0.0f) : n("fGrenadeAgeMax", 90.0f);
    if (life > 0.0f && p.age >= life) {
        kill(p);
        return;
    }
    if (!p.resting) {
        const Units3 to = integrate(p, dt, gravity_of(p));
        for (const ProjectileContact& c : m_world->sweep(p.position, to, p.radius, layers_of(p))) {
            if (!can_hit(p, c)) {
                continue;
            }
            // Havok drives this body in the game and all ten records are runes that sit where they land; a body
            // that bounces waits for body_sim.
            p.position = c.position;
            p.velocity = {};
            p.resting = true;
            if (!r.has(proj_alt_trigger)) {
                p.impacts.push_back(make_impact(p, c));
                p.impacts_processed = false;
            }
            break;
        }
        if (!p.resting) {
            p.position = to;
        }
    }
    node(p);
    check_explode(p, dt);
}

void Projectiles::step_barrier(Projectile& p) {
    if (p.age >= p.record->lifetime) {
        kill(p);
        return;
    }
    const float width = p.launch.area * n("fMagicUnitsPerFoot", 22.0f);
    for (const ProjectileContact& c : m_world->overlap(p.position, width, layers_of(p))) {
        if (can_hit(p, c) && std::ranges::none_of(p.impacts, [&](const Impact& i) { return i.ref == c.ref; })) {
            p.impacts.push_back(make_impact(p, c));
            p.impacts_processed = false;
        }
    }
}

// Havok carries a bounced arrow in the game. No arrow has a body here, so it flies on the world's gravity and comes to
// rest at its first contact, the way a dropped item rests until body_sim exists.
void Projectiles::step_bounced(Projectile& p, float dt) {
    const Units3 to = integrate(p, dt, 1.0f);
    const auto hits = m_world->sweep(p.position, to, p.radius, m_world->layer_row(layer_clutter));
    // It leaves the surface it struck, which touches it at the start of the first step.
    const auto hit = std::ranges::find_if(hits, [](const ProjectileContact& c) { return !c.actor && c.fraction > 0.0f; });
    p.position = hit == hits.end() ? to : hit->position;
    p.heading += 8.0f * dt; // the spin
    m_world->move_visual(p.id, p.position, p.heading, p.pitch);
    if (hit != hits.end()) {
        p.flight = ArrowFlight::stuck;
        leave_in_world(p);
        land(p, "came to rest");
    }
}

void Projectiles::step_stuck(Projectile& p) {
    if (p.placed && !m_game.placed().contains(p.placed)) {
        p.placed = 0; // taken
        kill(p);
        return;
    }
    if (!p.follows) {
        return;
    }
    ProjectileBody b;
    if (!m_world->present(p.follows) || !m_world->body(p.follows, b)) {
        kill(p);
        return;
    }
    p.position = b.position + p.offset;
    m_world->move_visual(p.id, p.position, p.heading, p.pitch);
}

// ---- hitscan -------------------------------------------------------------------------------

// RunTargetPick and the walk of its hits: the fraction of the ray that was used.
float Projectiles::run_pick(Projectile& p, const Units3& from, const Units3& to) {
    float limit = 1.0f, water = 0;
    if (m_world->water_height(from, water) && from[2] > water && to[2] < water) {
        limit = (from[2] - water) / (from[2] - to[2]);
    }
    for (const ProjectileContact& c : m_world->pick(from, to, layers_of(p))) {
        if (c.fraction > limit) {
            break;
        }
        if (can_hit(p, c) && !is_trigger(p, c) && add_impact(p, c)) {
            return c.fraction;
        }
    }
    return limit;
}

// ---- hits ----------------------------------------------------------------------------------

bool Projectiles::is_trigger(const Projectile& p, const ProjectileContact& c) const {
    return c.layer == layer_trigger || c.layer == layer_projectile_zone || c.layer == layer_gas_trap ||
           p.record->tells_triggers;
}

bool Projectiles::can_hit(const Projectile& p, const ProjectileContact& c) {
    const ProjectileRecord& r = *p.record;
    if (c.layer == layer_small_transparent || c.layer == layer_small_transparent_anim) {
        const bool stops = r.is(ProjectileType::lobber) || (r.is(ProjectileType::missile) && !r.has(proj_hitscan) &&
                                                            !r.has(proj_pass_small_transparent));
        if (!stops) {
            return false;
        }
    }
    const uint32_t shooter = p.launch.shooter;
    if (c.ref && c.ref == shooter && !(r.is(ProjectileType::lobber) && p.age >= 1.0f)) {
        return false;
    }
    ProjectileBody b;
    if (c.actor && m_world->body(c.ref, b) && b.ghost) {
        return false;
    }
    return true;
}

void Projectiles::apply_spell(const Projectile& p, uint32_t spell, uint32_t target, const Units3& at, const Units3& normal,
                              int source) {
    if (!on_magic_hit) {
        return;
    }
    const ExplosionRecord* e = p.explosion ? explosion(p.explosion) : nullptr;
    MagicHit hit;
    hit.caster = p.launch.shooter;
    hit.spell = spell;
    hit.target = target;
    hit.projectile = p.id;
    hit.casting_source = source;
    hit.position = at;
    hit.normal = normal;
    hit.area = p.launch.area;
    hit.effectiveness = p.power;
    hit.dual_cast = p.launch.dual_cast;
    hit.chain = e && e->has(expl_chain);
    on_magic_hit(hit);
}

// PreHitMagic: true when a ward ends the walk.
bool Projectiles::pre_hit_magic(Projectile& p, const ProjectileContact& c, bool& applied) {
    const LaunchData& l = p.launch;
    const Units3 back = p.direction * -1.0f;
    if (l.weapon && l.shooter && !is_actor(l.shooter) && c.actor) {
        const uint32_t ench = l.enchantment ? l.enchantment : l.weapon->enchantment;
        if (ench) {
            apply_spell(p, ench, c.ref, c.position, back, l.casting_source);
        }
    }
    if (!l.spell || l.no_magic) {
        return false;
    }
    if (c.layer == layer_ward && c.actor) {
        MagicHit ward;
        ward.caster = l.shooter;
        ward.spell = l.spell;
        ward.target = c.ref;
        ward.projectile = p.id;
        ward.position = c.position;
        ward.effectiveness = p.power;
        if (on_ward && on_ward(ward)) {
            if (!l.concentration && !p.record->is(ProjectileType::beam)) {
                kill(p);
            }
            return true;
        }
    }
    apply_spell(p, l.spell, c.ref, c.position, back, l.casting_source);
    applied = true;
    return false;
}

Impact Projectiles::make_impact(const Projectile& p, const ProjectileContact& c) {
    Impact i;
    i.ref = c.ref;
    i.position = c.position;
    i.normal = c.normal;
    i.layer = c.layer;
    i.body = c.body;
    i.mass = c.mass;
    i.part = c.part;
    i.actor = c.actor;
    i.material = material_of(c);
    ProjectileBody b;
    if (p.launch.always_hit && p.launch.desired_target && m_world->body(p.launch.desired_target, b)) {
        i.ref = p.launch.desired_target;
        i.position = b.position + Units3{0, 0, b.height * 0.5f};
        i.normal = p.direction * -1.0f;
        i.actor = is_actor(i.ref);
    }
    if (c.layer == layer_water) {
        m_world->ripple(c.position, 0.01f);
    }
    const uint32_t ipct = impact_of(impact_set(p), i.material);
    if (const forms::FormEntry* e = ipct ? m_game.db().find(ipct) : nullptr) {
        i.result = ImpactResult(integer(Form(m_game.db(), *e), "DATA.impact_result"));
    }
    return i;
}

void Projectiles::arrow_impact(Projectile& p, Impact& i) {
    const LaunchData& l = p.launch;
    if (i.actor && i.part == k_shield_part) {
        p.touched_shield = true;
        const float chance = std::min(perk(i.ref, ep_deflect, 0.0f), 100.0f);
        if (float(random() % 101u) < chance) {
            i.result = ImpactResult::bounce;
        }
    }
    const uint32_t weapon = l.weapon ? l.weapon->form : 0;
    if (l.enchantment) {
        apply_spell(p, l.enchantment, i.ref, i.position, i.normal, k_casting_source_instant);
    }
    if (i.actor && l.shooter) {
        if (l.poison) {
            m_game.casters().instant_cast(l.shooter, l.poison, i.ref, weapon);
        }
        if (l.shooter == k_player_ref) {
            if (const uint32_t spell = m_game.character().entry_point_spell(ep_combat_hit_spell, {weapon, i.ref})) {
                m_game.casters().instant_cast(i.ref, spell, i.ref); // the victim's own, as
            }
        }
    }
    if (i.result == ImpactResult::none) {
        const forms::Database& db = m_game.db();
        const forms::FormEntry* e = i.material ? db.find(i.material) : nullptr;
        i.result = !e                                                          ? ImpactResult::destroy
                   : (integer(Form(db, *e), "FNAM") & k_material_arrows_stick) ? ImpactResult::stick
                                                                               : ImpactResult::bounce;
    }
    if (p.sound) {
        m_world->stop_sound(p.sound);
        p.sound = 0;
    }
}

bool Projectiles::add_impact(Projectile& p, const ProjectileContact& c) {
    const ProjectileRecord& r = *p.record;
    if (r.missile_like() && !p.impacts.empty() && !p.impacts.back().resolved) {
        return false;
    }
    if (r.is(ProjectileType::beam) && !p.impacts.empty()) {
        return false;
    }
    if (r.is(ProjectileType::flame) &&
        (c.ref == p.launch.shooter || std::ranges::any_of(p.impacts, [&](const Impact& i) { return i.ref == c.ref; }))) {
        return false;
    }
    Impact i = make_impact(p, c);
    if (r.missile_like()) {
        if (i.result == ImpactResult::impale && !((k_impale_layers >> i.layer) & 1)) {
            return false;
        }
        if (r.is(ProjectileType::arrow)) {
            arrow_impact(p, i);
        } else if (i.result == ImpactResult::none) {
            i.result = ImpactResult::destroy; // a spell bolt ends at what it hits unless the IPCT says otherwise
        }
    }
    p.impacts.push_back(i);
    p.impacts_processed = false;
    return true;
}

void Projectiles::hit_actor(Projectile& p, Impact& i) {
    const uint32_t shooter = p.launch.shooter;
    Combat& combat = m_game.combat();
    if (shooter && is_actor(shooter)) {
        if (!p.npc_in_combat || combat.fighter(shooter).in_combat) {
            combat.projectile_hit(shooter, i.ref, p.id, p.launch.weapon, p.weapon_damage, p.touched_shield, i.position, p.direction);
        }
    } else {
        combat.explosion_hit(0, i.ref, p.weapon_damage); // no aggressor (5.4): armor, then the target's own processing
    }
    i.resolved = true;
}

void Projectiles::handle_hits(Projectile& p, std::span<const ProjectileContact> contacts) {
    if (p.record->is(ProjectileType::cone)) {
        cone_hits(p, contacts);
        return;
    }
    bool stopped = false, on_actor = false;
    for (const ProjectileContact& c : contacts) {
        if (!can_hit(p, c)) {
            continue;
        }
        if (is_trigger(p, c)) {
            const uint32_t source = p.launch.spell ? p.launch.spell : p.launch.weapon ? p.launch.weapon->form : 0;
            m_world->hit_event(c.ref, p.launch.shooter, source, p.record->form);
            continue;
        }
        bool applied = false;
        if (pre_hit_magic(p, c, applied)) {
            return;
        }
        if (add_impact(p, c)) {
            p.impacts.back().spell_applied = applied;
            if (applied) {
                play_effects(p, p.impacts.back());
            }
            stopped = true;
            on_actor = p.impacts.back().actor;
        }
        break;
    }
    if (!stopped) {
        return;
    }
    Impact& last = p.impacts.back();
    if (on_actor && p.weapon_damage > 0.0f) {
        hit_actor(p, last);
    }
    if (p.record->missile_like() && last.result == ImpactResult::destroy) {
        p.position = last.position;
        land(p, "destroyed");
        kill(p);
    }
}

bool Projectiles::cone_clear(const Projectile& p, const ProjectileContact& c) {
    if (m_world->pick(p.origin, c.position, k_cone_los).empty()) {
        return true;
    }
    ProjectileBody b;
    if (!c.actor || !m_world->body(c.ref, b)) {
        return false;
    }
    return m_world->pick(p.origin, b.position + Units3{0, 0, b.height * 0.5f}, k_cone_los).empty() ||
           m_world->pick(p.origin, b.position + Units3{0, 0, b.height}, k_cone_los).empty();
}

void Projectiles::cone_hits(Projectile& p, std::span<const ProjectileContact> contacts) {
    const ProjectileRecord& r = *p.record;
    for (const ProjectileContact& c : contacts) {
        if (!can_hit(p, c)) {
            continue;
        }
        if (is_trigger(p, c)) {
            m_world->hit_event(c.ref, p.launch.shooter, p.launch.spell, r.form);
            continue;
        }
        if ((c.mass > 0.0f || c.layer == layer_debris_large) && std::ranges::find(p.bodies, c.body) == p.bodies.end()) {
            p.bodies.push_back(c.body);
            if (cone_clear(p, c)) {
                const float force = impulse_scale(n("fConeProjectileForceBase", 0.5f) * r.impact_force, c.layer, c.mass);
                const float linear = force * n("fConeProjectileForceMultLinear", 1.0f) * k_havok_scale;
                const float spin = force * n("fConeProjectileForceMultAngular", 0.5f) * k_havok_scale;
                m_world->push(c.body, normalized(c.position - p.origin) * linear,
                              {roll(-spin, spin), roll(-spin, spin), roll(-spin, spin)}, c.position);
            }
        }
        if (c.actor && std::ranges::none_of(p.impacts, [&](const Impact& i) { return i.ref == c.ref; })) {
            Impact i = make_impact(p, c);
            i.spell_applied = true; // here or nowhere: LOS decides
            p.impacts.push_back(i);
            p.impacts_processed = false;
            if (cone_clear(p, c)) {
                bool applied = false;
                pre_hit_magic(p, c, applied);
            }
        }
    }
}

// ---- impacts -------------------------------------------------------------------------------

bool Projectiles::impacts_ready(const Projectile& p) const {
    return !(p.tracer && p.hitscan) || p.moved >= p.range - std::abs(p.speed * 0.0333f) || p.alpha <= 0.0f;
}

float Projectiles::spell_health_damage(uint32_t spell) const {
    float damage = 0;
    if (const Spell* s = spell ? m_game.magic().spell(spell) : nullptr) {
        for (const game::SpellEffect& e : s->effects) {
            const MagicEffect* effect = m_game.magic().effect(e.effect);
            if (effect && effect->actor_value == av_health && (effect->flags & effect_detrimental)) {
                damage += e.magnitude;
            }
        }
    }
    return damage;
}

void Projectiles::process_impacts(Projectile& p) {
    p.impacts_processed = true;
    for (ChainExplosion& c : m_chains) {
        if (c.in_flight == p.id && !p.impacts.empty()) {
            c.point = p.impacts.front().position;
            c.in_flight = 0;
            if (c.target && is_actor(c.target)) {
                c.effectiveness -= n("fMagicChainExplosionEffectivenessDelta", 0.1f);
                c.visited.push_back(c.target);
                c.target = 0;
            }
        }
    }
    const LaunchData& l = p.launch;
    const bool actor_shooter = l.shooter && is_actor(l.shooter);
    for (Impact& i : p.impacts) {
        if (p.weapon_damage <= 0.0f && !l.spell) {
            continue;
        }
        if (actor_shooter && i.actor) {
            if (!i.resolved && !l.spell) {
                hit_actor(p, i);
            }
        } else {
            if (i.ref && !i.actor) {
                m_world->damage_ref(i.ref, std::max(p.weapon_damage, spell_health_damage(l.spell)), l.shooter);
            }
            if (i.actor && !i.resolved && p.weapon_damage > 0.0f) {
                hit_actor(p, i);
            }
        }
        if (l.spell && !l.no_magic && !i.spell_applied) {
            i.spell_applied = true;
            apply_spell(p, l.spell, i.ref, i.position, i.normal, l.casting_source);
        }
    }
    const ExplosionRecord* e = p.explosion ? explosion(p.explosion) : nullptr;
    if ((!e || e->has(expl_chain) || p.record->has(proj_alt_trigger)) && !p.record->is(ProjectileType::cone)) {
        for (const Impact& i : p.impacts) {
            play_effects(p, i);
            push_impact(p, i);
        }
    }
}

void Projectiles::play_effects(Projectile& p, const Impact& i) {
    if (p.effects_played) {
        return;
    }
    p.effects_played = true;
    if (i.actor && !p.launch.spell) {
        return; // weapon hits on actors get theirs from the hit path
    }
    const uint32_t ipct = impact_of(impact_set(p), i.material);
    const forms::FormEntry* e = ipct ? m_game.db().find(ipct) : nullptr;
    if (!e) {
        return;
    }
    if (p.launch.shooter) {
        m_world->noise(p.launch.shooter, int(integer(Form(m_game.db(), *e), "DATA.sound_level")));
    }
    m_game.impacts().projectile(ipct, i.position, i.normal, p.direction, i.ref, !i.actor && i.mass <= 0.0f, p.decal_size);
}

float Projectiles::impulse_scale(float force, uint8_t layer, float mass) const {
    float scale = biped_layer(layer)                ? n("fGameplayImpulseMultBiped", 0.04f)
                  : layer == layer_props            ? n("fGameplayImpulseMultProp", 0.1f)
                  : layer == layer_trap             ? n("fGameplayImpulseMultTrap", 0.15f)
                  : layer == layer_debris_large     ? n("fGameplayImpulseMultDebrisLarge", 1.0f)
                                                    : n("fGameplayImpulseMultClutter", 1.0f);
    const float min_mass = n("fGameplayImpulseMinMass", 5.0f);
    if (mass < min_mass) {
        scale *= mass / min_mass;
    }
    return scale * n("fGameplayImpulseScale", 150.0f) * force;
}

void Projectiles::push_impact(const Projectile& p, const Impact& i) {
    if (i.mass > 0.0f) {
        const float force = impulse_scale(p.record->impact_force, i.layer, i.mass);
        m_world->push(i.body, p.direction * force, {}, i.position);
    }
}

void Projectiles::act_on_result(Projectile& p) {
    switch (p.impacts.back().result) {
    case ImpactResult::bounce: bounce(p); break;
    case ImpactResult::impale:
    case ImpactResult::stick: stick(p); break;
    default:
        land(p, "destroyed");
        kill(p);
        break;
    }
}

void Projectiles::bounce(Projectile& p) {
    const Impact& i = p.impacts.back();
    const Units3 d = i.normal; // reversed flight
    const Units3 reflected = i.normal * (2.0f * dot(d, i.normal)) - d;
    p.position = i.position;
    p.velocity = reflected * (n("fArrowBounceLinearSpeed", 0.1f) * p.record->speed);
    p.flight = ArrowFlight::bounced;
    p.limited = true;
    m_limited.push_back(p.id);
    m_world->trail(p.id, false);
    land(p, "bounced");
}

void Projectiles::leave_in_world(Projectile& p) {
    if (!p.limited) {
        p.limited = true;
        m_limited.push_back(p.id);
    }
    if (p.record->has(proj_can_pick_up) && p.launch.ammo) {
        Stack s;
        s.form = p.launch.ammo->form;
        s.count = 1;
        const float u = units::k_skyrim_unit_m;
        p.placed = m_game.place(s, {p.position[0] * u, p.position[1] * u, p.position[2] * u});
    }
}

void Projectiles::stick(Projectile& p) {
    const Impact& i = p.impacts.back();
    if (i.actor) {
        stick_actor(p, i);
        return;
    }
    const bool bolt = p.launch.ammo && !(p.launch.ammo->weapon_flags & k_ammo_non_bolt);
    const float depth = bolt ? n("fCombatBoltStickDepth", 5.0f) : n("fCombatMissileStickDepth", 20.0f);
    p.position = i.position + p.direction * depth;
    p.flight = ArrowFlight::stuck;
    p.stuck_at = m_time;
    ProjectileBody b;
    if (i.ref && i.mass > 0.0f && m_world->body(i.ref, b)) {
        p.follows = i.ref;
        p.offset = p.position - b.position;
    }
    m_world->move_visual(p.id, p.position, p.heading, p.pitch);
    m_world->trail(p.id, false);
    leave_in_world(p);
    land(p, "stuck");
}

void Projectiles::stick_actor(Projectile& p, const Impact& i) {
    m_world->trail(p.id, false);
    const Item* ammo = p.launch.ammo;
    const uint32_t bound = m_game.keyword("WeapTypeBoundArrow");
    if (ammo && !(ammo->weapon_flags & k_ammo_non_playable) && !(bound && ammo->has_keyword(bound))) {
        float chance = n("iArrowInventoryChance", 33.0f);
        if (p.launch.shooter == k_player_ref) {
            chance = perk(k_player_ref, ep_recover_arrow, chance, {i.ref});
        }
        if (float(random() % 100u) < chance) {
            Inventory& inventory = i.ref == k_player_ref ? m_game.player().inventory : m_game.container(i.ref);
            inventory.add(ammo->form, 1);
        }
    }
    ProjectileBody b;
    if (!p.model.empty() && m_world->body(i.ref, b)) {
        const bool bolt = ammo && !(ammo->weapon_flags & k_ammo_non_bolt);
        const float depth = bolt ? n("fCombatBoltStickDepth", 5.0f) : n("fCombatMissileStickDepth", 20.0f);
        const Units3 at = i.position + p.direction * depth;
        const auto ring = size_t(std::max(n("iMaxAttachedArrows", 3.0f), 0.0f));
        const auto on_target = [&] {
            return size_t(std::ranges::count_if(m_attached, [&](const AttachedArrow& a) { return a.target == i.ref; }));
        };
        while (on_target() >= ring && ring > 0) {
            const auto oldest = std::ranges::find_if(m_attached, [&](const AttachedArrow& a) { return a.target == i.ref; });
            m_world->destroy_visual(oldest->visual);
            m_attached.erase(oldest);
        }
        if (ring > 0) {
            AttachedArrow a;
            a.visual = m_next_ref++;
            a.target = i.ref;
            a.offset = at - b.position;
            a.heading = p.heading;
            a.pitch = p.pitch;
            m_world->create_visual(a.visual, p.model, at, a.heading, a.pitch, 1.0f);
            m_attached.push_back(a);
        }
    }
    land(p, "stuck in an actor");
    kill(p);
}

// ---- detonation ----------------------------------------------------------------------------

bool Projectiles::proximity(Projectile& p) {
    const float mult = m_world->exterior() ? n("fMineExteriorRadiusMult", 1.6f) : 1.0f;
    const float radius = p.record->proximity * mult;
    const uint32_t shooter = p.launch.shooter;
    Combat& combat = m_game.combat();
    const auto near_mine = [&](uint32_t ref) {
        ProjectileBody b;
        return ref != shooter && m_world->body(ref, b) && length(b.position - p.position) < radius &&
               !combat.fighter(ref).dead() && !(shooter && m_world->allied(shooter, ref));
    };
    if (std::ranges::any_of(m_world->actors(), near_mine)) {
        return true;
    }
    if (p.no_perk_roll || !near_mine(k_player_ref)) {
        return false;
    }
    if (int(random() % 100u) >= int(perk(k_player_ref, ep_mine_chance, 100.0f, {p.record->form}))) {
        p.no_perk_roll = true;
        return false;
    }
    return true;
}

void Projectiles::check_explode(Projectile& p, float dt) {
    if (!p.explosion || p.disarmed || p.exploded) {
        return;
    }
    const ProjectileRecord& r = *p.record;
    if (!r.has(proj_alt_trigger)) {
        if (!p.impacts.empty() && impacts_ready(p)) {
            detonate(p);
        }
        return;
    }
    if (p.timer >= std::numeric_limits<float>::max()) { // not armed
        if (r.proximity > 0.0f && proximity(p)) {
            p.timer = n("fMinesDelayMin", 0.2f);
        } else {
            return;
        }
    }
    p.timer -= dt;
    p.beep -= dt;
    if (p.beep <= 0.0f) {
        const float slow = n("fMinesBlinkSlow", 0.25f), fast = n("fMinesBlinkFast", 0.1f), last = n("fMinesBlinkMax", 2.0f);
        p.beep = p.timer < last ? fast + (slow - fast) * std::max(p.timer, 0.0f) / last : slow;
        m_world->blink(p.id);
        m_world->play_sound(r.countdown_sound, &p.position);
    }
    if (p.timer <= 0.0f) {
        detonate(p);
    }
}

void Projectiles::detonate(Projectile& p) {
    const ProjectileRecord& r = *p.record;
    const LaunchData& l = p.launch;
    p.exploded = true;
    ExplosionSpawn s;
    s.form = p.explosion;
    s.source = l.shooter;
    s.position = p.position;
    s.heading = p.heading;
    s.pitch = p.pitch;
    s.scale = l.scale;
    s.area = l.area;
    s.spell = l.spell;
    s.weapon = l.weapon;
    if (!p.impacts.empty()) {
        const Impact& first = p.impacts.front();
        s.position = first.position;
        s.hit_ref = first.actor ? first.ref : 0;
        if (r.missile_like()) {
            s.normal = first.normal;
        }
    }
    const SpellEffect* effect = first_effect(l.spell);
    if (effect && l.dual_cast) {
        s.scale = effect->dual_scale;
    }
    if (const Spell* spell = l.spell ? m_game.magic().spell(l.spell) : nullptr; spell && !spell->effects.empty()) {
        const MagicEffect* first = m_game.magic().effect(spell->effects[0].effect);
        s.friendly = first && !(first->flags & effect_hostile);
    }
    s.projectile = l.projectile;
    s.casting_source = l.casting_source;
    s.power = p.power;
    const uint32_t id = explode(s);
    const bool lobber = r.is(ProjectileType::lobber);
    if (lobber && on_magic_hit && l.spell) {
        MagicHit hit;
        hit.caster = l.shooter;
        hit.spell = l.spell;
        hit.explosion = id;
        hit.position = s.position;
        hit.area = l.area;
        hit.effectiveness = p.power;
        on_magic_hit(hit);
    }
    if (lobber || (!p.hitscan && (r.missile_like() || r.is(ProjectileType::cone)))) {
        land(p, "exploded");
        kill(p);
    }
}

uint32_t Projectiles::explode(const ExplosionSpawn& spawn) {
    const ExplosionRecord* r = explosion(spawn.form);
    if (!r) {
        return 0;
    }
    auto e = std::make_unique<Explosion>();
    e->id = m_next_ref++;
    e->record = r;
    e->spawn = spawn;
    const float base = r->has(expl_radius_in_units) ? r->radius : r->radius * n("fBSUnitsPerFoot", 22.0f);
    e->radius = (spawn.area > 0.0f ? spawn.area * n("fMagicUnitsPerFoot", 22.0f) : base) * spawn.scale;
    e->image_radius = r->image_radius * spawn.scale;
    const bool level = r->has(expl_world_orientation);
    m_world->create_visual(e->id, r->model, spawn.position, level ? 0.0f : spawn.heading, level ? 0.0f : spawn.pitch,
                           spawn.scale);
    if (r->sound1 || r->sound2) { // The game shakes as its sounds start
        m_world->shake_camera(spawn.position, n("fCameraShakeExplosionDistMult", 0.001f) * e->radius);
    }
    const uint32_t id = e->id;
    m_explosions.push_back(std::move(e));
    return id;
}

float Projectiles::explosion_damage(const Explosion& e) {
    float damage = e.record->damage * (n("fDamageGunWeapCondBase", 0.66f) + n("fDamageGunWeapCondMult", 0.34f));
    const Item* w = e.spawn.weapon;
    const uint32_t source = e.spawn.source;
    if (w && w->skill >= av_one_handed && w->skill <= av_enchanting && source && is_actor(source)) {
        const bool player = source == k_player_ref;
        const float low = player ? n("fDamagePCSkillMin", 1.0f) : n("fDamageSkillMin", 1.0f);
        const float high = player ? n("fDamagePCSkillMax", 1.5f) : n("fDamageSkillMax", 3.0f);
        damage *= low + (high - low) * m_game.actor_values(source).current(w->skill) / 100.0f;
    }
    return damage;
}

void Projectiles::start_explosion(Explosion& e) {
    const ExplosionRecord& r = *e.record;
    const ExplosionSpawn& s = e.spawn;
    e.started = true;
    if (trace) {
        log::info("explosion {}: radius {:.0f}, damage {:.1f}", r.editor_id, e.radius, explosion_damage(e));
    }
    e.hit_time = m_world->hit_time(r.model);
    e.end_time = std::max(3.0f, m_world->sequence_length(r.model));
    // Inside the swap power the flash IMAD 0x166 would take over, but it isn't in the load order: the model just doesn't
    // show.
    if (ProjectileBody player; m_world->body(k_player_ref, player) && !r.has(expl_no_flash) &&
                               falloff(length(player.position - s.position), e.radius) >
                                   n("fExplosionImageSpaceSwapPower", 0.6f)) {
        m_world->hide_visual(e.id, true);
    }
    if (r.light) {
        m_world->set_light(e.id, r.light, s.position, 1.0f);
    }
    m_world->play_sound(r.sound1, &s.position);
    m_world->play_sound(r.sound2, &s.position);
    if (s.source) {
        m_world->noise(s.source, r.sound_level);
    }
    if (r.has(expl_chain)) {
        // The constructors,: from a projectile the next hops are that projectile's, else the record's.
        ChainExplosion c;
        c.spawn = s;
        if (!c.spawn.projectile) {
            c.spawn.projectile = r.spawn_projectile;
        }
        c.point = s.position;
        c.radius = e.radius;
        if (s.hit_ref) {
            c.visited.push_back(s.hit_ref);
            c.effectiveness -= n("fMagicChainExplosionEffectivenessDelta", 0.1f);
        }
        m_chains.push_back(std::move(c));
    }
    if (r.placed_object) {
        if (explosion(r.placed_object)) {
            ExplosionSpawn next = s;
            next.form = r.placed_object;
            next.area = 0;
            explode(next);
        } else {
            m_world->place_object(r.placed_object, s.position, s.heading, s.source);
        }
    }
    float water = 0;
    const Units3 probe = s.position + Units3{0, 0, e.radius * n("fExplosionWaterRadiusRatio", 0.1f)};
    if (m_world->water_height(probe, water) && probe[2] < water) {
        const float splash = n("fExplosionSplashRadius", 250.0f);
        m_world->splash({s.position[0], s.position[1], water}, splash > 0.0f ? e.radius / splash : 1.0f);
    }
}

// the nearest contact a ray from the centre reaches is the only one that plays.
void Projectiles::explosion_surface(Explosion& e) {
    const ExplosionSpawn& s = e.spawn;
    float water = 0;
    const bool in_water = m_world->water_height(s.position, water) && s.position[2] < water;
    std::vector<const ProjectileContact*> order;
    for (const ProjectileContact& c : e.found) {
        if (!c.actor) {
            order.push_back(&c);
        }
    }
    std::ranges::sort(order, {}, [&](const ProjectileContact* c) { return length(c->position - s.position); });
    const Units3 normal = length(s.normal) > 0.0f ? s.normal : Units3{0, 0, 1};
    for (const ProjectileContact* c : order) {
        const bool close = length(c->position - s.position) < 1.0f;
        const Units3 from = close ? s.position + normal * 32.0f : s.position;
        const Units3 to = close ? s.position - normal * 32.0f : c->position;
        for (const ProjectileContact& hit : m_world->pick(from, to, k_explosion_los)) {
            if (hit.ref == c->ref) {
                m_game.impacts().explosion(*e.record, s.position, hit, in_water);
                return;
            }
        }
    }
    // The land: the overlap gives its body, no point on it.
    if (const auto hits = m_world->pick(s.position + normal * 32.0f, s.position - normal * 32.0f, k_explosion_los); !hits.empty()) {
        m_game.impacts().explosion(*e.record, s.position, hits.front(), in_water);
    }
}

Units3 Projectiles::mid_height(uint32_t ref) const {
    ProjectileBody b;
    return m_world->body(ref, b) ? b.position + Units3{0, 0, b.height * 0.5f} : Units3{};
}

// the nearest it can reach from where it stands.
bool Projectiles::chain_choose(ChainExplosion& c) {
    const Units3 centre = c.target ? mid_height(c.target) : c.point;
    if (c.target) {
        c.visited.push_back(c.target);
        c.target = 0;
    }
    float best = 3.4e38f;
    for (const ProjectileContact& hit : m_world->overlap(centre, c.radius, m_world->layer_row(layer_explosion))) {
        if (!hit.actor || hit.ref == c.spawn.source || std::ranges::find(c.visited, hit.ref) != c.visited.end()) {
            continue;
        }
        ProjectileBody b;
        if (!m_world->body(hit.ref, b) || b.ghost || m_game.combat().fighter(hit.ref).dead()) {
            continue;
        }
        const Units3 to = b.position + Units3{0, 0, b.height * 0.5f};
        const float away = length(to - centre);
        // the sight starts 32 units on its way.
        const Units3 from = centre + (to - centre) * (32.0f / std::max(away, 32.0f));
        const auto seen = m_world->pick(from, to, k_explosion_los);
        if (away < best && seen.empty()) {
            best = away;
            c.target = hit.ref;
        }
    }
    return c.target != 0;
}

bool Projectiles::chain_launch(ChainExplosion& c) {
    const Units3 to = mid_height(c.target) - c.point;
    LaunchData l;
    l.origin = c.point;
    l.projectile = c.spawn.projectile;
    l.shooter = c.spawn.source;
    l.actor_cause = c.spawn.source;
    l.desired_target = c.target;
    l.spell = c.spawn.spell;
    l.casting_source = c.spawn.casting_source;
    l.power = c.spawn.power;
    l.heading = std::atan2(to[0], to[1]);
    l.pitch = -std::atan2(to[2], std::hypot(to[0], to[1]));
    l.force_new = true;
    l.use_origin = true;
    c.in_flight = launch(l);
    c.waited = 0;
    return c.in_flight != 0;
}

// ChainExplosion::Update.
void Projectiles::update_chain(ChainExplosion& c, float dt) {
    if (c.effectiveness <= 0.0f || !c.spawn.projectile) {
        c.done = true;
    } else if (!c.target) {
        c.done = !chain_choose(c) || !chain_launch(c);
    } else if (!is_actor(c.target)) {
        c.done = true;
    } else if (!c.in_flight) {
        c.done = !chain_choose(c) || !chain_launch(c);
    } else {
        c.waited += dt;
        c.done = c.waited >= 1.0f;
    }
}

void Projectiles::find_targets(Explosion& e) {
    e.found = m_world->overlap(e.spawn.position, e.radius, m_world->layer_row(layer_explosion));
    if (std::ranges::none_of(e.found, [](const ProjectileContact& c) { return c.ref == k_player_ref; })) {
        ProjectileContact player;
        player.ref = k_player_ref;
        player.actor = true;
        player.layer = layer_charcontroller;
        ProjectileBody b;
        if (m_world->body(k_player_ref, b)) {
            player.position = b.position;
            e.found.push_back(player);
        }
    }
}

bool Projectiles::explosion_los(const Explosion& e, const ProjectileBody& target) {
    if (e.record->has(expl_ignore_los)) {
        return true;
    }
    const Units3 from = e.spawn.position + e.spawn.normal * n("fExplosionLOSBufferDistance", 24.0f);
    const Units3 to = target.position + Units3{0, 0, target.height * 0.5f};
    const bool ceiling = e.spawn.normal[2] < -0.98f;
    const float buffer = n("fExplosionLOSBuffer", 3.0f);
    return std::ranges::all_of(m_world->pick(from, to, k_explosion_los), [&](const ProjectileContact& c) {
        return ceiling && length(c.position - from) < buffer;
    });
}

bool Projectiles::knock_by_formula(uint32_t actor, float damage) {
    ActorValues& values = m_game.actor_values(actor);
    const float health = values.current(av_health), base = values.base(av_health);
    if (health <= 0.0f || health <= damage) {
        return false;
    }
    if (base > 0.0f && 100.0f * damage / base > n("fKnockdownBaseHealthThreshold", 75.0f)) {
        return true;
    }
    if (100.0f * damage / health <= n("fKnockdownCurrentHealthThreshold", 50.0f)) {
        return false;
    }
    const float agility = n("fKnockdownAgilMult", 1.0f) * 50.0f + n("fKnockdownAgilBase", 0.0f);
    const float value = damage * n("fKnockdownDamageMult", 0.3f) + n("fKnockdownDamageBase", 0.0f);
    const float chance = std::min(agility != 0.0f ? value / agility : 1.0f, n("fKnockdownChance", 0.25f));
    return float(random() % 1000u) < chance * 1000.0f;
}

void Projectiles::apply_to_targets(Explosion& e) {
    const ExplosionRecord& r = *e.record;
    const ExplosionSpawn& s = e.spawn;
    const float damage = explosion_damage(e);
    Combat& combat = m_game.combat();
    std::vector<uint32_t> done;
    for (const ProjectileContact& c : e.found) {
        ProjectileBody b;
        if (!c.ref || std::ranges::find(done, c.ref) != done.end() || (c.ref == s.source && c.ref != k_player_ref)) {
            continue;
        }
        done.push_back(c.ref);
        if (!m_world->body(c.ref, b)) {
            b.position = c.position;
        }
        if (b.ghost) {
            continue;
        }
        const float d = length(b.position - s.position);
        const float f = falloff(d, e.radius);
        const float image = e.image_radius > 0.0f ? falloff(d, e.image_radius) : 1.0f;
        if (c.ref == k_player_ref && image > 0.0f && r.image_space) {
            m_world->flash(r.image_space, image);
        }
        if (f <= 0.0f) {
            continue;
        }
        m_world->hit_event(c.ref, s.source, s.weapon ? s.weapon->form : r.form, e.id);
        if (!c.actor) {
            m_world->damage_ref(c.ref, damage * f, s.source);
            continue;
        }
        if (!explosion_los(e, b)) {
            continue;
        }
        if (!s.friendly && c.ref != s.source) {
            combat.explosion_hit(s.source, c.ref, damage * f);
        }
        if (r.enchantment && on_magic_hit && !(c.ref == s.source && hostile(r.enchantment))) {
            MagicHit hit;
            hit.caster = s.source;
            hit.spell = r.enchantment;
            hit.target = c.ref;
            hit.explosion = e.id;
            hit.casting_source = k_casting_source_instant;
            hit.position = s.position;
            on_magic_hit(hit);
        }
        if (combat.fighter(c.ref).dead()) {
            m_world->ragdoll(c.ref);
            continue;
        }
        const bool always = r.has(expl_knock_always), formula = r.has(expl_knock_formula);
        const bool knock = always && formula ? c.ref != k_player_ref : always || (formula && knock_by_formula(c.ref, damage * f));
        if (knock && f > n("fExplosionForceKnockdownMinimum", 0.15f)) {
            m_world->knock(c.ref, s.position);
        }
    }
}

bool Projectiles::hostile(uint32_t spell) const {
    const Spell* s = m_game.magic().spell(spell);
    return s && std::ranges::any_of(s->effects, [&](const game::SpellEffect& e) {
        const MagicEffect* effect = m_game.magic().effect(e.effect);
        return effect && (effect->flags & effect_hostile);
    });
}

void Projectiles::push_bodies(Explosion& e) {
    const ExplosionRecord& r = *e.record;
    if (r.force <= 0.0f) {
        return;
    }
    const uint32_t hit = e.spawn.hit_ref;
    const float linear = n("fExplosionForceMultLinear", 1.0f) * k_havok_scale;
    const float spin = n("fExplosionForceMultAngular", 1.0f) * k_havok_scale;
    for (const ProjectileContact& c : e.found) {
        if ((c.mass <= 0.0f && c.layer != layer_debris_large) || (r.has(expl_push_source_only) && c.ref != hit)) {
            continue;
        }
        Units3 d = normalized(c.position - e.spawn.position);
        if (biped_layer(c.layer)) {
            d[2] += n("fExplosionForceClutterUpBias", 0.5f);
        }
        const float force = c.ref && c.ref == hit
                                ? impulse_scale(r.force * n("fExplosionSourceRefMult", 3.0f), c.layer, c.mass)
                                : std::min(impulse_scale(r.force, c.layer, c.mass), n("fExplosionMaxImpulse", 8000.0f));
        const float a = force * spin;
        m_world->push(c.body, d * (force * linear), {roll(-a, a), roll(-a, a), roll(-a, a)}, c.position);
    }
}

// ---- the manager ---------------------------------------------------------------------------

void Projectiles::limit_pass() {
    const auto find = [&](uint32_t id) {
        const auto it = std::ranges::find_if(m_live, [id](const auto& p) { return p->id == id; });
        return it == m_live.end() ? nullptr : it->get();
    };
    std::erase_if(m_limited, [&](uint32_t id) {
        const Projectile* p = find(id);
        return !p || p->dead;
    });
    const auto limit = size_t(n("iProjectileMaxRefCount", 15.0f));
    while (m_limited.size() > limit) {
        kill(*find(m_limited.front()));
        m_limited.pop_front();
    }
}

void Projectiles::wobble() {
    const float duration = n("fArrowWobbleDuration", 1.0f), curve = n("fArrowWobbleCurve", 1.0f);
    const float amplitude = n("fArrowWobbleAmplitude", 0.1f), period = n("fArrowWobblePeriod", 0.05f);
    for (const auto& owned : m_live) {
        Projectile& p = *owned;
        if (p.dead || p.stuck_at < 0.0) {
            continue;
        }
        const float t = float(m_time - p.stuck_at);
        const float u = p.power > 0.0f ? t / (duration * p.power) : 1.0f;
        const float a = (1.0f - u) / (curve + u) * amplitude * p.power * curve;
        if (a <= 0.0f) {
            p.stuck_at = -1.0;
            m_world->move_visual(p.id, p.position, p.heading, p.pitch);
            continue;
        }
        m_world->move_visual(p.id, p.position, p.heading, p.pitch + std::sin(2.0f * k_pi * t / period) * a);
    }
}

void Projectiles::attached(float dt) {
    const float max_age = n("fAttachedArrowAgeMax", 10.0f);
    std::erase_if(m_attached, [&](AttachedArrow& a) {
        a.age += dt;
        ProjectileBody b;
        if (a.age > max_age || !m_world->body(a.target, b)) {
            m_world->destroy_visual(a.visual);
            return true;
        }
        m_world->move_visual(a.visual, b.position + a.offset, a.heading, a.pitch);
        return false;
    });
}

void Projectiles::update(float dt) {
    if (dt <= 0.0f) {
        return;
    }
    m_time += dt;
    const uint32_t space = m_world->space();
    for (size_t i = 0; i < m_live.size(); ++i) { // a step may launch: refer by index
        Projectile& p = *m_live[i];
        const bool pending = !p.impacts.empty() && !p.impacts_processed;
        if (!p.dead && (p.space != space || !m_world->loaded(p.position))) {
            land(p, "left the loaded world");
            kill(p);
        }
        if (p.dead) {
            if (pending) {
                process_impacts(p);
            }
            continue;
        }
        if (!p.has_3d) {
            if (!m_world->visual_ready(p.id)) {
                continue;
            }
            first_frame(p);
        }
        if (pending && impacts_ready(p)) {
            process_impacts(p);
        }
        step(p, dt);
        if (!p.dead) {
            p.age += dt;
            fade(p, dt);
        }
    }
    std::erase_if(m_live, [this](const std::unique_ptr<Projectile>& p) {
        if (!p->dead || (!p->impacts.empty() && !p->impacts_processed)) {
            return false;
        }
        m_world->destroy_visual(p->id);
        return true;
    });
    limit_pass();
    wobble();
    attached(dt);
    for (size_t i = 0; i < m_chains.size(); ++i) { // a launch can't add one, but index as the rest do
        update_chain(m_chains[i], dt);
    }
    std::erase_if(m_chains, [](const ChainExplosion& c) { return c.done; });
    for (size_t i = 0; i < m_explosions.size(); ++i) { // a placed explosion joins the list
        Explosion& e = *m_explosions[i];
        if (!e.started) {
            if (!m_world->visual_ready(e.id)) {
                continue;
            }
            start_explosion(e);
        }
        if (!e.applied && e.age >= e.hit_time) {
            e.applied = true;
            find_targets(e);
            explosion_surface(e);
            apply_to_targets(e);
        } else if (e.applied && !e.pushed) {
            e.pushed = true;
            push_bodies(e);
        }
        e.age += dt;
    }
    std::erase_if(m_explosions, [this](const std::unique_ptr<Explosion>& e) {
        if (e->age < e->end_time) {
            return false;
        }
        if (e->record->light) {
            m_world->remove_light(e->id);
        }
        m_world->destroy_visual(e->id);
        return true;
    });
    m_game.impacts().update(dt);
}

} // namespace spud::game
