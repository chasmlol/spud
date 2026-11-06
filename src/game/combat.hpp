#pragma once

// Hits: how a swing finds a target, what it does to health, stamina and both actors.
// Works in game units and degrees, as the formulas do; what it needs of bodies and graphs comes through CombatWorld.

#include "core/game_random.hpp"
#include "game/inventory.hpp"
#include "game/items.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::game {

struct Spell;

class Game;

using Units3 = std::array<float, 3>; // game units

inline constexpr uint32_t k_unarmed_weapon = 0x000001F4; // the default WEAP a bare hand attacks with

enum AttackFlag : uint32_t { attack_bash = 2, attack_power = 4, attack_left = 8, attack_rotating = 0x10 };

// One ATKD of a race or NPC_ with its ATKE.
struct AttackData {
    std::string event;
    float damage_mult = 1, attack_chance = 0;
    uint32_t spell = 0;
    uint32_t flags = 0;
    float attack_angle = 0, strike_angle = 0, stagger = 0;
    float knockdown = 0, recovery_time = 0, stamina_mult = 1;
    bool bash() const { return flags & attack_bash; }
    bool power() const { return flags & attack_power; }
    bool left() const { return flags & attack_left; }
};

enum HitFlag : uint32_t {
    hit_blocked = 0x1,
    hit_with_weapon = 0x2,
    hit_touched_shield = 0x4,
    hit_critical = 0x8,
    hit_critical_on_death = 0x10,
    hit_fatal = 0x20,
    hit_sneak = 0x800,
    hit_skip_sneak = 0x1000, // no sneak or critical
    hit_prediction = 0x2000,
    hit_bash = 0x4000,
    hit_timed_bash = 0x8000,
    hit_power = 0x10000,
    hit_left = 0x20000,
};

struct HitData {
    Units3 position{}, direction{};
    uint32_t attacker = 0, victim = 0;
    uint32_t source = 0; // the projectile's reference
    const AttackData* attack = nullptr;
    const Item* weapon = nullptr; // null for a bash
    const Stack* entry = nullptr; // the weapon's stack
    uint32_t critical_effect = 0, attack_spell = 0;
    float total = 0;              // what's left of the damage
    float physical = 0;           // total before armor
    float percent_blocked = 0;    // 0..1
    float stagger = 0;
    float pushback = 0;           // a trap's afPushback; 0 goes by the damage
    float condition = 1;          // 100 from a projectile; picks the critical factor
    float sneak_mult = 1;
    float reflected = 0;
    uint32_t flags = 0;
    int32_t skill = -1;
    bool has(uint32_t flag) const { return (flags & flag) != 0; }
};

// A bow runs bow, arrow attached, drawn, released, fired.
enum class AttackState : uint8_t {
    none = 0, draw = 1, swing = 2, hit = 3, window = 4, window_end = 5, bash = 6, bow = 8, arrow = 9, drawn = 10, released = 11, fired = 12,
};
enum class LifeState : uint8_t { alive = 0, unconscious = 3, reanimated = 4, restrained = 6, essential_down = 7, bleedout = 8, dead = 9 };

// One actor as combat sees it: the state the game keeps on Actor and its process.
struct Fighter {
    uint32_t ref = 0, npc = 0, race = 0;
    AttackState attack_state = AttackState::none;
    LifeState life = LifeState::alive;
    uint8_t knock_state = 0;
    bool sneaking = false, swimming = false, mounted = false, running = false;
    float move_speed = 0; // units/s, the player's: GetMovementSpeed
    std::array<float, 3> velocity{}; // units/s, the player's controller: GetVelocity
    bool weapon_drawn = false;
    uint32_t enhance = 0; // Enhance Weapon: the ENCH its hits carry while it lasts
    float down_wait = 0;  // down: until health starts to come back
    bool want_block = false;
    bool recoiling = false, staggered = false;
    bool in_kill_move = false;
    bool attack_on_sight = false;      // Actor bit 0x8000
    uint8_t critical_stage = 0;        // Actor
    uint8_t deferred_kill = 0;         // 0 off, 1 started, 2 and up: kills swallowed
    uint32_t deferred_killer = 0;
    bool invulnerable = false;
    uint32_t killer = 0; // the other side of a kill move, on its victim
    bool in_combat = false, searching = false;
    uint32_t combat_target = 0;
    std::vector<uint32_t> targets; // the combat group's
    const AttackData* attack = nullptr;
    float power_cooldown = 0, kill_timer = 0, stagger_timer = 0;
    std::vector<float> friend_hits; // ages in seconds
    float last_friend_hit = 1e9f;
    bool dead() const { return life == LifeState::dead; }
};

