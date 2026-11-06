#include "core/log.hpp"
#include "game/combat.hpp"

#include "game/game.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spud::game {

namespace {

using forms::Form;
using forms::Value;

// The actor values and entry points only hits name.
constexpr int k_shield_perks = 66, k_bow_stagger_bonus = 87, k_attack_damage_mult = 154, k_reflect_damage = 163;
constexpr int ep_crit_chance = 1, ep_crit_damage = 2, ep_enemy_crit_chance = 17, ep_sneak_mult = 18, ep_bash_damage = 26,
              ep_power_stamina = 27, ep_power_damage = 28, ep_incoming_stagger = 33, ep_target_stagger = 34,
              ep_incoming_damage = 36, ep_target_resist = 37, ep_percent_blocked = 39, ep_sweep = 50,
              ep_combat_hit_spell = 51, ep_bash_spell = 52, ep_soul_to_weapon = 78;
constexpr uint32_t k_shield_slot = 0x200; // biped slot 39
constexpr uint32_t k_weapon_non_hostile = 0x1000, k_weapon_minor_crime = 0x10;
constexpr uint32_t k_flag_essential = 0x2, k_flag_protected = 0x800;
constexpr int k_comment_hit = 29, k_comment_block = 35;
constexpr float k_two_pi = 2.0f * std::numbers::pi_v<float>;

bool melee_type(const Item* weapon) { return !weapon || weapon->animation_type <= 6; }
bool ranged_type(const Item* weapon) {
    return weapon && (weapon->animation_type == 7 || weapon->animation_type == 8 || weapon->animation_type == 9);
}

float heading_to(const Units3& from, const Units3& to) {
    return std::atan2(to[0] - from[0], to[1] - from[1]) * 180.0f / std::numbers::pi_v<float>;
}

float fold_angle(float degrees) {
    float a = std::fmod(std::abs(degrees), 360.0f);
    return a > 180.0f ? 360.0f - a : a;
}

} // namespace

Combat::Combat(Game& game) : m_game(game) {}
Combat::~Combat() = default;

float Combat::n(std::string_view name, float fallback) const { return m_game.settings().number(name, fallback); }

float Combat::perk(uint32_t owner, int point, float value, std::initializer_list<uint32_t> args) const {
    return owner == k_player_ref ? m_game.character().entry_point(point, value, args) : value;
}

uint32_t Combat::perk_spell(uint32_t owner, int point, std::initializer_list<uint32_t> args) const {
    return owner == k_player_ref ? m_game.character().entry_point_spell(point, args) : 0;
}

float Combat::av(uint32_t ref, int actor_value) { return m_game.actor_values(ref).current(actor_value); }

Fighter& Combat::fighter(uint32_t ref) {
    const auto [it, added] = m_fighters.try_emplace(ref);
    if (added) {
        Fighter& f = it->second;
        f.ref = ref;
        f.npc = m_game.base_of(ref);
        ActorValues& values = m_game.actor_values(ref);
        f.race = values.record().race;
        if (ref != k_player_ref) {
            values.derive = [this, ref](int actor_value) {
                return actor_value == av_damage_resist ? armor_sum(ref) : 0.0f;
            };
        }
    }
    return it->second;
}

Gear Combat::gear(uint32_t ref) {
    Gear g;
    const Items& items = m_game.items();
    auto add_armor = [&](const Item* item) {
        if (item->armor_type != 2) {
            g.armor.push_back(item);
        }
        if (item->biped_slots & k_shield_slot) {
            g.shield = item;
        }
    };
    if (ref == k_player_ref) {
        for (Stack& s : m_game.player().inventory.stacks()) {
            const Item* item = s.worn == Worn::none ? nullptr : items.find(s.form);
            if (!item) {
                continue;
            }
            if (item->kind == ItemKind::armor) {
                add_armor(item);
            } else if (item->kind == ItemKind::weapon) {
                const Held held{item, &s, std::max(s.extra.health, 1.0f)};
                if (s.worn == Worn::right || s.worn == Worn::both || s.worn == Worn::worn) {
                    g.right = held;
                }
                if (s.worn == Worn::left || s.worn == Worn::both) {
                    g.left = held;
                }
            }
        }
        return g;
    }
    std::vector<uint32_t> worn;
    uint32_t right = 0, left = 0;
    m_world->gear(ref, worn, right, left);
    for (uint32_t form : worn) {
        if (const Item* item = items.find(form); item && item->kind == ItemKind::armor &&
            std::ranges::find(g.armor, item) == g.armor.end()) {
            add_armor(item);
        }
    }
    g.right.item = right ? items.find(right) : nullptr;
    g.left.item = left ? items.find(left) : nullptr;
    return g;
}

float Combat::armor_sum(uint32_t ref) {
    // An NPC's own worn armor by, with the NPC settings; it has no tempering.
    const Fighter& f = fighter(ref);
    const float low = n("fArmorRatingBase", 1.0f), high = n("fArmorRatingMax", 2.5f);
    ActorValues& values = m_game.actor_values(f.ref);
    float total = 0;
    for (const Item* item : gear(ref).armor) {
        const float skill = values.current(item->armor_type == 0 ? av_light_armor : av_heavy_armor);
        const float factor = (high - low) * skill * 0.01f + low;
        total += std::ceil(float(item->armor_rating) / 100.0f * factor);
    }
    return total;
}

bool Combat::invulnerable(uint32_t ref) { return fighter(ref).invulnerable; }

bool Combat::blocking(uint32_t ref) { return m_world->graph_bool(ref, "IsBlocking"); }

void Combat::update(float dt) {
    std::vector<uint32_t> risen;
    for (auto& [ref, f] : m_fighters) {
        // down, threshold / fBleedoutRate a second, less in combat; none under SetNoBleedoutRecovery. It
        // starts 0.5 s in (measured). Essential actors stand at 1 health and that clears the damage; the
        // others at the threshold (*inferred*: the process keeps that number at +696).
        if ((f.life == LifeState::essential_down || f.life == LifeState::bleedout) && !m_game.ref_flag(ref, ref_no_bleedout_recovery) &&
            (f.down_wait -= dt) <= 0.0f) {
            ActorValues& v = m_game.actor_values(ref);
            const float threshold = bleedout_threshold(ref);
            float rate = threshold / n("fBleedoutRate", 15.0f) * v.current(155) * 0.01f;
            if (f.in_combat) {
                const float mult = v.current(134);
                rate *= mult != 0.0f ? mult : n("fEssentialDownCombatHealthRegenMult", 0.75f);
            }
            v.restore(av_health, rate * dt);
            if (v.current(av_health) >= (f.life == LifeState::essential_down ? 1.0f : threshold)) {
                risen.push_back(ref);
            }
        }
        f.power_cooldown = std::max(f.power_cooldown - dt, 0.0f);
        if (f.staggered && (f.stagger_timer -= dt) <= 0.0f) {
            f.staggered = false;
        }
        if (f.in_kill_move && (f.kill_timer -= dt) <= 0.0f) {
            end_kill_move(ref);
        }
        f.last_friend_hit += dt;
        const float keep = n("fFriendHitTimer", 10.0f);
        for (float& age : f.friend_hits) {
            age += dt;
        }
        std::erase_if(f.friend_hits, [keep](float age) { return age > keep; });
    }
    for (const uint32_t ref : risen) {
        get_up(ref);
    }
}

std::vector<uint32_t> Combat::fighting() const {
    std::vector<uint32_t> out;
    for (const auto& [ref, f] : m_fighters) {
        if (f.in_combat) {
            out.push_back(ref);
        }
    }
    return out;
}

// A magic item's keyword form holds its effects' keywords too: spells carry no KWDA, yet CastRightWard asks for MagicWard.
bool Combat::spell_has_keyword(const Spell& spell, uint32_t keyword) {
    if (std::ranges::find(spell.keywords, keyword) != spell.keywords.end()) {
        return true;
    }
    return std::ranges::any_of(spell.effects, [&](const SpellEffect& e) {
        const MagicEffect* m = m_game.magic().effect(e.effect);
        return m && std::ranges::find(m->keywords, keyword) != m->keywords.end();
    });
}

