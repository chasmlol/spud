#include "game/magic_target.hpp"

#include "game/caster.hpp"
#include "game/game.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace spud::game {

namespace {

constexpr int k_poison_resist = 40, k_magic_resist = 44, k_ward_power = 63, k_absorb_chance = 83, k_damage_resist = 39;
constexpr int ep_ward_absorb = 71, ep_max_summons = 68, ep_slow_time = 70;
constexpr int k_art_ward_broken = 125, k_art_ward_held = 126, k_art_telekinesis = 183, k_art_absorb = 204, k_soul_sound = 133;
constexpr uint16_t k_has_keyword = 560;
constexpr uint32_t k_actor_ghost = 0x20000000; // ACBS

bool value_class(const MagicEffect& m) {
    switch (m.archetype) {
    case archetype_value_modifier: case archetype_absorb: case archetype_dual_value_modifier: case archetype_invisibility:
    case archetype_night_eye: case archetype_paralysis: case archetype_value_and_parts: case archetype_peak_value_modifier:
    case archetype_grab_actor: case archetype_enhance_weapon:
        return m.actor_value >= 0;
    default: return false;
    }
}

float units_between(const Units3& a, const Units3& b) {
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

bool per_second_source(const ActiveEffect& e) {
    return e.worn || e.item->cast_type == cast_concentration || e.item->type == spell_disease || e.item->type == spell_ability;
}

bool applies_on_start(const ActiveEffect& e) {
    // The effect item's duration, not the active effect's: that one has the taper in it, which runs on after the hit.
    return (e.setting->flags & effect_recover) || (e.source->duration == 0 && !per_second_source(e));
}

// "%s is too powerful for %s.": the printf text with its two names.
std::string fill_names(std::string text, const std::string& first, const std::string& second) {
    for (const std::string* name : {&first, &second}) {
        if (const size_t at = text.find("%s"); at != std::string::npos) {
            text.replace(at, 2, *name);
        }
    }
    return text;
}

} // namespace

MagicTarget::MagicTarget(Game& game, uint32_t ref, ActorValues& values) : m_game(&game), m_ref(ref), m_values(values) {}

bool MagicTarget::causing_damage() const {
    return std::ranges::any_of(m_effects, [](const ActiveEffect& e) {
        return e.active && e.setting && e.setting->actor_value == av_health && (e.setting->flags & effect_detrimental) && value_class(*e.setting);
    });
}

bool MagicTarget::is_actor() const { return m_game->is_actor(m_ref); }

bool MagicTarget::alive() const { return is_actor() && !m_game->combat().fighter(m_ref).dead(); }

int MagicTarget::level() const { return m_values.record().level; }

float MagicTarget::number(std::string_view name, float fallback) const { return m_game->settings().number(name, fallback); }

float MagicTarget::perk(uint32_t owner, int point, float value, std::initializer_list<uint32_t> args) const {
    return owner == k_player_ref ? m_game->character().entry_point(point, value, args) : value;
}

bool MagicTarget::is_armour(uint32_t object) const {
    const Item* item = object ? m_game->items().find(object) : nullptr;
    return item && item->kind == ItemKind::armor;
}

bool MagicTarget::ghost() const { return m_ghost || (m_values.record().acbs_flags & k_actor_ghost); }

// ---- spells ------------------------------------------------------------------------------------------------------

bool MagicTarget::has_spell(uint32_t spell) const {
    if (std::ranges::find(m_spells, spell) != m_spells.end()) {
        return true;
    }
    const forms::Database& db = m_game->db();
    for (uint32_t owner : {m_values.record().npc, m_values.record().race}) {
        if (const forms::FormEntry* e = db.find(owner)) {
            for (const forms::Value* s : forms::Form(db, *e).all("SPLO")) {
                if (s->form == spell) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::vector<uint32_t> MagicTarget::all_spells() const {
    std::vector<uint32_t> out = m_spells;
    const forms::Database& db = m_game->db();
    const uint16_t level = uint16_t(std::max(m_values.record().level, 1));
    for (uint32_t owner : {m_values.record().npc, m_values.record().race}) {
        const forms::FormEntry* e = db.find(owner);
        if (!e) {
            continue;
        }
        for (const forms::Value* s : forms::Form(db, *e).all("SPLO")) {
            std::vector<Rolled> rolled;
            m_game->leveled().roll(s->form, 1, level, rolled);
            for (const Rolled& r : rolled) {
                if (std::ranges::find(out, r.form) == out.end()) {
                    out.push_back(r.form);
                }
            }
        }
    }
    return out;
}

bool MagicTarget::add_spell(uint32_t spell) {
    const Spell* s = m_game->magic().spell(spell);
    if (!s) {
        return false;
    }
    bool applied = false;
    if (s->permanent() && !has_effect_from(spell)) {
        applied = apply(spell, m_ref);
        if (applied && s->type == spell_disease && m_ref == k_player_ref && m_game->character().events.disease_contracted) {
            m_game->character().events.disease_contracted(spell);
        }
    }
    if (has_spell(spell)) {
        return applied;
    }
    m_spells.push_back(spell);
    return true;
}

bool MagicTarget::remove_spell(uint32_t spell) {
    const Spell* s = m_game->magic().spell(spell);
    bool result = false;
    if (s && s->permanent()) {
        result = dispel_spell(spell);
        if (s->type == spell_disease && m_ref == k_player_ref && m_game->character().events.disease_cured) {
            m_game->character().events.disease_cured(spell);
        }
    }
    const auto it = std::ranges::find(m_spells, spell);
    if (it == m_spells.end()) {
        return result;
    }
    m_game->casters().forget(m_ref, spell);
    m_spells.erase(it);
    return true;
}

void MagicTarget::cast_permanent() {
    const forms::Database& db = m_game->db();
    for (uint32_t owner : {m_values.record().npc, m_values.record().race}) {
        if (const forms::FormEntry* e = db.find(owner)) {
            for (const forms::Value* v : forms::Form(db, *e).all("SPLO")) {
                const Spell* s = m_game->magic().spell(v->form);
                if (s && s->permanent() && !has_effect_from(v->form)) {
                    apply(v->form, m_ref);
                }
            }
        }
    }
}

// ---- adding an effect --------------------------------------------------------------------------------------------

bool MagicTarget::apply(uint32_t spell, uint32_t caster, uint32_t effect_mask, uint32_t object) {
    const Spell* s = m_game->magic().spell(spell);
    if (!s) {
        return false;
    }
    bool any = false;
    for (size_t i = 0; i < s->effects.size(); ++i) {
        if (effect_mask & (1u << i)) {
            AddTarget t;
            t.caster = caster;
            t.item = s;
            t.effect = &s->effects[i];
            t.object = object;
            any = add_target(t) || any;
        }
    }
    if (any && !m_updating) {
        update(0.0f);
    }
    return any;
}

bool MagicTarget::combat_spell_apply(uint32_t spell, uint32_t caster) {
    const Spell* s = m_game->magic().spell(spell);
    if (!s) {
        return false;
    }
    CharacterEvents& events = m_game->character().events;
    if (s->type == spell_disease) {
        const auto resist = int(m_values.current(av_disease_resist));
        const float chance = float(100 - std::min(resist, 100)) * number("fMagicDiseaseTransferMult", 1.0f) +
                             number("fMagicDiseaseTransferBase", 0.0f);
        if (!(events.random_percent && events.random_percent() < chance)) {
            return false;
        }
    }
    const bool applied = apply(spell, caster);
    if (applied && s->type == spell_disease && m_ref == k_player_ref && events.notify) {
        events.notify(std::format("{} {}.", m_game->settings().text("sContractedDisease"), s->name), {});
    }
    return applied;
}

bool MagicTarget::add_target(const AddTarget& target) {
    const MagicEffect* m = target.item && target.effect ? m_game->magic().effect(target.effect->effect) : nullptr;
    if (!m) {
        return false;
    }
    AddTarget t = target;
    const Spell& item = *t.item;
    const bool hostile = (m->flags & effect_hostile) != 0;
    const bool actor = is_actor();
    if (actor) {
        const bool permanent = item.type == spell_ability || (m_ref == k_player_ref && item.permanent());
        if (ghost() && !item.constant_enchantment() && !permanent) {
            return false;
        }
        // Slot: a fighter's hostile magic only touches its group's targets.
        if (hostile && t.caster && t.caster != m_ref && t.caster != k_player_ref && m_game->is_actor(t.caster)) {
            const Fighter& f = m_game->combat().fighter(t.caster);
            if (f.in_combat && std::ranges::find(f.targets, m_ref) == f.targets.end()) {
                return false;
            }
        }
        if (t.area_target && t.location && hostile) {
            float passes = 1;
            if (check_ward(item, t.effect, 0.0f, passes, t.caster)) {
                return false;
            }
            t.effectiveness *= passes;
        }
    }
    if (actor && m->delivery != delivery_self && !item.ignores_resistance() && !item.no_absorb() && check_absorb(t)) {
        return false;
    }
    m_game->magic_world().effect_applied(m_ref, t.caster, m->form);
    if (!add_effect(t, *m)) {
        return false;
    }
    const bool custom = (archetype_info(m->archetype).flags & archetype_custom_skill) != 0;
    if (item.cast_type == cast_fire_and_forget && t.caster && !custom) {
        m_game->casters().use_skill(t.caster, item, t.effect, 1.0f);
    }
    if (actor && !item.permanent() && t.caster && m_game->base_of(t.caster) != m_game->base_of(m_ref) &&
        !(m->flags & effect_no_hit_event)) {
        m_game->magic_world().magic_hit(m_ref, t.caster, t.object ? t.object : item.form);
        const Item* ammo = t.object ? m_game->items().find(t.object) : nullptr;
        const bool contact = item.type == spell_enchantment && !(t.area_target && ammo && ammo->kind == ItemKind::ammo);
        if (item.hostile_count && item.type != spell_potion && !contact && m_game->is_actor(t.caster)) {
            m_game->combat().on_attacked(m_ref, t.caster);
        }
    }
    return true;
}

bool MagicTarget::check_absorb(const AddTarget& t) {
    // rolled per effect; the target gains the effect item's stored cost.
    if (float(m_game->combat().random() % 100) >= m_values.current(k_absorb_chance)) {
        return false;
    }
    m_values.restore(av_magicka, m_game->magic().effect_cost(*t.effect));
    if (m_absorb_art <= 0.0f) {
        m_absorb_art = number("fMagicAbsorbVisualTimer", 2.5f);
        m_game->magic_world().default_object(m_ref, k_art_absorb);
    }
    return true;
}

bool MagicTarget::check_ward(const Spell& item, const SpellEffect* effect, float relaunch, float& passes, uint32_t caster) {
    passes = 1;
    const float ward = m_values.current(k_ward_power);
    float power = 0;
    const Magic& magic = m_game->magic();
    if (effect) {
        const MagicEffect* m = magic.effect(effect->effect);
        power = m && (m->flags & effect_hostile) ? effect->magnitude : 0.0f;
    } else {
        for (const SpellEffect& e : item.effects) {
            const MagicEffect* m = magic.effect(e.effect);
            power += m && (m->flags & effect_hostile) ? e.magnitude : 0.0f;
        }
        if (item.cast_type == cast_concentration && item.delivery == delivery_aimed) {
            power *= relaunch;
        }
    }
    if (ward <= 0.0f) {
        return false;
    }
    const bool breaks = ward < power;
    m_game->script_event(m_ref, "OnWardHit", {{caster, "ObjectReference"}, {item.form, "Spell"}}, power <= 0.0f ? 0 : breaks ? 2 : 1);
    if (breaks) {
        passes = 1.0f - std::min(ward / power, 1.0f);
        power = ward;
    }
    m_game->magic_world().default_object(m_ref, breaks ? k_art_ward_broken : k_art_ward_held);
    m_values.mod(Modifier::damage, k_ward_power, -power);
    m_values.restore(av_magicka, power * perk(m_ref, ep_ward_absorb, 0.0f, {item.form}));
    return !breaks;
}

float MagicTarget::resistance(const AddTarget& t, const MagicEffect& m) const {
    // the test is on the item, so a helpful effect on a hostile spell is resisted too.
    const Spell& item = *t.item;
    const bool hostile = (m.flags & effect_hostile) != 0;
    if (item.hostile_count == 0 || is_armour(t.object) || (item.type == spell_potion && !hostile) ||
        (item.type == spell_ingredient && item.food)) {
        return 1.0f;
    }
    const int first = item.type == spell_poison ? k_poison_resist : k_magic_resist;
    float a = m_values.current(first);
    float b = m.resist_value >= 0 && m.resist_value != first ? m_values.current(m.resist_value) : 0.0f;
    if (m_ref == k_player_ref) {
        const float cap = number("fPlayerMaxResistance", 85.0f);
        a = std::min(a, cap);
        b = std::min(b, cap);
    }
    return a >= 100.0f || b >= 100.0f ? 0.0f : 1.0f - (a / 100.0f + b / 100.0f * (1.0f - a / 100.0f));
}

bool MagicTarget::level_check(const AddTarget& t, const MagicEffect& m, int& reason) const {
    // perked and dual cast, but not resisted.
    float magnitude = perk(t.caster, ep_spell_magnitude, t.magnitude.value_or(t.effect->magnitude), {t.item->form, m_ref});
    if (t.dual && (m.flags & effect_power_affects_magnitude)) {
        magnitude *= m_game->magic().dual_effectiveness(t.cast_cost);
    }
    if (float(level()) <= magnitude) {
        return true;
    }
    reason = 2;
    return false;
}

bool MagicTarget::can_add(const AddTarget& t, const MagicEffect& m, int& reason) {
    const bool actor = is_actor();
    Combat& combat = m_game->combat();
    switch (m.archetype) {
    case archetype_script: case archetype_telekinesis: case archetype_concussion: case archetype_cloak:
    case archetype_spawn_hazard: case archetype_grab_actor:
        return true;
    case archetype_value_modifier: case archetype_absorb: case archetype_dual_value_modifier: case archetype_value_and_parts:
    case archetype_accumulate_magnitude: case archetype_peak_value_modifier: case archetype_stagger:
        return alive();
    case archetype_calm: case archetype_demoralize: case archetype_frenzy: case archetype_rally: case archetype_banish:
        return alive() && level_check(t, m, reason);
    case archetype_turn_undead: {
        const bool undead = actor && (combat.fighter(m_ref).life == LifeState::reanimated ||
                                      m_game->conditions().value(k_has_keyword, m_ref, 0, m_game->keyword("ActorTypeUndead"), 0) != 0);
        return undead && alive() && level_check(t, m, reason);
    }
    case archetype_paralysis: {
        if (!alive()) {
            return false;
        }
        const Fighter& f = combat.fighter(m_ref);
        return (f.knock_state == 0 || f.knock_state == 6) && f.life != LifeState::unconscious;
    }
    case archetype_disarm: {
        if (!alive()) {
            return false;
        }
        const Fighter& f = combat.fighter(m_ref);
        const Item* weapon = combat.gear(m_ref).right.item;
        return f.weapon_drawn && weapon && (t.item->type == spell_voice || f.attack_state != AttackState::none) &&
               level_check(t, m, reason);
    }
    case archetype_command_summoned: return actor && commander != t.caster && level_check(t, m, reason);
    case archetype_light: { // one aimed light at a time
        if (t.item->delivery != delivery_self) {
            uint32_t& held = m_game->casters().aimed_light;
            if (held && held != m_ref) {
                m_game->magic_target(held).dispel_archetype(archetype_light, t.caster);
            }
            held = m_ref;
        }
        return true;
    }
    case archetype_open: case archetype_lock: return !actor;
    case archetype_detect_life: case archetype_guide: return t.caster == k_player_ref;
    case archetype_reanimate: {
        if (!actor) {
            return false;
        }
        const Fighter& f = combat.fighter(m_ref);
        return f.dead() && f.life != LifeState::essential_down && level_check(t, m, reason);
    }
    case archetype_soul_trap: {
        const uint32_t flags = m_values.record().acbs_flags;
        const bool kept = (flags & 0x2) || ((flags & 0x800) && t.caster != k_player_ref); // essential, protected
        return alive() && t.caster && m_ref != k_player_ref && !kept && m_game->soul_level_of(m_ref) != 0;
    }
    case archetype_werewolf_feed: return actor && combat.fighter(m_ref).dead() && !fed_on;
    case archetype_werewolf: case archetype_vampire_lord: return actor && !has_archetype(m.archetype);
    case archetype_enhance_weapon: return alive() && combat.fighter(m_ref).weapon_drawn;
    default: return actor;
    }
}

bool MagicTarget::check_add(const AddTarget& t, const MagicEffect& m, float resist, int& reason) {
    // The game; EffectWasDualCast reads the caster while these conditions run.
    Casters& casters = m_game->casters();
    casters.dual_flagged = t.dual ? t.caster : 0;
    const bool holds = m_game->conditions().evaluate(m.conditions, m_ref, t.caster);
    casters.dual_flagged = 0;
    const bool invulnerable = is_actor() && m_game->combat().fighter(m_ref).invulnerable;
    return resist > 0.0f && !(invulnerable && (m.flags & effect_hostile)) && holds && can_add(t, m, reason);
}

bool MagicTarget::refresh_matching(const AddTarget& t) {
    // ResetElapsedTimeMatchingEffects: any match ends AddEffect, which keeps one concentration effect alive.
    const Magic& magic = m_game->magic();
    bool any = false;
    for (ActiveEffect& e : m_effects) {
        if (e.dispelled || e.spell != t.item->form || e.caster != t.caster || !magic.same_effect(*e.source, *t.effect)) {
            continue;
        }
        if (value_class(*e.setting) && (e.object != t.object || e.casting_source != t.casting_source)) {
            continue;
        }
        any = true;
        e.elapsed = 0;
        if (!(e.setting->flags & effect_recover) && e.dual != t.dual) {
            const float dual = magic.dual_effectiveness(t.cast_cost);
            e.dual = t.dual;
            apply_power(e, t.dual ? dual : 1.0f / dual);
        }
    }
    return any;
}

void MagicTarget::dispel_matching(const AddTarget& t) {
    // DispelFromBoundObject.
    for (ActiveEffect& e : m_effects) {
        if (e.spell == t.item->form && e.caster == t.caster && e.object == t.object &&
            m_game->magic().same_effect(*e.source, *t.effect)) {
            e.dispelled = true;
        }
    }
}

void MagicTarget::apply_power(ActiveEffect& e, float power, bool hostile_only) const {
    const Spell& s = *e.item;
    const uint32_t flags = e.setting->flags;
    const bool no_magnitude = (flags & effect_no_magnitude) != 0;
    if (s.permanent() || s.type == spell_ingredient || s.constant_enchantment() || (hostile_only && !(flags & effect_hostile)) ||
        ((flags & effect_no_duration) && no_magnitude) || power == 1.0f || power < 0.0f) {
        return;
    }
    bool duration = no_magnitude, magnitude = !no_magnitude;
    if (!no_magnitude && e.dual && (flags & effect_power_affects_duration)) {
        duration = true;
        magnitude = (flags & effect_power_affects_magnitude) != 0;
    }
    if (duration) {
        e.duration *= power;
    }
    if (magnitude) {
        e.magnitude = std::copysign(std::max(std::abs(e.magnitude) * power, 1.0f), e.magnitude);
    }
}

void MagicTarget::on_add(ActiveEffect& e) {
    // the game first.
    const MagicEffect& m = *e.setting;
    if (m.archetype == archetype_night_eye) {
        e.magnitude = 1;
    }
    if (value_class(m) || (m.archetype == archetype_accumulate_magnitude && m.actor_value >= 0)) {
        const bool detrimental = (m.flags & effect_detrimental) != 0;
        const bool inverted = (actor_value_info(m.actor_value).flags & av_flag_damage_positive) != 0;
        if (detrimental != inverted) {
            e.magnitude = -e.magnitude;
        }
    }
    e.active = check_conditions(e);
    const uint32_t delivery = e.item->delivery;
    e.tethered = e.item->cast_type == cast_concentration && (delivery == delivery_self || delivery == delivery_target_location);
    e.no_expiry = e.item->permanent() || (e.tethered && (delivery == delivery_target_location || e.duration == 0.0f)) ||
                  (m.flags & effect_no_expiry);
}

void MagicTarget::dispel_with_keywords(const ActiveEffect& e) {
    // another item's effects sharing a keyword.
    for (ActiveEffect& other : m_effects) {
        if (other.spell == e.spell) {
            continue;
        }
        for (uint32_t keyword : e.setting->keywords) {
            if (std::ranges::find(other.setting->keywords, keyword) != other.setting->keywords.end()) {
                other.dispelled = true;
            }
        }
    }
}

bool MagicTarget::add_effect(const AddTarget& t, const MagicEffect& m) {
    const Spell& item = *t.item;
    const float resist = item.type == spell_ability || item.ignores_resistance() ? 1.0f : resistance(t, m);
    int reason = 0;
    if (!check_add(t, m, resist, reason)) {
        if (t.caster == k_player_ref && reason == 2) {
            m_game->notify(fill_names(m_game->settings().text("sMagicEffectResisted"),
                                      m_game->display_name_of(m_game->base_of(m_ref)), item.name));
        }
        return false;
    }
    const uint16_t archetype_flags = archetype_info(m.archetype).flags;
    MagicTarget& list = (archetype_flags & archetype_caster_list) && t.caster ? m_game->magic_target(t.caster) : *this;
    const bool armour = is_armour(t.object);
    if (!armour) {
        if (item.cast_type == cast_concentration || (item.type == spell_enchantment && item.extend_on_recast)) {
            if (list.refresh_matching(t)) {
                return true;
            }
        } else if (!(archetype_flags & archetype_stacks)) {
            list.dispel_matching(t);
        }
    }
    const bool no_magnitude = (m.flags & effect_no_magnitude) != 0, no_duration = (m.flags & effect_no_duration) != 0;
    ActiveEffect e;
    e.id = m_game->next_effect_id();
    e.spell = item.form;
    e.effect = t.effect->effect;
    e.caster = t.caster;
    e.target = list.m_ref;
    e.object = t.object;
    e.item = &item;
    e.source = t.effect;
    e.setting = &m;
    e.magnitude = no_magnitude ? 0.0f : t.effect->magnitude;
    e.duration = float(t.effect->duration) + (no_magnitude || no_duration ? 0.0f : m.taper_duration);
    e.has_conditions = !t.effect->conditions.empty();
    e.worn = armour;
    e.casting_source = t.casting_source;
    e.has_location = t.location.has_value();
    e.location = t.location.value_or(Units3{});
    if (&list != this) {
        e.link = m_ref; // the corpse of a Reanimate on its caster's list
    }
    // The post-creation callback, then the caster's AdjustActiveEffect.
    e.dual = t.dual;
    if (t.magnitude && !no_magnitude) {
        e.magnitude = *t.magnitude;
    }
    apply_power(e, t.power, t.power_hostile_only);
    if (t.dual && item.delivery != delivery_aimed) {
        apply_power(e, m_game->magic().dual_effectiveness(t.cast_cost));
    }
    apply_power(e, resist * t.effectiveness);
    if (t.caster) {
        // the caster's, then the target's.
        e.duration = perk(t.caster, ep_spell_duration, e.duration, {item.form, m_ref});
        e.magnitude = perk(t.caster, ep_spell_magnitude, e.magnitude, {item.form, m_ref});
        e.duration = perk(m_ref, ep_incoming_spell_duration, e.duration, {item.form});
        e.magnitude = perk(m_ref, ep_incoming_spell_magnitude, e.magnitude, {item.form});
    }
    if ((m.flags & effect_hostile) && item.type != spell_ability && !(item.type == spell_ingredient && item.food) &&
        !(m.flags & effect_painless)) {
        m_game->magic_world().pain(m_ref);
    }
    list.on_add(e);
    if (m.flags & effect_dispel_keywords) {
        list.dispel_with_keywords(e);
    }
    list.m_effects.push_back(e);
    return true;
}

// ---- dispelling --------------------------------------------------------------------------------------------------

bool MagicTarget::dispel_spell(uint32_t spell) {
    bool any = false;
    for (ActiveEffect& e : m_effects) {
        if (e.spell == spell && !e.dispelled) {
            e.dispelled = true;
            any = true;
        }
    }
    if (any && !m_updating) {
        update(0.0f);
    }
    return any;
}

void MagicTarget::dispel_effect(uint32_t id) {
    for (ActiveEffect& e : m_effects) {
        if (e.id == id) {
            e.dispelled = true;
        }
    }
    if (!m_updating) {
        update(0.0f);
    }
}

void MagicTarget::dispel_all() {
    for (ActiveEffect& e : m_effects) {
        e.dispelled = true;
    }
    if (!m_updating) {
        update(0.0f);
    }
}

void MagicTarget::dispel_archetype(uint32_t archetype, uint32_t caster) {
    bool any = false;
    for (ActiveEffect& e : m_effects) {
        if (e.setting->archetype == archetype && (!caster || e.caster == caster) && !e.dispelled) {
            e.dispelled = true;
            any = true;
        }
    }
    if (any && !m_updating) {
        update(0.0f);
    }
}

void MagicTarget::dispel_type(uint32_t type) {
    for (ActiveEffect& e : m_effects) {
        if (e.item->type == type) {
            e.dispelled = true;
        }
    }
}

void MagicTarget::dispel_summon(uint32_t summoned) {
    for (ActiveEffect& e : m_effects) {
        if (e.setting->archetype == archetype_summon_creature && e.link == summoned) {
            e.dispelled = true;
        }
    }
}

void MagicTarget::break_invisibility(const Spell& item) {
    const auto carries = [&](uint32_t archetype) {
        return std::ranges::any_of(item.effects, [&](const SpellEffect& s) {
            const MagicEffect* m = m_game->magic().effect(s.effect);
            return m && m->archetype == archetype;
        });
    };
    for (uint32_t archetype : {uint32_t(archetype_invisibility), uint32_t(archetype_etherealize)}) {
        if (!carries(archetype)) {
            dispel_archetype(archetype);
        }
    }
}

bool MagicTarget::has_magic_effect(uint32_t effect) const {
    return std::ranges::any_of(m_effects, [&](const ActiveEffect& e) { return e.effect == effect && !e.dispelled; });
}

bool MagicTarget::has_magic_effect_keyword(uint32_t keyword) const {
    return std::ranges::any_of(m_effects, [&](const ActiveEffect& e) {
        const auto& k = e.setting->keywords;
        return !e.dispelled && std::ranges::find(k, keyword) != k.end();
    });
}

bool MagicTarget::has_disease() const {
    return std::ranges::any_of(m_effects, [](const ActiveEffect& e) { return e.item->type == spell_disease && !e.dispelled; });
}

bool MagicTarget::has_effect_from(uint32_t spell) const {
    return std::ranges::any_of(m_effects, [&](const ActiveEffect& e) { return e.spell == spell; });
}

bool MagicTarget::has_archetype(uint32_t archetype) const {
    return std::ranges::any_of(m_effects, [&](const ActiveEffect& e) { return e.setting->archetype == archetype && !e.dispelled; });
}

// ---- values ------------------------------------------------------------------------------------------------------

bool MagicTarget::check_conditions(const ActiveEffect& e) const {
    return !e.has_conditions || m_game->conditions().evaluate(e.source->conditions, m_ref, e.caster);
}

float MagicTarget::current_magnitude(const ActiveEffect& e) const {
    // the taper at the end.
    const MagicEffect& m = *e.setting;
    const float left = e.duration - e.elapsed;
    if (!(m.flags & effect_recover) && m.taper_duration > 0.0001f && left < m.taper_duration) {
        return e.magnitude * std::pow(left / m.taper_duration, m.taper_curve) * m.taper_weight;
    }
    return e.magnitude;
}

void MagicTarget::write_value(ActiveEffect& e, int av, float amount) {
    const MagicEffect& m = *e.setting;
    const Spell& s = *e.item;
    const float before = m_values.current(av);
    if (m.flags & effect_recover) {
        const bool permanent_source = s.type == spell_ability || (s.type == spell_enchantment && s.cast_type == cast_constant) ||
                                      (s.type == spell_potion && av >= av_health && av <= av_stamina);
        if (m_ref == k_player_ref && !e.has_conditions && permanent_source) {
            if (av == av_health && m_values.current(av) + amount <= 0.0f) {
                m_values.restore(av, 1.0f - (m_values.current(av) + amount));
            }
            m_values.mod(Modifier::permanent, av, amount);
        } else {
            m_values.mod(Modifier::temporary, av, amount);
        }
    } else if (av == av_health && amount < 0.0f && is_actor()) {
        m_game->combat().damage_health(m_ref, -amount, (m.flags & effect_hostile) ? e.caster : 0);
    } else {
        m_values.mod(Modifier::damage, av, amount);
    }
    if (amount != 0.0f) {
        e.fraction = std::clamp((m_values.current(av) - before) / amount, 0.0f, 1.0f);
    }
}

void MagicTarget::remove_value(ActiveEffect& e, int av, float m) {
    // losing a buff doesn't kill.
    if (m > 0.0f) {
        const float after = m_values.current(av) - m;
        if (after < 0.0f) {
            m_values.restore(av, -after);
        }
        if (av == av_health && m_values.current(av) <= m + 1.0f) {
            m_values.restore(av, 1.0f);
        }
    }
    write_value(e, av, -m);
    if (av == av_health && is_actor() && m_game->combat().fighter(m_ref).dead() && m_values.current(av) > 0.0f) {
        m_values.mod(Modifier::damage, av, -m_values.current(av));
    }
}

void MagicTarget::write_all(ActiveEffect& e, float amount) {
    const MagicEffect& m = *e.setting;
    write_value(e, m.actor_value, amount);
    if (m.archetype == archetype_dual_value_modifier && m.second_actor_value >= 0) {
        write_value(e, m.second_actor_value, amount * m.second_weight);
    }
    if (m.archetype == archetype_absorb && e.caster && e.caster != m_ref) {
        // what the target loses the caster gains.
        m_game->magic_target(e.caster).write_value(e, m.actor_value, -amount);
    }
}

// ---- archetypes --------------------------------------------------------------------------------------------------

void MagicTarget::start_target_value(ActiveEffect& e) {
    // TargetValueModifierEffect, start: the value goes to where the archetype wants it, at once or over the duration.
    const MagicEffect& m = *e.setting;
    Combat& combat = m_game->combat();
    float wanted = 0;
    switch (m.archetype) {
    case archetype_calm: wanted = -1; break;
    case archetype_frenzy: wanted = 3; break;
    case archetype_rally: wanted = 4; break;
    default: break;
    }
    if (m.archetype == archetype_calm || m.archetype == archetype_frenzy) {
        combat.stop_combat(m_ref);
    }
    const float change = wanted - m_values.current(m.actor_value);
    if ((m.flags & effect_recover) || e.duration == 0.0f || (actor_value_info(m.actor_value).flags & av_flag_enumerated)) {
        e.magnitude = change;
        e.applied = change;
        write_value(e, m.actor_value, change);
    } else {
        e.magnitude = change / e.duration;
        e.timer = 1; // runs per second
    }
}

void MagicTarget::limit_summons() {
    // iMaxSummonedCreatures through entry point 68, half up. The longest out goes; raised corpses count.
    const int limit = int(std::floor(perk(m_ref, ep_max_summons, number("iMaxSummonedCreatures", 1.0f)) + 0.5f));
    const auto summons = [](const ActiveEffect& e) {
        return !e.dispelled && (e.setting->archetype == archetype_summon_creature || e.setting->archetype == archetype_reanimate);
    };
    for (auto count = std::ranges::count_if(m_effects, summons); count > limit; --count) {
        ActiveEffect* oldest = nullptr;
        for (ActiveEffect& e : m_effects) {
            if (summons(e) && (!oldest || e.elapsed > oldest->elapsed)) {
                oldest = &e;
            }
        }
        oldest->dispelled = true;
    }
}

void MagicTarget::start_summon(ActiveEffect& e) {
    Combat& combat = m_game->combat();
    MagicWorld& world = m_game->magic_world();
    CombatBody self;
    const bool placed = combat.world().body(e.caster, self);
    Units3 spot{};
    const bool found = world.summon_spot(e.caster, spot);
    Units3 at = e.location;
    if (!e.has_location) {
        at = found ? spot : self.position;
    } else if (placed && found && units_between(at, self.position) < number("fSummonDistanceCheckThreshold", 750.0f) &&
               units_between(spot, self.position) > units_between(at, self.position)) {
        at = spot;
    }
    e.link = world.summon(e.caster, e.setting->associated, at);
    if (e.link) {
        m_game->magic_target(e.link).commander = e.caster;
        limit_summons();
    }
}

void MagicTarget::start_reanimate(ActiveEffect& e) {
    Fighter& corpse = m_game->combat().fighter(e.link);
    if (!corpse.dead()) {
        return;
    }
    corpse.life = LifeState::reanimated;
    ActorValues& values = m_game->actor_values(e.link);
    values.restore(av_health, -values.modifier(av_health, Modifier::damage));
    m_game->magic_target(e.link).commander = e.caster;
    m_game->magic_world().reanimate(e.link, e.caster);
    limit_summons();
}

void MagicTarget::start_effect(ActiveEffect& e) {
    const MagicEffect& m = *e.setting;
    Combat& combat = m_game->combat();
    MagicWorld& world = m_game->magic_world();
    if (m.perk && m_ref == k_player_ref) {
        m_game->character().add_temporary_perk(m.perk);
    }
    if (value_class(m) && applies_on_start(e)) {
        // with no conditions a value other than health stops at 0.
        float a = e.magnitude;
        const int av = m.actor_value;
        if (!e.has_conditions && a < 0.0f && av != av_health && ((av >= 6 && av <= 23) || (av >= 25 && av <= 38))) {
            a = std::max(a, -m_values.current(av));
        }
        e.applied = a;
        write_all(e, a);
    }
    switch (m.archetype) {
    case archetype_cure_disease: dispel_type(spell_disease); break;
    case archetype_cure_addiction: dispel_type(spell_addiction); break;
    case archetype_cure_poison: dispel_type(spell_poison); break;
    case archetype_cure_paralysis: dispel_archetype(archetype_paralysis); break;
    case archetype_banish:
        if (commander) { // a summon goes home
            m_game->magic_target(commander).dispel_summon(m_ref);
            e.magnitude = 0;
            break;
        }
        start_target_value(e);
        break;
    case archetype_calm: case archetype_demoralize: case archetype_frenzy: case archetype_rally: case archetype_turn_undead:
        start_target_value(e);
        break;
    case archetype_paralysis:
        if (alive()) {
            world.paralyse(m_ref, true);
        }
        break;
    case archetype_bound_weapon: e.dispelled = e.dispelled || !m.associated; break;
    case archetype_summon_creature: start_summon(e); break;
    case archetype_reanimate: start_reanimate(e); break;
    case archetype_command_summoned: {
        // the command and the summon effect change hands.
        if (commander) {
            MagicTarget& old = m_game->magic_target(commander);
            MagicTarget& fresh = m_game->magic_target(e.caster);
            for (ActiveEffect& s : old.m_effects) {
                if (s.setting->archetype == archetype_summon_creature && s.link == m_ref && &old != &fresh) {
                    s.caster = fresh.m_ref;
                    s.target = fresh.m_ref;
                    fresh.m_effects.push_back(s);
                }
            }
            std::erase_if(old.m_effects, [this, &old, &fresh](const ActiveEffect& s) {
                return &old != &fresh && s.setting->archetype == archetype_summon_creature && s.link == m_ref;
            });
        }
        commander = e.caster;
        break;
    }
    case archetype_light: world.light(m_ref, m.associated, true); break;
    case archetype_detect_life: world.detect_life(e.caster, m.form, true, float(e.source->area) * number("fMagicUnitsPerFoot", 22.0f)); break;
    case archetype_telekinesis:
        if (world.telekinesis_start(e.caster)) {
            world.default_object(e.caster, k_art_telekinesis);
        } else {
            e.dispelled = true;
        }
        break;
    case archetype_grab_actor: e.dispelled = e.dispelled || !world.grab_actor(e.caster, true); break;
    case archetype_stagger: combat.stagger(m_ref, current_magnitude(e), e.caster); break;
    case archetype_disarm:
        combat.stagger(m_ref, 1.0f, e.caster);
        e.duration = 10;
        break;
    case archetype_etherealize: m_ghost = true; break;
    case archetype_slow_time: {
        const float player_rate = perk(m_ref, ep_slow_time, number("fVATSPlayerMagicTimeSlowdownMult", 0.6f), {e.spell});
        world.slow_time(player_rate, e.magnitude, true);
        e.duration *= world.time_factor();
        break;
    }
    case archetype_guide: {
        const int result = world.guide(e.caster, m.associated);
        if (result) {
            m_game->notify(m_game->settings().text(result == 1 ? "sMagicGuideNoMarker" : "sMagicGuideNoPath"));
            e.dispelled = true;
        }
        break;
    }
    case archetype_werewolf: case archetype_vampire_lord: world.transform(m_ref, m.associated, true); break;
    case archetype_werewolf_feed:
        fed_on = true;
        e.dispelled = true;
        break;
    case archetype_spawn_hazard:
        if (m.associated) {
            world.spawn_hazard(m.associated, m_ref, e.location, e.caster);
        }
        break;
    case archetype_enhance_weapon: world.enhance_weapon(m_ref, m.associated, true); break;
    case archetype_open: m_game->unlock(m_ref); break;
    default: break;
    }
    if (m_game->character().events.effect_started && m.has_scripts) {
        m_game->character().events.effect_started(e);
    }
}

void MagicTarget::finish_effect(ActiveEffect& e) {
    const MagicEffect& m = *e.setting;
    Combat& combat = m_game->combat();
    MagicWorld& world = m_game->magic_world();
    if (m.perk && m_ref == k_player_ref) {
        m_game->character().remove_temporary_perk(m.perk);
    }
    if (value_class(m) && (m.flags & effect_recover)) {
        remove_value(e, m.actor_value, e.applied);
        if (m.archetype == archetype_dual_value_modifier && m.second_actor_value >= 0) {
            remove_value(e, m.second_actor_value, e.applied * m.second_weight);
        }
        if (m.archetype == archetype_absorb && e.caster && e.caster != m_ref) {
            m_game->magic_target(e.caster).remove_value(e, m.actor_value, -e.applied);
        }
    }
    switch (m.archetype) {
    case archetype_accumulate_magnitude: remove_value(e, m.actor_value, e.applied); break;
    case archetype_frenzy:
        combat.stop_combat(m_ref);
        world.reevaluate_target(m_ref);
        [[fallthrough]];
    case archetype_calm: case archetype_demoralize: case archetype_rally: case archetype_turn_undead: case archetype_banish:
        if (e.applied != 0.0f) {
            remove_value(e, m.actor_value, e.applied);
        }
        break;
    case archetype_paralysis: world.paralyse(m_ref, false); break;
    case archetype_bound_weapon:
        if (e.link) {
            world.unbound_weapon(m_ref, e.link, e.casting_source);
        }
        break;
    case archetype_summon_creature:
        if (e.link) {
            combat.kill(e.link, 0);
            world.fade_summon(e.link, number("fSummonedCreatureFadeOutSeconds", 3.0f));
            m_game->magic_target(e.link).commander = 0;
        }
        break;
    case archetype_reanimate:
        if (e.link && !combat.fighter(e.link).dead()) {
            combat.kill(e.link, 0);
            m_game->magic_target(e.link).commander = 0;
        }
        break;
    case archetype_light:
        world.light(m_ref, m.associated, false);
        if (m_game->casters().aimed_light == m_ref) {
            m_game->casters().aimed_light = 0;
        }
        break;
    case archetype_detect_life: world.detect_life(e.caster, m.form, false, 0.0f); break;
    case archetype_telekinesis: world.telekinesis_stop(e.caster); break;
    case archetype_grab_actor: world.grab_actor(e.caster, false); break;
    case archetype_etherealize: m_ghost = false; break;
    case archetype_slow_time:
        world.slow_time(1.0f, 1.0f, false);
        m_game->sound("MAGShoutSlowTimeOut");
        break;
    case archetype_soul_trap: capture_soul(e); break;
    case archetype_werewolf: case archetype_vampire_lord: world.transform(m_ref, m.associated, false); break;
    case archetype_enhance_weapon: world.enhance_weapon(m_ref, m.associated, false); break;
    default: break;
    }
    if (m_game->character().events.effect_finished && m.has_scripts) {
        m_game->character().events.effect_finished(e);
    }
}

void MagicTarget::pulse_cloak(ActiveEffect& e, float dt) {
    e.timer -= dt;
    if (e.timer > 0.0f) {
        return;
    }
    e.timer += number("fCloakEffectUpdateInterval", 0.5f);
    // The cloak's magnitude is its radius in feet and what it hands on.
    const Spell* spell = m_game->magic().spell(e.setting->associated);
    CombatBody self;
    if (!spell || !alive() || !m_game->combat().world().body(m_ref, self)) {
        return;
    }
    const float magnitude = current_magnitude(e);
    for (uint32_t other : m_game->casters().actors_near(self.position, magnitude * 21.3333f, m_ref)) {
        for (const SpellEffect& s : spell->effects) {
            AddTarget t;
            t.caster = m_ref;
            t.item = spell;
            t.effect = &s;
            t.magnitude = magnitude;
            if (m_game->magic_target(other).add_target(t)) {
                m_game->casters().use_skill(m_ref, *spell, &s, 1.0f);
            }
        }
    }
}

void MagicTarget::accumulate(ActiveEffect& e, float dt) {
    // The refresh of a concentration cast resets elapsed, so the hold keeps its own time.
    const MagicEffect& m = *e.setting;
    e.timer += dt;
    if (e.timer < number("fMagicAccumulatingModifierEffectHoldDuration", 0.5f)) {
        return;
    }
    float rate = number("fMagicDefaultAccumulatingModifierEffectRate", 1.0f);
    float cap = e.source->magnitude;
    if (m.actor_value == k_ward_power) {
        const float skill = m.magic_skill >= 0 ? m_game->actor_values(e.caster).current(m.magic_skill) : 0.0f;
        rate = (number("fMagicWardPowerMaxMult", 0.0f) * skill + number("fMagicWardPowerMaxBase", 1.3f)) * e.magnitude;
        for (const ActiveEffect& other : m_effects) {
            if (other.setting->archetype == archetype_accumulate_magnitude && other.setting->actor_value == k_ward_power && !other.dispelled) {
                cap = std::max(cap, other.source->magnitude);
            }
        }
    }
    const float add = std::min(rate * dt, cap - e.applied);
    if (add > 0.0f) {
        e.applied += add;
        write_value(e, m.actor_value, add);
    }
}

void MagicTarget::capture_soul(const ActiveEffect& e) {
    Combat& combat = m_game->combat();
    if (!e.caster || !combat.fighter(m_ref).dead() || combat.fighter(e.caster).dead()) {
        return;
    }
    const int soul = soul_taken ? 0 : m_game->soul_level_of(m_ref);
    const float value = m_game->soul_level_value(soul);
    if (value <= 0.0f) {
        return;
    }
    // FindBestSoulGemVisitor: the smallest empty gem that holds it; an NPC's needs flag 0x20000, others avoid it on a tie.
    const bool npc_soul = m_game->has_npc_soul(m_ref);
    const auto npc_gem = [this](const Stack& s) {
        const forms::FormEntry* e = m_game->db().find(s.form);
        return e && (e->flags & 0x20000);
    };
    Inventory& inventory = m_game->inventory_of(e.caster);
    const Stack* best = nullptr;
    float best_value = 0;
    for (const Stack& s : inventory.stacks()) {
        const Item* gem = m_game->items().find(s.form);
        if (!gem || gem->kind != ItemKind::soul_gem || gem->soul || s.extra.soul || s.count <= 0 || (npc_soul && !npc_gem(s))) {
            continue;
        }
        const float holds = m_game->soul_level_value(gem->soul_capacity);
        if (holds >= value && (!best || best_value > holds || (!npc_soul && best_value == holds && npc_gem(*best) && !npc_gem(s)))) {
            best = &s;
            best_value = holds;
        }
    }
    if (best) {
        const auto index = size_t(best - inventory.stacks().data());
        const Item* gem = m_game->items().find(best->form);
        ItemExtra extra = best->extra;
        const uint32_t empty = best->form;
        inventory.remove_at(index, 1);
        if (gem->soul_capacity == soul && gem->soul_gem_filled) {
            inventory.add(gem->soul_gem_filled, 1);
        } else {
            extra.soul = uint8_t(soul);
            inventory.add(empty, 1, extra);
        }
        soul_taken = true;
    }
    if (e.caster == k_player_ref) {
        m_game->notify(m_game->settings().text(best ? "sSoulCaptured" : "sSoulGemTooSmall"));
        if (best) {
            m_game->magic_world().default_object(m_ref, k_soul_sound);
        }
    }
}

void MagicTarget::archetype_update(ActiveEffect& e, float dt) {
    const MagicEffect& m = *e.setting;
    Combat& combat = m_game->combat();
    switch (m.archetype) {
    case archetype_paralysis: e.dispelled = e.dispelled || !alive(); break;
    case archetype_bound_weapon:
        if (e.first_update && m.associated) {
            e.link = m.associated;
            m_game->magic_world().bound_weapon(m_ref, m.associated, e.casting_source);
        }
        break;
    case archetype_summon_creature:
        if (e.link && combat.fighter(e.link).dead()) {
            e.link = 0;
            e.dispelled = true;
        }
        break;
    case archetype_soul_trap:
        if ((e.caster && combat.fighter(e.caster).dead()) || !alive()) {
            e.dispelled = true;
        }
        break;
    case archetype_disarm:
        if (combat.fighter(m_ref).staggered) {
            m_game->magic_world().disarm(m_ref);
            e.dispelled = true;
        }
        break;
    case archetype_calm: case archetype_demoralize: case archetype_frenzy: case archetype_rally: case archetype_turn_undead:
    case archetype_banish:
        if (e.timer > 0.0f) {
            e.applied += e.magnitude * dt;
            write_value(e, m.actor_value, e.magnitude * dt);
        }
        break;
    case archetype_cloak: pulse_cloak(e, dt); break;
    case archetype_accumulate_magnitude: accumulate(e, dt); break;
    default: break;
    }
    e.first_update = false;
}

// ---- running -----------------------------------------------------------------------------------------------------

void MagicTarget::update_peak_chains() {
    // Effects with the same keyword and value: only the strongest counts.
    for (ActiveEffect& e : m_effects) {
        e.suppressed = false;
    }
    for (size_t i = 0; i < m_effects.size(); ++i) {
        ActiveEffect& a = m_effects[i];
        if (a.setting->archetype != archetype_peak_value_modifier || !a.setting->associated || a.dispelled) {
            continue;
        }
        for (size_t j = 0; j < m_effects.size(); ++j) {
            ActiveEffect& b = m_effects[j];
            if (i != j && !b.dispelled && b.setting->archetype == archetype_peak_value_modifier &&
                b.setting->associated == a.setting->associated && b.setting->actor_value == a.setting->actor_value &&
                (std::abs(b.magnitude) > std::abs(a.magnitude) || (std::abs(b.magnitude) == std::abs(a.magnitude) && j < i))) {
                a.suppressed = true;
            }
        }
    }
}

bool MagicTarget::tether_holds(const ActiveEffect& e) const {
    if (!m_game->casters().casting(e.caster, *e.item)) {
        return false;
    }
    if (e.target == e.caster) {
        return true;
    }
    const float area = float(e.source->area);
    CombatBody self, other;
    CombatWorld& world = m_game->combat().world();
    if (area == 0.0f || !world.body(e.target, self) || !world.body(e.caster, other)) {
        return false;
    }
    return units_between(self.position, other.position) <= area * number("fMagicUnitsPerFoot", 22.0f);
}

void MagicTarget::update_skill(ActiveEffect& e, float dt) {
    // for the archetypes whose running effect pays.
    const MagicEffect& m = *e.setting;
    if (e.caster != k_player_ref || !(archetype_info(m.archetype).flags & archetype_custom_skill)) {
        return;
    }
    const bool in_combat = is_actor() && m_game->combat().fighter(m_ref).in_combat;
    bool pays = true;
    float scale = 1;
    if (m.archetype == archetype_bound_weapon) {
        pays = in_combat;
    } else if (m.archetype == archetype_detect_life) {
        pays = false;
    } else if (value_class(m) || m.archetype == archetype_accumulate_magnitude) {
        pays = (m.actor_value != k_damage_resist && m.actor_value != k_ward_power) || in_combat;
        scale = e.fraction;
    }
    if (!pays) {
        return;
    }
    if (e.item->cast_type == cast_concentration) {
        if (m_game->casters().casting(e.caster, *e.item)) {
            m_game->casters().use_skill(e.caster, *e.item, e.source, dt * scale);
        }
    } else if (!e.skill_paid) {
        e.skill_paid = true;
        m_game->casters().use_skill(e.caster, *e.item, e.source, scale);
    }
}

void MagicTarget::update_effect(ActiveEffect& e, float dt) {
    const MagicEffect& m = *e.setting;
    if (e.tethered && !tether_holds(e)) {
        e.dispelled = true;
    }
    if (e.has_conditions || e.suppressed) {
        const float k = 1.0f / number("fActiveEffectConditionUpdateInterval", 1.0f);
        if (dt == 0.0f || std::floor(e.elapsed * k) != std::floor((e.elapsed + dt) * k) || e.suppressed) {
            e.active = !e.suppressed && check_conditions(e);
        }
    }
    if (e.active && !e.started && !e.dispelled) {
        e.started = true;
        e.finished = false;
        start_effect(e);
        if (m.art_on_hit(e.dual) || m.shader_on_hit(e.dual)) {
            m_game->magic_world().start_hit_effects(e);
        }
    }
    const bool lasting = e.no_expiry || e.worn;
    e.elapsed += dt;
    if (!lasting && e.duration > 0.0f) {
        e.elapsed = std::min(e.elapsed, e.duration);
    }
    if (e.active && e.started && !e.dispelled) {
        const bool recover = (m.flags & effect_recover) != 0;
        if (value_class(m) && !recover && (e.duration != 0.0f || per_second_source(e))) {
            write_all(e, current_magnitude(e) * dt);
        }
        archetype_update(e, dt);
        update_skill(e, dt);
    }
    const bool target_dead = is_actor() && m_game->combat().fighter(m_ref).dead();
    if ((!lasting && ((m.flags & effect_no_duration) || e.duration <= e.elapsed)) ||
        (target_dead && !(m.flags & effect_no_death_dispel))) {
        e.dispelled = true;
    }
    if (e.started && !e.finished && (e.dispelled || !e.active)) {
        e.finished = true;
        e.started = false;
        finish_effect(e);
        m_game->magic_world().stop_hit_effects(e.id);
    }
}

void MagicTarget::update(float dt) {
    if (m_updating) {
        return;
    }
    m_updating = true;
    m_absorb_art = std::max(m_absorb_art - dt, 0.0f);
    update_peak_chains();
    // By index, on a copy: starting an effect can add others.
    for (size_t i = 0; i < m_effects.size(); ++i) {
        ActiveEffect e = m_effects[i];
        // The game skips what can't change: a permanent flat effect with no conditions.
        if (e.started && !e.dispelled && !e.has_conditions && e.item->permanent() && (e.setting->flags & effect_recover) && !e.suppressed) {
            continue;
        }
        update_effect(e, dt);
        // Something else can have dispelled this one while it ran.
        e.dispelled = e.dispelled || m_effects[i].dispelled;
        m_effects[i] = e;
    }
    std::erase_if(m_effects, [](const ActiveEffect& e) { return e.dispelled && !e.started; });
    m_updating = false;
}

} // namespace spud::game
