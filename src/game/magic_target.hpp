#pragma once

// Magic on an actor: the spells it knows and the effects running on it, over its actor values, how an effect is added,
// runs and ends, and what each archetype does., 3 and. Knows nothing
// of bodies and graphs: what the world must do comes through MagicWorld.

#include "game/actor_values.hpp"
#include "game/combat.hpp"
#include "game/magic.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

namespace spud::game {

class Game;

enum CastingSource : uint8_t { source_left = 0, source_right = 1, source_voice = 2, source_instant = 3, source_none = 4 };

// One effect of a magic item on a target (ActiveEffect).
struct ActiveEffect {
    uint32_t id = 0; // this run's, for scripts
    uint32_t spell = 0;
    uint32_t effect = 0; // MGEF
    uint32_t caster = 0;
    uint32_t target = 0; // whose list it's on
    uint32_t object = 0; // the source: weapon, ammo or armour
    const Spell* item = nullptr;
    const SpellEffect* source = nullptr;
    const MagicEffect* setting = nullptr;
    float magnitude = 0, duration = 0, elapsed = 0;
    float applied = 0;  // what Start wrote, after its clamp
    float fraction = 1; // of the last write, what took (slot)
    Units3 location{};
    uint8_t casting_source = source_instant;
    bool has_location = false;
    bool has_conditions = false;
    bool no_expiry = false; // 0x4000
    bool tethered = false;  // 0x2000: ends with the cast
    bool worn = false;      // 0x100: an armour's, runs while it's worn
    bool dual = false;      // 0x1000
    bool active = true;     // the conditions held at the last check
    bool started = false, finished = false, dispelled = false;
    bool suppressed = false; // behind a stronger one of its peak chain
    bool skill_paid = false; // 0x80000
    bool first_update = true;
    // What an archetype keeps: the summon, corpse, weapon or light, and its pulse or hold timer.
    uint32_t link = 0;
    float timer = 0;
};

// AddTargetData: one call per effect per target.
struct AddTarget {
    uint32_t caster = 0;
    const Spell* item = nullptr;
    const SpellEffect* effect = nullptr;
    uint32_t object = 0;              // the source
    std::optional<float> magnitude;   // the caster's override, else the effect item's
    float effectiveness = 1;
    float power = 1;                  // the cast's own, AdjustActiveEffect's first step
    bool power_hostile_only = false;
    float cast_cost = 0;              // what dual effectiveness scales by
    uint8_t casting_source = source_instant;
    bool area_target = false;
    bool dual = false;
    std::optional<Units3> location;
};

// Projectile::LaunchData as a spell fills it. The other fields of the structure are the bow's.
struct SpellLaunch {
    Units3 origin{}, contact_normal{};
    uint32_t projectile = 0;     // PROJ
    uint32_t shooter = 0;
    uint32_t actor_cause = 0;    // who's to blame
    uint32_t desired_target = 0;
    uint32_t weapon = 0;         // a staff, say; the hit's source
    float heading = 0, pitch = 0; // radians, positive pitch looks down, as projectiles.hpp
    uint32_t spell = 0;
    uint8_t casting_source = source_none;
    uint32_t enchantment = 0, poison = 0;
    float area = 0, power = 1, scale = 1;
    bool dual = false;           // projectile flag 0x10000000
    bool concentration = false;
    bool always_hit = false, no_magic = false, auto_aim = true, force_new = false, use_origin = false;
};

// What the casting art's graph reacts to.
enum class CastPhase : uint8_t { none, charging, ready, released, sustained };

// What a ray from the magic node found.
struct MagicPick {
    uint32_t ref = 0;
    Units3 point{};
};

// The world side of magic. The defaults are a world with nothing in it: casts run, nothing shows.
class MagicWorld {
public:
    virtual ~MagicWorld() = default;