bool Combat::condition_value(uint16_t fn, uint32_t subject, uint32_t, uint32_t p1, uint32_t p2, double& out) {
    const auto is = [&out](bool b) {
        out = b ? 1.0 : 0.0;
        return true;
    };
    if (!subject) {
        return false;
    }
    switch (fn) {
    case 0: return is(fighter(subject).want_block);                              // GetWantBlocking
    case 46: return is(fighter(subject).dead());                                 // GetDead
    case 101: return is(fighter(subject).weapon_drawn);                          // IsWeaponMagicOut
    case 103: return is(fighter(subject).weapon_drawn && gear(subject).shield);  // IsShieldOut
    case 108: {                                                                  // GetWeaponAnimType
        const Item* w = gear(subject).right.item;
        const int type = w ? w->animation_type : 0;
        out = type >= 9 ? type + 3 : type;
        return true;
    }
    case 185: return is(fighter(subject).swimming);                              // IsSwimming
    case 286: return is(fighter(subject).sneaking);                              // IsSneaking
    case 568: return is(fighter(subject).running);                               // IsSprinting
    case 569: return is(blocking(subject));                                      // IsBlocking
    case 570:                                                                    // HasEquippedSpell
    case 571:                                                                    // GetCurrentCastingType
    case 572:                                                                    // GetCurrentDeliveryType
    case 596: {                                                                  // SpellHasKeyword
        // The hand's selected spell; the casting type asks the caster's current spell first.
        out = 0;
        if (p1 > source_instant) {
            return true;
        }
        Casters& casters = m_game.casters();
        const Spell* spell = m_game.magic().spell(casters.selected(subject, uint8_t(p1)));
        if (fn == 571 && casters.caster(subject, uint8_t(p1)).item()) {
            spell = casters.caster(subject, uint8_t(p1)).item();
        }
        if (spell) {
            out = fn == 570   ? double(spell->type != spell_enchantment)
                  : fn == 571 ? double(spell->cast_type)
                  : fn == 572 ? double(spell->delivery)
                              : double(spell_has_keyword(*spell, p2));
        }
        return true;
    }
    case 574: out = double(fighter(subject).attack_state); return true;          // GetAttackState
    case 627: return is(m_game.casters().dual_casting(subject));                 // IsDualCasting
    case 664: out = p1 <= 1 ? replaced_item[p1] : -1; return true;               // GetReplacedItemType
    case 580: return is(fighter(subject).life == LifeState::bleedout);           // IsBleedingOut
    case 597: {                                                                  // GetEquippedItemType
        const Gear g = gear(subject);
        // Both hands hold a two-handed weapon: BowAttack asks for type 7 at source 0.
        const int right = g.right.item ? g.right.item->animation_type : 0;
        const bool two_handed = right == 5 || right == 6 || right == 7 || right == 9;
        const Held& held = p1 == 0 && !two_handed ? g.left : g.right;
        if (m_game.casters().selected(subject, p1 == 0 ? source_left : source_right)) {
            out = 9;
        } else if (p1 == 0 && g.shield) {
            out = 10;
        } else if (!held.item) {
            out = 0;
        } else {
            out = held.item->animation_type == 9 ? 12 : held.item->animation_type;
        }
        return true;
    }
    case 589: {                                                                  // GetMovementDirection: 0, then 1 ahead, right, back, left
        constexpr float k_tau = 6.2831855f;
        float direction = 0;
        out = 0;
        if (m_world->moving(subject, direction)) {
            const float wrapped = std::fmod(std::fmod(direction, k_tau) + k_tau, k_tau);
            out = std::floor(std::fmod(wrapped * 0.63661975f + 0.5f, 4.0f)) + 1.0f;
        }
        return true;
    }
    case 672: return is(fighter(subject).attack_state != AttackState::none);     // IsAttacking
    case 673: {                                                                  // IsPowerAttacking
        const Fighter& f = fighter(subject);
        return is(f.attack_state != AttackState::none && f.attack && f.attack->power());
    }
    case 397: return is(m_world->limb_gone(subject, p1));                        // IsLimbGone
    case 678: return is(should_attack_kill(subject, p1));                        // ShouldAttackKill
    case 674: return is(last_hostile(subject));                                  // IsLastHostileActor
    case 701: return is(fighter(subject).staggered);                             // IsStaggered
    default: return false;
    }
}

// ---- attack data ---------------------------------------------------------------------------

const std::vector<AttackData>& Combat::attacks_of(uint32_t form) {
    const auto [it, added] = m_attacks.try_emplace(form);
    if (!added) {
        return it->second;
    }
    const forms::FormEntry* e = m_game.db().find(form);
    if (!e) {
        return it->second;
    }
    const Form f(m_game.db(), *e);
    const auto real = [](const Value& v, std::string_view name) {
        const Value* m = v.member(name);
        return m ? float(m->real) : 0.0f;
    };
    for (const forms::DecodedField& field : f.fields()) {
        if (field.sig == forms::sig("ATKD")) {
            AttackData a;
            const Value& v = field.value;
            a.damage_mult = real(v, "damage_mult");
            a.attack_chance = real(v, "attack_chance");
            const Value* spell = v.member("attack_spell");
            a.spell = spell ? spell->form : 0;
            const Value* flags = v.member("attack_flags");
            a.flags = flags ? uint32_t(flags->integer) : 0;
            a.attack_angle = real(v, "attack_angle");
            a.strike_angle = real(v, "strike_angle");
            a.stagger = real(v, "stagger");
            a.knockdown = real(v, "knockdown");
            a.recovery_time = real(v, "recovery_time");
            a.stamina_mult = real(v, "stamina_mult");
            it->second.push_back(std::move(a));
        } else if (field.sig == forms::sig("ATKE") && !it->second.empty()) {
            it->second.back().event = field.value.text;
        }
    }
    return it->second;
}

std::vector<AttackData> Combat::attacks(uint32_t ref) {
    const Fighter& f = fighter(ref);
    const std::vector<AttackData>& own = attacks_of(f.npc);
    return own.empty() ? attacks_of(f.race) : own;
}

const AttackData* Combat::attack_data(uint32_t ref, std::string_view event) {
    const Fighter& f = fighter(ref);
    for (const uint32_t form : {f.npc, f.race}) {
        for (const AttackData& a : attacks_of(form)) {
            if (a.event.size() == event.size() &&
                std::ranges::equal(a.event, event, [](char x, char y) { return std::tolower(x) == std::tolower(y); })) {
                return &a;
            }
        }
    }
    return nullptr;
}

bool Combat::attack_allowed(uint32_t ref, const AttackData* attack) {
    const Fighter& f = fighter(ref);
    if (!attack || f.in_combat && ref != k_player_ref) {
        return true; // a running combat controller decides for itself
    }
    if (attack->power() && f.power_cooldown > 0.0f) {
        return false;
    }
    if (attack_stamina(ref, *attack) > 0.0f && av(ref, av_stamina) <= 0.0f) {
        if (ref == k_player_ref) {
            m_world->blink_meter(av_stamina);
        }
        return false;
    }
    return true;
}

void Combat::attack_started(uint32_t ref, std::string_view event, uint32_t target) {
    Fighter& f = fighter(ref);
    std::string lower(event);
    std::ranges::transform(lower, lower.begin(), [](char c) { return char(std::tolower(c)); });
    if (lower.starts_with("pa_kill")) {
        f.attack_state = AttackState::draw;
        begin_kill_move(ref, 0);
        if (const uint32_t victim = target ? target : f.combat_target) {
            begin_kill_move(victim, ref);
        }
        return;
    }
    if (lower == "attackrelease") {
        if (f.attack_state == AttackState::drawn) {
            f.attack_state = AttackState::released;
        }
        return;
    }
    if (lower == "blockstart") {
        f.want_block = true;
        return;
    }
    if (lower == "blockstop") {
        f.want_block = false;
        return;
    }
    if (lower == "recoilstart" || lower == "recoillargestart") {
        f.recoiling = true;
        return;
    }
    if (lower == "bowattackstart" || lower == "crossbowattackstart") {
        f.attack_state = AttackState::bow;
        return;
    }
    // no match, and the old one stays until attack stop.
    if (const AttackData* attack = attack_data(ref, event)) {
        f.attack = attack;
        const Item* w = gear(ref).right.item;
        f.attack_state = attack->bash()                                               ? AttackState::bash
                         : w && (w->animation_type == 7 || w->animation_type == 9) ? AttackState::bow
                                                                                       : AttackState::draw;
    }
}

