// NPCs casting in a fight: which spells they treat as usable, what fills each hand, and the cast.

#include "game/combat_ai.hpp"

#include "core/game_random.hpp"
#include "game/game.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace spud::game {

namespace {

float lerp(float x, float a, float b) { return a + (b - a) * x; }

// The score functions: A, B and C of 1.2.
enum class Scoring : uint8_t { plain, target_value, script };
enum class Value : uint8_t { magnitude, cost };

struct Row {
    int32_t archetype, av; // av -1: any
    bool self, hostile;
    float weight;
    CasterKind kind;
    const char* min_cast; // what a per-second cost is multiplied by
    float min_cast_default;
    Value value;
    Scoring scoring;
};

constexpr float k_gravity = 686.37866f; // 9.80665 m/s2 in units
constexpr int k_av_paralysis = 53, k_av_invisibility = 54, k_av_ward = 63, k_av_damage_resist = 39;

// The table.
constexpr Row k_rows[] = {
    {0, av_health, false, true, 1, CasterKind::offensive, "fCombatMagicOffensiveMinCastTime", 3, Value::magnitude, Scoring::plain},
    {0, av_magicka, false, true, 0.5f, CasterKind::none, "fCombatMagicOffensiveMinCastTime", 3, Value::magnitude, Scoring::target_value},
    {0, av_stamina, false, true, 0.33f, CasterKind::none, "fCombatMagicOffensiveMinCastTime", 3, Value::magnitude, Scoring::target_value},
    {archetype_stagger, -1, false, true, 10, CasterKind::stagger, "fCombatMagicDefaultMinCastTime", 1, Value::magnitude, Scoring::plain},
    {archetype_disarm, -1, false, true, 10, CasterKind::disarm, "fCombatMagicDefaultMinCastTime", 1, Value::magnitude, Scoring::plain},
    {archetype_command_summoned, -1, false, true, 10, CasterKind::target_effect, "fCombatMagicTargetEffectMinCastTime", 10, Value::magnitude, Scoring::plain},
    {archetype_banish, av_confidence, false, true, 5, CasterKind::target_effect, "fCombatMagicTargetEffectMinCastTime", 10, Value::magnitude, Scoring::plain},
    {archetype_turn_undead, av_confidence, false, true, 3, CasterKind::target_effect, "fCombatMagicTargetEffectMinCastTime", 10, Value::magnitude, Scoring::plain},
    {archetype_paralysis, k_av_paralysis, false, true, 20, CasterKind::paralyze, "fCombatMagicParalyzeMinCastTime", 2, Value::cost, Scoring::plain},
    {archetype_script, -1, false, true, 1, CasterKind::script, "fCombatMagicDefaultMinCastTime", 1, Value::cost, Scoring::script},
    {0, av_health, true, false, 1, CasterKind::restore, "fCombatMagicDefaultMinCastTime", 1, Value::magnitude, Scoring::plain},
    {0, av_magicka, true, false, 0.33f, CasterKind::restore, "fCombatMagicDefaultMinCastTime", 1, Value::magnitude, Scoring::plain},
    {0, av_stamina, true, false, 0.1f, CasterKind::none, "fCombatMagicDefaultMinCastTime", 1, Value::magnitude, Scoring::plain},
    {0, k_av_ward, true, false, 1, CasterKind::ward, "fCombatMagicWardMinCastTime", 1.5f, Value::magnitude, Scoring::plain},
    {archetype_summon_creature, -1, true, false, 1, CasterKind::summon, "fCombatMagicSummonMinCastTime", 15, Value::cost, Scoring::plain},
    {archetype_summon_creature, -1, false, false, 1, CasterKind::summon, "fCombatMagicSummonMinCastTime", 15, Value::cost, Scoring::plain},
    {archetype_cloak, -1, true, false, 1, CasterKind::cloak, "fCombatMagicCloakMinCastTime", 5, Value::magnitude, Scoring::plain},
    {archetype_light, -1, true, false, 1, CasterKind::light, "fCombatMagicLightMinCastTime", 10, Value::magnitude, Scoring::plain},
    {archetype_invisibility, k_av_invisibility, true, false, 1, CasterKind::invisibility, "fCombatMagicInvisibilityMinCastTime", 15, Value::cost, Scoring::plain},
    {archetype_bound_weapon, -1, true, false, 1, CasterKind::bound_item, "fCombatMagicDefaultMinCastTime", 1, Value::cost, Scoring::plain},
    {0, k_av_damage_resist, true, false, 1, CasterKind::armor, "fCombatMagicArmorMinCastTime", 15, Value::magnitude, Scoring::plain},
    {archetype_reanimate, -1, false, false, 10, CasterKind::reanimate, "fCombatMagicReanimateMinCastTime", 15, Value::magnitude, Scoring::plain},
    {archetype_script, -1, true, false, 1, CasterKind::script, "fCombatMagicDefaultMinCastTime", 1, Value::cost, Scoring::script},
};

// Value modifiers also answer for these.
int32_t row_archetype(uint32_t archetype) {
    switch (archetype) {
    case archetype_dual_value_modifier: case archetype_absorb: case archetype_accumulate_magnitude: case archetype_peak_value_modifier:
        return 0;
    default: return int32_t(archetype);
    }
}

const Row* find_row(uint32_t archetype, int32_t av, bool self, bool hostile) {
    const int32_t wanted = row_archetype(archetype);
    for (const Row& r : k_rows) {
        if (r.archetype == wanted && r.self == self && r.hostile == hostile && (r.av < 0 || r.av == av)) {
            return &r;
        }
    }
    return nullptr;
}

float angle_to(const CombatBody& from, const CombatBody& to) {
    const float heading = std::atan2(to.position[0] - from.position[0], to.position[1] - from.position[1]) * 180.0f / std::numbers::pi_v<float>;
    return std::abs(std::remainder(heading - from.heading, 360.0f));
}

} // namespace

