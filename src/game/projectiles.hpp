#pragma once

// What flies, what it hits and what blows up: PROJ and EXPL records, launching from a bow or a caster, the flight of each
// class, impacts, arrows left in the world and explosions. Works in game units and radians as
// the design does (X pitch, positive looks down; Z heading); what it needs of the world comes through ProjectileWorld.

#include "core/game_random.hpp"
#include "game/combat.hpp"
#include "game/items.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::game {

class Game;

enum ProjectileFlag : uint16_t {
    proj_hitscan = 0x1,
    proj_explosion = 0x2,
    proj_alt_trigger = 0x4,
    proj_muzzle_flash = 0x8,
    proj_can_disable = 0x20,
    proj_can_pick_up = 0x40,
    proj_pass_small_transparent = 0x200,
    proj_no_combat_aim = 0x400,
};

enum class ProjectileType : uint16_t { missile = 0x01, lobber = 0x02, beam = 0x04, flame = 0x08, cone = 0x10, barrier = 0x20, arrow = 0x40 };

// PROJ.
struct ProjectileRecord {
    uint32_t form = 0;
    std::string editor_id, model, muzzle_model;
    uint16_t flags = 0;
    ProjectileType type = ProjectileType::missile;
    float gravity = 0, speed = 0, range = 0;
    uint32_t light = 0, muzzle_light = 0;
    float tracer_chance = 0, proximity = 0, timer = 0;
    uint32_t explosion = 0, sound = 0;
    float muzzle_duration = 0, fade_duration = 0, impact_force = 0;
    uint32_t countdown_sound = 0, disable_sound = 0, default_weapon = 0;
    float cone_spread = 0, radius = 0, lifetime = 0, relaunch = 0;
    uint32_t decal = 0;
    uint8_t layer = 7;             // what it sweeps on: its COLL's, else by type
    bool tells_triggers = false;   // the COLL has flag 0x1
    bool has(ProjectileFlag f) const { return (flags & f) != 0; }
    bool is(ProjectileType t) const { return type == t; }
    bool missile_like() const { return is(ProjectileType::missile) || is(ProjectileType::arrow); }
};

enum ExplosionFlag : uint32_t {
    expl_radius_in_units = 0x1,
    expl_world_orientation = 0x2,
    expl_knock_always = 0x4,
    expl_knock_formula = 0x8,
    expl_ignore_los = 0x10,
    expl_push_source_only = 0x20,
    expl_no_flash = 0x40,
    expl_chain = 0x80,
    expl_no_vibration = 0x100,
};

// EXPL.
struct ExplosionRecord {
    uint32_t form = 0;
    std::string editor_id, model;
    uint32_t enchantment = 0, image_space = 0;
    uint32_t light = 0, sound1 = 0, sound2 = 0, impact_set = 0, placed_object = 0, spawn_projectile = 0;
    float force = 0, damage = 0, radius = 0, image_radius = 0;
    uint32_t flags = 0;
    int32_t sound_level = 0;
    bool has(ExplosionFlag f) const { return (flags & f) != 0; }
};

inline constexpr int k_casting_source_none = 4, k_casting_source_instant = 3;

// Projectile::LaunchData (3.1).
struct LaunchData {
    Units3 origin{};
    Units3 contact_normal{}; // of a pick, zero for none
    uint32_t projectile = 0; // PROJ
    uint32_t shooter = 0;
    const Item* weapon = nullptr;
    const Item* ammo = nullptr;
    float heading = 0, pitch = 0;
    uint32_t actor_cause = 0, desired_target = 0;
    uint32_t spell = 0;
    int casting_source = k_casting_source_none;
    uint32_t enchantment = 0, poison = 0;
    float area = 0, power = 1, scale = 1;
    float weapon_health = 1; // the bow's tempering
    bool always_hit = false;
    bool no_magic = false;
    bool auto_aim = false;
    bool force_new = false;   // a Beam or Flame not to be reused
    bool use_origin = false;  // stays where it was launched, off the caster's node
    bool dual_cast = false;
    bool concentration = false;
};

enum DecalFlag : uint8_t { decal_alpha_blending = 0x2, decal_alpha_testing = 0x4, decal_no_subtextures = 0x8 };

// IPCT. Decal size comes from here, never from the TXST's own DODT.
struct ImpactRecord {
    uint32_t form = 0;
    std::string model;
    float duration = 0;
    uint32_t orientation = 0; // 0 surface normal, 1 projectile vector, 2 reflection
    float angle = 15, placement = 16;
    float min_width = 0, max_width = 0, depth = 0, shininess = 0;
    uint8_t decal_flags = 0;
    std::array<float, 3> color{1, 1, 1};
    uint32_t texture_set = 0, sound1 = 0, sound2 = 0, hazard = 0;
};

