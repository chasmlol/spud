#include "game/combat_ai.hpp"

#include "game/game.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace spud::game {

namespace {

float lerp(float x, float a, float b) { return a + (b - a) * x; }

// The fConfidence* table by the Confidence value 0..4, read between its steps.
float confidence_needed(const Settings& s, float confidence) {
    const std::array<float, 5> levels = {s.number("fConfidenceCowardly", 1000.0f), s.number("fConfidenceCautious", 2.0f),
                                         s.number("fConfidenceAverage", 1.0f), s.number("fConfidenceBrave", 0.5f),
                                         s.number("fConfidenceFoolhardy", 0.0f)};
    const float c = std::clamp(confidence, 0.0f, 4.0f);
    const auto low = size_t(c);
    return low >= 4 ? levels[4] : lerp(c - float(low), levels[low], levels[low + 1]);
}

} // namespace

float CombatAi::n(std::string_view name, float fallback) const { return m_game.settings().number(name, fallback); }

float CombatAi::random_unit() { return float(m_game.combat().random() % 100000) / 100000.0f; }

float CombatAi::style(uint32_t ref, std::string_view member, float fallback) const {
    const forms::Database& db = m_game.db();
    const forms::FormEntry* npc = db.find(m_game.base_of(ref));
    if (!npc) {
        return fallback;
    }
    const forms::Form f(db, *npc);
    const forms::Value* id = f.get("ZNAM");
    const forms::FormEntry* e = id ? db.find(id->form) : nullptr;
    if (!e) {
        return fallback;
    }
    const forms::Form s(db, *e);
    const forms::Value* v = s.get(member);
    return v ? float(v->real) : fallback;
}

namespace {

// An NPC's factions, through templates that give them (ACBS flag 0x4, Use Factions).
std::vector<uint32_t> factions_of(Game& game, uint32_t npc) {
    const forms::Database& db = game.db();
    std::vector<uint32_t> out;
    for (int depth = 0; depth < 16; ++depth) {
        const forms::FormEntry* e = db.find(npc);
        if (e && e->type == forms::sig("LVLN")) { // a leveled template: what it gives
            std::vector<Rolled> rolled;
            game.leveled().roll(npc, 1, 1, rolled);
            npc = rolled.empty() ? 0 : rolled.front().form;
            continue;
        }
        if (!e || e->type != forms::sig("NPC_")) {
            break;
        }
        const forms::Form f(db, *e);
        const forms::Value* acbs = f.get("ACBS");
        const forms::Value* flags = acbs ? acbs->path("template_flags") : nullptr;
        const forms::Value* parent = f.get("TPLT");
        if (parent && parent->form && flags && (flags->integer & 0x4)) {
            npc = parent->form;
            continue;
        }
        for (const forms::FactionMembership& m : forms::npc_factions(f)) {
            if (m.rank >= 0) {
                out.push_back(m.faction);
            }
        }
        break;
    }
    return out;
}

} // namespace

int CombatAi::reaction(uint32_t actor, uint32_t npc, uint32_t target, uint32_t target_npc) {
    Crime& crime = m_game.crime();
    const auto factions = [this](uint32_t form) -> const std::vector<uint32_t>& {
        const auto it = m_factions.find(form);
        return it != m_factions.end() ? it->second : m_factions.emplace(form, factions_of(m_game, form)).first->second;
    };
    // What scripts and quest aliases gave or took, over the record's.
    const auto changed = [this](uint32_t ref, const std::vector<uint32_t>& base, std::vector<uint32_t>& out) -> const std::vector<uint32_t>& {
        const RefChanges* c = m_game.find_changes(ref);
        if (!c || (c->factions.empty() && !c->left_all_factions)) {
            return base;
        }
        if (!c->left_all_factions) {
            std::ranges::copy_if(base, std::back_inserter(out), [c](uint32_t f) { return !c->factions.contains(f); });
        }
        for (const auto& [faction, rank] : c->factions) {
            if (rank >= 0) {
                out.push_back(faction);
            }
        }
        return out;
    };
    std::vector<uint32_t> own_now, theirs_now;
    const std::vector<uint32_t>&own = changed(actor, factions(npc), own_now), &theirs = changed(target, factions(target_npc), theirs_now);
    // The player, to anyone but a guard: a crime or enemy flag on any faction makes it enemy.
    if (target == k_player_ref && !crime.is_guard(npc)) {
        if (const uint32_t cf = crime.crime_faction_of(npc); cf && crime.is_enemy(cf)) {
            return 1;
        }
        if (std::ranges::any_of(own, [&](uint32_t f) { return crime.is_enemy(f); })) {
            return 1;
        }
    }
    const int rank = target == k_player_ref ? crime.relationship(npc, target_npc) : 0;
    int result = rank >= 2 ? 2 : rank > 0 ? 3 : rank < -2 ? 1 : 0;
    // Each pair of factions refines it, stopping at ally.
    for (const uint32_t a : own) {
        for (const uint32_t b : theirs) {
            if (result == 2) {
                return result;
            }
            const int r = crime.faction_reaction(a, b);
            if (result == 0 || (result == 1 && r != 0) || (result == 3 && r == 2)) {
                result = r;
            }
        }
    }
    return result;
}