// CombatMagicItemData. Nothing back when no effect finds a row with a caster: the NPC never uses the spell.
std::optional<MagicItem> CombatAi::magic_item(uint32_t ref, const Spell& spell, uint32_t target) {
    const Magic& magic = m_game.magic();
    ActorValues& values = m_game.actor_values(ref);
    MagicItem item;
    item.spell = &spell;
    float best = -1.0f;
    const Row* primary = nullptr;
    for (const SpellEffect& e : spell.effects) {
        const MagicEffect* effect = magic.effect(e.effect);
        if (!effect) {
            continue;
        }
        const bool plain_spell = spell.type == spell_plain || spell.type == spell_poison;
        if (plain_spell && effect->magic_skill >= 0 && values.current(effect->magic_skill) < effect->minimum_skill) {
            return std::nullopt; // CombatMagicItemSkillChecker, SPEL only (1.1)
        }
        const bool self = spell.delivery == delivery_self, hostile = (effect->flags & effect_hostile) != 0;
        const float concentration = n("fCombatMagicConcentrationScoreDuration", 3.0f);
        float d = spell.cast_type == cast_concentration ? concentration : std::max(float(e.duration), 1.0f);
        if (!(effect->flags & (effect_no_duration | effect_no_magnitude)) && effect->taper_duration > 0.0f) {
            d += effect->taper_duration * effect->taper_weight * 0.5f;
        }
        const auto add = [&](int32_t av, float av_weight) {
            const Row* row = find_row(effect->archetype, av, self, hostile);
            if (!row) {
                return;
            }
            float value = (row->value == Value::magnitude ? e.magnitude : effect->base_cost) * av_weight * d;
            if (row->scoring == Scoring::script) {
                value = effect->script_ai_score;
            } else {
                if (hostile && effect->resist_value >= 0 && target) {
                    value *= std::max(1.0f - m_game.actor_values(target).current(effect->resist_value) / 100.0f, 0.0f);
                }
                if (row->scoring == Scoring::target_value && (!target || m_game.actor_values(target).percent(av) <= 0.2f)) {
                    value = 0.0f;
                }
            }
            value *= row->weight;
            item.score += value;
            if (row->kind != CasterKind::none && value > best) {
                best = value;
                primary = row;
                item.effect = effect;
                item.primary = &e;
            }
        };
        add(effect->actor_value, 1.0f);
        if (effect->second_actor_value >= 0) {
            add(effect->second_actor_value, effect->second_weight);
        }
    }
    if (!primary) {
        return std::nullopt;
    }
    item.kind = primary->kind;
    item.duration = spell.cast_type == cast_concentration ? 0.0f : float(item.primary->duration);
    item.magnitude = item.primary->magnitude * std::max(float(item.primary->duration), 1.0f);
    // A shout pays in recovery, a potion in itself.
    const bool free = spell.type == spell_voice || spell.type == spell_potion || spell.type == spell_scroll;
    const bool staff = spell.type == spell_staff_enchantment;
    item.cost = free ? 0.0f : m_game.casters().item_cost(spell, ref);
    if (spell.cast_type == cast_concentration) {
        item.cost *= n(primary->min_cast, primary->min_cast_default);
    }
    if (item.cost > values.maximum(av_magicka) && !staff) {
        return std::nullopt; // AV 64 and 82 are excepted: a staff's charge is its own
    }

    if (spell.delivery == delivery_aimed) {
        const SpellEffect* top = magic.costliest(spell, true);
        const MagicEffect* effect = top ? magic.effect(top->effect) : item.effect;
        if (const ProjectileRecord* p = effect ? m_game.projectiles().record(effect->projectile) : nullptr) {
            float range = p->range;
            if (p->is(ProjectileType::lobber) && p->gravity > 0.0f) {
                range = p->speed * p->speed / (k_gravity * p->gravity);
            }
            item.max_range = std::min(range * n("fCombatProjectileMaxRangeMult", 0.9f), n("fCombatMaximumProjectileRange", 4096.0f));
            if (item.max_range > n("fCombatRangedDistance", 1024.0f)) {
                item.min_range = n("fCombatRangedMinimumRange", 256.0f);
            }
            if (const ExplosionRecord* x = m_game.projectiles().explosion(effect->explosion ? effect->explosion : p->explosion)) {
                item.min_range = std::max(item.min_range, x->radius);
            }
            item.min_range = std::clamp(item.min_range, 0.0f, std::max(item.max_range - 256.0f, 0.0f));
        }
    } else if (spell.delivery == delivery_target_actor || spell.delivery == delivery_target_location) {
        item.max_range = n("fCombatMaximumRange", 4096.0f);
    }

    // The list it sits in (1.5).
    const float tactical = n("fCombatMagicTacticalDuration", 30.0f);
    switch (item.kind) {
    case CasterKind::offensive: item.list = 0; break;
    case CasterKind::restore: item.list = 1; break;
    case CasterKind::ward: item.list = item.duration >= n("fCombatMagicBuffDuration", 30.0f) ? 2 : 3; break;
    case CasterKind::light: item.list = 4; break;
    case CasterKind::paralyze: item.list = item.duration >= n("fCombatParalyzeTacticalDuration", 10.0f) ? 4 : 5; break;
    case CasterKind::target_effect:
        item.list = item.duration >= n("fCombatParalyzeTacticalDuration", 10.0f) || item.duration == 0.0f ? 4 : 5;
        break;
    case CasterKind::script: item.list = item.duration >= tactical ? 4 : 0; break;
    default: item.list = item.duration >= tactical ? 4 : 5; break;
    }
    const float type_mult = spell.type == spell_voice            ? style(ref, "CSGD.shout_score_mult", 1.0f)
                           : staff                             ? style(ref, "CSGD.staff_score_mult", 1.0f)
                           : free                              ? 1.0f
                                                               : style(ref, "CSGD.magic_score_mult", 1.0f);
    item.score *= type_mult * (item.max_range > n("fCombatRangedDistance", 1024.0f) ? n("fCombatInventoryRangedScoreMult", 2.0f) : 1.0f);
    return item;
}

