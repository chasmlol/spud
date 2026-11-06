#pragma once

// Actors in the world: placed NPCs and the player's body, assembled from the forms database the way the game
// assembles them, drawn as skinned models posed by the GPU.
//
// ActorData reads records: it is shared by every space and safe to use from workers. ActorSet is one space's actors:
// which are near enough to exist, their models, their pose.

#include "anim/graph.hpp"
#include "engine/shader_particles.hpp"
#include "formats/forms_typed.hpp"
#include "fx/effect.hpp"
#include "formats/osk.hpp"
#include "formats/osv.hpp"
#include "game/face.hpp"
#include "formats/osw.hpp"
#include "physics/physics.hpp"
#include "render/renderer.hpp"

#include <deque>
#include <array>
#include <atomic>
#include <array>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

namespace spud::engine {

// A skeleton with its node names looked up and its rest pose in the model's space.
struct ActorSkeleton {
    osk::Skeleton nodes;
    std::unordered_map<std::string, uint32_t> by_name; // lowercase
    std::vector<glm::mat4> rest;                       // node -> model space
    std::vector<glm::mat4> rest_local;                 // node -> its parent
};

// A head part's morph files (cache paths) and the shape of its model they go with.
struct FacePart {
    std::string shape; // the head part's editor id, lowercase; "": the model holds this part alone
    std::string tri;   // what the face animates with
    std::string race;  // an assembled head's race morph
    std::string chargen; // its slider morphs
};

// One model an actor wears.
struct ActorPart {
    std::string mesh;         // cache path of the skinned .osm
    float morph_weight = 1.0f; // 0 the light end of a weight-morphed model, 1 the heavy one
    bool head = false;         // a head: its partitions go by ActorLook::head_hidden, not by `slots`
    bool assembled = false;    // a head part put together here: the game colours these, not FaceGen models
    uint32_t slots = 0;        // the biped slots that hold this model: its partitions for other slots are off
    std::string diffuse;       // texture set in place of the model's own, cache paths ("" keeps it)
    std::string normal;
    std::string specular;
    std::string face_tint;     // a FaceGen head's tint map
    glm::vec3 hair_color{1.0f}; // assembled hair, doubled as the game stores it
    bool has_hair_color = false;
    std::vector<FacePart> face;
    bool whole = false;        // an object's model: no partitions to choose
    const ots::Swap* swap = nullptr; // its base object's alternate textures, by shape name
    uint32_t weapon = 0;       // a geared-up weapon's WEAP: only the one in the right hand is drawn
    bool spare = false;        // ... past the Geared Up Weapons count: shown only in the hand
    bool no_scabbard = false;  // first person: `Scb` is removed
    bool left_hand = false;    // a left-hand weapon: on Shield, shown only when drawn
};

// How an actor looks, resolved from its records.
// What scripts put on an NPC or took off it, over what it wears by itself.
struct NpcGear {
    std::vector<uint32_t> on, off;
    bool bare = false;     // UnequipAll
    bool headless = false; // decapitated: it stays so until resurrected
};

struct ActorLook {
    uint32_t npc = 0;  // the NPC_ drawn
    uint32_t race = 0;
    bool female = false;
    float height = 1.0f; // the root's scale
    bool first_person = false;
    bool player = false; // the player's third-person body
    std::shared_ptr<const ActorSkeleton> skeleton;
    std::shared_ptr<const anim::Project> project; // its behaviour project, null for none
    std::shared_ptr<const fx::Effect> loop;       // an object with no project: the sequences of its .ose
    // What an assembled head bakes in.
    std::string race_morph;
    std::vector<std::pair<std::string, float>> chargen_morphs;
    float skinny = 0.0f; // SkinnyMorph's weight
    float weap_adj = 0.0f; // the graph's weapAdj, which sets how skinny or muscular its poses are
    int mood = 0;        // AIDT
    std::shared_ptr<const std::vector<std::byte>> face_tint; // a made face's tint map, a DDS file
    int32_t right_hand_type = 0;                  // the equipped weapon's animation type
    int32_t left_hand_type = 0;
    uint32_t right_weapon = 0;                    // ... its WEAP
    std::vector<std::pair<uint32_t, int32_t>> weapons; // what a fight can swap to: WEAP, animation type, best first
    std::string behaviour_folder;                 // where the race's graph lives, as IDLE DNAMs name it
    std::vector<uint32_t> armor;                  // the ARMOs worn
    uint32_t inventory_npc = 0;                   // the NPC_ its inventory comes from (template flag Inventory)
    std::vector<uint32_t> footstep_sets;          // the worn addons' distinct FSTS forms
    glm::vec3 skin_tone{1.0f};
    bool has_skin_tone = false;
    uint32_t head_hidden = 0; // biped slots whose partitions are off on the head and hair
    bool hair_worn = false;   // the hair slot is worn: hair without partitions goes altogether
    std::vector<ActorPart> parts;
    ActorPart stump;              // what its race wears once decapitated: no mesh, it can't be
    uint32_t decapitate_art = 0;  // the race's NAM7
    bool headless = false;        // it already was
};

// Something an actor's graph raised, by its project name (Graph's event drain), for the game's systems.
struct ActorEvent {
    uint32_t ref = 0;
    std::string tag;
    std::string payload;
    glm::vec3 position{0.0f}; // the reference's, metres
    bool player = false;
    bool first_person = false; // from the player's first-person graph
    float run_blend = 0.0f;    // the walk/run blend of the player's speed
    std::vector<uint32_t> footstep_sets; // the worn addons' distinct FSTS forms
};

struct PlacedActor {
    uint32_t ref = 0;
    glm::vec3 position{0.0f}; // metres
    float heading = 0.0f;     // radians, the game's z angle
    float scale = 1.0f;       // XSCL
    uint32_t base = 0;
    bool first_person = false; // the player's first-person body
    bool dead = false;         // Starts Dead
    // An animated object: a placed reference with a behaviour graph.
    bool object = false;
    bool starts_disabled = false;
    std::string mesh;          // its .osm, as the world names it
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// A corpse's pose in a plugin (XRGD, XRGB): local transforms of the skeleton's nodes with bodies, in walk order.
struct RagdollStart {
    struct Bone {
        uint32_t part = 0;
        glm::vec3 position{0.0f}; // units
        glm::vec3 rotation{0.0f}; // radians
    };
    std::vector<Bone> bones;
    glm::vec3 root_rotation{0.0f}; // radians, on the first child of the root
};

// A reference that goes off when its activate parent does (XAPR).
struct ActivateChild {
    uint32_t ref = 0;
    float delay = 0.0f; // seconds
};
using ActivateChildren = std::unordered_map<uint32_t, std::vector<ActivateChild>>;

// A placed actor and the space it starts in: its WRLD, or an interior's CELL.
struct WorldActor {
    PlacedActor placed;
    uint32_t space = 0;
};

// An idle marker or furniture: marker-only ones draw nothing, so they aren't among the world's references.
struct PlacedMarker {
    uint32_t ref = 0, base = 0;
    glm::vec3 position{}; // Skyrim units
    float heading = 0;
    float scale = 1;
};

struct Biped;

class ActorData {
public:
    // Opens <cache>/forms.osf and indexes every placed actor on a worker. Throws when there is no database.
    explicit ActorData(const std::filesystem::path& cache_root);
    ~ActorData();