bool CombatAi::should_attack(uint32_t actor, uint32_t npc, uint32_t target, uint32_t target_npc, float distance) {
    Combat& combat = m_game.combat();
    const Fighter& self = combat.fighter(actor);
    const Fighter& other = combat.fighter(target);
    if (self.dead() || other.dead() || actor == target || std::ranges::find(self.targets, target) != self.targets.end()) {
        return false;
    }
    // A summon or raised corpse: what fights its commander, or what the commander fights.
    if (const uint32_t lead = m_game.magic_target(actor).commander) {
        return target != lead && (other.combat_target == lead || combat.fighter(lead).combat_target == target);
    }
    if (const uint32_t lead = m_game.magic_target(target).commander; lead == actor) {
        return false;
    }
    const float aggression = m_game.actor_values(actor).current(av_aggression);
    // A teammate speaks for its leader: only what fights them.
    if (combat.world().follows_player(actor)) {
        return target != k_player_ref && (other.combat_target == k_player_ref || other.combat_target == actor);
    }
    if (target != k_player_ref && combat.world().follows_player(target) && other.in_combat == false) {
        return should_attack(actor, npc, k_player_ref, 7, distance);
    }
    if (aggression >= 3.0f) {
        return true;
    }
    if (aggression < 1.0f && !other.attack_on_sight && other.combat_target != actor) {
        return false; // most of a town, and no reaction needs working out
    }
    const int r = reaction(actor, npc, target, target_npc);
    if (other.combat_target == actor && target != k_player_ref) {
        return r != 2 && r != 3;
    }
    // AIDT's aggro radius: those with one attack only within it.
    const forms::FormEntry* e = m_game.db().find(npc);
    if (e) {
        const forms::Form f(m_game.db(), *e);
        const forms::Value* ai = f.get("AIDT");
        const forms::Value* behaviour = ai ? ai->path("aggro_radius_behavior") : nullptr;
        const forms::Value* attack = ai ? ai->path("attack") : nullptr;
        if (behaviour && behaviour->integer && attack && distance > float(attack->integer)) {
            return false;
        }
    }
    if (aggression >= 2.0f) {
        return r == 1 || r == 0;
    }
    if (aggression >= 1.0f) {
        return r == 1;
    }
    return r == 1 && other.attack_on_sight;
}

// the forced the game: a blow from an ally or friend doesn't start a fight, unless one of them is
// frenzied. Not here: the special combat factions (the arena's) and Actor bit 26, whose meaning wasn't read.
bool CombatAi::should_fight(uint32_t victim, uint32_t attacker) {
    if (victim == attacker) {
        return false;
    }
    if (victim == k_player_ref) {
        return true;
    }
    if (m_game.actor_values(victim).current(av_aggression) >= 3.0f || m_game.actor_values(attacker).current(av_aggression) >= 3.0f) {
        return true;
    }
    const int r = reaction(victim, m_game.base_of(victim), attacker, m_game.base_of(attacker));
    return r != 2 && r != 3;
}

void CombatAi::begin(Fighter& f, Brain& b) {
    b.started = true;
    // The hands are filled before the draw, so it draws what they hold.
    update_magic(f, b, 0.0f, m_game.combat().target_distance(f.ref, f.combat_target, true), false);
    const float confidence = m_game.actor_values(f.ref).current(av_confidence);
    if (confidence >= 1.0f && confidence <= 3.0f) {
        b.confidence_offset = lerp(random_unit(), n("fCombatConfidenceModifierMin", -0.5f), n("fCombatConfidenceModifierMax", 0.5f));
    }
    m_game.ai().stand_up(f.ref);
    f.weapon_drawn = true; // update() draws it, once they're on their feet
    m_game.combat().world().comment(f.ref, 0, f.combat_target); // the Attack line
}