// CombatInventory::Init, the spell part: the base form's, the race's and what was added.
void CombatAi::gather_spells(const Fighter& f, Brain& b) {
    b.magic.clear();
    const Magic& magic = m_game.magic();
    b.shouts.clear();
    b.potions.clear();
    for (const uint32_t form : m_game.magic_target(f.ref).all_spells()) {
        // AddForm: the highest variation with word and spell both set; unlocking doesn't come into it.
        if (const Shout* shout = magic.shout(form)) {
            for (int v = 2; v >= 0; --v) {
                const ShoutWord& w = shout->words[size_t(v)];
                const Spell* s = w.word ? magic.spell(w.spell) : nullptr;
                if (!s) {
                    continue;
                }
                if (auto item = magic_item(f.ref, *s, f.combat_target)) {
                    item->shout = shout;
                    item->variation = v;
                    b.shouts.push_back(*item);
                }
                break;
            }
            continue;
        }
        const Spell* s = magic.spell(form);
        if (s && (s->type == spell_plain || s->type == spell_poison) && s->cast_type != cast_constant) {
            if (auto item = magic_item(f.ref, *s, f.combat_target)) {
                b.magic.push_back(*item);
            }
        }
    }
    // Scrolls, and staves by what the fight can draw: both go in the hands' lists.
    for (const Stack& stack : m_game.inventory_of(f.ref).stacks()) {
        const Item* it = m_game.items().find(stack.form);
        const Spell* s = it && it->kind == ItemKind::scroll && stack.count > 0 ? magic.spell(stack.form) : nullptr;
        if (auto item = s ? magic_item(f.ref, *s, f.combat_target) : std::nullopt) {
            b.magic.push_back(*item);
        }
    }
    for (const uint32_t form : m_game.combat().world().weapons(f.ref)) {
        const Item* it = m_game.items().find(form);
        const Spell* s = it && it->animation_type == 8 && it->enchantment ? magic.spell(it->enchantment) : nullptr;
        if (auto item = s ? magic_item(f.ref, *s, f.combat_target) : std::nullopt) {
            item->object = form;
            b.magic.push_back(*item);
        }
    }
    // Any ALCH, food too; a poison's drink casts at nobody, what that does isn't known, so none here.
    for (const Stack& stack : m_game.inventory_of(f.ref).stacks()) {
        const Item* it = m_game.items().find(stack.form);
        const Spell* s = it && it->kind == ItemKind::potion && !it->poison() ? magic.spell(stack.form) : nullptr;
        if (s && stack.count > 0) {
            if (auto item = magic_item(f.ref, *s, f.combat_target)) {
                b.potions.push_back(*item);
            }
        }
    }
    const auto better = [](const MagicItem& x, const MagicItem& y) { return x.score > y.score; };
    std::stable_sort(b.magic.begin(), b.magic.end(), better);
    std::stable_sort(b.shouts.begin(), b.shouts.end(), better);
    std::stable_sort(b.potions.begin(), b.potions.end(), [](const MagicItem& x, const MagicItem& y) {
        const bool xr = x.kind == CasterKind::restore, yr = y.kind == CasterKind::restore;
        return xr != yr ? xr : x.score > y.score;
    });
}

void CombatAi::read_magic(const Fighter& f, Brain& b) {
    if (!b.magic_read) {
        b.magic_read = true;
        gather_spells(f, b);
    }
}