    // The actors placed in a space that the game would have enabled at the start of a game. Any thread.
    std::vector<PlacedActor> placed_in(const osw::World& world);
    RagdollStart ragdoll_start(uint32_t ref) const;
    ActorLook object_look(const std::string& mesh, uint32_t base);
    bool object_model(const std::string& mesh) const; // does it import as an animated object?
    void set_texture_swaps(const std::unordered_map<uint32_t, ots::Swap>* swaps,
                           const std::unordered_map<uint64_t, ots::Swap>* addons) {
        m_texture_swaps = swaps;
        m_addon_swaps = addons;
    }
    ActivateChildren activate_children(const osw::World& world) const; // by parent
    const std::filesystem::path& root() const { return m_root; }
    // The player's own face, over the NPC record's, as character creation edits it.
    void set_player_face(const game::FaceData& face);
    // What the player has equipped, over the record's outfit; `right` the weapon in the right hand. Any thread.
    void set_player_worn(std::vector<uint32_t> items, uint32_t right, uint32_t left);
    void set_player_beast(uint32_t race); // SetRace to a form the player takes: 0 back to their own
    // Every actor of the game that would be enabled at its start. Any thread.
    std::vector<WorldActor> placed_everywhere();
    const std::vector<PlacedMarker>& usable_markers(uint32_t space);
    float north_rotation(uint32_t cell) const; // an interior's, radians; 0 without a NorthMarker or before the index is up
    // The look of a base NPC_, for the placed reference `ref` (leveled picks are seeded by it). Any thread.
    ActorLook look(uint32_t npc, uint32_t ref, bool first_person = false);
    // What a leveled pick is worked out at. Any thread.
    void set_player_level(int32_t level) { m_player_level = level; }
    void set_zone_level(uint32_t zone, int32_t level); // the level a zone latched
    // Actor::Reset puts the original base back and resolves again: a reference's next look is a new pick.
    void reroll(uint32_t ref);
    // A reference created at run time: the zone and modifier its picks are worked out at (-1: none), and what its base
    // comes to, an NPC_ whatever the list.
    void set_created(uint32_t ref, uint32_t zone, int32_t modifier);
    std::vector<std::array<uint32_t, 3>> created() const; // ref, zone, modifier: for a save
    uint32_t created_npc(uint32_t base, uint32_t ref) const;
    void set_outfit(uint32_t npc, uint32_t outfit);
    void npc_equip(uint32_t ref, uint32_t form, bool on);
    void npc_bare(uint32_t ref);
    void npc_headless(uint32_t ref, bool headless);
    float dismembered_limb_velocity() const { return m_limb_velocity; }
    void set_dismembered_limb_velocity(float v) { m_limb_velocity = v; } // fCombatDismemberedLimbVelocity
    bool npc_is_headless(uint32_t ref) const;
    std::unordered_map<uint32_t, NpcGear> npc_gear() const;
    void set_npc_gear(std::unordered_map<uint32_t, NpcGear> gear);
    std::unordered_map<uint32_t, uint32_t> rerolls() const;
    void set_rerolls(std::unordered_map<uint32_t, uint32_t> rerolls);

    // An IDLE record's animation event (ENAM), "" for none. Any thread.
    std::string idle_event(std::string_view editor_id) const;
    // The MATT of a land texture (LTEX MNAM), 0 for none. Any thread.
    uint32_t land_material(uint32_t ltex) const;