// What the hand holds.
struct Held {
    const Item* item = nullptr;
    Stack* stack = nullptr; // the player's
    float health = 1;
};

// What an actor has equipped.
struct Gear {
    Held right, left;
    const Item* shield = nullptr;
    std::vector<const Item*> armor; // each form once, shield included, clothing left out
};

// A body in the world.
struct CombatBody {
    Units3 position{};
    float heading = 0;           // degrees, clockwise from north
    float radius = 0, height = 0;
    float scale = 1;             // the reference's own
    float full_scale = 1;        // with the NPC's height
    bool loaded = true;          // has 3D
    uint32_t space = 0;
};

// The world side of a hit. The defaults are a world with nobody in it.
class CombatWorld {
public:
    virtual ~CombatWorld() = default;
    virtual bool body(uint32_t, CombatBody&) { return false; }
    virtual bool in_interior() { return false; }
    virtual std::vector<uint32_t> high_actors() { return {}; } // the player isn't in it
    virtual bool line_of_sight(uint32_t, uint32_t) { return true; }
    virtual bool graph_bool(uint32_t, std::string_view) { return false; }
    virtual void graph_float(uint32_t, std::string_view, float) {}
    virtual void graph_int(uint32_t, std::string_view, int) {}
    virtual bool graph_value(uint32_t, std::string_view, int32_t&) { return false; }
    virtual bool graph_real(uint32_t, std::string_view, float&) { return false; }
    virtual bool want_cast(uint32_t, std::string_view, bool) { return false; } // false: the graph has no such variable
    // An action's event by the idle tree, sent once the graph's state takes it.
    virtual bool queue_action(uint32_t, std::string_view) { return false; }
    virtual void clear_actions(uint32_t) {}
    virtual bool graph_event(uint32_t, std::string_view) { return false; }
    // What an NPC can fight with, best first; putting one in the hand; whether a weapon is out of its sheath.
    virtual std::vector<uint32_t> weapons(uint32_t) { return {}; }
    virtual void swap_weapon(uint32_t, uint32_t) {}
    virtual void decapitate(uint32_t) {}
    virtual bool limb_gone(uint32_t, uint32_t) { return false; }
    virtual bool weapon_in_hand(uint32_t) { return false; }
    // Through the idle tree: the action's event for this actor, sent to its graph.
    virtual bool perform_action(uint32_t, std::string_view) { return false; }
    // The action's idle if it comes to a kill move on the combat target, and only then.
    virtual bool kill_move(uint32_t, std::string_view) { return false; }
    // What a ray from the eye over `length` units finds: an actor's reference, or 0, having dealt with anything else. The
    // weapon and whether it's a bash are for the impact on anything else.
    virtual uint32_t swing_pick(uint32_t, float, const Item* = nullptr, bool = false) { return 0; }
    virtual void gear(uint32_t, std::vector<uint32_t>&, uint32_t&, uint32_t&) {}
    virtual int detection(uint32_t, uint32_t) { return -1000; } // above 0: detected
    virtual void push(uint32_t, const Units3&, float, float) {}
    virtual void knockdown(uint32_t, const Units3&, float) {}
    virtual void action_sound(uint32_t, int) {}
    virtual void play_at(uint32_t, uint32_t) {} // a reference, an SNDR: the sound at it
    // and the direction of that movement off the facing, radians clockwise.
    virtual bool moving(uint32_t, float&) { return false; }
    virtual void hit_event(const HitData&) {}                           // OnHit
    virtual void died(uint32_t, uint32_t) {}
    virtual void blink_meter(int) {}
    virtual void comment(uint32_t, int, uint32_t) {}     // topic type 3 combat lines
    virtual void player_hit() {}                         // the player struck: IMAD GetHit
    virtual bool follows_player(uint32_t) { return false; }
    virtual void stand_up(uint32_t) {}                                  // a sitting or sleeping victim
};