// CombatBehaviorTreeUsePotion: a check every 0.25 s, the drink then a wait. the game has no
// animation or sound: the effects land and the potion goes. It isn't held back by a flight.
void CombatAi::update_potion(Fighter& f, Brain& b, float dt, float distance) {
    read_magic(f, b);
    b.potion_wait -= dt;
    if (b.potions.empty() || b.potion_wait > 0.0f) {
        return;
    }
    b.potion_wait = 0.25f;
    Inventory& inventory = m_game.inventory_of(f.ref);
    for (size_t i = 0; i < b.potions.size(); ++i) {
        const MagicItem& item = b.potions[i];
        if (item.kind == CasterKind::none || !caster_passes(f, b, item, false, distance)) {
            continue;
        }
        const uint32_t form = item.spell->form;
        m_game.magic_target(f.ref).apply(form, f.ref);
        inventory.remove(form, 1);
        if (item.kind == CasterKind::restore) {
            MagicHand hand; // NotifyStartCast is what adds to the restored sum; other casters' timers don't start
            hand.item = &item;
            notify_stop(f, b, hand);
        }
        if (inventory.count(form) <= 0) {
            b.potions.erase(b.potions.begin() + std::ptrdiff_t(i));
        }
        b.potion_wait = n("fCombatMagicDrinkPotionWaitTime", 3.0f);
        return;
    }
}

// CombatBehaviorTreeShout. The equip test lets recovery run to 5 s of ready; Enter asks for
// the voice, range, sight, the caster, the aim, in that order. Then the shout runs out and 1 s passes.
void CombatAi::update_shout(Fighter& f, Brain& b, float dt, float distance) {
    b.shout_wait -= dt;
    if (b.shouts.empty()) {
        return;
    }
    Casters& casters = m_game.casters();
    if (casters.shouting(f.ref)) {
        b.shout_wait = 1.0f;
        return;
    }
    if (b.shout_wait > 0.0f) {
        return;
    }
    b.shout_wait = 0.25f;
    const float recovery = casters.voice_recovery(f.ref);
    if (recovery > n("fCombatInventoryShoutMaxRecoveryTime", 5.0f)) {
        return;
    }
    const Agent* agent = m_game.ai().agent(f.ref);
    const bool flying = agent && agent->fly != FlyState::none && agent->fly != FlyState::perching;
    const float arc = n("fCombatMagicProjectileFiringArc", 30.0f);
    CombatWorld& world = m_game.combat().world();
    for (const MagicItem& item : b.shouts) {
        if (item.kind == CasterKind::none || !caster_passes(f, b, item, false, distance)) {
            continue;
        }
        if ((flying || chance(cast_chance(f, item))) && recovery <= 0.0f && world.graph_bool(f.ref, "bVoiceReady") &&
            in_range(f, item, f.combat_target, distance) &&
            world.line_of_sight(f.ref, f.combat_target) && caster_passes(f, b, item, true, distance) &&
            aim_ok(f, item, f.combat_target, arc)) {
            casters.npc_shout(f.ref, item.shout->form, item.variation);
        }
        return;
    }
}

bool CombatAi::affords(const Fighter& f, const MagicItem& item) const {
    constexpr int k_av_right_charge = 64;
    if (item.spell->type == spell_scroll) {
        return m_game.inventory_of(f.ref).count(item.spell->form) > 0;
    }
    return item.cost <= 0.0f || m_game.actor_values(f.ref).current(item.object ? k_av_right_charge : av_magicka) > item.cost;
}

bool CombatAi::has_effect_yet(uint32_t subject, const MagicItem& item) {
    return item.spell->cast_type != cast_concentration && m_game.magic_target(subject).has_effect_from(item.spell->form);
}

bool CombatAi::effect_conditions(const Fighter& f, const MagicItem& item, uint32_t subject) {
    return !item.effect || item.effect->conditions.empty() || m_game.conditions().evaluate(item.effect->conditions, subject, f.ref);
}

float CombatAi::restore_threshold(const Fighter& f, const MagicItem& item) const {
    // by the defensive mult.
    const float def = style(f.ref, "CSGD.defensive_mult", 0.5f);
    return item.effect && item.effect->actor_value == av_magicka
               ? lerp(def, n("fCombatRestoreMagickaPercentMin", 0.25f), n("fCombatRestoreMagickaPercentMax", 0.4f))
               : lerp(def, n("fCombatRestorePercentMin", 0.0f), n("fCombatRestoreHealthPercentMax", 0.5f));
}

bool CombatAi::below_restore(const Fighter& f, const MagicItem& item, float margin) const {
    const int av = item.effect ? item.effect->actor_value : av_health;
    return restore_threshold(f, item) + margin > m_game.actor_values(f.ref).percent(av) ||
           (av == av_health && f.life == LifeState::bleedout);
}

bool CombatAi::under_attack(const Fighter& f, float distance, bool starting) {
    Combat& combat = m_game.combat();
    const Fighter& target = combat.fighter(f.combat_target);
    CombatBody self, other;
    if (!combat.world().body(f.ref, self) || !combat.world().body(f.combat_target, other) || angle_to(self, other) > 90.0f) {
        return false;
    }
    const bool swinging = starting ? target.attack_state == AttackState::draw
                                   : target.attack_state == AttackState::draw || target.attack_state == AttackState::swing;
    const float reach = combat.melee_reach(f.combat_target) * n("fCombatMagicWardAttackReachMult", 1.5f) + self.radius + other.radius;
    if (swinging && distance < reach) {
        return true;
    }
    for (const uint8_t source : {source_left, source_right}) {
        Caster& c = m_game.casters().caster(f.combat_target, source);
        const CastState lowest = starting ? CastState::ready : CastState::charging;
        if (c.item() && c.item()->delivery == delivery_aimed && c.state() >= lowest && c.state() <= CastState::released) {
            return true;
        }
    }
    return false;
}