void CombatAi::select_target(Fighter& f, Brain& b, float dt) {
    Combat& combat = m_game.combat();
    // The dead are no target, and neither is anyone down: *inferred*, as the game plays; the rule itself isn't read.
    const auto gone = [&combat](uint32_t t) {
        const Fighter& o = combat.fighter(t);
        return o.dead() || o.life == LifeState::essential_down || o.life == LifeState::bleedout;
    };
    std::erase_if(f.targets, gone);
    if (f.combat_target && gone(f.combat_target)) {
        f.combat_target = 0;
        b.select_timer = 0;
    }
    b.select_timer -= dt;
    if (b.select_timer > 0.0f && f.combat_target) {
        return;
    }
    // CombatTargetSelectorStandard: the current until the timer ends, then the best score (4.2).
    uint32_t best = 0;
    float best_score = -1e9f;
    for (const uint32_t t : f.targets) {
        const float distance = combat.target_distance(f.ref, t, false);
        float score = (1.0f - std::min(distance, 2048.0f) / 2048.0f) * 1000.0f;
        if (t == f.combat_target) {
            score += 100.0f;
        }
        if (score > best_score) {
            best = t;
            best_score = score;
        }
    }
    const bool switched = best != f.combat_target;
    f.combat_target = best;
    b.select_timer = switched ? n("fCombatSelectTargetSwitchUpdateTime", 3.0f) : n("fCombatSelectTargetUpdateTime", 1.0f);
}

bool CombatAi::should_flee(const Fighter& f, Brain& b, float dt) {
    // 4.8: only an actor hurt lately weighs itself against its enemy.
    ActorValues& values = m_game.actor_values(f.ref);
    b.strength_timer -= dt;
    if (b.strength_timer <= 0.0f) {
        b.strength_timer = n("fCombatThreatRatioUpdateTime", 5.0f);
        b.threat_ratio = 100;
        if (b.since_hurt < 120.0f && values.current(av_aggression) != 3.0f) {
            float enemy = 0;
            for (const uint32_t t : f.targets) {
                enemy += m_game.actor_values(t).current(av_health);
            }
            b.threat_ratio = enemy > 0.0f ? values.current(av_health) / enemy : 100.0f;
        }
    }
    if (m_game.is_child(f.npc) || f.life == LifeState::essential_down) {
        return true;
    }
    return confidence_needed(m_game.settings(), values.current(av_confidence) + b.confidence_offset) > b.threat_ratio;
}

void CombatAi::block(Fighter& f, Brain& b, float dt, float distance) {
    CombatWorld& w = m_game.combat().world();
    const float def = style(f.ref, "CSGD.defensive_mult", 0.5f);
    if (b.blocking) {
        b.block_left -= dt;
        if (b.block_left <= 0.0f || distance > lerp(def, n("fCombatBlockStopDistanceMin", 128.0f), n("fCombatBlockStopDistanceMax", 384.0f))) {
            b.blocking = false;
            f.want_block = false;
            w.graph_event(f.ref, "blockStop");
        }
        return;
    }
    const Fighter& target = m_game.combat().fighter(f.combat_target);
    const bool swinging = target.attack_state == AttackState::draw || target.attack_state == AttackState::swing;
    const float reach = lerp(def, n("fCombatBlockStartDistanceMin", 32.0f), n("fCombatBlockStartDistanceMax", 192.0f));
    if (!swinging || distance > reach || f.attack_state != AttackState::none) {
        return;
    }
    float p = lerp(def, n("fCombatBlockChanceMin", 0.0f), n("fCombatBlockChanceMax", 1.0f));
    if (!m_game.combat().gear(f.ref).shield) {
        p *= n("fCombatBlockChanceWeaponMult", 0.25f);
    }
    if (chance(p * dt) && w.graph_event(f.ref, "blockStart")) {
        b.blocking = true;
        f.want_block = true;
        const float t = def * 2.0f - 1.0f;
        const float mid = n("fCombatBlockHoldTimeMid", 8.0f);
        b.block_left = t >= 0.0f ? lerp(t, mid, n("fCombatBlockHoldTimeMax", 10000.0f)) : lerp(-t, mid, n("fCombatBlockHoldTimeMin", 4.0f));
    }
}

