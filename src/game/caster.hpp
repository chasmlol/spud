#pragma once

// Casting: four casters per actor, how an item is requested, charged, paid for and delivered, dual casting, concentration,
// shouts, powers, scrolls, staves and charge. to 6. What the graph and the world must do comes
// through MagicWorld; the engine feeds keys and graph events in.

#include "game/magic_target.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::game {

class Casters;

enum class CastState : uint8_t { idle = 0, requested = 1, charging = 2, ready = 3, released = 4, applying = 5, sustained = 6, interrupting = 9 };

enum CastFailure : int {
    cast_passes = 0,
    cast_no_magicka = 1,
    cast_power_used = 2,
    cast_silent = 3,      // a lesser power cooling down, a flying mount: no text
    cast_multi_cast = 4,
    cast_no_charge = 5,
    cast_while_shouting = 6,
    cast_shout_while_casting = 7,
    cast_voice_recovering = 8,
    cast_archetype = 100,
};

// What a projectile or explosion brings back to its caster.
struct SpellImpact {
    const Spell* item = nullptr;
    uint32_t target = 0;   // 0: it hit nothing
    Units3 location{};
    float power = 1;       // the launch's, the effectiveness of what lands
    bool dual = false;     // projectile flag 0x10000000
    bool area = false;     // one of an explosion's targets
    uint32_t object = 0;   // the weapon or ammo it came from
};

class Caster {
public:
    Caster(Game& game, Casters& casters, uint32_t actor, uint8_t source);

    const Spell* item() const { return m_item; }
    CastState state() const { return m_state; }
    uint8_t source() const { return m_source; }
    bool dual() const { return m_dual; }
    bool busy() const { return m_item && m_state != CastState::idle; }

    // state 1 and the graph told; false when CheckCast refused. `target` is the stored
    // target delivery prefers, `object` the staff or scroll the item came from.
    bool request(const Spell& item, uint32_t target = 0, uint32_t object = 0, bool dual = false);
    void begin_cast();      // BeginCastLeft, Right, Voice: the charge
    void release();         // ready: the release goes to the graph
    bool release_cast();    // MLh, MRh, Voice_SpellFire_Event: the game
    void interrupt(bool refund);
    // CastSpellImmediate: no check, no cost. `blame` answers for the effects when it isn't this actor.
    bool cast_immediate(const Spell& item, uint32_t target, uint32_t object = 0, uint32_t blame = 0);
    // on a hit, through ReleaseCast.
    bool cast_on_target(const Spell& item, uint32_t target, uint32_t object);
    // The impact hand-off calls this on the shooter's caster for the launch's casting source. A hit on
    // the ward layer goes through MagicTarget::check_ward first.
    void projectile_hit(const SpellImpact& hit);

    CastFailure check_cast(const Spell& item) const; // for this state
    void update(float dt);

private:
    float cost_of(const Spell& item) const;
    int cost_av(const Spell& item) const;
    bool perform(std::string_view name) const;
    bool queue(std::string_view name) const;
    bool item_is_power() const; // the voice source casting anything but a word
    bool hand_wanted(bool on) const;
    void ready_action();
    bool m_following_graph = false; // the graph has taken this cast on: its events lead it
    float m_graph_wait = 0;
    float view_range() const;
    bool busy_elsewhere() const;
    bool archetype_refuses(const Spell& item) const;
    void cast_failed(CastFailure reason, bool in_release);
    void pay(float amount);
    void pay_charging(float dt);
    void sustain(float dt);
    void finish();
    AddTarget target_data(const SpellEffect& effect) const;
    bool pick_direct(uint32_t& direct, std::optional<Units3>& location);
    uint32_t touch_target() const;
    bool apply_to_targets(bool& took);
    void fire_projectile(const MagicEffect* effect);
    void spell_cast(bool took);

    Game* m_game;
    Casters* m_casters;
    uint32_t m_actor;
    uint8_t m_source;
    const Spell* m_item = nullptr;
    uint32_t m_target = 0, m_object = 0, m_blame = 0;
    CastState m_state = CastState::idle;
    float m_timer = 0, m_cost = 0, m_next_update = 0, m_relaunch = 0, m_charge_time = 0;
    float m_paid = 0;
    bool m_dual = false, m_skip_checks = false;
};

class Casters {
public:
    explicit Casters(Game& game);
    ~Casters();

    Caster& caster(uint32_t ref, uint8_t source);
    // What the hand or voice holds: a SPEL, a SHOU for the voice, a SCRL, or a staff's ENCH with the staff as `object`.
    void select(uint32_t ref, uint8_t source, uint32_t form, uint32_t object = 0);
    uint32_t selected(uint32_t ref, uint8_t source) const;
    // The spell is gone: out of the hands, and its cast stopped.
    void forget(uint32_t ref, uint32_t spell);

