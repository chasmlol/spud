#pragma once

// The contract of a behaviour-graph node.
//
// A node is a C++ class that mirrors one class of. Its members are declared once, in
// describe(): that one list loads them from .osb and resolves binding paths. Templates are made at load; a character
// gets its own copy (clone()) when the node first becomes active, and loses it when the node leaves the active set, so
// node state never outlives a deactivation. Everything else about order and timing belongs to the root (graph.cpp):
// a node never calls activate, update, handle_event, deactivate or generate on a child.

#include "anim/pose.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spud::anim {

class Behavior;
class Bindable;
class Graph;
class Node;
struct Character;
struct Record;

// hkbEventProperty: -1 sends nothing. Payloads are strings in vanilla (hkbStringEventPayload).
struct EventProperty {
    int32_t id = -1;
    std::shared_ptr<const std::string> payload;
};

struct Event {
    int32_t id = -1;
    const std::string* payload = nullptr;
    const Node* sender = nullptr;
};

inline constexpr uint32_t k_max_sync_points = 16;

// hkbGeneratorSyncInfo.
struct SyncInfo {
    struct Point {
        int32_t id = 0;
        float time = 0.0f;
    };
    std::array<Point, k_max_sync_points> points{};
    uint32_t point_count = 0;
    float base_frequency = 0.0f;
    float local_time = 0.0f;
    float playback_speed = 1.0f;
    bool cyclic = false;
    bool mirrored = false;
    bool additive = false;
};

// Declares a class's members. `name` is the member's name. A member the game writes
// back to a variable (a role with FLAG_OUTPUT, "Bindings") is declared with output = true.
class Describer {
public:
    virtual ~Describer() = default;
    virtual void member(const char* name, bool& v, bool output = false) = 0;
    virtual void member(const char* name, int8_t& v, bool output = false) = 0;
    virtual void member(const char* name, uint8_t& v, bool output = false) = 0;
    virtual void member(const char* name, int16_t& v, bool output = false) = 0;
    virtual void member(const char* name, uint16_t& v, bool output = false) = 0;
    virtual void member(const char* name, int32_t& v, bool output = false) = 0;
    virtual void member(const char* name, uint32_t& v, bool output = false) = 0;
    virtual void member(const char* name, float& v, bool output = false) = 0;
    virtual void member(const char* name, glm::vec4& v, bool output = false) = 0;
    virtual void member(const char* name, glm::quat& v, bool output = false) = 0;
    virtual void member(const char* name, std::string& v) = 0;
    virtual void member(const char* name, Qs& v) = 0;
    virtual void member(const char* name, EventProperty& v) = 0;
    virtual void member(const char* name, std::vector<float>& v) = 0;
    virtual void member(const char* name, std::vector<int32_t>& v) = 0;
    virtual void member(const char* name, std::vector<uint8_t>& v) = 0;
    virtual void member(const char* name, std::vector<std::string>& v) = 0;
    // A node, by pointer: the template. Clones keep pointing at templates; the root maps them.
    virtual void node(const char* name, Node*& v) = 0;
    virtual void nodes(const char* name, std::vector<Node*>& v) = 0;
    // Helper objects are held by value, so that a clone has its own. The member may be stored in the file inline or by
    // pointer; a null pointer leaves `exists` false.
    virtual void object(const char* name, Bindable& v, bool* exists = nullptr) = 0;
    template <typename T>
    void objects(const char* name, std::vector<T>& v) {
        object_list(name, [&v](size_t n) { v.resize(n); }, [&v](size_t i) -> Bindable& { return v[i]; });
    }

protected:
    virtual void object_list(const char* name, std::function<void(size_t)> resize,
                             std::function<Bindable&(size_t)> at) = 0;
};

// One binding (hkbVariableBindingSetBinding), as read from the file.
struct Binding {
    std::string path;
    int32_t variable = -1;
    bool character_property = false;
};

// hkbBindable: anything that may hold bindings.
class Bindable {
public:
    virtual ~Bindable() = default;
    virtual void describe(Describer& d) = 0;
    // Sub-objects that can hold bindings (collectBindables): helpers listed by the class, in order.
    virtual void collect_bindables(std::vector<Bindable*>&) {}