void Combat::animation_event(uint32_t ref, std::string_view tag, std::string_view payload) {
    Fighter& f = fighter(ref);
    if (tag == "HitFrame") {
        if (!f.dead()) {
            weapon_hit_frame(ref, payload == "Left");
            if (f.attack_state == AttackState::swing) {
                f.attack_state = AttackState::hit;
            }
        }
    } else if (tag == "weaponSwing" || tag == "weaponLeftSwing") {
        f.attack_state = AttackState::swing;
        // WeaponRight/the game,: the hand's WEAP swing sound at the actor.
        const Gear g = gear(ref);
        const Item* w = tag == "weaponSwing" ? g.right.item : g.left.item;
        if (w && w->swing_sound) { // bare hands have theirs in the clip: SoundPlay.WPNSwingUnarmed
            m_world->play_at(ref, w->swing_sound);
        }
    } else if (tag == "BeginWeaponDraw" || tag == "BeginWeaponSheathe") {
        // WeaponBeginDraw/the game,: the right hand's WEAP only.
        const Item* w = gear(ref).right.item;
        const uint32_t sound = !w ? 0 : tag == "BeginWeaponDraw" ? w->draw_sound : w->sheathe_sound;
        if (sound) {
            m_world->play_at(ref, sound);
        }
    } else if (tag == "arrowAttach") {
        f.attack_state = AttackState::arrow;
    } else if (tag == "BowDrawn") {
        f.attack_state = AttackState::drawn;
    } else if (tag == "arrowDetach") {
        f.attack_state = AttackState::none;
    } else if (tag == "arrowRelease") {
        m_game.projectiles().arrow_release(ref);
    } else if (tag == "AttackWinStart") {
        f.attack_state = AttackState::window;
    } else if (tag == "AttackWinEnd") {
        f.attack_state = AttackState::window_end;
    } else if (tag == "attackStop") {
        f.attack_state = AttackState::none;
        f.attack = nullptr;
    } else if (tag == "staggerStop") {
        f.staggered = false;
    } else if (tag == "recoilStop") {
        f.recoiling = false;
    } else if (tag == "KillActor") {
        if (f.killer) {
            kill(ref, f.killer);
        } else if (f.combat_target) {
            kill(f.combat_target, ref);
        }
    } else if (tag == "KillMoveEnd") {
        end_kill_move(ref);
    } else if (tag == "Decapitate") {
        m_world->decapitate(ref);
    }
}

// ---- damage --------------------------------------------------------------------------------

float Combat::weapon_base(uint32_t ref, const Held& held, float attack_mult) {
    const Item& w = *held.item;
    const bool player = ref == k_player_ref;
    const float base = float(w.damage) * n("fDamageWeaponMult", 1.0f);
    const float temper = w.animation_type == 8 ? 0.0f : m_game.crafting().improvement(w, std::max(held.health, 1.0f));
    float skill_mult = 1;
    if (w.skill >= k_first_skill && w.skill < k_first_skill + k_skill_count) {
        const float low = player ? n("fDamagePCSkillMin", 1.0f) : n("fDamageSkillMin", 1.0f);
        const float high = player ? n("fDamagePCSkillMax", 1.5f) : n("fDamageSkillMax", 3.0f);
        skill_mult = (high - low) * std::clamp(av(ref, w.skill), 0.0f, 100.0f) * 0.01f + low;
    }
    const float added = w.animation_type == 0 ? av(ref, av_unarmed_damage) : w.animation_type <= 6 ? av(ref, av_melee_damage) : 0.0f;
    return (base + temper) * (skill_mult * attack_mult) + added;
}

float Combat::unarmed_base(uint32_t ref, float attack_mult) { return av(ref, av_unarmed_damage) * attack_mult; }

float Combat::bash_base(uint32_t ref, const Gear& g, float attack_mult) {
    const bool player = ref == k_player_ref;
    const float skill = av(ref, av_block) * 0.01f;
    float amount;
    if (g.shield) {
        const float low = player ? n("fShieldBashPCMin", 0.1f) : n("fShieldBashMin", 0.1f);
        const float high = player ? n("fShieldBashPCMax", 0.5f) : n("fShieldBashMax", 0.5f);
        amount = float(g.shield->armor_rating) / 100.0f * (low + (high - low) * skill);
    } else {
        const float low = player ? n("fWeaponBashPCMin", 0.1f) : n("fWeaponBashMin", 0.1f);
        const float high = player ? n("fWeaponBashPCMax", 0.5f) : n("fWeaponBashMax", 0.5f);
        amount = (g.right.item ? float(g.right.item->damage) : 1.0f) * (low + (high - low) * skill);
    }
    return perk(ref, ep_bash_damage, amount) * attack_mult;
}

HitData Combat::populate_melee(uint32_t attacker, uint32_t victim, const Stack* entry, const Item* weapon, bool left) {
    HitData h;
    h.attacker = attacker;
    h.victim = victim;
    h.entry = entry;
    Fighter& a = fighter(attacker);
    const Gear g = gear(attacker);
    h.attack = a.attack;
    const float mult = h.attack ? h.attack->damage_mult : 1.0f;
    const bool bash = a.attack_state == AttackState::bash;
    if (m_predicting) {
        h.flags |= hit_prediction;
    }
    if (bash) {
        h.flags |= hit_bash;
        const AttackState state = fighter(victim).attack_state;
        if (state == AttackState::draw || state == AttackState::swing) {
            h.flags |= hit_timed_bash;
        }
        h.skill = av_block;
        h.total = bash_base(attacker, g, mult);
    } else if (weapon && weapon->form != k_unarmed_weapon) {
        h.weapon = weapon;
        h.skill = weapon->skill;
        const Held held{weapon, nullptr, entry ? std::max(entry->extra.health, 1.0f) : 1.0f};
        h.total = weapon_base(attacker, held, mult);
    } else {
        h.weapon = m_game.items().find(k_unarmed_weapon);
        h.total = unarmed_base(attacker, mult);
    }
    if (left) {
        h.flags |= hit_left;
    }
    CombatBody body;
    if (m_world->body(attacker, body)) {
        h.total *= body.scale;
    }
    h.stagger = stagger_of(h, g, bash);
    const uint32_t weapon_form = h.weapon ? h.weapon->form : 0;
    if (h.attack) {
        h.attack_spell = h.attack->spell;
        if (h.attack->power()) {
            h.flags |= hit_power;
            h.total = perk(attacker, ep_power_damage, h.total, {weapon_form, victim});
        }
    }
    h.total = perk(attacker, ep_weapon_damage, h.total, {weapon_form, victim});
    apply_crit_and_sneak(h);
    apply_armor_and_block(h, false);
    predict_fatal(h);
    return h;
}

bool Combat::in_combat_with(uint32_t ref, uint32_t target) {
    const Fighter& f = fighter(ref);
    return f.in_combat && (f.combat_target == target || std::ranges::find(f.targets, target) != f.targets.end());
}

bool Combat::sneak_attack(const HitData& h) {
    const Fighter& v = fighter(h.victim);
    const Fighter& a = fighter(h.attacker);
    if (h.attacker != k_player_ref || v.dead() || !a.sneaking || a.swimming || a.mounted) {
        return false;
    }
    const bool in_combat = in_combat_with(h.victim, h.attacker);
    const bool ranged = h.weapon && h.weapon->animation_type >= 7 && h.weapon->animation_type <= 9;
    if (v.in_combat && v.searching) {
        if (in_combat && !ranged) {
            return true;
        }
    } else if (in_combat) {
        return false; // its group, at the player, and detecting them or without a group
    }
    return m_world->detection(h.victim, h.attacker) <= 0;
}

float Combat::crit_chance(const HitData& h) {
    const Item* w = h.weapon;
    int steps = 9;
    if (w && w->form != k_unarmed_weapon) {
        const float x = h.condition * 0.1f;
        steps = int(std::ceil(x));
        if (steps > 1) {
            --steps;
        }
    }
    float factor = 1;
    if (w && w->animation_type != 0) {
        factor = float(steps + 1) * n("fWeaponConditionCriticalChanceMult", 0.1f);
    }
    const float weapon_mult = w && w->crit_mult >= 0.0f ? w->crit_mult : 1.0f;
    float chance = av(h.attacker, av_crit_chance) * factor * weapon_mult;
    const uint32_t form = w ? w->form : 0;
    chance = perk(h.attacker, ep_crit_chance, chance, {form, h.victim});
    return perk(h.victim, ep_enemy_crit_chance, chance, {form, h.attacker});
}

void Combat::apply_crit_and_sneak(HitData& h) {
    if (invulnerable(h.victim)) {
        return;
    }
    const uint32_t form = h.weapon ? h.weapon->form : 0;
    h.total = perk(h.attacker, ep_attack_damage, h.total, {form, h.victim});
    h.total = perk(h.victim, ep_incoming_damage, h.total, {h.attacker, form});
    if (!h.has(hit_skip_sneak)) {
        if (sneak_attack(h)) {
            static constexpr std::array<std::string_view, 10> names = {
                "fCombatSneakHandMult", "fCombatSneak1HSwordMult", "fCombatSneak1HDaggerMult", "fCombatSneak1HAxeMult",
                "fCombatSneak1HMaceMult", "fCombatSneak2HSwordMult", "fCombatSneak2HAxeMult", "fCombatSneakBowMult",
                "fCombatSneakStaffMult", "fCombatSneakCrossbowMult"};
            static constexpr std::array<float, 10> defaults = {1, 2, 2, 2, 2, 1, 1, 2, 1, 2};
            const size_t type = h.weapon ? h.weapon->animation_type : 10;
            float mult = type < names.size() ? n(names[type], defaults[type]) : 1.0f;
            mult = perk(h.attacker, ep_sneak_mult, mult, {form, h.victim});
            h.sneak_mult = mult;
            h.total *= mult;
            if (mult > 1.0f) {
                h.flags |= hit_sneak;
            }
        }
        const int chance = int(crit_chance(h) * 10.0f);
        if (chance > int(random() % 1000)) {
            h.flags |= hit_critical;
            if (h.weapon) {
                float extra = h.weapon->form == k_unarmed_weapon ? n("fCombatUnarmedCritDamageMult", 0.5f) * h.total
                                                                 : float(h.weapon->crit_damage);
                extra = perk(h.attacker, ep_crit_damage, extra, {form, h.victim});
                if (extra > 0.0f) {
                    h.total += extra;
                }
                h.critical_effect = h.weapon->crit_effect;
                if (h.weapon->crit_on_death) {
                    h.flags |= hit_critical_on_death;
                }
            }
        }
    }
    if (fighter(h.victim).in_kill_move) {
        h.total *= n("fCombatKillMoveDamageMult", 10.0f);
    }
    h.total *= av(h.attacker, k_attack_damage_mult);
}