    // The player's attack and shout keys as they stand; each frame.
    void set_keys(bool left, bool right, bool voice);
    // Each frame: keys, casters, shouts, cooldowns.
    void update(float dt);
    // A graph raised `tag` on this actor (BeginCast*, *_SpellFire_Event, InterruptCast).
    void animation_event(uint32_t ref, std::string_view tag);

    // An instant cast by the actor's instant caster; any reference can be the source.
    bool instant_cast(uint32_t caster, uint32_t spell, uint32_t target, uint32_t object = 0, uint32_t blame = 0);
    // A weapon's enchantment on a hit: charge, hand's caster, spent cost. `left` picks hand and AV 82.
    void weapon_enchantment(uint32_t attacker, const Held& held, uint32_t enchantment, bool left, uint32_t victim);

    bool casting(uint32_t ref, const Spell& item) const;
    bool dual_casting(uint32_t ref) const;
    bool can_dual_cast(uint32_t ref, const Spell& item);
    bool on_cooldown(uint32_t ref, uint32_t spell) const;
    float voice_recovery(uint32_t ref) const;
    void set_voice_recovery(uint32_t ref, float seconds) { actor(ref).shout.recovery = seconds; }
    bool shouting(uint32_t ref) const;
    const Shout* selected_shout(uint32_t ref) const;
    void unlock_word(uint32_t word) { m_words.push_back(word); } // form flag 0x10000
    bool word_unlocked(uint32_t word) const;
    const std::vector<uint32_t>& unlocked_words() const { return m_words; }
    void clear_words() { m_words.clear(); }
    // An NPC's shout, the variation its process picked.
    void npc_shout(uint32_t ref, uint32_t shout, int variation);

    // for an actor.
    float item_cost(const Spell& item, uint32_t actor) const;
    float charge_time(const Spell& item) const; // before dual casting
    // GetSkillUsageData and the player's advance. `scale` is dt or the share of the intended change.
    void use_skill(uint32_t actor, const Spell& item, const SpellEffect* effect, float scale);
    std::vector<uint32_t> actors_near(const Units3& at, float radius, uint32_t except);
    // The charge AV mirrors what the hand's entry holds, as equipping an item sets it.
    int prime_charge(uint32_t ref, bool left, const Held& held);
    void store_charge(uint32_t ref, bool left);

    uint32_t aimed_light = 0;   // player
    float ward_passes = 1;      // what a broken ward let through, for the impact that follows it
    uint32_t dual_flagged = 0;  // actor flag 0x40000000: the caster whose effect conditions see EffectWasDualCast

private:
    friend class Caster;
    struct ShoutState {
        int state = 0;
        const Shout* shout = nullptr;
        int variation = 0;
        float held = 0, waited = 0, recovery = 0;
    };
    struct Cooldown {
        uint32_t spell = 0;
        float left = 0;
    };
    struct Selected {
        uint32_t form = 0, object = 0;
    };
    struct ActorMagic {
        ActorMagic(Game& game, Casters& casters, uint32_t ref);
        uint32_t ref;
        std::array<Caster, 4> casters;
        std::array<Selected, 3> selected{};
        ShoutState shout;
        std::vector<Cooldown> cooldowns;
        std::array<uint32_t, 2> charged{}; // the items the charge AVs were set from
        std::array<bool, 2> charge_told{}; // player bits 4, 8
        std::array<const MagicEffect*, 2> hand_art{}, weapon_art{}; // what's on
        std::array<CastPhase, 2> hand_phase{};
    };
    ActorMagic& actor(uint32_t ref);
    const ActorMagic* find(uint32_t ref) const;
    void start_cooldown(uint32_t ref, const Spell& item);
    void consume_scroll(uint32_t ref, uint8_t source, uint32_t scroll);
    void update_keys(float dt);
    void hand_key(uint8_t hand, bool pressed, bool edge, float dt);
    void voice_key(bool pressed, bool edge);
    void request_selected(ActorMagic& a, uint8_t hand, bool dual);
    void start_shout(ActorMagic& a, const Shout& shout);
    void update_shout(ActorMagic& a, float dt);
    void update_hand_art(ActorMagic& a);
    int player_variation(ActorMagic& a, bool key, int state);
    void fire_shout(ActorMagic& a, int variation, int topic);
    void shout_fired(ActorMagic& a);
    float time_scale() const;

    Game& m_game;
    std::unordered_map<uint32_t, std::unique_ptr<ActorMagic>> m_actors;
    std::vector<ActorMagic*> m_order; // by index: a cast can bring in new actors
    std::vector<uint32_t> m_words;
    std::array<bool, 3> m_keys{}, m_was{};
    std::array<float, 2> m_held{};
    std::array<float, 2> m_chord{};     // a press waiting for the other hand
    std::array<bool, 2> m_requested{};  // since the key went down
};

} // namespace spud::game