uint32_t CombatAi::find_corpse(const Fighter& f, const MagicItem& item) {
    // FindCorpse: the highest level in reach that the effect takes.
    Combat& combat = m_game.combat();
    uint32_t best = 0;
    int best_level = -1;
    for (const uint32_t ref : combat.world().high_actors()) {
        if (!combat.fighter(ref).dead() || combat.target_distance(f.ref, ref, false) > n("fCombatMagicReanimateDistance", 1024.0f) ||
            !effect_conditions(f, item, ref)) {
            continue;
        }
        const int level = int(m_game.actor_values(ref).record().level);
        if (level > best_level) {
            best = ref;
            best_level = level;
        }
    }
    return best;
}

// CheckShouldEquip and CheckStartCast (section 2): `start` is the second.
bool CombatAi::caster_passes(const Fighter& f, Brain& b, const MagicItem& item, bool start, float distance) {
    // A summon's effect stays with its caster, wherever it was cast at.
    const uint32_t subject = item.spell->delivery == delivery_self || item.kind == CasterKind::summon ? f.ref : f.combat_target;
    const auto restricted = [&] { return b.restrict[size_t(item.kind)] > 0.0f; };
    const auto ready = [&] { return affords(f, item) && !has_effect_yet(subject, item); };
    const auto ready_conditions = [&] { return ready() && effect_conditions(f, item, subject); };
    const bool detected = b.seen_for >= 0.0f;
    const bool engaged = b.unseen < n("fCombatTargetEngagedLastSeenTime", 3.0f);
    const float long_time = n("fCombatMagicDefaultLongDuration", 30.0f), short_time = n("fCombatMagicDefaultShortDuration", 15.0f);
    const float tactical = n("fCombatMagicTacticalDuration", 30.0f);
    const Fighter& target = m_game.combat().fighter(f.combat_target);
    const float reach = m_game.combat().melee_reach(f.combat_target);
    const float units_per_foot = n("fBSUnitsPerFoot", 22.0f);
    const bool fleeing = b.fleeing;
    switch (item.kind) {
    case CasterKind::offensive:
        return !fleeing && (!start || (b.restrict[size_t(item.kind)] <= 0.0f && affords(f, item)));
    case CasterKind::restore:
        return !restricted() && ready_conditions() && below_restore(f, item, 0.0f);
    case CasterKind::ward: {
        if (!ready_conditions()) {
            return false;
        }
        if (!start && (m_game.actor_values(f.ref).percent(av_magicka) <= n("fCombatMagicWardMagickaEquipLimit", 0.5f) ||
                       distance > reach + n("fCombatMagicWardAttackRangeDistance", 384.0f))) {
            return false;
        }
        return item.duration > long_time ? detected : item.duration > short_time ? engaged : start ? under_attack(f, distance, true) : true;
    }
    case CasterKind::summon:
        return !fleeing && !restricted() && ready_conditions() && (item.duration > long_time ? detected : engaged);
    case CasterKind::stagger:
        if (!start) {
            return !fleeing;
        }
        return ready_conditions() && f.combat_target &&
               (target.attack_state != AttackState::none || distance < n("fCombatMagicStaggerDistance", 256.0f));
    case CasterKind::disarm:
        return !fleeing && !restricted() && target.weapon_drawn && affords(f, item) && effect_conditions(f, item, subject) &&
               distance < n("fCombatMagicDisarmDistance", 256.0f) + reach;
    case CasterKind::cloak: {
        if (fleeing || !ready()) {
            return false;
        }
        const float r = item.primary->magnitude * units_per_foot + n("fCombatMagicCloakDistanceMin", 128.0f);
        return distance < r || (item.duration >= tactical && reach < r);
    }
    case CasterKind::light:
        return !fleeing && f.searching && ready();
    case CasterKind::invisibility:
        return ready() && (start || fleeing || (distance > n("fCombatMagicInvisibilityDistance", 1024.0f) && !b.hands[1].item));
    case CasterKind::bound_item:
        return !fleeing && ready_conditions() && distance < n("fCombatMagicBoundItemDistance", 1024.0f) + reach;
    case CasterKind::armor:
        return !fleeing && ready() && (start || item.duration >= tactical || distance < n("fCombatMagicArmorDistanceMin", 256.0f) + reach);
    case CasterKind::target_effect:
        return !fleeing && ready_conditions();
    case CasterKind::paralyze:
        if (start) {
            return ready();
        }
        return !fleeing && !restricted() && ready_conditions() && distance < n("fCombatMagicParalyzeDistance", 384.0f) + std::max(item.max_range, reach);
    case CasterKind::reanimate:
        return !fleeing && !restricted() && affords(f, item) && find_corpse(f, item);
    case CasterKind::script:
        return !fleeing && !restricted() && ready_conditions();
    default: return false;
    }
}