void CombatAi::attack(Fighter& f, Brain& b, float dt, float distance) {
    Combat& combat = m_game.combat();
    for (auto& [attack, wait] : b.waits) {
        wait -= dt;
    }
    b.since_attack += dt;
    b.attack_idle -= dt;
    if (b.attack_idle > 0.0f || f.attack_state != AttackState::none || b.blocking || f.staggered || f.recoiling) {
        return;
    }
    b.attack_idle = 0.25f; // Attack Idle
    const float reach = combat.melee_reach(f.ref);
    if (distance > reach + n("fCombatAttackCheckTargetRangeDistance", 512.0f)) {
        return;
    }
    // Attack Selector: lerp(Off) and the target's state (4.5).
    const float off = style(f.ref, "CSGD.offensive_mult", 0.5f);
    float p = lerp(off, n("fCombatAttackChanceMin", 0.05f), n("fCombatAttackChanceMax", 1.0f));
    const Fighter& target = combat.fighter(f.combat_target);
    if (target.staggered || target.recoiling) {
        p *= std::max(style(f.ref, "CSME.attack_staggered_mult", 1.0f), style(f.ref, "CSME.power_attack_staggered_mult", 1.0f));
    }
    const float bonus_time = n("fCombatAttackChanceLastAttackBonusTime", 5.0f);
    p *= lerp(std::min(b.since_attack / bonus_time, 1.0f), 1.0f, n("fCombatAttackChanceLastAttackBonus", 1.5f));
    if (!chance(p) || distance > reach) {
        return;
    }
    // CombatBehaviorAttack: weighted by each ATKD's chance, that weight rolled again, then sent by its own event.
    std::vector<const AttackData*> candidates;
    float total = 0;
    const float stamina = m_game.actor_values(f.ref).current(av_stamina);
    for (const AttackData& listed : combat.attacks(f.ref)) {
        const AttackData* found = combat.attack_data(f.ref, listed.event);
        if (!found) {
            continue;
        }
        const AttackData& a = *found;
        const auto wait = b.waits.find(&a);
        if (a.attack_chance <= 0.0f || a.bash() || (wait != b.waits.end() && wait->second > 0.0f) ||
            (stamina <= 0.0f && combat.attack_stamina(f.ref, a) > 0.0f)) {
            continue;
        }
        candidates.push_back(&a);
        total += a.attack_chance;
    }
    if (candidates.empty()) {
        return;
    }
    float roll = random_unit() * total;
    const AttackData* pick = candidates.back();
    for (const AttackData* a : candidates) {
        roll -= a->attack_chance;
        if (roll <= 0.0f) {
            pick = a;
            break;
        }
    }
    if (!chance(pick->attack_chance)) {
        return;
    }
    CombatBody self, other;
    if (!combat.world().body(f.ref, self) || !combat.world().body(f.combat_target, other) ||
        !combat.in_hit_cone(f.ref, self.position, other.position, pick, n("fCombatAttackStrikeAngleMult", 0.67f), false, false, nullptr)) {
        return;
    }
    // The idle tree goes first, as for every action: this blow may be a finisher.
    if (combat.world().kill_move(f.ref, pick->power() ? "ActionRightPowerAttack" : "ActionRightAttack")) {
        b.since_attack = 0;
        return;
    }
    if (combat.world().graph_event(f.ref, pick->event)) {
        combat.attack_started(f.ref, pick->event);
        b.waits[pick] = pick->recovery_time;
        b.since_attack = 0;
    }
}

void CombatAi::shoot(Fighter& f, Brain& b, float dt, float distance) {
    // Ranged attack (4.7): lerp(Off) with the last attack bonus, the draw held for its time.
    CombatWorld& w = m_game.combat().world();
    b.since_attack += dt;
    if (b.draw_left >= 0.0f) {
        b.draw_left -= dt;
        if (b.draw_left < 0.0f) {
            w.graph_event(f.ref, "attackRelease");
            m_game.combat().attack_started(f.ref, "attackRelease");
        }
        return;
    }
    b.attack_idle -= dt;
    if (b.attack_idle > 0.0f || f.attack_state != AttackState::none) {
        return;
    }
    b.attack_idle = 0.25f;
    const float off = style(f.ref, "CSGD.offensive_mult", 0.5f);
    float p = lerp(off, n("fCombatRangedAttackChanceMin", 0.05f), n("fCombatRangedAttackChanceMax", 1.0f));
    const float bonus_time = n("fCombatRangedAttackChanceLastAttackBonusTime", 5.0f);
    p *= lerp(std::min(b.since_attack / bonus_time, 1.0f), 1.0f, n("fCombatRangedAttackChanceLastAttackBonus", 1.5f));
    const Item* bow = m_game.combat().gear(f.ref).right.item;
    const char* event = bow && bow->animation_type == 9 ? "crossbowAttackStart" : "bowAttackStart";
    if (chance(p) && w.graph_event(f.ref, event)) {
        m_game.combat().attack_started(f.ref, event);
        const bool near = distance < n("fCombatRangedAttackHoldTimeMinDistance", 256.0f);
        b.draw_left = near ? n("fCombatRangedAttackHoldTimeMax", 2.0f)
                           : lerp(random_unit(), n("fCombatRangedAttackHoldTimeMin", 1.0f), n("fCombatRangedAttackHoldTimeMax", 2.0f));
        b.since_attack = 0;
    }
}