    // Shapes to pose in the coming frame. Main thread; the renderer reads them until clear_skin_jobs.
    std::vector<render::SkinJob>& skin_jobs() { return m_skin_jobs; }
    std::vector<glm::vec4>& skin_bones() { return m_skin_bones; }
    std::vector<glm::vec4>& skin_bones_before() { return m_skin_bones_before; }
    std::vector<render::SkinFaceMorph>& skin_face() { return m_skin_face; }
    render::SkinFrame skin_frame(bool tick, float blend) {
        m_skin_decals_now.swap(m_skin_decals);
        m_skin_decals.clear();
        return {m_skin_jobs, m_skin_bones, m_skin_face, m_skin_bones_before, m_skin_decals_now, tick, blend};
    }
    uint32_t allocate_skin_decal() {
        if (m_next_skin_decal < render::k_skin_decal_slots) {
            return m_next_skin_decal++;
        }
        if (m_free_skin_decals.empty()) {
            return ~0u;
        }
        const uint32_t slot = m_free_skin_decals.back();
        m_free_skin_decals.pop_back();
        return slot;
    }
    void free_skin_decal(uint32_t slot) { m_free_skin_decals.push_back(slot); }
    std::vector<render::SkinDecalBuild>& skin_decal_builds() { return m_skin_decals; }
    void clear_skin_jobs() {
        m_skin_bones_before.clear();
        m_skin_jobs.clear();
        m_skin_bones.clear();
        m_skin_face.clear();
    }

private:
    std::shared_ptr<const ActorSkeleton> skeleton(const std::string& path);
    std::shared_ptr<const anim::Project> project(const std::string& path);
    void index();
    bool enabled_at_start(const forms::Form& ref, int depth = 0) const;
    int32_t spawn_level(uint32_t ref, int32_t& modifier) const;
    struct Created {
        uint32_t zone = 0;
        int32_t modifier = -1;
    };
    std::unordered_map<uint32_t, Created> m_created;
    std::vector<uint32_t> leveled_pick(const forms::Form& list, int32_t level, int32_t modifier, uint32_t seed) const;
    uint32_t pick_npc(uint32_t list, int32_t level, int32_t modifier, uint32_t seed, int depth) const;
    uint32_t template_root(uint32_t npc, uint16_t flag, int32_t level, int32_t modifier, uint32_t seed) const;
    void add_armor(const forms::Form& armor, ActorLook& look, const forms::Form& race, struct Biped& biped,
                   float weight) const;
    std::vector<uint32_t> head_parts(const forms::Form& traits, const forms::Form& race, bool is_female,
                                     const std::vector<uint32_t>* own = nullptr) const;
    FacePart face_part(const forms::Form& part, std::string shape) const;
    void bake_morphs(const game::FaceData& face, const forms::Form& race, ActorLook& out) const;
    void add_head_parts(const forms::Form& traits, const forms::Form& race, ActorLook& out) const;
    void add_equipped(const std::vector<uint32_t>& items, uint32_t right, uint32_t left, ActorLook& out) const;
    void add_gear_weapons(const NpcGear& gear, ActorLook& out) const;
    void add_stump(const forms::Form& race, float weight, ActorLook& out) const;
    void add_weapons(const forms::Form& inventory, const forms::Form& traits, std::span<const uint32_t> outfit,
                     int32_t level, uint32_t seed, ActorLook& out) const;
    void wear(uint32_t item, int32_t level, uint32_t seed, ActorLook& look, const forms::Form& race, Biped& biped,
              float weight, uint32_t& worn_mask, int depth, std::span<const uint32_t> off = {}) const;
    std::string skinned_path(std::string_view model) const;
public:
    // An ANIO by EditorID, any case: what an AnimObjLoad event names.
    struct AnimObject {
        std::string mesh, unload; // the cache path; the graph event that puts it away, lower case
    };
    const AnimObject* anim_object(const std::string& editor_id);
private:

    std::filesystem::path m_root;
    const std::unordered_map<uint32_t, ots::Swap>* m_texture_swaps = nullptr;
    const std::unordered_map<uint64_t, ots::Swap>* m_addon_swaps = nullptr; // by addon << 3 | variant
    mutable std::mutex m_face_mutex;
    std::optional<game::FaceData> m_player_face;
    std::optional<std::vector<uint32_t>> m_player_worn;
    uint32_t m_player_beast = 0; // the race of the form the player has taken, 0 for their own
    uint32_t m_player_right = 0, m_player_left = 0;
    std::unique_ptr<forms::Database> m_db;
    std::shared_future<void> m_indexed;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_by_cell;  // interior cell -> ACHRs
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_by_world; // WRLD -> ACHRs
    std::unordered_map<uint32_t, std::vector<PlacedMarker>> m_markers; // WRLD or interior CELL
    std::unordered_map<uint32_t, float> m_north;                       // interior CELL
    std::unordered_map<std::string, uint32_t> m_idles;              // IDLE by lowercase EditorID
    std::unordered_map<std::string, AnimObject> m_anim_objects;     // ANIO by lowercase EditorID
    // Game settings the level of a leveled actor depends on (GMST), read when the index is built.
    float m_level_scaling = 1.0f;
    float m_level_mults[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // by XLCM: easy, medium, hard, very hard, none
    int32_t m_level_adjust_up = 0, m_level_adjust_down = 0;
    std::atomic<int32_t> m_player_level{1};
    mutable std::mutex m_level_mutex;
    std::unordered_map<uint32_t, int32_t> m_zone_levels; // zones latched so far
    std::unordered_map<uint32_t, uint32_t> m_rerolls;    // reference -> times its pick was made again
    std::unordered_map<uint32_t, uint32_t> m_outfits;    // NPC_ -> what scripts dressed it in, over DOFT
    std::unordered_map<uint32_t, NpcGear> m_gear;        // reference -> what scripts changed on it
    float m_limb_velocity = 10.0f;
    std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<const ActorSkeleton>> m_skeletons;
    std::unordered_map<std::string, std::shared_future<std::shared_ptr<const anim::Project>>> m_projects;
    std::vector<render::SkinDecalBuild> m_skin_decals, m_skin_decals_now;
    std::vector<uint32_t> m_free_skin_decals;
    uint32_t m_next_skin_decal = 0;
    std::vector<render::SkinJob> m_skin_jobs;
    std::vector<glm::vec4> m_skin_bones;
    std::vector<glm::vec4> m_skin_bones_before;
    std::vector<render::SkinFaceMorph> m_skin_face;
};

// What Space draws of one actor.
struct ActorDraw {
    render::MeshId mesh;
    uint32_t variant;
};
// A skinned decal on an actor.
struct ActorSkinDecal {
    uint32_t slot = 0, texture = 0;
    float age = 0.0f, lifetime = 30.0f, fade = 1.0f;
};
// What a hit asks of one: world space, metres.
struct SkinDecalLook {
    glm::vec3 position{0.0f}, axis_x{0.0f}, axis_y{0.0f}, direction{0.0f};
    float size = 0.0f;
    uint32_t texture = 0;
    uint32_t limit = 25;    // uMaxSkinDecalPerActor
    float lifetime = 30.0f; // fDecalLifetime
};
// One group of a part again, with a membrane's material (DrawList::add_over).
struct ActorMembrane {
    render::MeshId mesh;
    uint32_t variant, group, material, state;
};

// An effect shader over an actor's meshes, what stays through its run. Its values change every
// frame, through a material block.
struct MembraneLook {
    uint32_t key = 0;          // the effect's: another key builds it again
    std::string fill, palette; // cache paths
    uint32_t technique = 0;    // the pass's bits, less what comes of the mesh
    uint32_t blend_src = 6, blend_dst = 0;
    bool ignore_base_alpha = false, no_weapons = false, skin_only = false;
    bool weapon = false; // only what the hands hold, beside anything over the body
};

// The game's settings, the game's.
struct WeaponBloodLook {
    float amount = 0.0f;     // added to the level
    float lifetime = 10.0f;  // fWeaponBloodLifetime
    float fade = 2.0f;       // fWeaponBloodFadeDuration
    float falloff = 0.3f;    // fWeaponBloodFalloffThreshold
    float rgb_scale = 0.85f; // fWeaponBloodAlphaToRGBScale
};

// The channels of a head, as the game has them: phonemes, modifiers, expressions.
inline constexpr int k_face_phonemes = 16;
inline constexpr int k_face_modifiers = 17;
inline constexpr int k_face_expressions = 17;
inline constexpr int k_face_channels = k_face_phonemes + k_face_modifiers + k_face_expressions;

class ActorSet {
public:
    ActorSet(render::Renderer& renderer, ActorData& data, std::shared_ptr<const osw::World> world);
    ~ActorSet();
    ActorSet(const ActorSet&) = delete;
    ActorSet& operator=(const ActorSet&) = delete;

    // Written into the space's instance table; the slot is the space's to free.
    struct Visual {
        uint32_t ref = 0;
        glm::mat3 rotation{1.0f};
        glm::vec3 position{0.0f};
        float scale = 1.0f;
        glm::vec4 sphere{0.0f};
        std::vector<ActorDraw> draws;
        std::vector<ActorMembrane> membranes;
        std::vector<ActorSkinDecal> skin_decals;
        uint32_t instance = ~0u;
        bool moved = true; // the instance needs writing
        // Where the last tick found it: frames between ticks show it on its way from there.
        glm::vec3 before_position{0.0f};
        glm::mat3 before_rotation{1.0f};
        uint64_t moved_tick = 0;  // the tick that moved it last
        mutable bool seen = false; // drawn since the last tick, in a view or a shadow
        bool hidden = false;
        bool casts_shadow = true; // the first-person body doesn't
    };