    // ---- casting -----------------------------------------------------------------------------------------------
    // False: the game's own Projectiles takes it.
    virtual bool launch_spell(const SpellLaunch&) { return false; }
    virtual bool magic_node(uint32_t, uint8_t, Units3&) { return false; } // the source's node
    virtual void hand_art(uint32_t, uint8_t, const MagicEffect*) {}       // actor, source, what's ready (null: off)
    virtual void hand_cast(uint32_t, uint8_t, CastPhase) {}               // where that hand's cast stands, for the art's graph
    virtual void weapon_art(uint32_t, uint8_t, const MagicEffect*) {}     // on the weapon in that hand
    virtual bool look_angles(uint32_t, float&, float&) { return false; }  // heading, pitch as SpellLaunch's
    virtual bool combat_aim(uint32_t, uint32_t, float&, float&) { return false; } // an NPC's controller at its target
    virtual bool view_ray(uint32_t, float, MagicPick&) { return false; }  // along the view, this many units
    virtual uint32_t crosshair_actor(uint32_t) { return 0; }
    virtual bool snap_to_navmesh(Units3&) { return true; } // the nearest triangle clear of actors within 512 units
    virtual bool ray_on_ground(uint32_t, float, Units3&) { return false; } // layers 1, 13, 17 only
    virtual void place_explosion(uint32_t, const Units3&, uint32_t) {}     // EXPL, where, its caster
    virtual bool mount_forbids(uint32_t, const Spell&) { return false; }   // on a flying mount, by 4.2's rule
    virtual void cast_sound(uint32_t, uint32_t, bool) {}                   // actor, spell, on
    virtual void cast_noise(uint32_t, uint32_t) {}                         // actor, sound level, to detection
    virtual void image_space(uint32_t) {}                                  // the player's IMAD
    virtual void fail_art(uint32_t, const Spell&) {}
    virtual void help_message(int) {}
    virtual void say(uint32_t, int) {}                                     // a topic type
    virtual void spell_cast_event(uint32_t, uint32_t) {}                   // OnSpellCast: actor, item

    // ---- effects -----------------------------------------------------------------------------------------------
    virtual void default_object(uint32_t, int) {}                          // art or a sound, at a reference
    virtual void start_hit_effects(const ActiveEffect&) {}                 // on the effect's target
    virtual void stop_hit_effects(uint32_t) {}                             // the effect's id: they play out their end
    virtual void pain(uint32_t) {}
    virtual void effect_applied(uint32_t, uint32_t, uint32_t) {}           // OnMagicEffectApply: target, caster, MGEF
    virtual void magic_hit(uint32_t, uint32_t, uint32_t) {}                // OnHit: target, caster, weapon or item
    virtual void paralyse(uint32_t, bool) {}
    virtual void reevaluate_target(uint32_t) {}                            // for high-process actors
    virtual void disarm(uint32_t) {}
    virtual void bound_weapon(uint32_t, uint32_t, uint8_t) {}              // actor, WEAP, hand: equips, keeping what it held
    virtual void unbound_weapon(uint32_t, uint32_t, uint8_t) {}            // if still held, the old item back
    virtual bool summon_spot(uint32_t, Units3&) { return false; }
    virtual uint32_t summon(uint32_t, uint32_t, const Units3&) { return 0; } // caster, NPC_, where: the placed actor
    virtual void fade_summon(uint32_t, float) {}
    virtual void reanimate(uint32_t, uint32_t) {}                          // corpse, caster: gets up, commanded
    virtual void light(uint32_t, uint32_t, bool) {}                        // target, LIGH, on
    virtual void spawn_hazard(uint32_t, uint32_t, const Units3&, uint32_t) {} // HAZD, ref, where, caster
    virtual bool telekinesis_start(uint32_t) { return false; }             // takes the crosshair ref; false: ungrabbable
    virtual void telekinesis_stop(uint32_t) {}
    virtual bool holding(uint32_t) { return false; }                       // player
    virtual int guide(uint32_t, uint32_t) { return 2; }                    // caster, HAZD: 0 shown, 1 no marker, 2 no path
    virtual void detect_life(uint32_t, uint32_t, bool, float) {}           // caster, MGEF, on, units: its hit shader on the area's actors
    virtual void transform(uint32_t, uint32_t, bool) {}                    // actor, RACE, on
    virtual void slow_time(float, float, bool) {}                          // player rate, world rate, on
    virtual bool time_slowed() { return false; }
    virtual float time_factor() { return 1; }
    virtual bool grab_actor(uint32_t, bool) { return false; }
    virtual void enhance_weapon(uint32_t, uint32_t, bool) {}               // actor, ENCH, on
};

class MagicTarget {
public:
    MagicTarget(Game& game, uint32_t ref, ActorValues& values);
    MagicTarget(const MagicTarget&) = delete;

    uint32_t ref() const { return m_ref; }
    ActorValues& values() { return m_values; }

    // ---- spells -----------------------------------------------------------------------------------------------
    bool has_spell(uint32_t spell) const; // the added spells, the base form's, the race's
    std::vector<uint32_t> all_spells() const; // ... each once, leveled lists rolled at the actor's level
    bool add_spell(uint32_t spell);
    bool remove_spell(uint32_t spell);
    // the abilities of an NPC's base and race take hold.
    void cast_permanent();
    std::vector<uint32_t>& added_spells() { return m_spells; }
    const std::vector<uint32_t>& added_spells() const { return m_spells; }

