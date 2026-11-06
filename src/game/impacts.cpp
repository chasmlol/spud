#include "game/impacts.hpp"

#include "game/game.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spud::game {

namespace {

using forms::Form;
using forms::Value;

constexpr float k_two_pi = 2.0f * std::numbers::pi_v<float>;
constexpr uint32_t k_default_impact_set = 0x276;    // DefaultImpactDataSet
constexpr uint32_t k_default_body_part_data = 0x1D; // DefaultBodyPartData
constexpr uint32_t k_get_hit_modifier = 0x162;      // GetHit, 0.25 s [R]
constexpr uint32_t k_race_flies = 0x80, k_race_no_spray = 0x2000, k_race_no_decal = 0x4000;
constexpr uint32_t k_race_no_weapon_blood = 0x2000000;
constexpr float k_blood_reach = 512.0f, k_blood_depth = 48.0f, k_default_placement = 16.0f;
constexpr uint8_t k_layer_decal = 43;
constexpr uint64_t k_hazard_drop_layers = 141314; // the row sets on layer 43: Static, Water, Terrain, Ground

float dot(const Units3& a, const Units3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Units3 add(const Units3& a, const Units3& b, float s = 1.0f) { return {a[0] + b[0] * s, a[1] + b[1] * s, a[2] + b[2] * s}; }
Units3 scaled(const Units3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float length(const Units3& a) { return std::sqrt(dot(a, a)); }
Units3 unit(const Units3& a) {
    const float l = length(a);
    return l > 1e-6f ? scaled(a, 1.0f / l) : Units3{0, 0, 0};
}
Units3 cross(const Units3& a, const Units3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

float real(const Form& f, std::string_view path) {
    const Value* v = f.get(path);
    return !v ? 0.0f : v->kind == Value::Kind::real ? float(v->real) : float(v->integer);
}
uint32_t form_of(const Form& f, std::string_view path) {
    const Value* v = f.get(path);
    return v ? v->form : 0;
}
uint32_t integer(const Form& f, std::string_view path) {
    const Value* v = f.get(path);
    return v ? uint32_t(v->integer) : 0;
}

// a frame with `direction` as its third column, the model's Z.
DecalAxes basis(const Units3& direction) {
    const Units3 d = unit(direction);
    const float x = std::abs(d[0]), y = std::abs(d[1]), z = std::abs(d[2]);
    const Units3 t = x > y || x > z ? (y > x || y > z ? Units3{-d[1], d[0], 0} : Units3{-d[2], 0, d[0]}) : Units3{0, -d[2], d[1]};
    const Units3 v = unit(cross(t, d));
    return {unit(cross(v, d)), v, d};
}

} // namespace

Impacts::Impacts(Game& game) : m_game(game) {}
Impacts::~Impacts() = default;

float Impacts::n(std::string_view name, float fallback) const { return m_game.settings().number(name, fallback); }
float Impacts::roll(float low, float high) { return low + (high - low) * float(m_random() >> 8) * (1.0f / 16777216.0f); }
ProjectileWorld& Impacts::world() { return m_game.projectiles().world(); }

const ImpactRecord* Impacts::record(uint32_t form) {
    const auto [it, added] = m_records.try_emplace(form);
    if (added) {
        const forms::Database& db = m_game.db();
        const forms::FormEntry* e = form ? db.find(form) : nullptr;
        if (e) {
            const Form f(db, *e);
            auto r = std::make_unique<ImpactRecord>();
            r->form = form;
            if (const Value* m = f.get("MODL"); m && !m->text.empty()) {
                r->model = model_path(m->text);
            }
            r->duration = real(f, "DATA.effect_duration");
            r->orientation = integer(f, "DATA.effect_orientation");
            r->angle = real(f, "DATA.angle_threshold");
            r->placement = real(f, "DATA.placement_radius");
            r->min_width = real(f, "DODT.min_width");
            r->max_width = real(f, "DODT.max_width");
            r->depth = real(f, "DODT.depth");
            r->shininess = real(f, "DODT.shininess");
            r->decal_flags = uint8_t(integer(f, "DODT.flags"));
            if (f.get("DODT")) {
                r->color = {real(f, "DODT.color.r") / 255.0f, real(f, "DODT.color.g") / 255.0f,
                            real(f, "DODT.color.b") / 255.0f};
            }
            r->texture_set = form_of(f, "DNAM");
            r->sound1 = form_of(f, "SNAM");
            r->sound2 = form_of(f, "NAM1");
            r->hazard = form_of(f, "NAM2");
            it->second = std::move(r);
        }
    }
    return it->second.get();
}

uint32_t Impacts::impact_of(uint32_t set, uint32_t material) const {
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = set ? db.find(set) : nullptr;
    if (!e) {
        return 0;
    }
    const Form f(db, *e);
    for (const Value* v : f.all("PNAM")) {
        const Value* m = v->member("material");
        const Value* i = v->member("impact");
        if (m && i && m->form == material) {
            return i->form;
        }
    }
    return 0;
}

// Section 5 with the frame-time bits left out: they are off at full frame rate, which leaves the distance.
uint32_t Impacts::distance_bits(const Units3& at) {
    return world().camera_distance(at) > n("fGunParticleCameraDistance", 1024.0f) ? 3u : 0u;
}

DecalAxes Impacts::spun_basis(const Units3& direction) {
    const DecalAxes b = basis(direction);
    const float spin = roll(0.0f, k_two_pi), c = std::cos(spin), s = std::sin(spin);
    return {add(scaled(b[0], c), b[1], s), add(scaled(b[1], c), b[0], -s), b[2]};
}

int Impacts::roll_subtexture(const ImpactRecord& r) { return r.decal_flags & decal_no_subtextures ? -1 : int(m_random() % 4u); }

void Impacts::particle(const ImpactRecord& r, const Units3& at, const Units3& direction) {
    if (!r.model.empty()) {
        world().temp_particle(r.model, at, spun_basis(direction), 1.0f, r.duration);
    }
}

void Impacts::add_decal(const ImpactRecord& r, const Units3& at, const DecalAxes& axes, float width, float height, float depth,
                        float placement, int subtexture, uint32_t ref) {
    // No DNAM, no decal; a record without DODT has no size.
    if (!r.texture_set || width <= 0.0f) {
        return;
    }
    DecalRequest d;
    d.impact = &r;
    d.ref = ref;
    d.position = at;
    d.axes = axes;
    d.width = width;
    d.height = height;
    d.depth = depth;
    d.placement = placement;
    d.subtexture = subtexture;
    world().add_decal(d);
}

void Impacts::projectile(uint32_t ipct, const Units3& at, const Units3& normal, const Units3& velocity, uint32_t ref, bool decal,
                         float& size) {
    const ImpactRecord* r = record(ipct);
    if (!r) {
        return;
    }
    const uint32_t bits = distance_bits(at);
    const Units3 v = unit(scaled(velocity, -1.0f));
    const Units3 direction = r->orientation == 1   ? v
                             : r->orientation == 2 ? add(scaled(normal, 2.0f * dot(normal, v)), v, -1.0f)
                                                   : normal;
    particle(*r, at, direction); // bit 8 only, and only frame time sets that
    if (!(bits & 1u)) {
        world().play_sound(r->sound1, &at);
    }
    if (!(bits & 2u) && decal) {
        size = size > 0.0f ? size : roll(r->min_width, r->max_width);
        add_decal(*r, at, spun_basis(normal), size, size, r->depth, k_default_placement, roll_subtexture(*r), ref);
    }
    if (r->hazard) {
        HazardSpawn h;
        h.form = r->hazard;
        h.position = at;
        h.normal = normal;
        create_hazard(h);
    }
}

void Impacts::melee_object(uint32_t attacker, const Item* weapon, bool bash, const ProjectileContact& c, const Units3& swing) {
    uint32_t set = weapon ? weapon->impact_set : 0;
    if (bash) {
        const Gear g = m_game.combat().gear(attacker);
        const Item* with = g.shield ? g.shield : weapon;
        set = with ? with->bash_impact_set : 0;
        if (const Item* fist = m_game.items().find(k_unarmed_weapon); !set && fist) {
            set = fist->bash_impact_set;
        }
    }
    const ImpactRecord* r = record(impact_of(set, c.material));
    if (!r) {
        return;
    }
    const uint32_t bits = distance_bits(c.position);
    // The weapon's direction with its normal component removed, whatever the record's orientation.
    const Units3 tangent = add(swing, c.normal, -dot(swing, c.normal));
    if (!(bits & 0xEu)) {
        particle(*r, c.position, length(tangent) > 1e-4f ? tangent : c.normal);
    }
    if (!(bits & 0x9u)) {
        world().play_sound(r->sound1, &c.position);
        world().play_sound(r->sound2, &c.position);
    }
    if (!(bits & 2u) && c.mass <= 0.0f) {
        const float width = roll(r->min_width, r->max_width);
        add_decal(*r, c.position, spun_basis(c.normal), width, width, r->depth, k_default_placement, roll_subtexture(*r), c.ref);
    }
}

void Impacts::explosion(const ExplosionRecord& e, const Units3& centre, const ProjectileContact& c, bool in_water) {
    const ImpactRecord* r = record(impact_of(e.impact_set, c.material));
    if (!r) {
        return;
    }
    const Units3 back = unit(add(centre, c.position, -1.0f));
    const Units3 direction = r->orientation == 1   ? back
                             : r->orientation == 2 ? add(scaled(c.normal, 2.0f * dot(c.normal, back)), back, -1.0f)
                                                   : c.normal;
    if (c.mass <= 0.0f) {
        const float width = roll(r->min_width, r->max_width);
        add_decal(*r, c.position, spun_basis(c.normal), width, width, r->depth, k_default_placement, roll_subtexture(*r), c.ref);
    }
    if (!in_water) {
        particle(*r, c.position, direction);
    }
    world().play_sound(r->sound1, &c.position);
    world().play_sound(r->sound2, &c.position);
}

// The attacker's equipped object's set, else the attacker's race's.
uint32_t Impacts::blood_impact_set(const HitData& hit) {
    if (hit.weapon && hit.weapon->impact_set) {
        return hit.weapon->impact_set;
    }
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = hit.attacker ? db.find(m_game.combat().fighter(hit.attacker).race) : nullptr;
    return e ? form_of(Form(db, *e), "NAM5") : 0;
}

// The severable set of the hit's body part ( [I]). Hits don't pick a location yet, so the BPTD's first part with one.
uint32_t Impacts::body_part_set(uint32_t race) const {
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = race ? db.find(race) : nullptr;
    const uint32_t data = e ? form_of(Form(db, *e), "GNAM") : 0;
    const forms::FormEntry* b = db.find(data ? data : k_default_body_part_data);
    if (!b) {
        return 0;
    }
    const Form f(db, *b);
    for (const Value* v : f.all("BPND")) {
        if (const Value* s = v->member("severable_impact_data_set"); s && s->form) {
            return s->form;
        }
    }
    return 0;
}

void Impacts::environment_blood(const HitData& hit, const Units3& direction, const DecalAxes& axes, int subtexture,
                                uint32_t material) {
    const ImpactRecord* r = record(impact_of(body_part_set(m_game.combat().fighter(hit.victim).race), material));
    if (!r || roll(0.0f, 1.0f) >= n("fCombatEnvironmentBloodChance", 0.75f)) {
        return;
    }
    const Units3 ray = unit(add(direction, Units3{0, 0, 0.5f}, -1.0f));
    const Units3 to = add(hit.position, ray, k_blood_reach);
    for (const ProjectileContact& c : world().pick(hit.position, to, world().layer_row(k_layer_decal))) {
        if (!c.actor && c.mass <= 0.0f) {
            const float width = roll(r->min_width, r->max_width);
            add_decal(*r, c.position, axes, width, width, k_blood_depth, r->placement, subtexture, c.ref);
            return;
        }
    }
}

void Impacts::blood(const HitData& hit) {
    if (n("bDisableAllGore", 0.0f) != 0.0f) {
        return;
    }
    const forms::Database& db = m_game.db();
    const uint32_t race = m_game.combat().fighter(hit.victim).race;
    const forms::FormEntry* e = race ? db.find(race) : nullptr;
    const uint32_t flags = e ? integer(Form(db, *e), "DATA.flags") : 0;
    if (flags & k_race_flies || (flags & k_race_no_spray && flags & k_race_no_decal)) {
        return;
    }
    // not a bow, staff or crossbow.
    if (hit.weapon && hit.attacker && !hit.source && !hit.has(hit_left) && !(flags & k_race_no_weapon_blood) &&
        hit.total > std::max(n("fMinBloodDamage", 1.0f), 1.0f)) {
        if (const forms::FormEntry* w = db.find(hit.weapon->form); w && w->type == forms::sig("WEAP")) {
            const uint32_t type = integer(Form(db, *w), "DNAM.animation_type");
            if (type != 7 && type != 8 && type != 9) {
                world().weapon_blood(hit.attacker, roll(0.1f, 0.4f));
            }
        }
    }
    const Units3 jitter{roll(-0.3f, 0.3f), roll(-0.3f, 0.3f), roll(-0.3f, 0.3f)};
    const Units3 direction = unit(add(scaled(hit.direction, 2.0f), jitter));
    if (hit.victim == k_player_ref) {
        const float count = roll(-1.0f, 1.0f) * n("fBloodSplatterCountRandomMargin", 0.0f) +
                            n("fBloodSplatterCountDamageMult", 0.1f) * hit.total + n("fBloodSplatterCountDamageBase", 0.0f) +
                            n("fBloodSplatterCountBase", 2.0f);
        world().screen_blood(std::max(int(count), 0));
    }
    const uint32_t material = e ? form_of(Form(db, *e), "NAM4") : 0;
    uint32_t ipct = impact_of(blood_impact_set(hit), material);
    if (!ipct) {
        ipct = impact_of(k_default_impact_set, material);
    }
    const ImpactRecord* r = record(ipct);
    if (!r) {
        return;
    }
    if (!r->model.empty()) {
        particle(*r, hit.position, direction);
    }
    // The floor's blood takes the skin decal's rotation and subtexture.
    if (!r->model.empty() || r->texture_set) {
        const DecalAxes axes = spun_basis(scaled(direction, -1.0f));
        if (r->texture_set && !(flags & k_race_no_decal)) {
            SkinDecalRequest d;
            d.impact = r;
            d.victim = hit.victim;
            d.position = hit.position;
            d.direction = direction;
            d.axes = axes;
            d.size = roll(r->min_width, r->max_width);
            d.forced = hit.attacker == k_player_ref;
            world().skin_decal(d);
        }
        environment_blood(hit, direction, axes, roll_subtexture(*r), material);
    }
}

void Impacts::player_hurt() { world().flash(k_get_hit_modifier, 1.0f); }

// ---- hazards (18) --------------------------------------------------------------------------------------------------------

const HazardRecord* Impacts::hazard_record(uint32_t form) {
    const auto [it, added] = m_hazard_records.try_emplace(form);
    if (added) {
        const forms::Database& db = m_game.db();
        if (const forms::FormEntry* e = form ? db.find(form) : nullptr) {
            const Form f(db, *e);
            auto r = std::make_unique<HazardRecord>();
            r->form = form;
            if (const Value* m = f.get("MODL"); m && !m->text.empty()) {
                r->model = model_path(m->text);
            }
            r->limit = integer(f, "DATA.limit");
            r->radius = real(f, "DATA.radius");
            r->lifetime = real(f, "DATA.lifetime");
            r->target_interval = real(f, "DATA.target_interval");
            r->flags = integer(f, "DATA.flags");
            r->spell = form_of(f, "DATA.spell");
            r->light = form_of(f, "DATA.light");
            r->sound = form_of(f, "DATA.sound");
            it->second = std::move(r);
        }
    }
    return it->second.get();
}

uint32_t Impacts::create_hazard(const HazardSpawn& s) {
    const HazardRecord* r = hazard_record(s.form);
    if (!r || (!s.skip_rate_limit && m_time - m_last_hazard < n("fHazardMinimumSpawnInterval", 0.1f))) {
        return 0;
    }
    const float radius = s.radius > 0.0f ? s.radius : r->radius;
    if (!s.skip_spacing) {
        const float reach = radius * n("fBSUnitsPerFoot", 22.0f);
        for (const auto& h : m_hazards) {
            const Units3 d = add(h->position, s.position, -1.0f);
            if (h->record == r && h->source == s.source && dot(d, d) < reach * reach * n("fHazardSpacingMult", 0.8f)) {
                return 0;
            }
        }
    }
    if (r->limit > 0 && s.source == k_player_ref) {
        std::vector<Hazard*> own;
        for (const auto& h : m_hazards) {
            if (h->record == r && h->source == s.source) {
                own.push_back(h.get());
            }
        }
        if (own.size() >= r->limit) {
            Hazard* oldest = *std::ranges::max_element(own, {}, &Hazard::age);
            oldest->lifetime = oldest->age; // told to end
        }
    }
    auto h = std::make_unique<Hazard>();
    h->id = m_next_ref++;
    h->record = r;
    h->source = s.source;
    h->position = s.position;
    h->radius = radius;
    h->lifetime = s.no_lifetime ? -1.0f : s.lifetime > 0.0f ? s.lifetime : r->lifetime;
    if (r->flags & hazard_align_normal && dot(s.normal, s.normal) > 0.0f) {
        const Units3 u = unit(s.normal);
        h->heading = std::atan2(u[0], u[1]);
        h->pitch = -std::asin(std::clamp(u[2], -1.0f, 1.0f));
    }
    world().create_visual(h->id, r->model, h->position, h->heading, h->pitch, 1.0f);
    m_last_hazard = m_time;
    const uint32_t id = h->id;
    m_hazards.push_back(std::move(h));
    return id;
}

// The actors in reach get the spell once, not if it's already on them.
void Impacts::pulse(const Hazard& h) {
    const HazardRecord& r = *h.record;
    const Spell* spell = m_game.magic().spell(r.spell);
    if (!spell) {
        return;
    }
    const float reach = h.radius * n("fBSUnitsPerFoot", 22.0f);
    const bool hostile = std::ranges::any_of(spell->effects, [&](const SpellEffect& e) {
        const MagicEffect* m = m_game.magic().effect(e.effect);
        return m && (m->flags & effect_hostile);
    });
    std::vector<uint32_t> targets = world().actors();
    targets.push_back(k_player_ref);
    for (const uint32_t ref : targets) {
        ProjectileBody b;
        if ((r.flags & hazard_player_only && ref != k_player_ref) || !world().body(ref, b) ||
            length(add(b.position, h.position, -1.0f)) > reach) {
            continue;
        }
        if (hostile && h.source && (ref == h.source || world().allied(h.source, ref))) {
            continue;
        }
        if (!m_game.magic_target(ref).has_effect_from(r.spell)) {
            m_game.casters().instant_cast(h.source ? h.source : ref, r.spell, ref);
        }
    }
}

void Impacts::update_hazard(Hazard& h, float dt) {
    const HazardRecord& r = *h.record;
    if (h.first_frame) {
        h.first_frame = false;
        if (r.flags & hazard_drop_to_ground) {
            const Units3 from = add(h.position, Units3{0, 0, 32.0f});
            const Units3 to = add(from, Units3{0, 0, -n("fHazardDropMaxDistance", 1024.0f)});
            if (const auto hits = world().pick(from, to, k_hazard_drop_layers); !hits.empty()) {
                h.position = hits.front().position;
                world().move_visual(h.id, h.position, h.heading, h.pitch);
            }
        }
        if (r.light) {
            world().set_light(h.id, r.light, h.position, 1.0f);
        }
        if (r.sound) {
            h.sound = world().play_sound(r.sound, &h.position);
        }
    }
    h.next_pulse -= dt;
    if (h.next_pulse <= 0.0f) {
        h.next_pulse += r.target_interval > 0.0f ? r.target_interval : n("fHazardDefaultTargetInterval", 0.25f);
        pulse(h);
    }
    h.age += dt;
}

void Impacts::update(float dt) {
    m_time += dt;
    for (const auto& h : m_hazards) {
        update_hazard(*h, dt);
    }
    // Past its lifetime the model's graph gets StopEffect; the plain NIFs refuse, so it goes at once.
    std::erase_if(m_hazards, [this](const std::unique_ptr<Hazard>& h) {
        if (h->lifetime < 0.0f || h->age < h->lifetime) {
            return false;
        }
        if (h->sound) {
            world().stop_sound(h->sound);
        }
        if (h->record->light) {
            world().remove_light(h->id);
        }
        world().destroy_visual(h->id);
        return true;
    });
}

} // namespace spud::game