float Combat::percent_blocked(const HitData& h, const Gear& g) {
    const float block = av(h.victim, av_block);
    const bool with_weapon = !g.shield && g.right.item && fighter(h.victim).weapon_drawn;
    const bool bare = !g.shield && !g.right.item;
    if (bare && (h.source || !melee_type(h.weapon))) {
        return 0;
    }
    const float scale = g.shield ? float(g.shield->armor_rating) / 100.0f
                                 : h.weapon && !h.has(hit_bash) ? float(h.weapon->damage) : 0.0f;
    const float low = n("fArmorRatingBase", 1.0f), high = n("fArmorRatingMax", 2.5f);
    const float factor = low + (high - low) * block * 0.01f;
    float p;
    if (with_weapon || bare) {
        p = factor * scale * n("fBlockWeaponScaling", 0.05f) * 0.01f + n("fBlockWeaponBase", 0.05f);
    } else {
        p = (factor + av(h.victim, k_shield_perks)) * scale * n("fShieldScalingFactor", 0.25f) * 0.01f +
            n("fShieldBaseFactor", 0.08f);
    }
    return h.has(hit_power) ? p * n("fBlockPowerAttackMult", 0.75f) : p;
}

void Combat::apply_armor_and_block(HitData& h, bool skip_block) {
    Fighter& v = fighter(h.victim);
    if (h.victim == k_player_ref && v.life == LifeState::bleedout) {
        h.total *= n("fCombatPlayerBleedoutHealthDamageMult", 1.0f);
    }
    h.physical = h.total;
    const Gear g = gear(h.victim);
    float rating = av(h.victim, av_damage_resist) * n("fArmorScalingFactor", 0.15f) * 0.01f +
                   float(g.armor.size()) * n("fArmorBaseFactor", 0.02f);
    if (h.attacker && h.weapon) {
        rating = perk(h.attacker, ep_target_resist, rating, {h.weapon->form, h.victim});
    }
    rating = std::min(rating, n("fMaxArmorRating", 95.0f) * 0.01f);
    h.total *= 1.0f - rating;
    if (h.weapon && h.weapon->resist >= 0) {
        const float r = std::min(av(h.victim, h.weapon->resist), 100.0f) * 0.01f;
        if (r > 0.0f && r <= 1.0f) {
            h.total *= 1.0f - r;
        }
    }
    if (!skip_block && blocking(h.victim)) {
        CombatBody self, source;
        const uint32_t from = h.source ? h.source : h.attacker;
        const bool angle = from && m_world->body(h.victim, self) && m_world->body(from, source) &&
                           in_hit_cone(h.victim, self.position, source.position, nullptr, 1.0f, false, false, nullptr);
        if (h.has(hit_touched_shield) || angle) {
            h.flags |= hit_blocked;
            if (!g.shield && g.right.item && v.weapon_drawn) {
                h.flags |= hit_with_weapon;
            }
            float p = perk(h.victim, ep_percent_blocked, percent_blocked(h, g));
            p = std::min(p, n("fPlayerMaxResistance", 85.0f) * 0.01f);
            h.percent_blocked = p;
            if (p > 0.0f) {
                h.total *= 1.0f - p;
            }
        }
    }
    if (melee_type(h.weapon)) {
        const float r = std::min(av(h.victim, k_reflect_damage), 100.0f) * 0.01f;
        if (r > 0.0f) {
            h.reflected = r * h.total;
        }
    }
    if (invulnerable(h.victim)) {
        h.total = h.physical = h.stagger = 0;
    }
}

void Combat::predict_fatal(HitData& h) {
    if (invulnerable(h.victim)) {
        return;
    }
    if (av(h.victim, av_health) - h.total * m_game.damage_mult(difficulty, h.victim == k_player_ref) <= 0.0f) {
        h.flags |= hit_fatal;
    }
}

bool Combat::killable(uint32_t victim, uint32_t attacker) {
    const Fighter& f = fighter(victim);
    if (f.life == LifeState::reanimated) {
        return true;
    }
    // The flags as they stand: SetEssential and SetProtected change them on the base.
    const uint32_t base = m_game.base_of(victim);
    return !m_game.base_flag(base, k_flag_essential) && !m_game.is_child(f.npc) &&
           (!m_game.base_flag(base, k_flag_protected) || attacker == k_player_ref);
}

bool Combat::damage_health(uint32_t victim, float amount, uint32_t attacker) {
    Fighter& v = fighter(victim);
    if (v.invulnerable || v.in_kill_move || v.dead()) {
        return false;
    }
    if (v.life == LifeState::essential_down && killable(victim, attacker)) {
        v.life = LifeState::alive;
    }
    amount *= m_game.damage_mult(difficulty, victim == k_player_ref);
    if (amount <= 0.0f) {
        return false;
    }
    ActorValues& values = m_game.actor_values(victim);
    if (attacker) {
        m_game.combat_ai().hurt(victim);
    }
    values.killable_now = killable(victim, attacker);
    values.mod(Modifier::damage, av_health, -amount);
    values.killable_now = false;
    const float health = values.current(av_health);
    // for an NPC not down yet, the further below the threshold the likelier, by fBleedoutRate against fBleedoutMin.
    if (health > 0.0f && victim != k_player_ref && v.life == LifeState::alive) {
        const float threshold = bleedout_threshold(victim);
        if (health < threshold && (threshold - health) / threshold * n("fBleedoutRate", 15.0f) >= n("fBleedoutMin", 5.0f) &&
            n("fBleedoutCheck", 1.0f) >= float(random()) * 2.3283064e-10f) {
            go_down(victim, LifeState::bleedout);
        }
    }
    if (health > 0.0f) {
        return false;
    }
    kill(victim, attacker);
    return v.dead();
}

void Combat::stop_combat_alarm(uint32_t ref) {
    for (const uint32_t other : fighting()) {
        Fighter& f = fighter(other);
        std::erase(f.targets, ref);
        if (f.combat_target == ref) {
            f.combat_target = 0;
        }
    }
    stop_combat(ref);
}

void Combat::end_deferred_kill(uint32_t ref) {
    Fighter& f = fighter(ref);
    const bool swallowed = f.deferred_kill > 1;
    f.deferred_kill = 0;
    if (swallowed) {
        kill(ref, f.deferred_killer);
    }
}

float Combat::bleedout_threshold(uint32_t ref) {
    ActorValues& v = m_game.actor_values(ref);
    const ActorRecord& r = v.record();
    if (r.bleedout_override >= 0) {
        return float(r.bleedout_override);
    }
    return (r.klass ? r.bleedout_default : n("fBleedoutDefault", 0.2f)) * v.permanent(av_health);
}

// Bleedout.
void Combat::go_down(uint32_t ref, LifeState state) {
    Fighter& f = fighter(ref);
    f.life = state;
    f.down_wait = 0.5f;
    f.attack_state = AttackState::none;
    f.attack = nullptr;
    m_game.ai().hold(ref, true);
    if (!m_world->perform_action(ref, "ActionBleedoutStart")) {
        m_world->graph_event(ref, "BleedOutStart");
    }
    m_game.script_event(ref, "OnEnterBleedout");
}

void Combat::get_up(uint32_t ref) {
    Fighter& f = fighter(ref);
    if (f.life == LifeState::essential_down) {
        ActorValues& v = m_game.actor_values(ref);
        v.restore(av_health, -v.modifier(av_health, Modifier::damage));
    }
    f.life = LifeState::alive;
    m_game.ai().hold(ref, false);
    if (!m_world->perform_action(ref, "ActionBleedoutStop")) {
        m_world->graph_event(ref, "BleedOutStop");
    }
}