    // The loaded area, uGridsToLoad:General / 2 cells each way from the camera's cell.
    void set_grid_radius(int32_t cells) { m_grid_radius = cells; }
    // A frame between world ticks: nothing starts, ends or steps.
    void set_still(bool still) { m_still = still; }
    // A space behind a door on a frame between ticks: actors still arrive and leave, but nothing steps or poses, as nothing
    // a pose makes reaches the GPU until the next tick.
    void set_between_ticks(bool between) { m_between_ticks = between; }
    // Graphs run between ticks on the workers (play's clock), or within the tick. Anything that touches an actor from
    // outside the tick joins them first: see join().
    void set_ahead(bool ahead) { m_ahead = ahead; }
    void join() const; // the graphs in flight are done
    uint64_t tick() const { return m_tick; }
    // Main thread, each frame: starts and ends actors around `camera`, finishes what has loaded, and runs their graphs
    // by the seconds given to set_time.
    void update(const glm::vec3& camera);
    // An actor moved by the AI, and the actors the AI has in this space: those no longer in it end.
    void move(uint32_t ref, const glm::vec3& feet, float heading, float speed = 0.0f, float pitch = 0.0f);
    // The root motion the actor's clips have made since last asked, units in its own space (x right, y ahead).
    glm::vec3 take_motion(uint32_t ref);
    // A mount, moved by the player's character with all its channels.
    struct PlayerBody;
    void drive(uint32_t ref, const PlayerBody& body);
    // A skeleton node from the actor's feet, metres, in its model's space as last posed.
    glm::vec3 node_offset(uint32_t ref, std::string_view lower_name) const;
    // The same in the world, as last posed, "" the root: false when the actor or the node isn't there.
    bool node_world(uint32_t ref, std::string_view lower_name, glm::mat3& rotation, glm::vec3& position) const;
    // Degrees the player's animation adds to the field of view: the Camera Control node's local z in the body that
    // shows.
    float camera_fov_offset() const;
    // The player's camera node in the body that shows: from the feet, metres, in the world's axes, and turned from
    // the body's own frame. False while no body has a pose.
    bool camera_node(std::string_view lower_name, glm::vec3& offset, glm::mat3& rotation) const;
    // A node's local pose in the third-person body, game units: what a camera delta reads. False: not there yet.
    bool camera_delta(std::string_view node, glm::vec3& translation, glm::quat& rotation) const;
    // Has a body to draw effects on (either of the player's).
    bool has_3d(uint32_t ref) const;
    // The membrane over the actor (both of the player's bodies) this frame; false: nothing to put it on yet. The body and
    // what the hands hold have one each: a mesh's property holds one.
    // the player's first-person weapon gets the same.
    void add_weapon_blood(uint32_t ref, const WeaponBloodLook& look);
    // False: none, at the actor's limit or with no slot free.
    bool add_skin_decal(uint32_t ref, const SkinDecalLook& look);
    bool set_membrane(uint32_t ref, const MembraneLook& look, const render::GpuMaterialAnim& values);
    void clear_membrane(uint32_t ref, uint32_t key);
    void set_placed(std::vector<PlacedActor> placed);
    // A reference a script made whose model is an animated object, as an ash pile's is. False for any other model.
    bool add_object(uint32_t ref, uint32_t base, const std::string& mesh, const glm::vec3& position);
    void remove_object(uint32_t ref);
    void set_time(float dt) { m_dt = dt; }
    // The kill camera's rates on the graphs' time, on top of the world's.
    // First person in the saddle looks from the rider's head.
    void set_player_head_hidden(bool hidden) { m_player_head_hidden = hidden; }
    void set_graph_rates(float player, float others) {
        m_player_graph_rate = player;
        m_other_graph_rate = others;
    }
    // Game state held still for comparing frames: every actor takes one fixed step of its graph, then stands.
    void set_hold(bool hold) { m_hold = hold; }
    // Where foot IK casts its rays; null: no foot IK.
    void set_physics(physics::World* world);
    // The player's body at the feet, turned to `heading`.
    struct PlayerBody {
        bool third_person = false; // which body shows; neither when the player isn't walking
        bool first_person = false;
        bool dead = false; // the third-person body dies as soon as it's there
        glm::vec3 feet{0.0f};
        float heading = 0.0f;
        float pitch = 0.0f;     // degrees, the Pitch channel
        float speed = 0.0f;     // units per second
        float direction = 0.0f; // 0 ahead, 0.25 right, 0.5 behind
        float run_blend = 0.0f; // of the walk and run speeds, for footsteps
        float turn_delta = 0.0f; // degrees, the TurnDelta channel
        bool sneaking = false;
        bool sprinting = false;
    };
    void set_player(const PlayerBody& body);
    std::vector<Visual*>& visuals() { return m_visuals; }
    // The first-person body hangs off the camera: the view's feet and heading this frame, not the last tick's.
    void hold_player(const glm::vec3& eye_feet, const glm::vec3& feet, float heading, bool third);
    bool held_player(const Visual& v, glm::vec3& position, glm::mat3& rotation) const;
    size_t actors() const { return m_live.size(); }
    // Sends an animation event to an actor's graph now, as Notify (the player's body is k_player_body); false when
    // nothing took it. For combat, AI and scripts.
    bool send_event(uint32_t ref, std::string_view event);
    // Sent after each tick until the graph takes it. The game's handlers run between a graph's ticks, and a cast depends on
    // that: its action has to land before the state BeginCast entered runs again, or CastStop ends it.
    void queue_event(uint32_t ref, std::string event);
    bool dying(uint32_t ref); // the death clip still leads its ragdoll
    void new_player_body(); // a load after a death: the corpse goes, and the next set_player starts it
    bool death_refused(uint32_t ref); // its graph has no DeathAnim: the race's own death idle goes through Action Death Wait
    void death_taken(uint32_t ref);
    void clear_events(uint32_t ref);
    // Plays an IDLE record: its animation event.
    bool play_idle(uint32_t ref, std::string_view idle_editor_id);
    bool set_variable(uint32_t ref, std::string_view name, float value); // by the variable's type
    // What the actor's head and eyes follow, metres; null: nothing. Any time: a graph ahead on a worker takes it next tick.
    void set_look_target(uint32_t ref, const glm::vec3* target);
    // PlayGamebryoAnimation on an object the sequences drive. False for anything else.
    bool play_sequence(uint32_t ref, const std::string& name, bool start_over, float ease_in);
    float foot_drop(uint32_t ref) const; // metres; for the third-person camera, which goes down with the body
    std::optional<glm::vec3> head_position(uint32_t ref); // metres
    // The actor says a voice line: its lip track, by cache path without extension, starts now.
    // `emotion` and `strength` are its response's TRDT.
    void speak(uint32_t ref, const std::string& line, int emotion, int strength);
    void stop_speaking(uint32_t ref);
    // Animated objects: which references they stand for, for what draws and collides the placed reference.
    struct ObjectChange {
        uint32_t ref = 0;
        bool live = false;   // it took over from the placed reference; false: it gave it back
        std::string collision; // the .osp of its bodies
        float scale = 1.0f;
    };
    std::vector<ObjectChange> take_object_changes();
    struct ObjectMove {
        uint32_t ref = 0;
        std::vector<physics::NodePose> nodes;
    };
    std::vector<ObjectMove> take_object_moves();
    // What goes off with a reference; nothing until the space's references are read.
    std::span<const ActivateChild> activate_children(uint32_t parent);
    // Whether a reference is disabled now: an object that is doesn't draw.
    void set_disabled_test(std::function<bool(uint32_t)> test) { m_disabled = std::move(test); }
    // fEye{Pitch,Heading}{Min,Max}OffsetEmotion{Angry,Fear,Happy,Sad,Neutral}: pitch min, max, heading min, max, by emotion.
    void set_eye_wander(const std::array<std::array<float, 4>, 5>& ranges) { m_eye_wander = ranges; }
    // SetExpressionOverride, ClearExpressionOverride: an expression by index, locked until cleared.
    void set_expression_override(uint32_t ref, int index, float strength);
    void clear_expression_override(uint32_t ref);
    // The player's body comes again from ActorData, with what has changed.
    void rebuild_player();
    bool player_ready() const; // the body showing is the one last asked for
    bool get_bool(uint32_t ref, std::string_view name) const;
    bool get_int(uint32_t ref, std::string_view name, int32_t& out) const;
    bool get_float(uint32_t ref, std::string_view name, float& out) const;
    std::vector<std::string> graph_nodes(uint32_t ref) const; // `actor ... graph`
    // What an actor's look put on it, for combat.
    void gear(uint32_t ref, std::vector<uint32_t>& armor, uint32_t& right) const;
    std::string behaviour_folder(uint32_t ref) const;
    // The actor dies: its death animation, then its ragdoll. False when it has none.
    bool kill(uint32_t ref);
    // A synchronised pair: one scene, each side's event. False, and nothing started, unless both graphs take it.
    bool start_pair(uint32_t lead, uint32_t follower, std::string_view lead_event, std::string_view follower_event);
    // In one, and where the marks have put it.
    bool paired(uint32_t ref, glm::vec3& feet, float& heading) const;
    bool feet(uint32_t ref, glm::vec3& feet, float& heading) const; // where it shows; false for none
    bool node_world(uint32_t ref, std::string_view lower_name, glm::vec3& at) const; // metres
    // That view's body has its graph up.
    bool player_body_ready(bool third_person) const;
    void drop_body(uint32_t ref); // SetCriticalStage's GooEnd and DisintegrateEnd: nothing shows from now on
    bool dead(uint32_t ref) const; // dying, or a corpse; an actor not loaded counts by how it was placed
    uint32_t ragdoll_of(uint32_t ref) const; // its physics ragdoll, 0 for none
    // What a corpse has on it: the NPC_ its inventory comes from, what it wears and holds.
    bool worn_items(uint32_t ref, uint32_t& inventory_npc, std::vector<uint32_t>& items) const;
    bool weapon_drawn(uint32_t ref) const;
    // The lines of the skeleton an effect shader's particles are born on, world space, game units: each node with a
    // ragdoll body, from its parent.
    void bone_segments(uint32_t ref, std::vector<BoneSegment>& out) const;
    // A held weapon's line, as `` measures it: from its node to the bound's centre and on by the radius.
    bool weapon_ray(uint32_t ref, std::string_view node, BoneSegment& out) const;
    std::vector<uint32_t> weapons(uint32_t ref) const; // a fight's choice
    void swap_weapon(uint32_t ref, uint32_t form);
    // Starts again from its record next frame, alive. False: it was placed dead.
    bool resurrect(uint32_t ref);
    void restart(uint32_t ref) { end(ref); } // what it wears changed
    // The Decapitate tag: the head leaves the body, thrown or (a look made again) dropped. False: it can't, or already has.
    bool decapitate(uint32_t ref, bool thrown);
    uint32_t decapitate_art(uint32_t ref) const; // what it bleeds with
    // A hit on a corpse: its bodies' velocities gain `strength` m/s away from `centre`.
    void push(uint32_t ref, const glm::vec3& centre, float strength);
    static constexpr uint32_t k_player_body = 0x14; // whichever body shows
    // What the graphs raised since the last clear_events.
    const std::vector<ActorEvent>& events() const { return m_finished_events; }
    void clear_events() { m_finished_events.clear(); }
    float animate_ms() const { return m_animate_ms; } // the graphs' time in the last frame that ran them
    // The actors that exist right now, for `state`: reference, base NPC, position (metres).
    struct Entry {
        uint32_t ref, npc;
        glm::vec3 position;
        std::vector<std::string> clips; // the clip generators playing
        std::vector<std::string> parts; // its models and the biped slots that hold them
    };
    std::vector<Entry> list() const;
    // The living NPCs and creatures here that have a body, with where they stand.
    struct Solid {
        uint32_t ref = 0, npc = 0, race = 0;
        bool female = false;
        glm::vec3 feet{0.0f};
        float heading = 0.0f;
        float scale = 1.0f; // The game's
    };
    void solids(std::vector<Solid>& out) const;
    // Who is here, by reference, without the strings: what runs every tick asks this.
    struct Who {
        uint32_t ref, npc;
        glm::vec3 position;
    };
    const std::vector<Who>& who();
    bool busy() const; // actors near the camera still loading

private:
    // A head shape's morphs: which the face's channels are, and what stays on it.
    struct FaceShape {
        const render::FaceMorphs* tri = nullptr;
        std::array<int16_t, k_face_channels> channels; // the morph of each, -1 for none
        std::vector<render::SkinFaceMorph> fixed;
    };
    // A model's bone and the skeleton's node it follows: `node` * `skin`, or the node alone, or `skin` as it stands.
    struct RowBone {
        uint32_t part, bind; // indices into Live::parts and its binds
        bool operator==(const RowBone&) const = default;
    };
    struct BoneBind {
        int32_t node = -1;
        int32_t drawn_node = -1; // a rigid model's place once the weapon is out
        glm::mat4 skin{1.0f};
    };
    struct Part {
        std::vector<BoneBind> binds; // parallel to the model's bones, from the first pose on
        ActorPart source;
        std::vector<std::array<uint32_t, 3>> face_morphs; // parallel to source.face: tri, race, chargen
        std::vector<std::unique_ptr<FaceShape>> face;     // by shape, null for none
        render::MeshId mesh = render::k_invalid_mesh;
        uint32_t variant = render::k_no_variant;
        std::vector<uint64_t> targets;
        std::vector<render::TextureId> textures; // in place of the model's own
        // An animation object: joins the parts when the graph first asks for it, and stays, folded away, once put down.
        std::string prop, prop_unload; // lower case: the ANIO's EditorID, and its unload event
        bool prop_shown = false;
    };
    // What the head shows this frame, by channel, and the line it's saying.
    struct Face {
        std::array<float, k_face_channels> weights{};
        std::shared_ptr<const osv::LipTrack> lip;
        std::future<std::shared_ptr<const osv::LipTrack>> lip_loading;
        float lip_time = 0.0f;
        int64_t lip_row = -1;
        int emotion = 0;  // the line's, TRDT
        int strength = 0;
        // UpdateBlink: 0 waiting, 1 closing, 2 opening, 3 parked.
        int blink_state = 0;
        float eye_heading = 0.0f, eye_pitch = 0.0f; // UpdateEyeTracking's current angles, radians
        float eye_wander = 0.0f, eye_off_heading = 0.0f, eye_off_pitch = 0.0f; // UpdateEyeOffsets: until the next, and its angles
        bool morphs = true; // among those morphed this frame; the others hold what they showed
        std::array<float, k_face_channels> shown{};
        bool eyes_tracking = false;
        float blink_timer = -1.0f;
        uint32_t random = 0;
        bool eyes_closed = false;
        bool eyes_shut = false; // done
        bool override_lock = false;
        int target = -2;           // the expression moving to, -1 none, -2 not decided
        float target_strength = 0.0f;
    };
    // A running graph and the pose it gives the skeleton's nodes.
    struct Animation {
        ~Animation() {
            if (character.sync_scene) {
                character.sync_scene->abandon(&character);
            }
        }
        anim::Character character;
        std::unique_ptr<anim::Graph> graph;
        anim::Output output;
        std::vector<int32_t> bones;    // per skeleton node: the graph's bone, -1 for none
        std::vector<glm::mat4> nodes;  // node -> model space
        std::atomic<bool> looking{false}; // set_look_target, in Skyrim units
        std::atomic<float> look_x{0.0f}, look_y{0.0f}, look_z{0.0f};
        bool hands_out = false;        // what the graph was last told of the weapon
        bool head_tracking = false;    // what the graph was last told
        // In a paired animation: both have left the scene once this is set.
        std::shared_ptr<std::atomic<bool>> pair_done;
        // The body below the controller: foot IK's feedback, summed.
        std::atomic<float> foot_drop{0.0f}; // metres
        float speed = 0.0f;            // channels
        float direction = 0.0f;
        float pitch = 0.0f;
        float run_blend = 0.0f;
        float turn_delta = 0.0f;
        bool sneaking = false;
        bool sprinting = false;
        bool forward = true;  // the Direction channel's flips
        bool right = false;
        bool turning = false;
        bool weapon_drawn = false; // weaponDraw to weaponSheathe: the weapon is in the hand
        std::deque<std::string> queued; // events that wait for a state: sent right after the graph's tick, until taken
        int queued_tries = 0;
        bool moving = false;
        std::vector<glm::mat4> shown_nodes; // an object's, as the physics world last heard them
        // Dying: the graph animates the ragdoll's keyframed bodies until one touches something or the clip ends.
        bool dying = false;
        bool no_body = false; // dead, with no ragdoll to fall as
        bool ragdoll_event = false;
        bool laid_out = false; // the plugin's pose is in `keyframed`
        // A corpse: the bodies pose it, in the physics world when there is one.
        bool corpse = false;
        uint32_t ragdoll = 0;
        std::vector<physics::RagdollBodyState> bodies; // world space
        std::vector<anim::Qs> keyframed, last_keyframed; // model space, while dying
        bool collecting = false; // someone wants the root motion
        glm::vec3 motion{0.0f};
    };
    struct Membrane {
        uint32_t key = 0;
        uint32_t block = 0;
        std::vector<render::TextureId> textures;
        std::vector<ActorMembrane> draws;
    };
    // An object that loops a sequence of its model with no graph: the bones come from the sequence.
    struct ObjectLoop {
        std::unique_ptr<fx::EffectInstance> effect;
        std::vector<uint32_t> node_of; // by skeleton node: the effect's, ose::k_none for none
        std::vector<glm::mat4> nodes;
        float time = 0.0f;
        bool played = false; // a script has run a sequence: its nodes take the collision along
    };
    // What the right hand's weapon shows. A left-hand weapon isn't drawn.
    struct WeaponBlood {
        WeaponBloodLook look;
        float level = 0.0f, age = 0.0f, lifetime = 0.0f;
        int threshold = -1; // what `draws` were made with
        std::vector<ActorMembrane> draws;
    };
    // A head off its body: its parts keep the pose of the cut, and go where one free body goes.
    struct Severed {
        uint32_t ragdoll = 0;
        std::vector<glm::mat4> nodes; // the actor's, at the cut
        glm::mat4 from{1.0f};         // the body at the cut, undone, by the actor's place then
        glm::mat4 now{1.0f};          // those nodes to where the head is, in the actor's space
    };
    struct Live {
        std::optional<Severed> severed;
        std::vector<glm::vec4> last_rows; // the bones of its last pose
        std::vector<RowBone> row_plan;    // the bones a pose writes, in order
        std::vector<glm::vec4> ready_rows; // the next pose's, from the graph's worker; empty: none yet
        bool ready_drawn = false;          // with the weapon out, when it worked them out
        bool snap = false;                // no way between the last pose and the next: a jump, a ragdoll starting
        bool posed = false;               // the GPU has a pose of it
        PlacedActor placed;
        std::future<ActorLook> building;
        ActorLook look;
        bool built = false;
        bool finished = false;
        bool held = false; // took its one step under set_hold
        std::vector<Part> parts;
        Visual visual;
        std::unique_ptr<Animation> animation;
        std::unique_ptr<ObjectLoop> loop;
        std::array<Membrane, 2> membranes; // the body, the weapon
        WeaponBlood blood;
        Face face;
    };