// What the best weapon scores, with the range penalties of combat-actors 3.3: the rival for list 0.
float CombatAi::weapon_rival(const Fighter& f, float distance) {
    Combat& combat = m_game.combat();
    const Held held = combat.gear(f.ref).right;
    const Item* w = held.item;
    const bool ranged = w && (w->animation_type == 7 || w->animation_type == 9);
    float score = w ? combat.weapon_base(f.ref, held, 1.0f) * w->speed : 0.0f;
    score *= ranged ? n("fCombatDPSBowSpeedMult", 0.2f) * style(f.ref, "CSGD.ranged_score_mult", 1.0f)
                    : n("fCombatDPSMeleeSpeedMult", 1.0f) * style(f.ref, "CSGD.melee_score_mult", 1.0f);
    const float range = ranged ? n("fCombatMaximumRange", 4096.0f) : n("fCombatInventoryMeleeEquipRange", 256.0f) + combat.melee_reach(f.ref);
    if (range < distance) {
        score *= n("fCombatInventoryMaxRangeScoreMult", 0.1f) * range / distance;
    }
    return score;
}

float CombatAi::item_score_now(const MagicItem& item, float distance) const {
    float score = item.score;
    if (item.max_range > 0.0f && item.max_range < distance) {
        score *= n("fCombatInventoryMaxRangeScoreMult", 0.1f) * item.max_range / distance;
    } else if (item.min_range > 0.0f && item.min_range + n("fCombatInventoryMinRangeUnequippedBonus", 256.0f) > distance) {
        score *= n("fCombatInventoryMinRangeScoreMult", 0.1f);
    }
    return score;
}

// BuildSlotLists for one NPC: each hand walks the lists in order and takes the first item that may go in it.
void CombatAi::fill_hands(Fighter& f, Brain& b, float distance) {
    static constexpr int k_order[] = {1, 2, 4, 0, 3, 5, 0};
    Combat& combat = m_game.combat();
    float pool = m_game.actor_values(f.ref).current(av_magicka);
    const float required = n("fCombatInventoryResourceCurrentRequiredMult", 1.0f);
    const float rival = weapon_rival(f, distance);
    const bool shield = combat.gear(f.ref).shield != nullptr;
    for (const int hand : {0, 1}) {
        const MagicItem* pick = nullptr;
        for (const int list : k_order) {
            if (pick || (hand == 0 && list == 3 && shield)) {
                break;
            }
            for (const MagicItem& item : b.magic) {
                const float paid = item.object ? 0.0f : item.cost;
                if (item.list != list || (item.object && hand == 0) || paid * required >= pool || !caster_passes(f, b, item, false, distance)) {
                    continue;
                }
                if (list == 0 && hand == 1 && item_score_now(item, distance) < rival) {
                    continue;
                }
                pick = &item;
                break;
            }
            if (!pick && list == 0 && hand == 1 && combat.gear(f.ref).right.item) {
                break; // the weapon stays
            }
        }
        if (pick && !pick->object) {
            pool -= pick->cost;
        }
        hold(f, b, hand, pick);
    }
}

void CombatAi::hold(Fighter& f, Brain& b, int hand, const MagicItem* item) {
    MagicHand& h = b.hands[size_t(hand)];
    const uint8_t source = hand == 0 ? source_left : source_right;
    Casters& casters = m_game.casters();
    if (h.item == item || casters.caster(f.ref, source).busy()) {
        return;
    }
    // a hand keeps what it holds for the type's minimum, while weapons are out.
    if (h.item && h.held < n("fCombatInventoryMinEquipTimeMagic", 3.0f)) {
        return;
    }
    CombatWorld& world = m_game.combat().world();
    const uint32_t staff = item ? item->object : 0;
    if (h.item && h.item->object && !staff) {
        // CombatBehaviorEquipObject: the staff goes back for the best weapon.
        for (const uint32_t form : world.weapons(f.ref)) {
            const Item* w = m_game.items().find(form);
            if (w && w->animation_type != 8) {
                world.swap_weapon(f.ref, form);
                break;
            }
        }
    }
    h = MagicHand{};
    h.item = item;
    if (staff) {
        world.swap_weapon(f.ref, staff);
        casters.prime_charge(f.ref, false, m_game.combat().gear(f.ref).right);
    }
    casters.select(f.ref, source, item ? item->spell->form : 0, staff);
    const Item* weapon = m_game.combat().gear(f.ref).right.item;
    const int empty = hand == 1 && weapon ? weapon->animation_type : 0;
    world.graph_int(f.ref, hand == 0 ? "iLeftHandType" : "iRightHandType", item && !staff ? k_hand_spell : empty);
}

float CombatAi::cast_chance(const Fighter& f, const MagicItem& item) const {
    if (item.kind != CasterKind::offensive) {
        return 1.0f;
    }
    const float off = style(f.ref, "CSGD.offensive_mult", 0.5f);
    return item.spell->cast_type == cast_concentration
               ? lerp(off, n("fCombatCastConcentrationOffensiveMagicChanceMin", 0.25f), n("fCombatCastConcentrationOffensiveMagicChanceMax", 1.0f))
               : lerp(off, n("fCombatCastImmediateOffensiveMagicChanceMin", 0.05f), n("fCombatCastImmediateOffensiveMagicChanceMax", 0.75f));
}

bool CombatAi::aim_ok(const Fighter& f, const MagicItem& item, uint32_t target, float arc) {
    if (item.spell->delivery != delivery_aimed && item.spell->delivery != delivery_target_actor) {
        return true;
    }
    Combat& combat = m_game.combat();
    CombatBody self, other;
    return combat.world().body(f.ref, self) && combat.world().body(target, other) && angle_to(self, other) <= arc;
}