class Combat {
public:
    explicit Combat(Game& game);
    ~Combat();

    void set_world(CombatWorld* world) { m_world = world ? world : &m_nobody; }
    CombatWorld& world() { return *m_world; }
    Fighter& fighter(uint32_t ref);
    Gear gear(uint32_t ref);
    float weapon_speed(uint32_t ref, bool left); // the graph's weaponSpeedMult,
    float bleedout_threshold(uint32_t ref);      // health below which it can go down, the game
    int difficulty = 2; // iDifficulty: 0 Novice .. 5 Legendary
    std::function<uint32_t()> random = [this] { return m_random(); };

    // Each frame: cooldowns, kill move and stagger timers, friendly hits forgotten.
    void update(float dt);
    // The combat functions of conditions, for any actor.
    bool spell_has_keyword(const Spell& spell, uint32_t keyword);
    std::array<int, 2> replaced_item{-1, -1}; // left, right: what a hand held before this equip, as the graph's type
    bool condition_value(uint16_t function, uint32_t subject, uint32_t target, uint32_t param1, uint32_t param2, double& out);
    std::vector<uint32_t> fighting() const;
    // The attacks an actor can make: its own ATKD list, then the race's.
    std::vector<AttackData> attacks(uint32_t ref);

    // A graph raised `tag` on this actor (HitFrame, weaponSwing, KillActor and the rest).
    void animation_event(uint32_t ref, std::string_view tag, std::string_view payload);

    // The attack data whose ATKE is `event`, the NPC_'s list first, null for none.
    const AttackData* attack_data(uint32_t ref, std::string_view event);
    // The actor means to attack with this event: refused when out of stamina or cooling down (6.4).
    bool attack_allowed(uint32_t ref, const AttackData* attack);
    // The event starts: attack state from its name, kill moves too.
    void attack_started(uint32_t ref, std::string_view event, uint32_t target = 0);
    // It never played: no one dies of it.
    void cancel_kill_move(uint32_t ref) {
        Fighter& f = fighter(ref);
        f.in_kill_move = false;
        f.killer = 0;
    }

    void weapon_hit_frame(uint32_t ref, bool left);
    void hit_target(uint32_t attacker, uint32_t victim, uint32_t projectile, bool left);
    // What projectiles and explosions hand over, ready for armor; `damage` is what the launch set.
    void trap_hit(uint32_t victim, uint32_t trap, float damage, float pushback, const Units3& at, const Units3& direction);
    void projectile_hit(uint32_t attacker, uint32_t victim, uint32_t projectile, const Item* weapon, float damage,
                        bool touched_shield, const Units3& at, const Units3& direction);
    void explosion_hit(uint32_t attacker, uint32_t victim, float damage);
    // Health damage with the difficulty multiplier; true when the actor is now dead.
    bool damage_health(uint32_t victim, float amount, uint32_t attacker);
    bool killable(uint32_t victim, uint32_t attacker);
    bool should_attack_kill(uint32_t attacker, uint32_t victim);
    // a blow of the weapon in hand, no attack data, as a shot would land.
    bool predict_kill(uint32_t attacker, uint32_t victim);
    void kill(uint32_t ref, uint32_t killer, bool force = false); // force: KillEssential
    void go_down(uint32_t ref, LifeState state);                  // essential down or bleedout
    void get_up(uint32_t ref);
    // A victim stops short of a fight, or an actor takes one up; both ends of the game.
    void start_combat(uint32_t ref, uint32_t target);
    // A stagger from outside a hit, a magic effect's: the magnitude goes straight to the game.
    void stagger(uint32_t victim, float magnitude, uint32_t attacker) { start_stagger(victim, magnitude, attacker); }
    void stop_combat(uint32_t ref);
    // StopCombatAlarm: out of every group's targets too.
    void stop_combat_alarm(uint32_t ref);
    void end_deferred_kill(uint32_t ref);
    bool in_combat_with(uint32_t ref, uint32_t target);
    bool last_hostile(uint32_t subject);
    std::function<void(uint32_t ref, uint32_t target)> on_combat_start;