using DecalAxes = std::array<Units3, 3>; // columns: width, height, the axis

// DecalCreationData.
struct DecalRequest {
    const ImpactRecord* impact = nullptr;
    uint32_t ref = 0; // what it lands on, 0 for the land
    Units3 position{};
    DecalAxes axes{};
    float width = 0, height = 0, depth = 0, placement = 16;
    int subtexture = -1; // -1 the whole texture, else a quadrant of the 2 x 2 atlas
};

// Blood on an actor. `forced`: the player struck, so no distance or per-frame limit.
struct SkinDecalRequest {
    const ImpactRecord* impact = nullptr;
    uint32_t victim = 0;
    Units3 position{}, direction{};
    DecalAxes axes{};
    float size = 0;
    bool forced = false;
};

enum class ImpactResult : uint8_t { none = 0, destroy = 1, bounce = 2, impale = 3, stick = 4 };

// A contact of a sweep, a ray or an overlap: `ref` 0 for the land.
struct ProjectileContact {
    uint32_t ref = 0;
    Units3 position{};
    Units3 normal{0, 0, 1};
    uint8_t layer = 0;
    uint32_t material = 0; // MATT
    float fraction = 0;    // along the sweep
    uint32_t body = 0;     // what push() takes
    float mass = 0;        // above 0 for a dynamic body
    bool actor = false;
    int part = -1;         // the biped part struck on an actor, 18 the shield
};

// A body as projectiles see it. `position` is the reference's.
struct ProjectileBody {
    Units3 position{};
    Units3 velocity{};
    float height = 0, radius = 0;
    bool seated = false, ghost = false;
};

// Where a shot starts and points: the fire node of 3.5, the view of a player.
struct ShotAim {
    Units3 origin{};
    float heading = 0, pitch = 0;
    bool node = false;         // a fire node was found
    bool first_person = false; // the player's own view
    bool third_person = false;
};

// Everything a spell needs at a contact (5.5) or from an explosion (6.6).
struct MagicHit {
    uint32_t caster = 0, spell = 0, target = 0;
    uint32_t projectile = 0, explosion = 0;
    int casting_source = k_casting_source_none;
    Units3 position{}, normal{};
    float area = 0;
    float effectiveness = 1;  // the projectile's power
    bool dual_cast = false;
    bool chain = false;       // the explosion is a Chain
};