bool CombatAi::in_range(const Fighter& f, const MagicItem& item, uint32_t target, float distance) {
    if (item.max_range <= 0.0f) {
        return item.spell->delivery != delivery_touch || distance <= m_game.combat().melee_reach(f.ref);
    }
    return distance <= item.max_range;
}

void CombatAi::notify_stop(const Fighter& f, Brain& b, MagicHand& h) {
    // caster: the restrict timers start as a cast ends.
    const MagicItem& item = *h.item;
    switch (item.kind) {
    case CasterKind::summon: b.restrict[size_t(item.kind)] = n("fCombatMagicSummonRestrictTime", 30.0f); break;
    case CasterKind::disarm: b.restrict[size_t(item.kind)] = n("fCombatMagicDisarmRestrictTime", 30.0f); break;
    case CasterKind::reanimate: b.restrict[size_t(item.kind)] = n("fCombatMagicReanimateRestrictTime", 30.0f); break;
    case CasterKind::paralyze: b.restrict[size_t(item.kind)] = n("fCombatMagicParalyzeRestrictTime", 30.0f); break;
    case CasterKind::script: b.restrict[size_t(item.kind)] = item.effect ? item.effect->script_ai_delay : 0.0f; break;
    case CasterKind::offensive:
        if (item.spell->cast_type == cast_concentration) {
            // the wait comes off a hand's next offensive cast.
            const float off = style(f.ref, "CSGD.offensive_mult", 0.5f);
            const float low = n("fCombatCastConcentrationOffensiveMagicWaitTimeMin", 0.5f);
            const float high = lerp(off, n("fCombatCastConcentrationOffensiveMagicWaitTimeMax", 2.5f), low);
            b.restrict[size_t(item.kind)] = std::max(low, lerp(random_unit(), low, high) - m_game.casters().charge_time(*item.spell));
        }
        break;
    case CasterKind::restore: {
        // TrackRestored: past the threshold, the hand rests.
        const int av = item.effect ? item.effect->actor_value : av_health;
        b.restored += item.magnitude;
        if (b.restored > m_game.actor_values(f.ref).base(av) * (restore_threshold(f, item) + n("fCombatRestoreStopCastThreshold", 0.25f))) {
            b.restrict[size_t(item.kind)] = av == av_magicka ? n("fCombatRestoreMagickaRestrictTime", 10.0f) : n("fCombatRestoreHealthRestrictTime", 15.0f);
            b.restored = 0;
        }
        break;
    }
    default: break;
    }
}

bool CombatAi::should_stop(const Fighter& f, Brain& b, const MagicHand& h, float distance) {
    const MagicItem& item = *h.item;
    switch (item.kind) {
    case CasterKind::offensive: return h.cast_left <= 0.0f;
    case CasterKind::restore:
        return (h.cast_left <= 0.0f && h.casting_for >= n("fCombatMagicConcentrationMinCastTime", 2.0f)) ||
               !below_restore(f, item, n("fCombatRestoreStopCastThreshold", 0.25f));
    case CasterKind::ward:
        if (m_game.actor_values(f.ref).percent(av_magicka) < n("fCombatMagicWardMagickaCastLimit", 0.25f)) {
            return true;
        }
        if (under_attack(f, distance, false)) {
            b.ward_left = n("fCombatMagicWardCooldownTime", 1.0f);
        }
        return b.ward_left <= 0.0f;
    case CasterKind::stagger: return !f.combat_target || m_game.combat().fighter(f.combat_target).staggered;
    case CasterKind::cloak:
        return item.primary->magnitude * n("fBSUnitsPerFoot", 22.0f) + n("fCombatMagicCloakDistanceMax", 384.0f) < distance;
    case CasterKind::light: return !f.searching;
    case CasterKind::armor:
        return distance > n("fCombatMagicArmorDistanceMax", 512.0f) + m_game.combat().melee_reach(f.combat_target);
    default: return false;
    }
}

