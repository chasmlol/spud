#pragma once

// Static world collision and the character controller, on Jolt. Jolt stays behind this header.
//
// World streams the collision of one .osw worldspace around a focus point (the player), on its own small
// radius: terrain cells as heightfields, every reference as one fixed body made from its mesh's .osp shapes.
// Shapes are read and built on loader threads; update() adds and removes finished bodies within a time budget
// and is called on the simulation thread, never concurrently with a character step. Body IDs are fixed by the cell and reference index,
// so contacts are solved in the same order however the area was streamed in.
//
// Clutter with one rigid body is simulated: its reference follows take_moved_refs.

#include "formats/osw.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <limits>
#include <optional>
#include <span>

namespace spud::physics {

struct WorldSettings {
    float load_radius = 64.0f;   // metres: cells this close to the focus get collision
    float unload_radius = 112.0f; // ... and keep it until they are this far
    float ready_radius = 24.0f;  // metres: ready() needs every cell this close
    double budget_ms = 0.5;      // time per update for adding and removing bodies (on the simulation thread)
    unsigned loader_threads = 2;
    // Bit n: shapes on Skyrim collision layer n are solid for the player's controller. From the plugins' COLL
    // records (see character_layers); this default is the game's built-in matrix row for
    // CHARCONTROLLER, without the trigger, water and zone layers that never block.
    uint64_t character_layers = 0;
};

struct WorldStats {
    uint32_t cells_loaded = 0;
    uint32_t cells_loading = 0; // requested, not yet in the world
    uint32_t bodies = 0;
    uint32_t shapes_cached = 0; // mesh collision shapes alive
    uint32_t shapes_missing = 0; // meshes without an .osp (nothing solid)
    uint64_t shape_bytes = 0;
};

struct RayHit {
    float distance = 0;
    glm::vec3 normal{0, 0, 1};
    uint8_t layer = 0;     // Skyrim collision layer of the surface hit
    uint32_t material = 0; // its SkyrimHavokMaterial
};

// The layers solid for a character controller (layer 30 CHARCONTROLLER): its row of the collision matrix, minus
// layers flagged as trigger volumes or sensors. `rows[n]` is layer n's row, `flags[n]` its COLL flags.
uint64_t character_layers(const uint64_t* rows, const uint32_t* flags, size_t count);
// the game's built-in row, for rules files without COLL records.
uint64_t default_character_layers();

// A ragdoll to simulate: metres and kilograms, body transforms in world space.
struct RagdollBodySpec {
    float radius = 0.0f;
    glm::vec3 a{0.0f}, b{0.0f}; // the capsule's ends in the body
    float mass = 1.0f;
    glm::vec3 inertia{1.0f}; // the diagonal
    float friction = 0.0f;
    float restitution = 0.0f;
    float linear_damping = 0.0f;
    float angular_damping = 0.0f;
    float gravity_factor = 1.0f;
    float max_linear_velocity = 0.0f;
    float max_angular_velocity = 0.0f;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 linear_velocity{0.0f};
    glm::vec3 angular_velocity{0.0f};
    float force_scale = 1.0f; // of a push_ragdoll
    uint32_t collision_filter = 0; // the file's: its body part, for what it collides with in its own ragdoll
};
struct RagdollConstraintSpec {
    int32_t body_a = -1, body_b = -1;
    bool hinge = false;
    glm::vec3 pivot_a{0.0f}, pivot_b{0.0f}; // in each body
    glm::mat3 axes_a{1.0f}, axes_b{1.0f};   // columns: twist, plane, cone; a hinge's axis first
    float twist_min = 0.0f, twist_max = 0.0f;
    float cone_max = 0.0f;
    float plane_min = 0.0f, plane_max = 0.0f;
    float hinge_min = 0.0f, hinge_max = 0.0f;
    float friction = 0.0f;
};
struct RagdollSpec {
    std::vector<RagdollBodySpec> bodies;
    std::vector<RagdollConstraintSpec> constraints;
    bool asleep = false; // lying where it was left: it wakes when something touches it
};
struct RagdollBodyState {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};
// Where an animated object's node is this frame, metres.
struct NodePose {
    uint32_t node = 0;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};
struct MovedRef {
    uint32_t form_id = 0;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

class World {
public:
    World(const std::filesystem::path& cache_root, std::shared_ptr<const osw::World> world,
          WorldSettings settings = {});
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Once per simulation tick, never concurrently with Character::step. Requests cells around `focus`, adds
    // finished ones and removes far ones within the budget.
    void update(const glm::vec3& focus);
    // Collision is complete within ready_radius of `p` (the character may move).
    bool ready(const glm::vec3& p) const;
    // Requests are outstanding near the last focus.
    bool busy() const;
    WorldStats stats() const;
    const WorldSettings& settings() const { return m_settings; }
    // A reference enabled or disabled against its record: the cell it's in rebuilds with the next update.
    void set_ref_disabled(uint32_t form_id, bool disabled);