void Combat::kill(uint32_t ref, uint32_t killer, bool force) {
    Fighter& f = fighter(ref);
    // In the air a kill waits for the ground: the game holds the same deferral a script's StartDeferredKill does (measured:
    // a dragon at -211 health flew on and was dead 5 to 8 s later).
    if (!f.deferred_kill && !f.dead() && m_game.ai().crash_land(ref)) {
        f.deferred_kill = 1;
    }
    if (f.deferred_kill) { // Actor::Kill: before the dead and essential checks
        f.deferred_kill = uint8_t(std::min(f.deferred_kill + 1, 255));
        f.deferred_killer = killer;
        return;
    }
    if (f.dead()) {
        return;
    }
    if (!force && !killable(ref, killer)) { // Actor::Kill: an essential actor goes down instead (state 7)
        if (f.life != LifeState::essential_down) {
            go_down(ref, LifeState::essential_down);
        }
        return;
    }
    f.life = LifeState::dead;
    f.in_kill_move = false;
    f.attack_state = AttackState::none;
    f.attack = nullptr;
    stop_combat(ref);
    if (killer == k_player_ref) {
        m_game.crime().report_murder(ref, f.npc);
    }
    m_game.count_death(m_game.base_of(ref));
    m_world->died(ref, killer);
}

// ---- stamina and stagger -------------------------------------------------------------------

float Combat::attack_stamina(uint32_t ref, const AttackData& a) {
    if (a.bash()) {
        return (a.power() ? n("fStaminaPowerBashBase", 10.0f) : n("fStaminaBashBase", 5.0f)) * a.stamina_mult;
    }
    if (!a.power()) {
        return 0;
    }
    const Gear g = gear(ref);
    const Item* w = g.right.item;
    const float weight = w ? w->weight : 1.0f;
    float cost = (weight * n("fStaminaAttackWeaponMult", 0.1f) + n("fStaminaAttackWeaponBase", 7.0f)) *
                 n("fPowerAttackStaminaPenalty", 5.0f);
    cost = perk(ref, ep_power_stamina, cost, {w ? w->form : k_unarmed_weapon});
    return cost * a.stamina_mult;
}

float Combat::block_stamina(const HitData& h) const {
    return h.percent_blocked * h.physical * n("fStaminaBlockDmgMult", 0.25f) +
           h.stagger * n("fStaminaBlockStaggerMult", 5.0f) + n("fStaminaBlockBase", 10.0f);
}

float Combat::weapon_stagger(uint32_t attacker, uint32_t victim, const Item* weapon, const AttackData* attack) {
    float raw = attacker ? (weapon ? weapon->stagger : 1.0f) * (attack ? attack->stagger : 0.0f) : 1.0f;
    float mass = av(victim, av_mass);
    if (victim == k_player_ref) {
        mass *= n("fStaggerPlayerMassMult", 2.0f);
    }
    const float d = n("fStaggerMassMult", 1.0f) * mass + n("fStaggerMassBase", 0.0f);
    float s = std::abs(d) <= 0.0001f ? raw : raw / d;
    if (weapon && (weapon->animation_type == 7 || weapon->animation_type == 9) && attacker) {
        s += av(attacker, k_bow_stagger_bonus);
    }
    return s - std::max((mass - n("fStaggerMassOffsetBase", 3.0f)) * n("fStaggerMassOffsetMult", 0.15f), 0.0f);
}

float Combat::bash_stagger(uint32_t attacker, uint32_t victim, const Gear& g, const AttackData* attack) {
    const Fighter& v = fighter(victim);
    const float a = attack ? attack->stagger : 0.0f;
    float k = 1;
    if (g.shield) {
        k = n("fStaggerShieldMult", 1.0f);
    } else if (const Item* w = g.right.item) {
        const uint8_t type = w->animation_type;
        k = type == 5 || type == 6 || type == 7 || type == 9 ? n("fStagger2WeapMult", 0.5f) : n("fStagger1WeapMult", 0.2f);
    }
    float c = 1;
    if (v.recoiling) {
        c *= n("fStaggerRecoilingMult", 1.5f);
    }
    if (v.running) {
        c *= n("fStaggerRunningMult", 1.5f);
    }
    if (blocking(victim)) {
        c *= n("fStaggerBlockingMult", 1.0f);
    }
    const float s = (av(attacker, av_block) * n("fStaggerAttackMult", 0.01f) + n("fStaggerAttackBase", 0.5f)) /
                    (av(victim, av_mass) * n("fStaggerMassMult", 1.0f) + n("fStaggerMassBase", 0.0f)) * c * k * a;
    return std::max(s, n("fStaggerMin", 0.05f));
}

float Combat::stagger_of(HitData& h, const Gear& attacker, bool bash) {
    float s = bash ? bash_stagger(h.attacker, h.victim, attacker, h.attack)
                   : weapon_stagger(h.attacker, h.victim, h.weapon, h.attack);
    if (s > 0.0f && blocking(h.victim)) {
        const Gear g = gear(h.victim);
        float rating = 0;
        if (g.shield) {
            rating = float(g.shield->armor_rating) / 100.0f;
        } else if (g.right.item && fighter(h.victim).weapon_drawn) {
            switch (g.right.item->animation_type) {
            case 2: rating = n("fStaggerDaggerAR", 5.0f); break;
            case 1: case 3: case 4: rating = n("fStagger1WeapAR", 10.0f); break;
            case 5: case 6: rating = n("fStagger2WeapAR", 15.0f); break;
            case 7: case 9: rating = n("fStaggerBowAR", 5.0f); break;
            default: break;
            }
        }
        const float mass = av(h.victim, av_mass);
        const float r = n("fStaggerBlockAttackShieldMult", -0.005f) * rating + n("fStaggerBlockAttackShieldBase", 1.0f) +
                        n("fStaggerBlockMult", 0.005f) * av(h.victim, av_block) + n("fStaggerBlockBase", 0.5f) +
                        std::max((mass - n("fStaggerMassOffsetBase", 3.0f)) * n("fStaggerMassOffsetMult", 0.15f), 0.0f);
        s = std::max(s - r, n("fStaggerMin", 0.05f));
    }
    return s;
}

void Combat::start_stagger(uint32_t victim, float magnitude, uint32_t attacker) {
    Fighter& v = fighter(victim);
    CombatBody self;
    if (!m_world->body(victim, self) || !self.loaded || v.staggered || v.life == LifeState::restrained ||
        v.life == LifeState::unconscious || v.dead() || (v.knock_state != 0 && v.knock_state != 6)) {
        return;
    }
    if (attacker) {
        magnitude = perk(attacker, ep_target_stagger, magnitude, {victim});
    }
    magnitude = std::min(perk(victim, ep_incoming_stagger, magnitude, {attacker}), 1.0f);
    if (magnitude <= 0.0f) {
        return;
    }
    float direction = 0;
    CombatBody other;
    if (attacker && m_world->body(attacker, other)) {
        float turn = (self.heading - heading_to(self.position, other.position)) * std::numbers::pi_v<float> / 180.0f;
        turn = std::fmod(turn, k_two_pi);
        direction = (turn < 0.0f ? turn + k_two_pi : turn) / k_two_pi;
    }
    m_world->graph_float(victim, "staggerMagnitude", magnitude);
    m_world->graph_float(victim, "staggerDirection", direction);
    if (m_world->perform_action(victim, "ActionStaggerStart")) {
        v.staggered = true;
        v.stagger_timer = n("fStaggerMaxDuration", 3.0f);
    }
}

void Combat::knockback(const HitData& h) {
    const Fighter& v = fighter(h.victim);
    const forms::FormEntry* race = m_game.db().find(v.race);
    // Not a projectile's. A trap has no attacker: the shove goes from the contact to the feet.
    if (!race || (h.source && h.attacker) || (!h.source && !h.attacker)) {
        return;
    }
    const Form f(m_game.db(), *race);
    const Value* flags = f.get("DATA.flags");
    if (flags && (flags->integer & 0x400)) {
        return;
    }
    CombatBody self, other;
    if (!m_world->body(h.victim, self)) {
        return;
    }
    if (h.attacker) {
        if (!m_world->body(h.attacker, other)) {
            return;
        }
    } else {
        other.position = h.position;
    }
    const float force = h.pushback > 0.0f ? h.pushback : (std::trunc(h.physical) * n("fKnockbackDamageMult", 10.0f) + n("fKnockbackDamageBase", 50.0f)) *
                        (n("fKnockbackAgilMult", -0.008f) * 50.0f + n("fKnockbackAgilBase", 1.0f));
    const Units3 d = {self.position[0] - other.position[0], self.position[1] - other.position[1],
                      self.position[2] - other.position[2]};
    m_world->push(h.victim, d, std::min(force, n("fKnockbackForceMax", 512.0f)), n("fKnockbackTime", 1.0f));
}

// ---- finding the target --------------------------------------------------------------------