    // ---- effects ----------------------------------------------------------------------------------------------
    // one effect of an item on this target.
    bool add_target(const AddTarget& target);
    // All of an item's effects in `effect_mask`, from no cast: potions, abilities, what scripts apply.
    bool apply(uint32_t spell, uint32_t caster, uint32_t effect_mask = 0xFFFFFFFF, uint32_t object = 0);
    // a disease rolls DiseaseResist first.
    bool combat_spell_apply(uint32_t spell, uint32_t caster);
    // True when the ward took it all. `effect` null: the projectile's ward layer, the sum of
    // the hostile effects times `relaunch` for concentration aimed. `passes` is what's left of the effectiveness.
    bool check_ward(const Spell& item, const SpellEffect* effect, float relaunch, float& passes, uint32_t caster = 0);

    bool dispel_spell(uint32_t spell); // the effects only
    void dispel_effect(uint32_t id);
    void dispel_all();
    void dispel_archetype(uint32_t archetype, uint32_t caster = 0);
    // a cast of anything else ends invisibility and the ethereal state.
    void break_invisibility(const Spell& item);

    const std::vector<ActiveEffect>& effects() const { return m_effects; }
    bool causing_damage() const; // a running effect taking health
    bool has_magic_effect(uint32_t effect) const;
    bool has_magic_effect_keyword(uint32_t keyword) const;
    bool has_disease() const;
    bool has_effect_from(uint32_t spell) const; // IsSpellTarget, the game
    bool has_archetype(uint32_t archetype) const;

    bool ghost() const;
    void set_ghost(bool ghost) { m_ghost = ghost; }
    uint32_t commander = 0; // who summoned or raised this one
    bool fed_on = false;    // a werewolf's mark on a corpse
    bool soul_taken = false;

    // elapsed time, conditions, ticks, ends. dt 0 starts what was just added.
    void update(float dt);

    // A save's change layer (game_save.cpp). What the effects wrote is in the values, so loading writes nothing.
    void save(BinaryWriter& w) const;
    void load(BinaryReader& r);
    int level() const;

private:
    bool is_actor() const;
    bool alive() const;
    float number(std::string_view name, float fallback) const;
    float perk(uint32_t owner, int point, float value, std::initializer_list<uint32_t> args = {}) const;
    bool is_armour(uint32_t object) const;

    bool add_effect(const AddTarget& t, const MagicEffect& m);
    bool check_absorb(const AddTarget& t);
    float resistance(const AddTarget& t, const MagicEffect& m) const;
    bool check_add(const AddTarget& t, const MagicEffect& m, float resist, int& reason);
    bool can_add(const AddTarget& t, const MagicEffect& m, int& reason);
    bool level_check(const AddTarget& t, const MagicEffect& m, int& reason) const;
    bool refresh_matching(const AddTarget& t);
    void dispel_matching(const AddTarget& t);
    void apply_power(ActiveEffect& e, float power, bool hostile_only = false) const;
    void on_add(ActiveEffect& e);
    void dispel_with_keywords(const ActiveEffect& e);
    void insert(ActiveEffect e);

    bool check_conditions(const ActiveEffect& e) const;
    float current_magnitude(const ActiveEffect& e) const;
    void write_value(ActiveEffect& e, int av, float amount);
    void remove_value(ActiveEffect& e, int av, float amount);
    void write_all(ActiveEffect& e, float amount);
    void update_peak_chains();
    void update_effect(ActiveEffect& e, float dt);
    bool tether_holds(const ActiveEffect& e) const;
    void update_skill(ActiveEffect& e, float dt);
    void dispel_type(uint32_t type);
    void dispel_summon(uint32_t summoned);

    void start_effect(ActiveEffect& e);
    void finish_effect(ActiveEffect& e);
    void start_target_value(ActiveEffect& e);
    void start_summon(ActiveEffect& e);
    void start_reanimate(ActiveEffect& e);
    void limit_summons();
    void archetype_update(ActiveEffect& e, float dt);
    void pulse_cloak(ActiveEffect& e, float dt);
    void accumulate(ActiveEffect& e, float dt);
    void capture_soul(const ActiveEffect& e);

    Game* m_game;
    uint32_t m_ref;
    ActorValues& m_values;
    std::vector<uint32_t> m_spells;
    std::vector<ActiveEffect> m_effects;
    bool m_updating = false;
    bool m_ghost = false;
    float m_absorb_art = 0; // fMagicAbsorbVisualTimer's countdown
};

} // namespace spud::game