// The world side of a projectile. The defaults are a world with nothing in it.
class ProjectileWorld {
public:
    virtual ~ProjectileWorld() = default;
    // All that a sphere meets from a to b on the layers in the mask (bit n: layer n), nearest first, a body once.
    virtual std::vector<ProjectileContact> sweep(const Units3&, const Units3&, float, uint64_t) { return {}; }
    // The same for a ray.
    virtual std::vector<ProjectileContact> pick(const Units3&, const Units3&, uint64_t) { return {}; }
    // What overlaps a sphere (`layers` as sweep).
    virtual std::vector<ProjectileContact> overlap(const Units3&, float, uint64_t) { return {}; }
    virtual uint64_t layer_row(uint8_t) { return ~uint64_t(0); } // the layers a layer collides with
    virtual void shake_camera(const Units3&, float) {}
    virtual bool body(uint32_t, ProjectileBody&) { return false; }
    virtual bool water_height(const Units3&, float&) { return false; }
    virtual uint32_t space() { return 0; }             // where the player is
    virtual bool exterior() { return true; }
    virtual bool loaded(const Units3&) { return true; } // the cell has its collision
    virtual bool present(uint32_t) { return true; }     // a reference still has its 3D, undeleted and enabled
    virtual bool aim(uint32_t, bool, ShotAim&) { return false; }            // `crossbow`
    virtual bool magic_node(uint32_t, int, Units3&) { return false; }       // the hand of a casting source
    virtual bool combat_aim(uint32_t, const Item*, Units3&) { return false; }
    // The offset of a point from the middle of the screen, as fractions of it; false when behind the camera.
    virtual bool on_screen(const Units3&, float&, float&) { return false; }
    virtual std::vector<uint32_t> actors() { return {}; } // high process, the player left out
    virtual bool allied(uint32_t, uint32_t) { return false; } // a mine spares its shooter's allies
    // A visual per projectile, by its id: `pitch` and `heading` as the reference's angles.
    virtual void create_visual(uint32_t, std::string_view, const Units3&, float, float, float) {}
    virtual bool visual_ready(uint32_t) { return true; }
    virtual void move_visual(uint32_t, const Units3&, float, float) {}
    virtual void hide_visual(uint32_t, bool) {}
    virtual void destroy_visual(uint32_t) {}
    virtual void visual_alpha(uint32_t, float) {}
    virtual void tracer_nodes(uint32_t, bool) {}     // which of the first two children shows
    virtual void flame_length(uint32_t, float) {}    // fFlameProjectileLength
    virtual void blink(uint32_t) {}                  // a mine's beep
    virtual void trail(uint32_t, bool) {}            // an arrow's
    virtual void set_light(uint32_t, uint32_t, const Units3&, float) {}
    virtual void remove_light(uint32_t) {}
    // A sound form at a point, or 2D with null: a handle for the two below, 0 for none.
    virtual uint32_t play_sound(uint32_t, const Units3*) { return 0; }
    virtual void move_sound(uint32_t, const Units3&) {}
    virtual void stop_sound(uint32_t) {}
    virtual float camera_distance(const Units3&) { return 0; }
    // a model with `frame` as its axes, at a scale, for at least its IPCT duration.
    virtual void temp_particle(std::string_view, const Units3&, const DecalAxes&, float, float) {}
    virtual void add_decal(const DecalRequest&) {}
    virtual void screen_blood(int) {} // this many splats
    virtual void weapon_blood(uint32_t, float) {} // the attacker, what the level gains
    virtual void skin_decal(const SkinDecalRequest&) {}
    virtual void noise(uint32_t, int) {}
    virtual void ripple(const Units3&, float) {}
    virtual void splash(const Units3&, float) {}
    virtual void flash(uint32_t, float) {}                                        // an IMAD at a strength
    virtual void muzzle_flash(const ProjectileRecord&, const Units3&) {}
    virtual void push(uint32_t, const Units3&, const Units3&, const Units3&) {}   // body, linear, angular, point
    virtual void damage_ref(uint32_t, float, uint32_t) {}                         // the destruction path
    virtual void hit_event(uint32_t, uint32_t, uint32_t, uint32_t) {}             // target, source, form, projectile
    virtual void knock(uint32_t, const Units3&) {}                                // ragdoll from a point
    virtual void ragdoll(uint32_t) {}
    virtual void place_object(uint32_t, const Units3&, float, uint32_t) {}        // base, at, heading, cause
    virtual float hit_time(std::string_view) { return 0; }  // the "Hit" key of an explosion's sequence
    virtual float sequence_length(std::string_view) { return 0; }
};

struct Impact {
    uint32_t ref = 0;
    Units3 position{}, normal{};
    uint8_t layer = 0;
    uint32_t material = 0, body = 0;
    float mass = 0;
    int part = -1;
    bool actor = false;
    ImpactResult result = ImpactResult::none;
    bool resolved = false;      // the weapon hit
    bool spell_applied = false;
};

enum class ArrowFlight : uint8_t { flying, bounced, stuck };

// A live projectile: a reference with the state of its class. Flags are named here; the design gives their bits.
struct Projectile {
    uint32_t id = 0;
    const ProjectileRecord* record = nullptr;
    LaunchData launch;
    uint32_t space = 0;
    std::string model;
    Units3 position{}, velocity{}, direction{};
    float heading = 0, pitch = 0;
    float moved = 0, range = 0, age = 0, alpha = 1;
    float power = 1, weapon_damage = 0, speed = 0;
    uint32_t explosion = 0;
    ArrowFlight flight = ArrowFlight::flying;
    std::vector<Impact> impacts;
    bool has_3d = false;
    bool dead = false;
    bool impacts_processed = false;
    bool effects_played = false;
    bool exploded = false;
    bool hitscan = false, tracer = false;
    bool fading = false;
    bool npc_in_combat = false;  // 0x80000: the shooter must still be fighting
    bool touched_shield = false; // 0x8000000
    bool disarmed = false;
    bool no_perk_roll = false;   // 0x4000
    bool limited = false;
    bool resting = false;        // a Lobber that has landed
    uint32_t sound = 0;
    float timer = 3.4e38f, beep = 0; // to detonation, FLT_MAX until armed
    Units3 origin{};                 // a Cone's
    float radius = 0;
    float stage = 0;                 // a Flame's timer, a Cone's ground timer
    std::vector<uint32_t> bodies;    // what a Cone has pushed
    // Stuck.
    uint32_t follows = 0;
    Units3 offset{};
    double stuck_at = -1;            // when, while it wobbles
    uint32_t placed = 0;             // the stack left for the player to take
    float decal_size = 0;            // drawn at its first decal
};

