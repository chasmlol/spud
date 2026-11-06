#pragma once

// A character's behaviour graph: the root's loops and what a project shares.

#include "anim/clip.hpp"
#include "anim/node.hpp"
#include "anim/ragdoll.hpp"
#include "formats/osb.hpp"

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::anim {

class FootIkDriver; // modifiers.cpp
class Graph;
struct GlobalTransitions; // state_machine.hpp

enum class VariableType : int8_t { boolean = 0, int8 = 1, int16 = 2, int32 = 3, real = 4, pointer = 5, vector3 = 6, vector4 = 7, quaternion = 8 };

// hkbVariableValueSet: one word per variable, quads beside them.
struct VariableValues {
    std::vector<int32_t> words; // BOOL and INT*: the value; REAL: the float's bits; quads: an index into `quads`
    std::vector<glm::vec4> quads;
};

// One behaviour file: its name tables and the maps to the project's external ids.
class Behavior {
public:
    std::string path;
    std::vector<std::string> event_names;
    std::vector<std::string> variable_names;
    std::vector<VariableType> variable_types;
    std::vector<uint32_t> event_flags;
    VariableValues initial_values;
    Node* root_generator = nullptr;
    // Internal to external and back; -1 when the other side has no such name.
    std::vector<int32_t> event_to_external;
    std::unordered_map<int32_t, int32_t> event_from_external;
    std::vector<int32_t> variable_to_root; // local index to the root behaviour's index
    std::vector<int32_t> property_to_character; // local character property index to the character's
    int32_t event_internal(int32_t external) const {
        auto it = event_from_external.find(external);
        return it != event_from_external.end() ? it->second : -1;
    }
    int32_t event_id(std::string_view name) const; // internal id by name, -1 for none
};

// What a BSSynchronizedClipGenerator shares with its scene.
struct SyncMarks {
    bool lead = false;    // bLeadCharacter
    bool at_mark = false; // bAtMark
    const void* owner = nullptr; // the Character
    Qs start_ms, start_ws, end_ws;
};

// hkbSynchronizedAnimationScene: shared by the participants of one paired animation, set on Character::sync_scene before
// their generators activate. make_sync_scene gives the game's own (generators.cpp).
class SyncScene {
public:
    virtual ~SyncScene() = default;
    virtual bool all_in_scene() const = 0;
    virtual bool all_at_marks() const = 0;
    virtual float time() const = 0;
    virtual void set_time(float time) = 0;
    virtual void join(SyncMarks& marks) = 0;
    virtual void leave(SyncMarks& marks) = 0;
    virtual void left_early(SyncMarks& marks) = 0;
    // A graph that has stopped for good, a dead body's or one being unloaded: it will never deactivate its generator.
    virtual void abandon(const void*) {}
};
// `on_all_left` sends PairEnd (PairedStop if unhandled and no actor is dead) and clears
// the pairing flags; `on_left_early` clears the player's flag.
std::shared_ptr<SyncScene> make_sync_scene(uint32_t participants, std::function<void()> on_all_left,
                                           std::function<void()> on_left_early);

// The ragdoll as hkbPoseMatchingGenerator sees it; bones are the ragdoll skeleton's.
class RagdollPoses {
public:
    virtual ~RagdollPoses() = default;
    virtual uint32_t bone_count() const = 0;
    // A model-space animation pose through animationToRagdollSkeletonMapper: bone positions.
    virtual void map_pose(std::span<const Qs> model_pose, std::vector<glm::vec3>& positions) const = 0;
    // The live ragdoll read with this worldFromModel, in the space map_pose gives.
    virtual void current_pose(const Qs& world_from_model, std::vector<glm::vec3>& positions) const = 0;
    virtual float body_speed(int32_t bone) const = 0; // the rigid body's speed, 0 if there's no body
};

// A text key of a Gamebryo sequence.
struct SequenceKey {
    std::string name;
    float time = 0.0f;
};

// What Bethesda's or Havok's code provides a node with, for one character.
struct Character {
    const class Project* project = nullptr;
    const Skeleton* skeleton = nullptr;
    // Scale and position of the character in the world, as the game's root node (units).
    Qs world_from_model;
    // What foot IK added to track 0 this frame, towards where the legs reach (units, world space); and whether it ran.
    glm::vec3 foot_ik_feedback{0.0f};
    bool foot_ik_on = false;
    // Ray down the world for foot IK: from, to; returns true and the hit point and normal. Null: none.
    std::function<bool(const glm::vec3&, const glm::vec3&, glm::vec3&, glm::vec3&)> cast_ray;
    // Look-at target in world space while one is set.
    bool has_look_target = false;
    glm::vec3 look_target{0.0f};
    // The game's notifications (annotations, raised events it listens to): by name.
    std::function<void(std::string_view event, const std::string* payload)> on_event;
    // The collision filter the next cast_ray stands for: foot IK's collisionFilterInfo, or BSLimbIKModifier's layer 43
    // (the caller adds the actor's group there).
    uint32_t ray_filter_info = 0;
    // whether the world has shapes in this box. Null: no utility, the modifier does nothing.
    std::function<bool(const glm::vec3& lo, const glm::vec3& hi)> shapes_in_box;
    // by userInfo, may change whether there's a target and where it is, in world space. Null:
    // userInfo 1 falls back on look_target.
    std::function<void(uint32_t user_info, bool& has_target, glm::vec3& location)> direct_at_target;
    // false for "don't test"; otherwise the camera's position and the actor's
    // movement direction. Null: no interface.
    std::function<bool(glm::vec3& target, glm::vec3& movement)> pass_by_target;
    // Null: speedOut is the goal speed, the game's own fallback.
    std::function<float(int32_t state, float direction, float goal_speed)> sample_speed;
    // hkbFootIkControlsModifier's output (track 11): hkbFootIkGains in file order, and the track's onFraction. Written while
    // the graph generates, read and cleared by run_foot_ik_driver.
    std::array<float, 12> foot_ik_gains{};
    float foot_ik_fraction = 0.0f;
    std::shared_ptr<FootIkDriver> foot_ik; // made on first use, when the project has driver info
    // Random numbers for nodes that draw them (the game's seeded generator per graph).
    uint32_t random_state = 0x9E3779B9u;
    float random_float(); // 0..1
    // The paired animation this character is in; null plays alone against a default scene.
    std::shared_ptr<SyncScene> sync_scene;
    RagdollPoses* ragdoll = nullptr; // null: none
    // Does any of these ragdoll bones touch something, for BSRagdollContactListenerModifier. Null: nothing touches.
    std::function<bool(std::span<const int32_t> bones)> ragdoll_touching;
    // The object's.kf sequences, by name; null: none. Start activates it at
    // time offset -time, reports whether it exists, whether it loops, and its text keys. Update returns its length.
    std::function<bool(const std::string& sequence, float time, bool& looping, std::vector<SequenceKey>& keys)> sequence_start;
    std::function<float(const std::string& sequence, float time, float weight)> sequence_update;
    std::function<void(const std::string& sequence)> sequence_stop;
};

// A clip's annotation as an event, external id, by "Annotations to triggers". The payload lives as long as the project,
// as an event in the queue only points at it.
struct BindingTrigger {
    float time = 0.0f;
    int32_t event = -1;
    std::shared_ptr<const std::string> payload;
};

// hkaMirroredSkeleton: Q[b] of "Mirroring", M beside them for the parent of a root.
struct MirroredSkeleton {
    glm::quat axis{1.0f, 0.0f, 0.0f, 0.0f};
    std::vector<int16_t> bone_pairs;
    std::vector<glm::quat> rotations;
};

// The shared, immutable side of a project: behaviours and their node templates, clips, the skeleton.
class Project {
public:
    // Loads <cache>/<project>.osb and everything it names. Any thread. Throws on a malformed file.
    static std::shared_ptr<const Project> load(const std::filesystem::path& cache, const std::string& project);
    ~Project();

    const Skeleton& skeleton() const { return *m_skeleton; }
    const Ragdoll* ragdoll() const { return m_ragdoll.get(); } // null: the character has none
    Behavior& root() const { return *m_behaviors.front(); }
    const std::vector<std::unique_ptr<Behavior>>& behaviors() const { return m_behaviors; }
    const std::vector<std::string>& external_events() const { return m_external_events; }
    int32_t external_event(std::string_view name) const;
    int32_t root_variable(std::string_view name) const;
    // Character properties (hkbCharacterData): names, types and values, pointer properties by bone weights.
    int32_t character_property(std::string_view name) const;
    // A clip by its name in the character's animationNames, or by path; null when absent.
    const Clip* clip(std::string_view name) const;
    // A clip generator's root motion by its name, null for none.
    const ClipMotion* motion(std::string_view clip_generator) const;
    const std::string& path() const { return m_path; }
    const VariableValues& character_values() const { return m_character_values; }
    // hkbProjectData.worldUpWS (the physics world's up when there is one), hkbCharacterData's model axes.
    const glm::vec3& world_up() const { return m_world_up; }
    const glm::vec3& model_up() const { return m_model_up; }
    const glm::vec3& model_forward() const { return m_model_forward; }
    const glm::vec3& model_right() const { return m_model_right; }
    const Bindable* foot_ik_info() const { return m_foot_ik_info.get(); } // hkbFootIkDriverInfo, null for none
    const std::vector<VariableType>& character_types() const { return m_character_types; }
    const std::vector<std::vector<float>>& character_bone_weights() const { return m_character_bone_weights; }
    // Filled when the project loads; see bind_annotations and make_mirrored_skeleton below.
    const std::vector<BindingTrigger>& binding_triggers(const Clip& clip) const {
        static const std::vector<BindingTrigger> none;
        auto it = m_binding_triggers.find(&clip);
        return it != m_binding_triggers.end() ? it->second : none;
    }
    const MirroredSkeleton& mirrored_skeleton() const { return m_mirrored_skeleton; }
    // The clip's annotation track names, one per transform track; empty when the clip's file doesn't carry them.
    const std::vector<std::string>& track_names(const Clip& clip) const {
        static const std::vector<std::string> none;
        auto it = m_track_names.find(&clip);
        return it != m_track_names.end() ? it->second : none;
    }

    int8_t default_event_mode = 0; // hkbProjectData.defaultEventMode
    std::shared_ptr<const GlobalTransitions> global_transitions; // built by link_state_machines

private:
    friend class ProjectLoader;
    std::unordered_map<const Clip*, std::vector<BindingTrigger>> m_binding_triggers;
    std::unordered_map<const Clip*, std::vector<std::string>> m_track_names;
    MirroredSkeleton m_mirrored_skeleton;
    std::string m_path;
    std::shared_ptr<const Skeleton> m_skeleton;
    std::shared_ptr<const Ragdoll> m_ragdoll;
    std::vector<std::unique_ptr<Behavior>> m_behaviors; // root first
    std::vector<std::unique_ptr<Node>> m_nodes;         // every template, owned here
    std::vector<std::unique_ptr<Bindable>> m_helpers;
    std::vector<std::string> m_external_events;
    std::unordered_map<std::string, int32_t> m_event_ids;
    std::unordered_map<std::string, int32_t> m_variable_ids;
    std::unordered_map<std::string, int32_t> m_property_ids;
    std::unordered_map<std::string, std::unique_ptr<Clip>> m_clips;
    std::unordered_map<std::string, const Clip*> m_clips_by_file;
    std::unordered_map<std::string, ClipMotion> m_motion;
    VariableValues m_character_values;
    std::vector<VariableType> m_character_types;
    std::vector<std::vector<float>> m_character_bone_weights;
    glm::vec3 m_world_up{0.0f, 0.0f, 1.0f};
    glm::vec3 m_model_up{0.0f, 0.0f, 1.0f};
    glm::vec3 m_model_forward{1.0f, 0.0f, 0.0f};
    glm::vec3 m_model_right{0.0f, -1.0f, 0.0f};
    std::unique_ptr<Bindable> m_foot_ik_info; // made by make_foot_ik_info, loaded like any helper
};

// A binding made concrete for a clone: the address of its member, and the variable.
struct RecordBinding {
    void* address = nullptr;
    int32_t variable = -1;
    uint8_t kind = 0;
    uint8_t type = 0;
    bool output = false;
    bool character_property = false;
    bool enable = false;
};

// hkbNodeInfo.
struct Record {
    SyncInfo sync;
    Node* template_node = nullptr;
    std::unique_ptr<Node> clone;
    Behavior* behavior = nullptr;
    bool forwards_sync = false;
    int32_t sync_target = -1;
    uint32_t parents = 0;
    uint32_t child_count = 0;
    uint32_t first_child = 0;
    int32_t sync_parent = -1;
    bool has_sync_parent = false;
    int32_t event_parent = -1;
    bool ignore_events = false;
    bool generator = false;
    bool frozen = true;
    bool activated = false;
    float timestep = 0.0f;
    std::vector<RecordBinding> bindings;
    bool has_outputs = false;
    bool fresh = false;       // made by this collection
    bool moved = false;       // its clone came from the old list
    Record* moved_to = nullptr; // an old record: where its clone went
};

class Graph {
public:
    Graph(std::shared_ptr<const Project> project, Character& character);
    ~Graph();

    Character& character() { return m_character; }
    const Project& project() const { return *m_project; }

    // A frame: update, then drain events (up to 5 rounds), then generate into `output`.
    void update(float dt);
    void generate(Output& output);

    // The game's entry points. Sends an event now, returning whether anything took it.
    bool notify(std::string_view event);
    bool set_bool(std::string_view name, bool v);
    bool set_int(std::string_view name, int32_t v);
    bool set_float(std::string_view name, float v);
    bool set_vector(std::string_view name, const glm::vec4& v);
    std::vector<std::string> active_nodes() const; // what's running, by name: root last
    bool get_bool(std::string_view name, bool& v) const;
    bool get_int(std::string_view name, int32_t& v) const;
    bool get_float(std::string_view name, float& v) const;

    // The active clips' root motion over the last `dt` seconds ("Clip-data
    // motion"): translation in units, multiplied by `scale`, and Euler angles. False when no clip has any.
    bool root_motion(float dt, float scale, glm::vec3& translation, glm::vec3& angles);

    // For nodes.
    void request_update_active_nodes() { m_update_active_nodes = true; }
    void raise(Context& ctx, const EventProperty& e);
    void raise(Context& ctx, int32_t internal_id, const std::string* payload = nullptr);
    Node* clone_of(const Node* template_node) const;
    Record* record_of(const Node* template_node);
    // Whether a node is active; during a collection, also whether it was when the collection began.
    bool was_active(const Node* template_node) const {
        return m_index.contains(template_node) || m_old_index.contains(template_node);
    }
    // nextStartStateIndexOverride of a state machine that has no clone yet: its next activation takes it from here.
    std::unordered_map<const Node*, int32_t> start_overrides;
    Record* sync_record(Record* r); // follows forwarding
    void forward_sync(Node& node, const Node* child); // updateSync of a pass-through: use the child's record
    void copy_sync(Node& node, const Node* child);    // ... or a copy of it, isMirrored cleared
    // Places an active node on its timeline at `time`, and its sync children on theirs (generators.cpp).
    void join(Context& ctx, const Node* template_node, float time);
    // The game's active clip list, walked down from the root (generators.cpp).
    void active_clips(std::vector<ActiveClip>& out);
    // The modify step: everything that holds child modifiers runs them through this.
    void modify_step(Context& ctx, Modifier* template_modifier, Output& output);
    void modify_sync_step(Context& ctx, Modifier* template_modifier, SyncInfo& sync);
    // Variables, by a behaviour's own index.
    const VariableValues& values() const { return m_values; }
    VariableValues& values() { return m_values; }

private:
    struct Queued {
        int32_t id;
        const std::string* payload;
    };

    void activate();
    void update_active_nodes();
    void collect(Node* node, Behavior* behavior, bool force);
    void timesteps(float dt);
    void update_modifier(Context& ctx, Record& r, float timestep);
    void drain();
    void deliver(int32_t external, const std::string* payload, uint8_t& flag);
    void resolve_bindings(Record& r);
    void copy_in(Record& r);
    void copy_back(Record& r);
    Context context(Behavior* behavior);
    void global_transitions(Context& ctx, int32_t external, const std::string* payload);

    std::shared_ptr<const Project> m_project;
    Character& m_character;
    VariableValues m_values;
    std::vector<std::unique_ptr<Record>> m_records; // leaves first, the root generator last
    std::unordered_map<const Node*, uint32_t> m_index;
    std::vector<std::unique_ptr<Record>> m_old_records; // the list being replaced
    std::unordered_map<const Node*, uint32_t> m_old_index;
    bool m_discard_raised = false; // the node running ignores events: what it raises is lost
    std::vector<int32_t> m_children;
    bool m_update_active_nodes = false;
    bool m_active = false;
    std::deque<Queued> m_queue;
    Behavior* m_queue_map = nullptr;
    std::vector<std::unique_ptr<Output>> m_outputs;
};

// A node class's maker, registered by each node family (register_*_nodes) before the first project loads.
using NodeFactory = std::unique_ptr<Node> (*)();
void register_node(std::string_view cls, NodeFactory make);
template <typename T>
void register_node(std::string_view cls) {
    register_node(cls, +[]() -> std::unique_ptr<Node> { return std::make_unique<T>(); });
}

void set_reference(Node& reference, Behavior* behavior); // graph_nodes.cpp: fills an hkbBehaviorReferenceGenerator

// For the project's loader (generators.cpp): a clip's first stage of annotations, once events are linked, and the mirror
// tables from hkbMirroredSkeletonInfo.
std::vector<BindingTrigger> bind_annotations(const Project& project, const Clip& clip);
MirroredSkeleton make_mirrored_skeleton(const Skeleton& skeleton, const glm::vec4& mirror_axis,
                                        std::vector<int16_t> bone_pairs);

// state_machine.cpp: compiles expressions and builds the global transition table. After the id maps are complete.
void link_state_machines(Project& project);

void register_structural_nodes(); // graph_nodes.cpp
void register_state_machine_nodes(); // state_machine.cpp
void register_generator_nodes();     // generators.cpp
void register_modifier_nodes();      // modifiers.cpp

// hkbCharacterData.footIkDriverInfo, to load with describe().
std::unique_ptr<Bindable> make_foot_ik_info(); // modifiers.cpp
// hkbFootIkDriver: places the feet on `output`, once a frame after generate, with the update's dt, where the game allows it
// Does nothing for a project without driver info.
void run_foot_ik_driver(Character& character, Output& output, float dt); // modifiers.cpp

} // namespace spud::anim