float Combat::melee_reach(uint32_t ref) {
    const Fighter& f = fighter(ref);
    float reach;
    if (f.attack_state == AttackState::bash) {
        reach = n("fCombatBashReach", 128.0f);
    } else if (const Item* w = gear(ref).right.item) {
        reach = w->reach * n("fCombatDistance", 128.0f);
    } else {
        const forms::FormEntry* e = m_game.db().find(f.race);
        const Value* v = e ? forms::Form(m_game.db(), *e).get("DATA.unarmed_reach") : nullptr;
        reach = v ? float(v->real) : 0.0f;
    }
    CombatBody body;
    return m_world->body(ref, body) ? reach * body.full_scale : reach;
}

float Combat::target_distance(uint32_t a, uint32_t b, bool slope_rule) {
    CombatBody x, y;
    if (!m_world->body(a, x) || !m_world->body(b, y) || x.space != y.space) {
        return std::numeric_limits<float>::infinity();
    }
    const float dx = x.position[0] - y.position[0], dy = x.position[1] - y.position[1];
    const float dz = x.position[2] - y.position[2];
    const float flat = std::sqrt(dx * dx + dy * dy);
    float distance = std::sqrt(flat * flat + dz * dz);
    const bool swim = fighter(a).swimming && fighter(b).swimming;
    if (swim || (slope_rule && std::abs(dz) >= n("fAICombatSlopeDifference", 128.0f))) {
        const float x_top = x.position[2] + x.height, y_top = y.position[2] + y.height;
        if ((x.position[2] <= y_top && x.position[2] >= y.position[2]) || (x_top <= y_top && x_top >= y.position[2]) ||
            (y.position[2] <= x_top && y.position[2] >= x.position[2]) || (y_top <= x_top && y_top >= x.position[2])) {
            distance = flat;
        }
    }
    return distance - x.radius - y.radius;
}

bool Combat::in_hit_cone(uint32_t actor, const Units3& from, const Units3& to, const AttackData* attack, float mult,
                         bool rotating, bool dead_target, float* out) {
    CombatBody body;
    if (!m_world->body(actor, body)) {
        return false;
    }
    float offset = 0;
    if (attack && !(rotating && (attack->flags & attack_rotating))) {
        offset = attack->attack_angle;
    }
    const float angle = fold_angle(body.heading + offset - heading_to(from, to));
    if (out) {
        *out = angle;
    }
    const float limit = (attack ? attack->strike_angle : n("fCombatHitConeAngle", 35.0f)) * mult;
    return angle <= limit || (dead_target && angle <= limit * n("fCombatDeadActorHitConeMult", 2.0f));
}

uint32_t Combat::find_target(uint32_t ref, float reach, const AttackData* attack) {
    Fighter& a = fighter(ref);
    if (a.mounted) {
        reach = n("fMountedAttackRange", reach);
    }
    CombatBody self;
    if (!m_world->body(ref, self)) {
        return 0;
    }
    const auto passes = [&](uint32_t other, bool slope, float* angle) {
        CombatBody body;
        if (!m_world->body(other, body) || !body.loaded || target_distance(ref, other, slope) > reach) {
            return false;
        }
        return in_hit_cone(ref, self.position, body.position, attack, 1.0f, false, fighter(other).dead(), angle);
    };
    if (a.combat_target) {
        return passes(a.combat_target, true, nullptr) ? a.combat_target : 0;
    }
    uint32_t best = 0;
    float best_angle = std::numeric_limits<float>::infinity();
    for (uint32_t other : m_world->high_actors()) {
        float angle = 0;
        if (other == ref || (fighter(other).dead() && ref != k_player_ref) || !passes(other, false, &angle)) {
            continue;
        }
        if (angle <= best_angle) {
            best = other;
            best_angle = angle;
        }
    }
    if (ref != k_player_ref) {
        float angle = 0;
        if (passes(k_player_ref, false, &angle) && angle <= best_angle) {
            best = k_player_ref;
        }
    }
    if (ref == k_player_ref && best && !m_world->line_of_sight(ref, best)) {
        return 0;
    }
    return best;
}

void Combat::weapon_hit_frame(uint32_t ref, bool left) {
    Fighter& a = fighter(ref);
    const AttackData* attack = a.attack;
    const bool use_left = left || (attack && attack->left());
    const Gear g = gear(ref);
    const Item* weapon = use_left ? g.left.item : g.right.item;
    if (attack && !a.invulnerable) {
        if (attack->power() && ref == k_player_ref) {
            a.power_cooldown = n("fPowerAttackCoolDownTime", 2.0f);
        }
        if (const float cost = attack_stamina(ref, *attack); cost > 0.0f) {
            ActorValues& values = m_game.actor_values(ref);
            values.mod(Modifier::damage, av_stamina, -cost);
            if (values.current(av_stamina) <= 0.0f && ref == k_player_ref) {
                m_world->blink_meter(av_stamina);
            }
        }
    }
    // the weapon's sound level; the race's unarmed level isn't read, so fists count as Normal.
    m_world->action_sound(ref, weapon ? weapon->sound_level : 1);
    const float reach = melee_reach(ref);
    uint32_t target = find_target(ref, reach, attack);
    if (!target) {
        const uint8_t type = weapon ? weapon->animation_type : 0;
        const float length = a.attack_state == AttackState::bash ? n("iActivatePickLength", 180.0f)
                           : !weapon || type == 0                ? n("fObjectHitH2HReach", 64.0f)
                           : type == 5 || type == 6              ? n("fObjectHitTwoHandReach", 112.0f)
                                                                 : n("fObjectHitWeaponReach", 150.0f);
        const uint32_t picked = m_world->swing_pick(ref, length, weapon, a.attack_state == AttackState::bash);
        if (picked && (ref == k_player_ref || picked == a.combat_target)) {
            target = picked;
        }
    }
    if (perk(ref, ep_sweep, 0.0f, {weapon ? weapon->form : k_unarmed_weapon}) != 0.0f) {
        CombatBody self;
        if (m_world->body(ref, self)) {
            const float mult = n("fMeleeSweepViewAngleMult", 2.0f);
            for (uint32_t other : m_world->high_actors()) {
                CombatBody body;
                if (other != ref && other != target && m_world->body(other, body) &&
                    target_distance(ref, other, false) <= reach &&
                    in_hit_cone(ref, self.position, body.position, attack, mult, true, fighter(other).dead(), nullptr)) {
                    hit_target(ref, other, 0, false);
                }
            }
        }
    }
    if (target) {
        hit_target(ref, target, 0, use_left);
        m_game.magic_target(ref).dispel_archetype(archetype_invisibility);
        m_game.magic_target(ref).dispel_archetype(archetype_etherealize);
    } else if (attack && attack->spell) {
        m_game.casters().instant_cast(ref, attack->spell, 0);
    }
}

// ---- resolving a hit -----------------------------------------------------------------------

float Combat::weapon_speed(uint32_t ref, bool left) {
    const Gear g = gear(ref);
    const Item* weapon = left ? g.left.item : g.right.item;
    if (!weapon) {
        return 1.0f;
    }
    float mult = m_game.actor_values(ref).current(left ? 132 : 85);
    mult = mult <= 0.0f || std::abs(mult) < 0.001f ? 1.0f : mult;
    if (weapon->animation_type == 5 || weapon->animation_type == 6) {
        mult *= m_game.settings().number("fWeaponTwoHandedAnimationSpeedMult", 1.5f);
    }
    return mult * weapon->speed;
}

void Combat::apply_spell(uint32_t spell, uint32_t victim) {
    // what stays is added, anything else is cast by the victim's own instant caster.
    const Spell* s = m_game.magic().spell(spell);
    if (s && s->permanent()) {
        m_game.magic_target(victim).add_spell(spell);
    } else {
        m_game.casters().instant_cast(victim, spell, victim);
    }
}

void Combat::weapon_effects(uint32_t attacker, const Held& held, bool left, uint32_t victim) {
    if (!held.item) {
        return;
    }
    const uint32_t hit_spell = perk_spell(attacker, ep_combat_hit_spell, {held.item->form, victim});
    if (hit_spell) {
        apply_spell(hit_spell, victim);
    }
    uint32_t ench = held.stack && held.stack->extra.enchantment ? held.stack->extra.enchantment : held.item->enchantment;
    ench = ench ? ench : fighter(attacker).enhance; // leaves an enchanted weapon as it is
    if (ench) {
        m_game.casters().weapon_enchantment(attacker, held, ench, left, victim);
    }
    if (held.stack && held.stack->extra.poison && held.stack->extra.poison_doses > 0) {
        Stack& s = *held.stack;
        m_game.casters().instant_cast(attacker, s.extra.poison, victim, held.item->form);
        if (--s.extra.poison_doses <= 0) {
            s.extra.poison = 0;
        }
    }
}