    void build_membrane(Live& actor, Membrane& membrane, const MembraneLook& look);
    void drop_membrane(Live& actor, Membrane& membrane);
    void update_weapon_blood(Live& actor);
    void update_skin_decals(Live& actor);
    void drop_weapon_blood(Live& actor);
    void start(const PlacedActor& placed);
    void place_player(uint32_t ref, bool visible, bool first_person, const PlayerBody& body);
    void finish(Live& actor);
    void finish_parts(Live& actor, size_t first, size_t last, glm::vec3& lo, glm::vec3& hi);
    void update_props(); // what the graphs asked of animation objects since the last tick
    void pose(Live& actor);
    void pose_rows(const Live& actor, const std::vector<glm::mat4>& nodes, std::vector<glm::vec4>& rows) const;
    void animate_graph(Live& a);
    void animate_finish(Live& a);
    void build_nodes(Live& a);
    void index_placed();
    std::vector<physics::RagdollBodyState> m_head_state;
    bool sever(Live& a, bool thrown);
    void follow_head(Live& a);
    static void bind_bones(Part& part, const render::SkinnedModel& model, const ActorSkeleton* skeleton);
    void start_animation(Live& actor);
    void run_loop(Live& actor);
    void animate(Live& actor);
    void animate_face(Live& actor);
    void track_eyes(Live& a);
    void wander_eyes(Face& face);
    void gate_faces();
    void update_blink(Face& face);
    void update_expression(Live& actor);
    void bind_face(Live& actor, Part& part, const render::SkinnedModel& model);
    void run_graph(Live& actor);
    void keyframe_ragdoll(Live& actor);
    bool lay_out(Live& actor);
    void start_ragdoll(Live& actor);
    void follow_ragdoll(Live& actor);
    physics::RagdollSpec ragdoll_spec(const Live& actor) const;
    Live* live_actor(uint32_t ref);
    void end(uint32_t ref);
    void reposition(Live& actor, const PlayerBody& body);
    bool hidden_now(const Live& actor) const;
    void rebuild_visuals();
    glm::vec3 m_first_person_feet{0.0f}, m_third_person_feet{0.0f};
    glm::mat3 m_player_rotation{1.0f};
    bool m_third_person_held = false;