    // Ragdolls, from any thread: they enter the simulation at the next update(). `ragdoll_state` gives the latest step's
    // body transforms, false until there is one.
    uint32_t add_ragdoll(RagdollSpec spec);
    void remove_ragdoll(uint32_t id);
    bool ragdoll_state(uint32_t id, std::vector<RagdollBodyState>& out) const;
    // A hit: each body's velocity gains `strength` (m/s, by its force_scale), away from `centre`.
    void push_ragdoll(uint32_t id, const glm::vec3& centre, float strength);
    // The references whose trap shapes (L_TRAP, L_CLOUDTRAP) a capsule overlaps. Safe beside update(), as raycast_shared.
    struct TrapTouch {
        uint32_t form = 0;
        glm::vec3 position{0.0f}, velocity{0.0f}; // the contact, and the trap's body there
    };
    void traps_touching(const glm::vec3& a, const glm::vec3& b, float radius, std::vector<TrapTouch>& out) const;
    // Does a capsule touch anything solid? Safe beside update(), as raycast_shared.
    bool touching_shared(const glm::vec3& a, const glm::vec3& b, float radius) const;

    // First solid surface along a ray (what the character collides with). Not concurrent with update().
    std::optional<RayHit> raycast(const glm::vec3& from, const glm::vec3& direction, float max_distance) const;
    // The same from another thread than the one running update(): waits while bodies are being added or removed.
    std::optional<RayHit> raycast_shared(const glm::vec3& from, const glm::vec3& direction, float max_distance) const;
    // A sphere swept from `from` to `to`, hitting only shapes whose Skyrim layer has its bit set in `layers`
    // (among those loaded for the character). The first hit: fraction along the sweep, the contact point on the
    // surface, its normal. Not concurrent with update().
    struct SweepHit {
        float fraction = 1;
        glm::vec3 point{0};
        glm::vec3 normal{0, 0, 1};
        uint8_t layer = 0;
    };
    std::optional<SweepHit> sphere_cast(const glm::vec3& from, const glm::vec3& to, float radius, uint64_t layers) const;
    // Every body a sphere swept from `from` to `to` meets on `layers`, nearest first, each once: for projectiles.
    // Any thread, like raycast_shared. A radius near 0 is a ray.
    struct SweepContact {
        float fraction = 1;
        glm::vec3 point{0};
        glm::vec3 normal{0, 0, 1};
        uint8_t layer = 0;
        uint32_t material = 0; // its SkyrimHavokMaterial
        uint32_t ref = 0;      // the reference's FormID, 0 for the land and collision markers
        uint32_t body = 0;     // for push_body
        float mass = 0;        // above 0 for a dynamic body
    };
    std::vector<SweepContact> sphere_sweep(const glm::vec3& from, const glm::vec3& to, float radius, uint64_t layers) const;
    // The bodies overlapping a sphere on `layers`: `point` is the body's position. Any thread.
    std::vector<SweepContact> sphere_overlap(const glm::vec3& centre, float radius, uint64_t layers) const;
    // Whether the cell under a point has its collision. Any thread.
    bool cell_loaded(const glm::vec3& p) const;
    // An impulse on a dynamic body at a point (kg m/s). Any thread; applied by the next update().
    void push_body(uint32_t body, const glm::vec3& linear, const glm::vec3& angular, const glm::vec3& at);
    // A body for an object made at run time, from its model's collision, reported by take_moved_refs under `form_id`. False
    // when the model has none. Any thread; it joins at the next update() once loaded.
    // `fixed` keeps a model the game doesn't simulate where it is put, as a destructible's new stage; otherwise it comes to
    // rest on what's under it.
    bool add_object(uint32_t form_id, const std::string& mesh, const glm::vec3& position, const glm::quat& rotation,
                    bool fixed = false);
    void remove_object(uint32_t form_id);
    // An animated object's bodies, keyframed to its nodes: `file` is its .osp with Body::node set, in the cache. The reference's
    // own fixed body stands aside while they're in. Any thread; they join at the next update() once loaded.
    void add_animated(uint32_t form_id, const std::string& file, float scale);
    void remove_animated(uint32_t form_id);
    void move_animated(uint32_t form_id, std::vector<NodePose> nodes);
    // An impulse on a reference's body (kg m/s), waking it. Any thread; applied by the next update().
    void impulse_ref(uint32_t form_id, const glm::vec3& impulse);
    // Its velocity from here on (m/s), waking it. Any thread; applied by the next update().
    void steer_ref(uint32_t form_id, const glm::vec3& velocity);
    bool ref_dynamic(uint32_t form_id) const; // a body the world simulates
    // The references whose bodies moved since the last call, for what draws them. Any thread.
    std::vector<MovedRef> take_moved_refs();
    // Where a reference's collision body is now (metres): moved clutter included. Nothing when it isn't loaded.
    std::optional<glm::vec3> ref_position(uint32_t form_id) const;
    float ref_mass(uint32_t form_id) const; // 0 for what has no moving body
    // Whether a reference collides (an open door doesn't). Any thread; applied by the next update().
    void set_ref_solid(uint32_t form_id, bool solid);
    // Boxes that stand where the moving leaves of open doors are now, in place of what was there. Any thread; applied by
    // the next update(). At most k_max_leaves.
    struct Leaf {
        glm::vec3 centre{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 half{0.0f};
    };
    static constexpr uint32_t k_max_leaves = 16;
    // Actors around the player, whose controllers stop it: each the hull a character has, kept to `feet` and turned by
    // `heading` (radians, the game's z angle). They take effect at the next update(). At most k_max_actors.
    struct ActorBody {
        uint32_t form_id = 0;
        glm::vec3 feet{0.0f};
        float heading = 0.0f;
        glm::vec3 half_extents{0.0f}; // metres, the race skeleton's bound, as its scale takes it
        glm::vec2 centre{0.0f};       // the bound's, on the ground
        float scale = 1.0f;
    };
    void set_actor_bodies(std::span<const ActorBody> actors);
    static constexpr uint32_t k_max_actors = 48;
    void set_leaves(std::span<const Leaf> leaves);

