// The structural nodes.

#include "anim/graph.hpp"

#include <algorithm>

namespace spud::anim {

namespace {

// hkbBehaviorReferenceGenerator: a transparent link to another file's root generator. The file's hkbBehaviorGraph has no
// node of its own here: the root reaches its root generator from Behavior.
class BehaviorReference final : public Generator {
public:
    std::string behavior_name;
    Behavior* behavior = nullptr; // filled when the project loads

    std::unique_ptr<Node> clone() const override { return std::make_unique<BehaviorReference>(*this); }
    Behavior* behavior_below() const override { return behavior; }
    void describe(Describer& d) override { d.member("behaviorName", behavior_name); }
    void children(uint32_t flags, std::vector<Child>& out) override {
        if (behavior && !(flags & children_ignore_references)) {
            out.push_back({behavior->root_generator, false, false, true, false});
        }
    }
    void generate(Context&, std::span<const Output* const> kids, Output& output, float) override {
        if (!kids.empty()) {
            output.copy_from(*kids[0]);
        }
    }
    void update_sync(Context& ctx) override {
        if (behavior) {
            ctx.graph.forward_sync(*this, behavior->root_generator);
        }
    }
};

class ModifierGenerator final : public Generator {
public:
    Node* modifier = nullptr;
    Node* generator = nullptr;

    std::unique_ptr<Node> clone() const override { return std::make_unique<ModifierGenerator>(*this); }
    const Node* wrapped_generator() const override { return generator; }
    void describe(Describer& d) override {
        d.node("modifier", modifier);
        d.node("generator", generator);
    }
    void children(uint32_t flags, std::vector<Child>& out) override {
        out.push_back({generator, false, false, true, false});
        if (modifier && !(flags & children_generators_only)) {
            out.push_back({modifier, false, false, false, false});
        }
    }
    void generate(Context& ctx, std::span<const Output* const> kids, Output& output, float time_offset) override {
        if (!kids.empty()) {
            output.copy_from(*kids[0]);
        }
        if (time_offset == 0.0f && modifier) {
            ctx.graph.modify_step(ctx, static_cast<Modifier*>(modifier), output);
        }
    }
    void update_sync(Context& ctx) override {
        ctx.graph.copy_sync(*this, generator);
        if (modifier && record) {
            ctx.graph.modify_sync_step(ctx, static_cast<Modifier*>(modifier), record->sync);
        }
    }
};

class ModifierList final : public Modifier {
public:
    std::vector<Node*> modifiers;

    std::unique_ptr<Node> clone() const override { return std::make_unique<ModifierList>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.nodes("modifiers", modifiers);
    }
    void children(uint32_t flags, std::vector<Child>& out) override {
        if ((flags & children_generators_only) || ((flags & children_active_only) && !enable)) {
            return;
        }
        for (Node* m : modifiers) {
            out.push_back({m, false, false, false, false});
        }
    }
    void modify(Context& ctx, Output& output) override {
        for (Node* m : modifiers) {
            if (m) {
                ctx.graph.modify_step(ctx, static_cast<Modifier*>(m), output);
            }
        }
    }
    void modify_sync(Context& ctx, SyncInfo& sync) override {
        for (Node* m : modifiers) {
            if (m) {
                ctx.graph.modify_sync_step(ctx, static_cast<Modifier*>(m), sync);
            }
        }
    }
};

class ManualSelector final : public Generator {
public:
    std::vector<Node*> generators;
    int8_t selected = 0;
    int8_t current = 0;

    std::unique_ptr<Node> clone() const override { return std::make_unique<ManualSelector>(*this); }
    void describe(Describer& d) override {
        d.nodes("generators", generators);
        d.member("selectedGeneratorIndex", selected);
        d.member("currentGeneratorIndex", current, true);
    }
    int8_t clamped() const {
        if (generators.empty()) {
            return 0;
        }
        return static_cast<int8_t>(std::clamp<int>(selected, 0, int(generators.size()) - 1));
    }
    void activate(Context&) override { current = clamped(); }
    void update(Context& ctx, float) override {
        const int8_t next = clamped();
        if (next != current) {
            if (generators[size_t(current)] != generators[size_t(next)]) {
                ctx.graph.request_update_active_nodes();
            }
            current = next;
        }
    }
    void children(uint32_t flags, std::vector<Child>& out) override {
        if (flags & children_active_only) {
            if (!generators.empty()) {
                out.push_back({generators[size_t(current)], false, false, false, false});
            }
            return;
        }
        for (Node* g : generators) {
            out.push_back({g, false, false, false, false});
        }
    }
    void generate(Context&, std::span<const Output* const> kids, Output& output, float) override {
        if (!kids.empty()) {
            output.copy_from(*kids[0]);
        }
    }
    void update_sync(Context& ctx) override {
        if (!generators.empty()) {
            ctx.graph.forward_sync(*this, generators[size_t(current)]);
        }
    }
};

class EventDrivenModifier final : public Modifier {
public:
    Node* modifier = nullptr;
    int32_t activate_event = -1, deactivate_event = -1;
    bool active_by_default = false;
    bool is_active = false;

    std::unique_ptr<Node> clone() const override { return std::make_unique<EventDrivenModifier>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.node("modifier", modifier);
        d.member("activateEventId", activate_event);
        d.member("deactivateEventId", deactivate_event);
        d.member("activeByDefault", active_by_default);
    }
    void activate(Context&) override { is_active = active_by_default; }
    void handle_event(Context& ctx, const Event& e) override {
        if (!modifier) {
            return;
        }
        if (is_active && deactivate_event != -1 && deactivate_event == e.id) {
            is_active = false;
        } else if (!is_active && activate_event != -1 && activate_event == e.id) {
            is_active = true;
        } else {
            return;
        }
        ctx.graph.request_update_active_nodes();
        ctx.event_triggered_transition |= 2;
    }
    void children(uint32_t flags, std::vector<Child>& out) override {
        if (flags & children_generators_only) {
            return;
        }
        if (!(flags & children_active_only) || (is_active && enable)) {
            out.push_back({modifier, false, false, false, false});
        }
    }
    void modify(Context& ctx, Output& output) override {
        if (is_active && modifier) {
            ctx.graph.modify_step(ctx, static_cast<Modifier*>(modifier), output);
        }
    }
    void modify_sync(Context& ctx, SyncInfo& sync) override {
        if (is_active && modifier) {
            ctx.graph.modify_sync_step(ctx, static_cast<Modifier*>(modifier), sync);
        }
    }
};

} // namespace

void set_reference(Node& node, Behavior* behavior) { static_cast<BehaviorReference&>(node).behavior = behavior; }

void register_structural_nodes() {
    register_node<BehaviorReference>("hkbBehaviorReferenceGenerator");
    register_node<ModifierGenerator>("hkbModifierGenerator");
    register_node<ModifierList>("hkbModifierList");
    register_node<ManualSelector>("hkbManualSelectorGenerator");
    register_node<EventDrivenModifier>("hkbEventDrivenModifier");
}

} // namespace spud::anim