// The score of combat-actors 3.3: the bow-or-sword choice. The ranges are the game's (combat-magic 1.3).
float CombatAi::weapon_score(const Fighter& f, const Item& w, bool equipped, float distance) {
    Combat& combat = m_game.combat();
    const bool ranged = w.animation_type == 7 || w.animation_type == 9;
    float score = combat.weapon_base(f.ref, Held{&w, nullptr, 1.0f}, 1.0f) * w.speed;
    score *= ranged ? n("fCombatDPSBowSpeedMult", 0.2f) * style(f.ref, "CSGD.ranged_score_mult", 1.0f)
                    : n("fCombatDPSMeleeSpeedMult", 1.0f) * style(f.ref, "CSGD.melee_score_mult", 1.0f);
    const float max_range = ranged ? n("fCombatMaximumProjectileRange", 4096.0f)
                                   : n("fCombatInventoryMeleeEquipRange", 256.0f) + combat.melee_reach(f.ref);
    const float min_range = ranged ? n("fCombatRangedMinimumRange", 256.0f) : 0.0f;
    if (max_range + (equipped ? n("fCombatInventoryMaxRangeEquippedBonus", 384.0f) : 0.0f) < distance) {
        score *= n("fCombatInventoryMaxRangeScoreMult", 0.1f) * max_range / distance;
    } else if (min_range > 0.0f && min_range + (equipped ? 0.0f : n("fCombatInventoryMinRangeUnequippedBonus", 256.0f)) > distance) {
        score *= n("fCombatInventoryMinRangeScoreMult", 0.1f);
    }
    return score + (equipped ? n("fCombatInventoryEquippedScoreBonus", 0.1f) : 0.0f);
}

bool CombatAi::update_weapon(Fighter& f, Brain& b, float dt, float distance) {
    CombatWorld& world = m_game.combat().world();
    b.weapon_held += dt;
    if (b.swap_to) {
        b.swap_left -= dt;
        if (world.weapon_in_hand(f.ref) && b.swap_left > 0.0f) {
            return true; // the sheathing has still to come
        }
        world.swap_weapon(f.ref, b.swap_to);
        world.perform_action(f.ref, "ActionDraw");
        b.swap_to = 0;
        b.weapon_held = 0;
        return true;
    }
    b.weapon_check -= dt;
    if (b.weapon_check > 0.0f || f.attack_state != AttackState::none || b.draw_left >= 0.0f) {
        return false;
    }
    b.weapon_check = 1.0f; // CombatInventory::Update, every second
    if (b.weapon_held < n("fCombatInventoryMinEquipTimeWeapon", 5.0f) || !world.weapon_in_hand(f.ref)) {
        return false;
    }
    const Item* held = m_game.combat().gear(f.ref).right.item;
    float best = held ? weapon_score(f, *held, true, distance) : -1.0f;
    uint32_t pick = 0;
    for (const uint32_t form : world.weapons(f.ref)) {
        const Item* w = m_game.items().find(form);
        if (w && w != held && w->animation_type != 8) { // a staff comes through the magic lists
            if (const float score = weapon_score(f, *w, false, distance); score > best) {
                best = score;
                pick = form;
            }
        }
    }
    if (!pick) {
        return false;
    }
    b.swap_to = pick;
    b.swap_left = 2.0f; // no sheathing to wait for on a graph that doesn't play one
    if (!world.perform_action(f.ref, "ActionSheath")) {
        b.swap_left = 0.0f;
    }
    return true;
}