// Explosion::Explosion's inputs (6.2).
struct ExplosionSpawn {
    uint32_t form = 0; // EXPL
    uint32_t source = 0, hit_ref = 0;
    Units3 position{}, normal{};
    float heading = 0, pitch = 0;
    float scale = 1, area = 0;
    uint32_t spell = 0;
    const Item* weapon = nullptr;
    bool friendly = false;
    uint32_t projectile = 0; // what set it off: a chain goes on with the same
    int casting_source = k_casting_source_none;
    float power = 1;
};

// ChainExplosion: hops from actor to actor.
struct ChainExplosion {
    ExplosionSpawn spawn;
    Units3 point{};
    float radius = 0;
    float effectiveness = 1;
    uint32_t target = 0;
    uint32_t in_flight = 0; // the projectile
    float waited = 0;
    std::vector<uint32_t> visited;
    bool done = false;
};

struct Explosion {
    uint32_t id = 0;
    const ExplosionRecord* record = nullptr;
    ExplosionSpawn spawn;
    float radius = 0, image_radius = 0;
    float age = 0, hit_time = 0, end_time = 3;
    bool started = false, applied = false, pushed = false;
    std::vector<ProjectileContact> found;
};

class Projectiles {
public:
    explicit Projectiles(Game& game);
    ~Projectiles();

    void set_world(ProjectileWorld* world) { m_world = world ? world : &m_nothing; }
    ProjectileWorld& world() { return *m_world; }
    std::function<uint32_t()> random = [this] { return m_random(); };
    // Where a spell lands: Caster::projectile_hit (5.5, 6.6).
    std::function<void(const MagicHit&)> on_magic_hit;
    // The ward test on layer 29: true when it stops the projectile (5.5).
    std::function<bool(const MagicHit&)> on_ward;
    // weapon, ammo, power.
    std::function<void(const Item*, const Item*, float)> on_bow_shot;
    bool trace = false; // logs where each projectile ends

    const ProjectileRecord* record(uint32_t form);
    const ExplosionRecord* explosion(uint32_t form);

    // the projectile's id, 0 when the record isn't a PROJ.
    uint32_t launch(const LaunchData& given);
    // A caster's launch: the spell's own MGEF projectile unless one is given.
    uint32_t launch_spell(LaunchData data);
    // The game (3.5) for the hand's weapon.
    bool arrow_release(uint32_t shooter);
    // The kill camera: told of each of the player's shots before it goes, and what it went as. It can aim the shot.
    std::function<void(LaunchData&)> before_player_shot;
    std::function<void(uint32_t id)> after_player_shot;
    uint32_t tracked = 0;     // the projectile the camera follows: it turns at its desired target (4.5)
    float player_rate = 1.0f; // the player's projectiles take the shot's player rate
    const Projectile* find(uint32_t id) const;
    // with the equipped ammo when none is passed. `held` may be null.
    bool fire_weapon(uint32_t shooter, const Item* weapon, const Item* ammo = nullptr, const Held* held = nullptr,
                     uint32_t enchantment = 0, uint32_t poison = 0);
    uint32_t explode(const ExplosionSpawn& spawn);
    // The game on a mine: disarms it (5.8). Arrows are taken through Game::take.
    bool activate(uint32_t projectile);

    // The attack button on a bow: pressed, held so long, let go.
    void bow_pressed();
    void bow_held(float seconds);
    void bow_released();

    // The manager, once a frame.
    void update(float dt);

    // The formulas, for checks.
    float draw_power(float seconds, const Item& weapon, uint32_t shooter);
    float power_speed_mult(float power) const;
    float bow_speed_mult(const Item* weapon) const;
    float gravity_of(const Projectile& p) const;
    float explosion_damage(const Explosion& e);
    // The MATT of a Havok material: the CRC of its lower-cased MNAM.
    uint32_t material_of_havok(uint32_t havok);

    std::span<const std::unique_ptr<Projectile>> live() const { return m_live; }
    std::span<const std::unique_ptr<Explosion>> explosions() const { return m_explosions; }

private:
    struct SpellEffect {
        uint32_t explosion = 0, projectile = 0, impact_set = 0;
        float dual_scale = 1;
    };
    struct AttachedArrow {
        uint32_t visual = 0, target = 0;
        Units3 offset{};
        float heading = 0, pitch = 0, age = 0;
    };
    struct DrawEntry {
        float seconds = 0;
        double released = -1;
    };

    float n(std::string_view name, float fallback) const;
    float perk(uint32_t owner, int point, float value, std::initializer_list<uint32_t> args = {}) const;
    float roll(float low, float high);
    bool is_actor(uint32_t ref) const;
    const SpellEffect* first_effect(uint32_t spell);
    float pop_draw();
    uint64_t layers_of(const Projectile& p);
    uint32_t impact_set(const Projectile& p);
    uint32_t impact_of(uint32_t set, uint32_t material);
    uint32_t material_of(const ProjectileContact& c);
    void restart(Projectile& p, const LaunchData& data);