    std::shared_ptr<const std::vector<Binding>> bindings; // shared by a template and its clones
    int32_t enable_binding = -1;                          // indexOfBindingToEnable
};

// hkbBoneWeightArray: one weight per bone; past the end, or empty, a bone weighs 1. Bound through a pointer character
// property (`boneWeights`, `spBoneWeight`): the root replaces the member's contents.
struct BoneWeightArray : Bindable {
    std::vector<float> weights;
    void describe(Describer& d) override { d.member("boneWeights", weights); }
    float weight(size_t bone) const { return bone < weights.size() ? weights[bone] : 1.0f; }
};

enum ChildFlags : uint32_t {
    children_active_only = 1,
    children_generators_only = 2,
    children_ignore_references = 4,
};

struct Child {
    Node* node = nullptr; // a template; null keeps its place
    bool ignore_events = false;
    bool force_reactivate = false;
    bool sync_to_parent = false;
    bool frozen = false;
};

// hkbContext: built for each call into the graph.
struct Context {
    Graph& graph;
    Character& character;
    Behavior* behavior = nullptr;
    uint8_t event_triggered_transition = 0; // bit 0 a transition started, bit 1 an event-driven modifier toggled
};

class Node : public Bindable {
public:
    std::string name;
    uint32_t user_data = 0; // bit 0: update is skipped, bit 1: modify is skipped
    Record* record = nullptr; // the character's active record of this clone; valid while active

    virtual std::unique_ptr<Node> clone() const = 0;
    virtual bool is_generator() const { return false; }
    // A reference to another behaviour: what is below this node belongs to that one.
    virtual Behavior* behavior_below() const { return nullptr; }
    // hkbModifierGenerator: the generator it wraps, which a transition effect looks through. Null otherwise.
    virtual const Node* wrapped_generator() const { return nullptr; }

    virtual void activate(Context&) {}
    virtual void update(Context&, float /*timestep*/) {}
    virtual void handle_event(Context&, const Event&) {}
    virtual void deactivate(Context&) {}
    virtual void children(uint32_t /*flags*/, std::vector<Child>&) {}
};

// One entry of the game's active clip list, what its root motion reads. The weights are
// products down the tree (Graph::active_clips), so no clip holds them: `blend_weight` is the entry's +12 float, and
// `motion_weight`, the same product with each blender child's worldFromModelWeight, is the +16 weight that root motion
// uses. `playback_speed` is the raw field, and `name` the clip generator's node name.
struct ActiveClip {
    const std::string* name = nullptr;
    float playback_speed = 1.0f;
    float blend_weight = 1.0f;
    float motion_weight = 1.0f;
    float local_time = 0.0f;
    bool mirrored = false;
};

class Generator : public Node {
public:
    bool is_generator() const override { return true; }
    // `children`: the generator children's outputs, in children() order. `output` starts as a copy of nothing: a
    // pass-through node copies its child (Output::copy_from).
    virtual void generate(Context&, std::span<const Output* const> /*children*/, Output&, float /*time_offset*/) {}
    // Keep Record::sync current, or forward: sync_from_child / record forwarding (graph.hpp).
    virtual void update_sync(Context&) {}
    // Slots 26 and 27: placing this node on a shared timeline (Graph::join), and restarting it under a fading echo.
    virtual void set_local_time(Context&, float /*sync_time*/) {}
    virtual void start_echo(Context&, float /*duration*/) {}
    // A clip fills in what it knows of itself; everything else returns false and is walked through.
    virtual bool clip_state(ActiveClip&) const { return false; }
    // How much of active child `index` (children() order) reaches this node: its blend and motion weights.
    virtual void child_weight(size_t /*index*/, float& /*blend*/, float& /*motion*/) const {}
};

class Modifier : public Node {
public:
    bool enable = true;
    virtual void modify(Context&, Output&) {}
    virtual void modify_sync(Context&, SyncInfo&) {}
    void describe(Describer& d) override { d.member("enable", enable); }
};

} // namespace spud::anim