    struct Impl;
    Impl& impl() { return *m_impl; }

private:
    WorldSettings m_settings;
    std::unique_ptr<Impl> m_impl;
};

// The character controller's body, as Skyrim builds the player's: a convex hull from the skeleton's bounding box
struct CharacterSettings {
    glm::vec3 half_extents{0.3286f, 0.25f, 0.9144f}; // metres, scaled: the BBX bound (game fallback 23, 17.5, 64 units)
    float step_up = 0.4429f;        // max(31 units x scale, 0.75 x radius), metres
    float max_slope_deg = 47.0f;    // walkable limit; steeper contacts are walls
    float convex_radius = 0.05f;    // Havok's shell around the hull's vertices
    float mass = 70.0f;             // pushes on dynamic bodies
    // Havok material IDs whose surfaces always count as steps (the stairs materials), sorted.
    std::vector<uint32_t> stairs_materials;
};

// Controller states, numbered as the game numbers them.
enum class ControllerState : uint8_t { on_ground = 0, jumping = 1, in_air = 2, swimming = 5 };

// What one controller step is asked to do.
struct ControllerInput {
    glm::vec3 desired{0};           // m/s: horizontal on land (along the ground when standing), 3-D when swimming
    glm::vec2 facing{0, 1};         // the character's facing, horizontal unit vector (the ground frame's forward)
    bool jump = false;              // jump requested (taken off on the next step if allowed now)
    float jump_speed = 0;           // m/s
    float gravity = 9.81f;          // m/s^2, positive
    float falling_gravity_mult = 1.35f;
    float air_control = 0.3f;       // fraction of the gap to `desired` closed per step in the air
    float unsupported_gravity_mult = 4.0f;
    float unsupport_cast = 1.0f;    // m
    // Water at the character: the surface height (metres; NaN where there is none) and the body height the
    // submersion level is measured against.
    float water_height = std::numeric_limits<float>::quiet_NaN();
    float body_height = 1.8f;
    // The feet height the water update measured from, metres (NaN: the character's own). It differs only on the
    // step after a teleport, which still sees the water state of the old place.
    float water_feet_z = std::numeric_limits<float>::quiet_NaN();
    // The water current pushing the character this step, m/s; added by the OnGround and Swimming states.
    glm::vec3 external{0};
};

// One point of the character proxy's manifold.
struct ManifoldPoint {
    glm::vec3 position{0}; // on the surface, metres
    glm::vec3 normal{0, 0, 1};
    float distance = 0;    // from the hull's Havok surface, metres
    int tag = 0;           // 1 triangle face, 2 triangle edge, 0 other shapes
    uint8_t layer = 0;
};

struct CharacterState {
    glm::vec3 position{0};  // feet
    glm::vec3 velocity{0};  // after collision
    glm::vec3 requested_velocity{0}; // before it: what a landing graph hears
    ControllerState state = ControllerState::on_ground;
    bool on_ground = false; // standing (state on_ground with support)
    bool on_steep = false;  // supported only by ground too steep to walk on (sliding)
    bool supported = false; // the support check found ground (flag 0x100)
    glm::vec3 ground_normal{0, 0, 1};
    uint8_t ground_layer = 0;
    uint32_t ground_material = 0;
    float fall_start = 0;   // m: highest point of the current airborne phase (0 = none)
    float air_time = 0;     // s in the air
    bool landed = false;    // this step ended a fall
    float fall_distance = 0; // m, set on landing: from the highest point to here
    bool swimming = false;  // the actor's swimming flag
    float submersion = 0;   // water depth at the feet over the body height, 0..1
    glm::vec3 surface_normal{0, 0, 1}; // the support query's surface normal (touched planes, z > 0.08)
    std::vector<ManifoldPoint> manifold;
};

class Character {
public:
    Character(World& world, const CharacterSettings& settings, const glm::vec3& feet);
    ~Character();
    Character(const Character&) = delete;
    Character& operator=(const Character&) = delete;

    // Teleport, standing on whatever is below. Not concurrent with World::update.
    void set_position(const glm::vec3& feet);
    // Actor::SetPosition's placement (the fall-through rescue, load doors): set_position, and the controller's
    // velocity set to zero.
    void place(const glm::vec3& feet);
    // The proxy's velocity, carried over when the character moves into another world (a load door).
    void set_velocity(const glm::vec3& velocity);
    // One fixed step of Skyrim's controller: support check, the state's velocity update, then collision.
    // Not concurrent with World::update; safe on any one thread.
    void step(const ControllerInput& input, float dt);
    CharacterState state() const;
    const CharacterSettings& settings() const { return m_settings; }

    struct Impl;

private:
    CharacterSettings m_settings;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spud::physics