    // ArrowFlight.
    void first_frame(Projectile& p);
    void auto_aim(Projectile& p);
    void track(Projectile& p);
    void set_velocity(Projectile& p);
    void step(Projectile& p, float dt);
    void step_missile(Projectile& p, float dt);
    void step_cone(Projectile& p, float dt);
    void step_beam(Projectile& p, float dt);
    void step_flame(Projectile& p, float dt);
    void step_lobber(Projectile& p, float dt);
    void step_barrier(Projectile& p);
    void step_bounced(Projectile& p, float dt);
    void step_stuck(Projectile& p);
    void follow_node(Projectile& p);
    void node(Projectile& p);
    Units3 integrate(Projectile& p, float dt, float gravity);
    void sweep(Projectile& p, const Units3& from, const Units3& to, float radius);
    void fade(Projectile& p, float dt);
    void kill(Projectile& p);
    void land(Projectile& p, const char* how);
    float run_pick(Projectile& p, const Units3& from, const Units3& to);
    void flame_pick(Projectile& p);
    void cone_ground(Projectile& p);
    bool cone_clear(const Projectile& p, const ProjectileContact& c);

    // Hits.
    void handle_hits(Projectile& p, std::span<const ProjectileContact> contacts);
    void cone_hits(Projectile& p, std::span<const ProjectileContact> contacts);
    bool can_hit(const Projectile& p, const ProjectileContact& c);
    bool is_trigger(const Projectile& p, const ProjectileContact& c) const;
    bool pre_hit_magic(Projectile& p, const ProjectileContact& c, bool& applied);
    void apply_spell(const Projectile& p, uint32_t spell, uint32_t target, const Units3& at, const Units3& normal,
                     int source);
    bool add_impact(Projectile& p, const ProjectileContact& c);
    Impact make_impact(const Projectile& p, const ProjectileContact& c);
    void arrow_impact(Projectile& p, Impact& i);
    void hit_actor(Projectile& p, Impact& i);
    bool impacts_ready(const Projectile& p) const;
    void process_impacts(Projectile& p);
    void act_on_result(Projectile& p);
    void bounce(Projectile& p);
    void stick(Projectile& p);
    void stick_actor(Projectile& p, const Impact& i);
    void leave_in_world(Projectile& p);
    void play_effects(Projectile& p, const Impact& i);
    void push_impact(const Projectile& p, const Impact& i);
    float impulse_scale(float force, uint8_t layer, float mass) const;
    float spell_health_damage(uint32_t spell) const;
    bool hostile(uint32_t spell) const;

    // Explosions.
    void check_explode(Projectile& p, float dt);
    bool proximity(Projectile& p);
    void detonate(Projectile& p);
    void start_explosion(Explosion& e);
    void find_targets(Explosion& e);
    void update_chain(ChainExplosion& c, float dt);
    bool chain_choose(ChainExplosion& c);
    bool chain_launch(ChainExplosion& c);
    Units3 mid_height(uint32_t ref) const;
    void explosion_surface(Explosion& e);
    void apply_to_targets(Explosion& e);
    bool explosion_los(const Explosion& e, const ProjectileBody& target);
    bool knock_by_formula(uint32_t actor, float damage);
    void push_bodies(Explosion& e);

    void limit_pass();
    void wobble();
    void attached(float dt);

    Game& m_game;
    ProjectileWorld m_nothing;
    ProjectileWorld* m_world = &m_nothing;
    std::unordered_map<uint32_t, std::unique_ptr<ProjectileRecord>> m_records;
    std::unordered_map<uint32_t, std::unique_ptr<ExplosionRecord>> m_explosion_records;
    std::unordered_map<uint32_t, SpellEffect> m_spell_effects;
    std::unordered_map<uint32_t, uint32_t> m_race_materials;
    std::unordered_map<uint32_t, uint32_t> m_havok_materials;
    std::vector<std::unique_ptr<Projectile>> m_live;
    std::vector<std::unique_ptr<Explosion>> m_explosions;
    std::vector<ChainExplosion> m_chains;
    std::deque<uint32_t> m_limited;
    std::vector<AttachedArrow> m_attached;
    std::deque<DrawEntry> m_draws;
    double m_time = 0;
    uint32_t m_next_ref = 0xFF800000; // the game's created references sit in the FF range
    std::mt19937 m_random{session_seed()};
};

} // namespace spud::game