    render::Renderer& m_renderer;
    ActorData& m_data;
    std::shared_ptr<const osw::World> m_world;
    std::future<std::vector<PlacedActor>> m_placing;
    std::vector<PlacedActor> m_placed;
    std::vector<PlacedActor> m_placed_dead; // the AI's lists don't hold them
    std::unordered_set<uint32_t> m_no_body; // their 3D has gone, the references stay
    std::array<std::array<float, 4>, 5> m_eye_wander{};
    bool m_placed_ready = false;
    std::unordered_map<uint32_t, std::unique_ptr<Live>> m_live;
    std::unordered_map<uint32_t, std::vector<physics::RagdollBodyState>> m_corpses; // left behind, in world space
    std::vector<Visual*> m_visuals;
    bool m_visuals_dirty = false;
    bool m_starting = false; // more actors to start next frame
    float m_dt = 0.0f;
    float m_player_graph_rate = 1.0f, m_other_graph_rate = 1.0f;
    bool m_player_head_hidden = false;
    physics::World* m_physics = nullptr;
    glm::vec3 m_camera{0.0f};
    uint32_t m_made_tints = 0;
    std::vector<ObjectChange> m_object_changes;
    std::vector<ObjectMove> m_object_moves;
    std::function<bool(uint32_t)> m_disabled;
    std::future<ActivateChildren> m_children_loading;
    ActivateChildren m_children;
    mutable std::vector<ActorEvent> m_events;
    std::vector<ActorEvent> m_finished_events; // up to the last finished tick: what events() reads
    void finish_events();
    struct PropEvent {
        uint32_t ref = 0;
        std::string tag, payload;
    };
    std::vector<PropEvent> m_prop_events;
    std::vector<uint32_t> m_props_loading; // actors with a prop whose model isn't in yet
    float m_animate_ms = 0.0f;
    int32_t m_grid_radius = 2; // the loaded area: uGridsToLoad / 2
    bool m_still = false;
    bool m_between_ticks = false;
    bool m_ahead = false;
    mutable bool m_in_flight = false;
    size_t m_events_from = 0;
    uint64_t m_tick = 0;
    std::vector<Live*> m_animating; // this tick's, reused
    std::vector<RowBone> m_row_plan; // pose's, reused
    std::vector<std::future<ActorLook>> m_abandoned; // looks of actors who left before they were built
    std::vector<Who> m_who;
    std::unordered_map<uint32_t, uint32_t> m_placed_at; // where a reference is in m_placed
    std::mutex m_events_mutex;
    bool m_hold = false;
};

} // namespace spud::engine