void CombatAi::cast_hand(Fighter& f, Brain& b, int hand, float dt, float distance) {
    MagicHand& h = b.hands[size_t(hand)];
    h.held += dt;
    if (!h.item) {
        return;
    }
    const MagicItem& item = *h.item;
    const uint8_t source = hand == 0 ? source_left : source_right;
    Casters& casters = m_game.casters();
    CombatWorld& world = m_game.combat().world();
    Caster& caster = casters.caster(f.ref, source);
    const uint32_t target = item.kind == CasterKind::reanimate ? h.target
                            : item.spell->delivery == delivery_self ? f.ref : f.combat_target;
    const float arc = n("fCombatMagicProjectileFiringArc", 30.0f);
    h.cast_left -= dt;

    if (h.stage == MagicHand::Stage::idle) {
        h.idle -= dt;
        if (h.idle > 0.0f || !f.weapon_drawn) {
            return;
        }
        h.idle = 0.25f; // the tree's Idle
        const bool concentration = item.spell->cast_type == cast_concentration;
        if (caster.busy() || !world.graph_bool(f.ref, hand == 0 ? "bMLh_Ready" : "bMRh_Ready") ||
            !in_range(f, item, target, distance) || !caster_passes(f, b, item, true, distance)) {
            return;
        }
        if (item.kind == CasterKind::reanimate) {
            h.target = find_corpse(f, item);
            if (!h.target) {
                return;
            }
        }
        if (concentration && (!world.line_of_sight(f.ref, target) || !aim_ok(f, item, target, arc))) {
            return;
        }
        // One creature at a time: the other hand holding the same spell waits (*inferred*, the limit would only send one back).
        if (const MagicHand& other = b.hands[size_t(1 - hand)];
            (item.kind == CasterKind::summon || item.kind == CasterKind::reanimate) && other.item && other.item->kind == item.kind &&
            other.stage != MagicHand::Stage::idle) {
            return;
        }
        const float chance_now = cast_chance(f, item);
        const bool dual = hand == 0 && b.hands[1].item && b.hands[1].item->spell == item.spell && chance(n("fCombatMagicDualCastChance", 0.33f) * chance_now) &&
                          casters.can_dual_cast(f.ref, *item.spell);
        if (!dual && !chance(chance_now)) {
            return;
        }
        if (!caster.request(*item.spell, item.kind == CasterKind::reanimate ? h.target : target, 0, dual)) {
            return;
        }
        h.stage = concentration ? MagicHand::Stage::concentrating : MagicHand::Stage::charging;
        h.casting_for = 0;
        h.check = 1.0f;
        h.hold = -1.0f;
        if (concentration) {
            const float off = style(f.ref, "CSGD.offensive_mult", 0.5f);
            const float low = n("fCombatCastConcentrationOffensiveMagicCastTimeMin", 4.0f);
            h.cast_left = lerp(random_unit(), low, lerp(off, low, n("fCombatCastConcentrationOffensiveMagicCastTimeMax", 10.0f))) + casters.charge_time(*item.spell);
        }
        return;
    }

    if (h.stage == MagicHand::Stage::charging) {
        if (caster.state() == CastState::idle) {
            h.stage = MagicHand::Stage::idle;
            return;
        }
        if (caster.state() != CastState::ready) {
            return;
        }
        if (h.hold < 0.0f) {
            // Hold time.
            float x = style(f.ref, "CSGD.offensive_mult", 0.5f);
            h.hold = 0.01f;
            if (item.kind == CasterKind::offensive) {
                if (distance < n("fCombatCastImmediateOffensiveMagicHoldTimeMinDistance", 256.0f)) {
                    x = 1.0f;
                }
                const float top = lerp(x, n("fCombatCastImmediateOffensiveMagicHoldTimeMax", 1.5f), n("fCombatCastImmediateOffensiveMagicHoldTimeMin", 0.5f));
                h.hold = lerp(random_unit(), n("fCombatCastImmediateOffensiveMagicHoldTimeAbsoluteMin", 0.1f), top);
            }
        }
        h.hold -= dt;
        if (h.hold > 0.0f) {
            return;
        }
        if (target != f.ref && (!world.line_of_sight(f.ref, target) || !aim_ok(f, item, target, arc))) {
            h.hold = 0.25f; // it keeps holding, retrying
            return;
        }
        caster.release();
        h.stage = MagicHand::Stage::released;
        return;
    }

    if (h.stage == MagicHand::Stage::released) {
        if (caster.state() == CastState::idle) {
            notify_stop(f, b, h);
            h.stage = MagicHand::Stage::idle;
        }
        return;
    }

    // Concentration: the first check a second in, then every 0.25 s.
    h.casting_for += dt;
    h.check -= dt;
    if (h.check > 0.0f) {
        return;
    }
    h.check = 0.25f;
    const float wide = arc * n("fCombatMagicConcentrationFiringArcMult", 1.5f);
    if (caster.state() == CastState::idle || should_stop(f, b, h, distance) || !in_range(f, item, target, distance) || !aim_ok(f, item, target, wide)) {
        if (caster.state() != CastState::idle) {
            caster.interrupt(false);
        }
        notify_stop(f, b, h);
        h.stage = MagicHand::Stage::idle;
    }
}

float CombatAi::magic_stop_distance(const Fighter& f, const Brain& b) const {
    // The desired list's ranges feed movement: the offensive hand's optimal range.
    float stop = -1.0f;
    for (const MagicHand& h : b.hands) {
        if (h.item && h.item->max_range > 0.0f) {
            const float optimal = h.item->min_range + (h.item->max_range - h.item->min_range) * n("fCombatInventoryOptimalRangePercent", 0.75f);
            stop = stop < 0.0f ? optimal : std::min(stop, optimal);
        }
    }
    return stop;
}

void CombatAi::update_magic(Fighter& f, Brain& b, float dt, float distance, bool seen) {
    b.seen_for = seen ? std::max(b.seen_for, 0.0f) + dt : b.seen_for;
    for (float& t : b.restrict) {
        t = std::max(t - dt, 0.0f);
    }
    b.ward_left -= dt;
    b.inventory -= dt;
    if (b.inventory <= 0.0f) {
        b.inventory = 1.0f; // CombatInventory::Update, every second
        read_magic(f, b);
        if (!b.magic.empty()) {
            fill_hands(f, b, distance);
        }
    }
    cast_hand(f, b, 0, dt, distance);
    cast_hand(f, b, 1, dt, distance);
    update_shout(f, b, dt, distance);
}

} // namespace spud::game
