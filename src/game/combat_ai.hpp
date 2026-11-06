#pragma once

// NPCs in a fight: target, whether to stand or flee, moving in, attacking, blocking.

#include "game/combat.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace spud::game {

class Game;
struct MagicEffect;
struct Spell;
struct SpellEffect;

// CombatMagicCaster's kinds.
enum class CasterKind : uint8_t {
    none, offensive, restore, ward, summon, stagger, disarm, cloak, light, invisibility, bound_item, armor, target_effect,
    paralyze, reanimate, script, count,
};

// iLeftHandType and iRightHandType for a spell (the graphs' magic states take 8 and 9, staff and spell).
inline constexpr int k_hand_spell = 9;

// CombatInventoryItemMagic.
struct Shout;

struct MagicItem {
    const Spell* spell = nullptr;
    const Shout* shout = nullptr; // a shout's: `spell` is its word's
    uint32_t object = 0;          // a staff's WEAP: `spell` is its enchantment, paid in charge
    int variation = 0;
    const MagicEffect* effect = nullptr; // the primary effect's
    const SpellEffect* primary = nullptr;
    CasterKind kind = CasterKind::none;
    int list = 0;
    float score = 0, cost = 0, min_range = 0, max_range = 0, duration = 0, magnitude = 0;
};

// What one hand holds and how far its cast has got.
struct MagicHand {
    enum class Stage : uint8_t { idle, charging, released, concentrating };
    const MagicItem* item = nullptr;
    Stage stage = Stage::idle;
    float held = 0, idle = 0, hold = -1, check = 0, cast_left = 0, casting_for = 0;
    uint32_t target = 0; // a reanimate's corpse
};

class CombatAi {
public:
    explicit CombatAi(Game& game) : m_game(game) {}

    // The game (0 neutral, 1 enemy, 2 ally, 3 friend) and the game.
    // `npc` is the actor's resolved NPC_, what a leveled base came to.
    int reaction(uint32_t actor, uint32_t npc, uint32_t target, uint32_t target_npc);
    bool should_attack(uint32_t actor, uint32_t npc, uint32_t target, uint32_t target_npc, float distance);
    bool should_fight(uint32_t victim, uint32_t attacker); // back, after a blow

    // Each frame, for everyone in combat but the player.
    void update(float dt);
    void hurt(uint32_t ref) { brain(ref).since_hurt = 0; }
    // The last thing that happened to a fight: its actor goes back to its packages.
    bool fleeing(uint32_t ref) const {
        const auto it = m_brains.find(ref);
        return it != m_brains.end() && it->second.fleeing;
    }
    void forget(uint32_t ref) { m_brains.erase(ref); }

private:
    struct Brain {
        float attack_idle = 0;          // Attack Idle between tries
        float select_timer = 0;         // until the target is looked at again
        float strength_timer = 0;       // until the threat ratio is worked out again
        float since_attack = 1e9f;      // my last attack
        float since_hurt = 1e9f;
        float unseen = 0;               // how long nobody has seen the target
        float block_left = 0;           // a block being held
        float draw_left = -1;           // a bow being held drawn, below 0 for none
        float confidence_offset = 0;    // fixed for one combat
        float threat_ratio = 100;
        bool started = false, fleeing = false, blocking = false;
        std::unordered_map<const AttackData*, float> waits;
        std::vector<MagicItem> magic;
        bool magic_read = false;
        std::array<MagicHand, 2> hands{}; // left, right
        std::array<float, size_t(CasterKind::count)> restrict{};
        float inventory = 0;   // until the hands are looked at again
        float seen_for = -1;   // below 0: the target hasn't been seen yet
        float ward_left = 0;   // the ward's cooldown once an attack ends
        float restored = 0;    // what a restore has given since it last rested
        std::vector<MagicItem> potions, shouts; // best first; a potion's restores before the rest
        float draw_retry = 0;    // until the draw is asked for again
        float flee_retry = 0;
        int flee_tries = 0;
        float weapon_held = 0;   // how long the right hand has had this one
        float weapon_check = 0;  // until the weapons are scored again
        uint32_t swap_to = 0;    // the weapon coming out, once the other is away
        float swap_left = 0;
        float potion_wait = 0, shout_wait = 0;  // Drink Potion Wait or Idle, Cast Shout Wait or Idle
    };

    Brain& brain(uint32_t ref) { return m_brains[ref]; }
    float n(std::string_view name, float fallback) const;
    float style(uint32_t ref, std::string_view member, float fallback) const;
    float random_unit();
    bool chance(float value) { return random_unit() < value; }
    void begin(Fighter& f, Brain& b);
    void select_target(Fighter& f, Brain& b, float dt);
    bool should_flee(const Fighter& f, Brain& b, float dt);
    void block(Fighter& f, Brain& b, float dt, float distance);
    void attack(Fighter& f, Brain& b, float dt, float distance);
    void shoot(Fighter& f, Brain& b, float dt, float distance);
    float weapon_score(const Fighter& f, const Item& w, bool equipped, float distance);
    bool update_weapon(Fighter& f, Brain& b, float dt, float distance); // true while changing weapons

    // combat_magic.cpp.
    std::optional<MagicItem> magic_item(uint32_t ref, const Spell& spell, uint32_t target);
    void gather_spells(const Fighter& f, Brain& b);
    void read_magic(const Fighter& f, Brain& b);
    void update_potion(Fighter& f, Brain& b, float dt, float distance);
    void update_shout(Fighter& f, Brain& b, float dt, float distance);
    bool affords(const Fighter& f, const MagicItem& item) const;
    bool has_effect_yet(uint32_t subject, const MagicItem& item);
    bool effect_conditions(const Fighter& f, const MagicItem& item, uint32_t subject);
    float restore_threshold(const Fighter& f, const MagicItem& item) const;
    bool below_restore(const Fighter& f, const MagicItem& item, float margin) const;
    bool under_attack(const Fighter& f, float distance, bool starting);
    uint32_t find_corpse(const Fighter& f, const MagicItem& item);
    bool caster_passes(const Fighter& f, Brain& b, const MagicItem& item, bool start, float distance);
    float weapon_rival(const Fighter& f, float distance);
    float item_score_now(const MagicItem& item, float distance) const;
    void fill_hands(Fighter& f, Brain& b, float distance);
    void hold(Fighter& f, Brain& b, int hand, const MagicItem* item);
    float cast_chance(const Fighter& f, const MagicItem& item) const;
    bool aim_ok(const Fighter& f, const MagicItem& item, uint32_t target, float arc);
    bool in_range(const Fighter& f, const MagicItem& item, uint32_t target, float distance);
    void notify_stop(const Fighter& f, Brain& b, MagicHand& h);
    bool should_stop(const Fighter& f, Brain& b, const MagicHand& h, float distance);
    void cast_hand(Fighter& f, Brain& b, int hand, float dt, float distance);
    float magic_stop_distance(const Fighter& f, const Brain& b) const;
    void update_magic(Fighter& f, Brain& b, float dt, float distance, bool seen);

    Game& m_game;
    std::unordered_map<uint32_t, Brain> m_brains;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_factions; // by NPC_, from the records
};

} // namespace spud::game