void CombatAi::update(float dt) {
    Combat& combat = m_game.combat();
    Ai& ai = m_game.ai();
    for (const uint32_t ref : combat.fighting()) {
        Fighter& f = combat.fighter(ref);
        if (ref == k_player_ref || f.dead() || f.life == LifeState::essential_down || f.life == LifeState::bleedout) {
            continue;
        }
        if (f.in_kill_move) {
            continue; // the clip has it; the weapon stays out and the target stands
        }
        Brain& b = brain(ref);
        if (!b.started) {
            begin(f, b);
        }
        b.since_hurt += dt;
        select_target(f, b, dt);
        if (!f.combat_target) {
            ai.stand_down(ref);
            combat.stop_combat(ref);
            forget(ref);
            continue;
        }
        // Lost: nobody sees the target for fCombatDetectionLostRemoveTime (4.9).
        const bool seen = combat.world().detection(ref, f.combat_target) > 0 || f.combat_target != k_player_ref;
        b.unseen = seen ? 0.0f : b.unseen + dt;
        if (const bool searching = b.unseen > 0.0f; searching != f.searching) {
            f.searching = searching;
            m_game.script_event(ref, "OnCombatStateChanged", {{f.combat_target, "Actor"}}, searching ? 2 : 1);
        }
        if (b.unseen > n("fCombatDetectionLostRemoveTime", 30.0f)) {
            std::erase(f.targets, f.combat_target);
            f.combat_target = 0;
            continue;
        }
        const float distance = combat.target_distance(ref, f.combat_target, true);
        update_potion(f, b, dt, distance);
        if (should_flee(f, b, dt)) {
            if (!b.fleeing) {
                b.fleeing = true;
                combat.world().comment(ref, 30, f.combat_target);
                b.flee_tries = 0;
            }
            // Until a way out is found: another search can hold the request up, and straight away may not be on a navmesh.
            const Agent* agent = ai.agent(ref);
            if (agent && agent->path.empty() && !agent->searching && (b.flee_retry -= dt) <= 0.0f) {
                b.flee_retry = 0.5f;
                ai.stand_up(ref);
                ai.flee(ref, f.combat_target, b.flee_tries++);
            }
            continue;
        }
        if (b.fleeing) {
            b.fleeing = false;
        }
        // Drawing takes while anything else plays: on from a chair, or after an idle.
        const Agent* agent = ai.agent(ref);
        b.draw_retry -= dt;
        // An event the agent has yet to send comes first: IdleForceDefaultState after the draw would undo it, and the hand would
        // still read as full.
        if (b.draw_retry <= 0.0f && !b.swap_to && agent && agent->use == UseState::using_it) {
            b.draw_retry = 1.0f; // sat down as the fight began: the walk to the chair ended after begin() stood them up
            ai.stand_up(ref);
        } else if (b.draw_retry <= 0.0f && !b.swap_to && (!agent || agent->use == UseState::none)) {
            if (agent && !agent->event.empty()) {
                // Still unsent: the graph won't have it (a chair's exit in the chair-eating state). IdleForceDefaultState takes its
                // place, and that one every idle answers.
                b.draw_retry = 1.0f;
                ai.stand_up(ref);
            } else if (!combat.world().weapon_in_hand(ref)) {
                b.draw_retry = 1.0f;
                if (!combat.world().perform_action(ref, "ActionDraw")) {
                    ai.stand_up(ref); // an idle holds them, one that missed its way out
                }
            }
        }
        update_magic(f, b, dt, distance, seen);
        if (const float stop = magic_stop_distance(f, b); stop >= 0.0f) {
            ai.close_on(ref, f.combat_target, stop);
        }
        if (b.hands[1].item) {
            if (magic_stop_distance(f, b) < 0.0f) {
                ai.close_on(ref, f.combat_target, combat.melee_reach(ref) * 0.8f);
            }
            continue;
        }
        if (update_weapon(f, b, dt, distance)) {
            continue;
        }
        const Item* weapon = combat.gear(ref).right.item;
        if (weapon && (weapon->animation_type == 7 || weapon->animation_type == 9)) {
            ai.close_on(ref, f.combat_target, n("fCombatRangedDistance", 1024.0f));
            shoot(f, b, dt, distance);
            continue;
        }
        ai.close_on(ref, f.combat_target, combat.melee_reach(ref) * 0.8f);
        block(f, b, dt, distance);
        attack(f, b, dt, distance);
    }
}

} // namespace spud::game