void Combat::player_attack_xp(const HitData& h, const Gear& g) {
    if (fighter(h.victim).dead() || m_game.is_child(fighter(h.victim).npc) || h.total <= 0.0f) {
        return;
    }
    if (h.has(hit_bash)) {
        const float amount = g.shield ? h.total * n("fShieldBashSkillUseMult", 0.0f) + n("fShieldBashSkillUseBase", 5.0f)
                                      : h.total * n("fWeaponBashSkillUseMult", 0.0f) + n("fWeaponBashSkillUseBase", 1.0f);
        const Item* item = g.shield ? g.shield : g.right.item;
        m_game.use_skill(av_block, amount, item ? item->form : 0, 2);
    } else if (h.weapon && h.weapon->form != k_unarmed_weapon && h.skill >= 0) {
        m_game.use_skill(h.skill, float(h.weapon->damage), h.weapon->form, h.has(hit_power) ? 1 : 0);
    }
}

void Combat::player_victim_xp(const HitData& h) {
    const Gear g = gear(k_player_ref);
    if (h.has(hit_blocked)) {
        const float amount = (h.physical - h.total) * n("fWeaponBlockSkillUseMult", 1.0f) + n("fWeaponBlockSkillUseBase", 0.0f);
        const Item* item = g.shield ? g.shield : g.right.item;
        m_game.use_skill(av_block, amount, item ? item->form : 0, 0);
        return;
    }
    uint32_t shield_bit = k_shield_slot, body_bit = 0x4;
    if (const forms::FormEntry* e = m_game.db().find(fighter(k_player_ref).race)) {
        const Form race(m_game.db(), *e);
        if (const Value* v = race.get("DATA.shield_biped_object"); v && v->integer >= 0) {
            shield_bit = 1u << v->integer;
        }
        if (const Value* v = race.get("DATA.body_biped_object"); v && v->integer >= 0) {
            body_bit = 1u << v->integer;
        }
    }
    int light = 0, heavy = 0;
    for (const Item* item : g.armor) {
        if (item->armor_rating <= 0) {
            continue;
        }
        for (uint32_t bit = 1; bit != 0; bit <<= 1) {
            if ((item->biped_slots & bit) && bit != shield_bit) {
                (item->armor_type == 0 ? light : heavy) += bit == body_bit ? 2 : 1;
            }
        }
    }
    const int r = int(random() % 6);
    if (r < light) {
        m_game.use_skill(av_light_armor, h.physical, 0, 0);
    } else if (r < light + heavy) {
        m_game.use_skill(av_heavy_armor, h.physical, 0, 0);
    }
}

void Combat::resolve(HitData& h, const Gear& attacker_gear) {
    const uint32_t attacker = h.attacker, victim = h.victim;
    const Fighter& a = fighter(attacker);
    if (attacker == k_player_ref) {
        player_attack_xp(h, attacker_gear);
    }
    if (victim == k_player_ref) {
        player_victim_xp(h);
    }
    const bool suppress = attacker != k_player_ref && a.in_combat && !in_combat_with(attacker, victim);
    if (suppress) {
        return;
    }
    apply_hit(victim, h);
    if (h.reflected > 0.0f) {
        damage_health(attacker, h.reflected, victim);
    }
    if (h.has(hit_bash)) {
        if (const uint32_t spell = perk_spell(attacker, ep_bash_spell, {victim})) {
            apply_spell(spell, victim);
        }
    }
}

void Combat::hit_target(uint32_t attacker, uint32_t victim, uint32_t projectile, bool left) {
    const Gear g = gear(attacker);
    const Held* held = left ? &g.left : &g.right;
    if (left && !g.left.item && g.right.item && g.right.item->animation_type >= 5 && g.right.item->animation_type != 8) {
        held = &g.right;
        left = false;
    }
    const bool bash = fighter(attacker).attack_state == AttackState::bash;
    if (!bash) {
        weapon_effects(attacker, *held, left, victim);
    }
    HitData h = populate_melee(attacker, victim, held->stack, bash ? nullptr : held->item, left);
    h.source = projectile;
    resolve(h, g);
}

void Combat::projectile_hit(uint32_t attacker, uint32_t victim, uint32_t projectile, const Item* weapon, float damage,
                            bool touched_shield, const Units3& at, const Units3& direction) {
    HitData h;
    h.position = at;
    h.direction = direction;
    h.attacker = attacker;
    h.victim = victim;
    h.source = projectile;
    h.weapon = weapon;
    h.skill = weapon ? weapon->skill : -1;
    h.condition = 100;
    h.total = damage;
    if (touched_shield) {
        h.flags |= hit_touched_shield;
    }
    CombatBody body;
    if (attacker && m_world->body(attacker, body)) {
        h.total *= body.scale;
    }
    const uint32_t form = weapon ? weapon->form : 0;
    h.total = perk(attacker, ep_weapon_damage, h.total, {form, victim});
    h.stagger = weapon_stagger(attacker, victim, weapon, nullptr);
    apply_crit_and_sneak(h);
    apply_armor_and_block(h, !touched_shield);
    predict_fatal(h);
    resolve(h, gear(attacker));
}

void Combat::explosion_hit(uint32_t attacker, uint32_t victim, float damage) {
    HitData h;
    h.attacker = attacker;
    h.victim = victim;
    h.total = perk(attacker, ep_weapon_damage, damage, {0, victim});
    apply_armor_and_block(h, true);
    apply_crit_and_sneak(h);
    apply_hit(victim, h);
}

// Actor::ProcessTrapHit and its HitData: no attacker, so armor, blocking and stagger are what's left
// of a melee hit. The trap is the hit's source.
void Combat::trap_hit(uint32_t victim, uint32_t trap, float damage, float pushback, const Units3& at, const Units3& direction) {
    HitData h;
    h.position = at;
    h.direction = direction;
    h.victim = victim;
    h.source = trap;
    h.total = damage;
    h.physical = damage;
    h.pushback = pushback;
    apply_armor_and_block(h, false);
    predict_fatal(h);
    h.stagger = weapon_stagger(0, victim, nullptr, nullptr);
    apply_hit(victim, h);
}

bool Combat::predict_kill(uint32_t attacker, uint32_t victim) {
    if (fighter(victim).dead() || !killable(victim, attacker)) {
        return false;
    }
    Fighter& a = fighter(attacker);
    const AttackData* current = a.attack;
    a.attack = nullptr;
    m_predicting = true;
    const Gear g = gear(attacker);
    const HitData h = populate_melee(attacker, victim, g.right.stack, g.right.item, false);
    m_predicting = false;
    a.attack = current;
    return h.has(hit_fatal);
}

bool Combat::should_attack_kill(uint32_t attacker, uint32_t victim) {
    if (fighter(victim).dead() || !killable(victim, attacker)) {
        return false;
    }
    Fighter& a = fighter(attacker);
    const AttackData* best = nullptr;
    for (const uint32_t form : {a.npc, a.race}) {
        for (const AttackData& attack : attacks_of(form)) {
            if (!best || attack.damage_mult > best->damage_mult) {
                best = &attack;
            }
        }
    }
    const AttackData* current = a.attack;
    a.attack = best;
    m_predicting = true;
    const Gear g = gear(attacker);
    const HitData h = populate_melee(attacker, victim, g.right.stack, g.right.item, false);
    m_predicting = false;
    a.attack = current;
    return h.has(hit_fatal);
}

// The game (2.8), its position: 32 units from the victim towards the attacker, at the body's middle.
void Combat::pick_hit_location(HitData& h) {
    CombatBody v, a;
    if (h.direction != Units3{} || !m_world->body(h.victim, v)) {
        return;
    }
    Units3 from = v.position;
    if (h.attacker && m_world->body(h.attacker, a)) {
        from = a.position;
    }
    const float dx = v.position[0] - from[0], dy = v.position[1] - from[1];
    const float length = std::max(std::hypot(dx, dy), 1e-3f);
    h.direction = {dx / length, dy / length, 0.0f};
    h.position = {v.position[0] - h.direction[0] * 32.0f, v.position[1] - h.direction[1] * 32.0f, v.position[2] + v.height * 0.5f};
}