    // The formulas, for the card and checks.
    float weapon_base(uint32_t ref, const Held& held, float attack_mult);
    float unarmed_base(uint32_t ref, float attack_mult);
    float bash_base(uint32_t ref, const Gear& gear, float attack_mult);
    float crit_chance(const HitData& hit);
    float percent_blocked(const HitData& hit, const Gear& gear);
    float attack_stamina(uint32_t ref, const AttackData& attack);
    float block_stamina(const HitData& hit) const;
    float weapon_stagger(uint32_t attacker, uint32_t victim, const Item* weapon, const AttackData* attack);
    float bash_stagger(uint32_t attacker, uint32_t victim, const Gear& gear, const AttackData* attack);
    float melee_reach(uint32_t ref);
    float target_distance(uint32_t a, uint32_t b, bool slope_rule);
    bool in_hit_cone(uint32_t actor, const Units3& from, const Units3& to, const AttackData* attack, float mult,
                     bool rotating, bool dead_target, float* angle);

    HitData populate_melee(uint32_t attacker, uint32_t victim, const Stack* entry, const Item* weapon, bool left);
    void apply_crit_and_sneak(HitData& hit);
    void apply_armor_and_block(HitData& hit, bool skip_block);
    void apply_hit(uint32_t victim, HitData& hit);
    void on_attacked(uint32_t victim, uint32_t attacker);

private:
    float n(std::string_view name, float fallback) const;
    float perk(uint32_t owner, int point, float value, std::initializer_list<uint32_t> args = {}) const;
    uint32_t perk_spell(uint32_t owner, int point, std::initializer_list<uint32_t> args = {}) const;
    float av(uint32_t ref, int actor_value);
    bool blocking(uint32_t ref);
    bool invulnerable(uint32_t ref);
    float armor_sum(uint32_t ref);
    float stagger_of(HitData& hit, const Gear& attacker, bool bash);
    bool sneak_attack(const HitData& hit);
    void predict_fatal(HitData& hit);
    uint32_t find_target(uint32_t ref, float reach, const AttackData* attack);
public:
    // who a swing starting now would go for.
    uint32_t melee_target(uint32_t ref) { return find_target(ref, melee_reach(ref), nullptr); }
private:
    void weapon_effects(uint32_t attacker, const Held& held, bool left, uint32_t victim);
    void apply_spell(uint32_t spell, uint32_t victim);
    void player_attack_xp(const HitData& hit, const Gear& gear);
    void resolve(HitData& hit, const Gear& attacker);
    void pick_hit_location(HitData& hit);
    void player_victim_xp(const HitData& hit);
    void start_stagger(uint32_t victim, float magnitude, uint32_t attacker);
    void knockback(const HitData& hit);
    void begin_kill_move(uint32_t ref, uint32_t killer);
    void end_kill_move(uint32_t ref);
    void forgive_or_report(uint32_t victim, uint32_t attacker);
    const std::vector<AttackData>& attacks_of(uint32_t form);

    Game& m_game;
    CombatWorld m_nobody;
    CombatWorld* m_world = &m_nobody;
    std::unordered_map<uint32_t, Fighter> m_fighters;
    std::unordered_map<uint32_t, std::vector<AttackData>> m_attacks;
    std::mt19937 m_random{session_seed()};
    bool m_predicting = false;
};

} // namespace spud::game