void Combat::apply_hit(uint32_t victim, HitData& h) {
    Fighter& v = fighter(victim);
    const bool kill_move = v.in_kill_move;
    if (v.knock_state == 5) {
        return;
    }
    m_world->hit_event(h);
    const uint32_t attacker = h.attacker;
    const uint32_t form = h.weapon ? h.weapon->form : 0;
    const bool living = !v.dead();
    if (attacker == k_player_ref && living && victim != k_player_ref) {
        const std::string name(m_game.display_name_of(v.npc));
        if (h.has(hit_critical)) {
            m_game.notify("Critical Strike on " + name, "UISneakAttack");
        }
        if (h.has(hit_sneak)) {
            const bool ranged = h.weapon && (h.weapon->animation_type == 7 || h.weapon->animation_type == 9);
            m_game.use_skill(av_sneak, ranged ? n("fSneakAttackSkillUsageRanged", 2.5f) : n("fSneakAttackSkillUsageMelee", 20.0f),
                             form, 0);
            m_game.notify(std::format("Sneak attack for {:.1f}X damage!", h.sneak_mult), "UISneakAttack");
        }
    }
    bool comment = false;
    if (h.total > 0.0f) {
        const float health_before = av(victim, av_health);
        const bool attacker_in_kill_move = attacker && fighter(attacker).in_kill_move;
        const bool killed = damage_health(victim, h.total, attacker) || attacker_in_kill_move;
        if (living && h.has(hit_critical) && h.critical_effect && (killed || !h.has(hit_critical_on_death)) &&
            n("bDisableAllGore", 0.0f) == 0.0f) {
            m_game.casters().instant_cast(victim, h.critical_effect, victim);
        }
        if (living && killed && attacker == k_player_ref && h.weapon) {
            const Gear g = gear(attacker);
            const Held& held = h.has(hit_left) ? g.left : g.right;
            const float percent = perk(attacker, ep_soul_to_weapon, 0.0f, {victim, form});
            if (percent > 0.0f && held.stack && held.stack->extra.charge >= 0.0f) {
                const float max = held.stack->extra.max_charge ? float(held.stack->extra.max_charge)
                                                               : float(held.item->enchantment_amount);
                const float soul = m_game.soul_value_of(v.npc);
                held.stack->extra.charge = std::min(held.stack->extra.charge + soul * percent * 0.01f, max);
            }
        }
        if (!killed) {
            comment = float(random() % 10000) / 10000.0f < n("fCombatSpeakHitChance", 0.35f) ||
                      (health_before > 0.0f && h.total / health_before > n("fCombatSpeakHitThreshold", 0.1f));
            if (v.knock_state == 0) {
                if (h.has(hit_timed_bash)) {
                    v.recoiling = m_world->perform_action(victim, "ActionRecoilLarge");
                } else if (h.stagger > 0.0f) {
                    const bool always = victim == k_player_ref && n("bPlayerAlwaysStaggered", 0.0f) != 0.0f;
                    if (always || h.has(hit_blocked | hit_bash) || !melee_type(h.weapon) ||
                        int(random() % 100) < int(n("iStaggerAttackChance", 100.0f))) {
                        start_stagger(victim, h.stagger, attacker);
                    }
                }
            }
        }
    }
    if (victim == k_player_ref && attacker && attacker != k_player_ref) {
        m_game.impacts().player_hurt();
    }
    CombatBody body;
    if (m_world->body(victim, body) && body.loaded) {
        if (h.percent_blocked <= 0.0f) {
            if (!kill_move) {
                knockback(h);
            }
            if (!h.has(hit_bash) && (h.total > 0.0f || (h.weapon && h.weapon->form != k_unarmed_weapon))) {
                pick_hit_location(h);
                m_game.impacts().blood(h);
            }
        } else {
            m_world->perform_action(victim, "ActionBlockHit");
            m_world->comment(victim, k_comment_block, 0);
            if (!h.source && attacker && !h.has(hit_bash) && !v.dead()) {
                fighter(attacker).recoiling = m_world->perform_action(attacker, "ActionRecoil");
            }
            const float cost = block_stamina(h);
            if (av(victim, av_stamina) > 0.0f && cost > 0.0f) {
                m_game.actor_values(victim).mod(Modifier::damage, av_stamina, -cost);
            }
        }
    }
    if (h.attack_spell && attacker) {
        m_game.magic_target(victim).combat_spell_apply(h.attack_spell, attacker);
    }
    if (!kill_move && attacker && h.attack && h.attack->knockdown > 0.0f) {
        CombatBody other;
        if (m_world->body(attacker, other)) {
            m_world->knockdown(victim, other.position, h.attack->knockdown);
        }
    }
    if (attacker) {
        on_attacked(victim, attacker);
    }
    if (victim == k_player_ref && attacker != k_player_ref) {
        m_world->player_hit(); // blocked or not
    }
    if (comment) {
        m_world->comment(victim, k_comment_hit, attacker);
    }
}

void Combat::forgive_or_report(uint32_t victim, uint32_t attacker) {
    Fighter& v = fighter(victim);
    const Gear g = gear(attacker);
    if (attacker == k_player_ref) {
        const int relation = m_game.crime().faction_reaction(v.npc, k_player_base);
        const bool follower = m_world->follows_player(victim);
        if (relation == 2 || relation == 3 || follower) {
            const bool friendly = relation == 3 || follower;
            int allowed = v.in_combat ? int(n(friendly ? "iFriendHitCombatAllowed" : "iAllyHitCombatAllowed", friendly ? 3.0f : 1000.0f))
                                      : int(n(friendly ? "iFriendHitNonCombatAllowed" : "iAllyHitNonCombatAllowed", friendly ? 0.0f : 3.0f));
            if (m_game.ignores_friendly_hits(v.npc)) {
                allowed = 1000;
            }
            if (allowed > 0) {
                if (allowed >= 1000) {
                    v.friend_hits.clear();
                }
                if (v.last_friend_hit >= n("fFriendMinimumLastHitTime", 0.5f)) {
                    v.friend_hits.push_back(0.0f);
                }
                v.last_friend_hit = 0;
                if (int(v.friend_hits.size()) <= allowed) {
                    m_world->comment(victim, k_comment_hit, attacker);
                    return;
                }
            }
        }
        m_game.crime().report_attack(victim, v.npc);
    }
    m_world->stand_up(victim);
    if (m_game.combat_ai().should_fight(victim, attacker)) { // 5.3 step 5
        start_combat(victim, attacker);
    }
    (void)g;
}

// IsLastHostileActor: those near enough who fight the player, the subject the only one or none at all. The flying
// reach, fHostileFlyingActorExteriorDistance, and a commanded actor counting as its commander aren't here.
bool Combat::last_hostile(uint32_t subject) {
    const auto player = m_game.ref_position(k_player_ref);
    if (!player) {
        return false;
    }
    const float reach = n(m_world->in_interior() ? "fHostileActorInteriorDistance" : "fHostileActorExteriorDistance", 3000.0f);
    int others = 0;
    for (const uint32_t ref : fighting()) {
        const Fighter& f = fighter(ref);
        const auto at = m_game.ref_position(ref);
        if (ref == k_player_ref || f.dead() || !at || m_game.ref_disabled(ref) || !in_combat_with(ref, k_player_ref)) {
            continue;
        }
        const float x = (*at)[0] - (*player)[0], y = (*at)[1] - (*player)[1], z = (*at)[2] - (*player)[2];
        if (x * x + y * y + z * z <= reach * reach && ref != subject) {
            ++others;
        }
    }
    return others == 0;
}

void Combat::on_attacked(uint32_t victim, uint32_t attacker) {
    Fighter& v = fighter(victim);
    if (v.dead() || victim == attacker || !attacker) {
        return;
    }
    const Item* w = gear(attacker).right.item;
    if (w && (w->weapon_flags & k_weapon_non_hostile)) {
        return;
    }
    m_game.magic_target(victim).dispel_archetype(archetype_calm);
    if (in_combat_with(victim, attacker)) {
        return;
    }
    forgive_or_report(victim, attacker);
}

void Combat::start_combat(uint32_t ref, uint32_t target) {
    Fighter& f = fighter(ref);
    if (f.dead() || ref == target) {
        return;
    }
    if (std::ranges::find(f.targets, target) == f.targets.end()) {
        f.targets.push_back(target);
    }
    const bool started = !f.in_combat;
    f.in_combat = true;
    if (!f.combat_target) {
        f.combat_target = target;
    }
    if (started && on_combat_start) {
        on_combat_start(ref, target);
    }
    if (started) {
        m_game.script_event(ref, "OnCombatStateChanged", {{target, "Actor"}}, 1);
    }
}

void Combat::stop_combat(uint32_t ref) {
    Fighter& f = fighter(ref);
    const bool was = f.in_combat;
    f.in_combat = f.searching = false;
    f.combat_target = 0;
    f.targets.clear();
    if (was) {
        m_game.script_event(ref, "OnCombatStateChanged", {{0, "Actor"}}, 0);
    }
}

void Combat::begin_kill_move(uint32_t ref, uint32_t killer) {
    Fighter& f = fighter(ref);
    f.in_kill_move = true;
    f.killer = killer;
    f.kill_timer = n("fKillMoveMaxDuration", 10.0f);
}

void Combat::end_kill_move(uint32_t ref) {
    Fighter& f = fighter(ref);
    const uint32_t killer = f.killer;
    f.in_kill_move = false;
    f.killer = 0;
    if (killer) {
        kill(ref, killer);
    }
}

} // namespace spud::game
