#pragma once

// hkbStateMachine with its states, transitions and conditions, and hkbBlendingTransitionEffect
// The expression engine and hkbEvaluateExpressionModifier live in the
// .cpp.

#include "anim/graph.hpp"

#include <optional>
#include <span>

namespace spud::anim {

struct CompiledExpressions;
struct ExpressionNames;
class StateMachine;

// hkbBlendingTransitionEffect: a generator with two inputs. Each transition plays its own copy of the effect
// in the file, because transitions share effect objects and two of them can run at once; see
// StateMachine::start.
class BlendingTransitionEffect final : public Generator {
public:
    enum Flag : uint16_t {
        flag_ignore_from_world_from_model = 1,
        flag_sync = 2,
        flag_ignore_to_world_from_model = 4,
    };

    int8_t self_transition_mode = 0;
    int8_t event_mode = 0;
    float duration = 0.0f;
    float to_generator_start_time_fraction = 0.0f;
    uint16_t flags = 0;
    int8_t end_mode = 0;
    int8_t blend_curve = 0;

    // The inputs, as templates, or an older transition's effect when this one interrupted it.
    Node* from = nullptr;
    Node* to = nullptr;
    float time_remaining = 0.0f;
    float time_in_transition = 0.0f;

    std::unique_ptr<Node> clone() const override { return std::make_unique<BlendingTransitionEffect>(*this); }
    void describe(Describer& d) override;
    void activate(Context& ctx) override;
    void update(Context& ctx, float timestep) override;
    void deactivate(Context& ctx) override;
    void children(uint32_t flags, std::vector<Child>& out) override;
    void generate(Context& ctx, std::span<const Output* const> children, Output& output,
                  float time_offset) override;
    void update_sync(Context& ctx) override;

    bool is_done() const;

private:
    int8_t effective_event_mode() const { return event_mode != 0 ? event_mode : m_default_event_mode; }
    void apply_self_transition(Context& ctx);

    int8_t m_default_event_mode = 0;
    bool m_apply_self_transition = false;
    bool m_first_sync = true;
};

// hkbExpressionCondition and hkbStringCondition. The member holds one or the other, and only the first ever
// decides.
struct TransitionCondition : Bindable {
    std::string expression;
    std::string condition_string;
    std::shared_ptr<const CompiledExpressions> compiled;

    void describe(Describer& d) override {
        d.member("expression", expression);
        d.member("conditionString", condition_string);
    }
    bool is_true(Context& ctx) const;
};

// hkbStateMachineTimeInterval.
struct TimeInterval : Bindable {
    int32_t enter_event_id = -1;
    int32_t exit_event_id = -1;
    float enter_time = 0.0f;
    float exit_time = 0.0f;

    void describe(Describer& d) override {
        d.member("enterEventId", enter_event_id);
        d.member("exitEventId", exit_event_id);
        d.member("enterTime", enter_time);
        d.member("exitTime", exit_time);
    }
};

// hkbStateMachineTransitionInfo.
struct TransitionInfo : Bindable {
    enum Flag : int32_t {
        flag_use_trigger_interval = 1,
        flag_use_initiate_interval = 2,
        flag_uninterruptible_while_playing = 4,
        flag_uninterruptible_while_delayed = 8,
        flag_delay_state_change = 16,
        flag_disabled = 32,
        flag_disallow_return_to_previous_state = 64,
        flag_disallow_random_transition = 128,
        flag_disable_condition = 256,
        flag_allow_self_transition_by_transition_from_any_state = 512,
        flag_is_global_wildcard = 1024,
        flag_is_local_wildcard = 2048,
        flag_from_nested_state_id_is_valid = 4096,
        flag_to_nested_state_id_is_valid = 8192,
        flag_abut_at_end_of_from_generator = 16384,
    };

    TimeInterval trigger_interval;
    TimeInterval initiate_interval;
    Node* transition = nullptr; // the effect; null for an immediate change
    TransitionCondition condition;
    bool has_condition = false;
    int32_t event_id = -1;
    int32_t to_state_id = 0;
    int32_t from_nested_state_id = 0;
    int32_t to_nested_state_id = 0;
    int16_t priority = 0;
    int16_t flags = 0;

    void describe(Describer& d) override {
        d.object("triggerInterval", trigger_interval);
        d.object("initiateInterval", initiate_interval);
        d.node("transition", transition);
        d.object("condition", condition, &has_condition);
        d.member("eventId", event_id);
        d.member("toStateId", to_state_id);
        d.member("fromNestedStateId", from_nested_state_id);
        d.member("toNestedStateId", to_nested_state_id);
        d.member("priority", priority);
        d.member("flags", flags);
    }
};

// hkbStateMachineTransitionInfoArray.
struct TransitionInfoArray : Bindable {
    std::vector<TransitionInfo> transitions;
    void describe(Describer& d) override { d.objects("transitions", transitions); }
};

// hkbEventProperty as an element of hkbStateMachineEventPropertyArray.
struct NotifyEvent : Bindable {
    EventProperty event;
    void describe(Describer& d) override;
};

// hkbStateMachineEventPropertyArray.
struct NotifyEventArray : Bindable {
    std::vector<NotifyEvent> events;
    void describe(Describer& d) override { d.objects("events", events); }
};

// hkbStateMachineStateInfo. Only `enable` and `probability` can change at run time, so a clone shares the
// file's lists: a machine is cloned on every activation, and those lists are most of its size.
struct StateInfo : Bindable {
    std::shared_ptr<NotifyEventArray> enter_notify_events;
    std::shared_ptr<NotifyEventArray> exit_notify_events;
    std::shared_ptr<TransitionInfoArray> transitions;
    Node* generator = nullptr;
    std::string name;
    int32_t state_id = 0;
    float probability = 1.0f;
    bool enable = true;

    // From the nesting table: the machine nested in this state, and the behaviour it lives in.
    const StateMachine* nested = nullptr;
    Behavior* nested_behavior = nullptr;

    void describe(Describer& d) override;
};

// The nesting table, shared by a project's characters.
struct GlobalTransitions {
    struct Entry {
        StateMachine* machine = nullptr;
        Behavior* behavior = nullptr;
        int32_t parent = -1;       // the entry of the machine above
        int32_t parent_state = -1; // the state of that machine this one is nested in
    };
    struct Listener {
        int32_t entry = -1;
        int32_t wildcard = -1;
    };
    std::vector<Entry> entries;
    std::unordered_map<int32_t, std::vector<Listener>> listeners; // by external event id
};

class StateMachine final : public Generator {
public:
    enum StartStateMode : int8_t { start_default = 0, start_sync = 1, start_random = 2, start_chooser = 3 };
    enum SelfTransitionMode : int8_t {
        self_no_transition = 0,
        self_to_start_state = 1,
        self_force_to_start_state = 2,
    };

    EventProperty event_to_send_when_state_or_transition_changes;
    int32_t start_state_id = 0;
    int32_t return_to_previous_state_event_id = -1;
    int32_t random_transition_event_id = -1;
    int32_t transition_to_next_higher_state_event_id = -1;
    int32_t transition_to_next_lower_state_event_id = -1;
    int32_t sync_variable_index = -1;
    bool wrap_around_state_id = false;
    int8_t max_simultaneous_transitions = 32;
    int8_t start_state_mode = start_default;
    int8_t self_transition_mode = self_no_transition;
    std::vector<StateInfo> states;
    std::shared_ptr<TransitionInfoArray> wildcard_transitions;

    int32_t current_state_id = 0;
    int32_t previous_state_id = 0;
    bool is_active = false;

    std::unique_ptr<Node> clone() const override;
    void describe(Describer& d) override;
    void collect_bindables(std::vector<Bindable*>& out) override;
    void activate(Context& ctx) override;
    void update(Context& ctx, float timestep) override;
    void handle_event(Context& ctx, const Event& event) override;
    void deactivate(Context& ctx) override;
    void children(uint32_t flags, std::vector<Child>& out) override;
    void generate(Context& ctx, std::span<const Output* const> children, Output& output,
                  float time_offset) override;
    void update_sync(Context& ctx) override;
    void start_echo(Context&, float) override { m_echo_next_update = true; }

    // The entry point for code outside the graph: no event is involved.
    void go_to_state(Context& ctx, int32_t state_id, bool immediate, bool force, bool return_to_previous);

    int32_t state_index(int32_t state_id) const; // -1 for an unknown id
    void set_next_start_override(int32_t index) { m_next_start_override = index; }

    // Load time: state ids, and everything compiled against the behaviour's names.
    void link(const ExpressionNames& names);
    // One global wildcard firing, with the wildcard's condition already passed.
    static void fire_global(Context& ctx, const GlobalTransitions& table, int32_t entry, int32_t wildcard);

private:
    // Names a transition. `machine` is the one whose wildcard list holds it for a global transition,
    // otherwise null.
    struct Reference {
        int32_t from_state_index = -1; // -1 for the wildcard list
        int32_t transition_index = -1;
        const StateMachine* machine = nullptr;
        const TransitionInfo* info = nullptr;
    };
    struct Prospective {
        Reference transition;
        Reference effect; // the transition whose effect is played; differs only for global transitions
        int32_t to_state_id = 0;
    };
    struct ActiveTransition {
        std::shared_ptr<BlendingTransitionEffect> effect;
        Reference transition;
        int32_t to_state_id = 0;
        bool return_to_previous = false;
    };
    struct DelayedTransition {
        Prospective prospective;
        bool return_to_previous = false;
    };
    using Prospects = std::vector<Prospective>;

    const StateMachine* template_of() const { return m_template ? m_template : this; }
    Node* active_child() const;
    const std::vector<TransitionInfo>& wildcard_list() const;
    const std::vector<TransitionInfo>& state_list(size_t index) const;
    uint8_t flags_of(const Reference& r) const;

    int32_t choose_start_state(Context& ctx);
    size_t weighted_choice(Context& ctx, std::span<const int32_t> state_indices) const;
    void write_sync(Context& ctx, int32_t state_id);

    void reset_intervals(size_t state_index);
    void set_state(Context& ctx, int32_t state_id, bool return_to_previous, bool notify_children);
    void send_enter(Context& ctx);
    void send_exit(Context& ctx);
    void notify_nested(Context& ctx, bool enter);

    bool blocked() const;
    bool any_uninterruptible_delayed() const;
    bool passes(Context& ctx, const TransitionInfo& t, const Reference& r, int32_t event, bool any_event,
                std::optional<int32_t> target) const;
    bool ready(const Prospective& p) const;
    bool ready_to_start(Context& ctx, const Prospective& p) const;
    void gather_list(Context& ctx, bool wildcard, int32_t event, bool any_event,
                     std::optional<int32_t> target, Prospects& out) const;
    void gather(Context& ctx, int32_t event, bool any_event, std::optional<int32_t> target,
                Prospects& out) const;
    bool return_to_previous_of(const Prospective& p, int32_t event) const;
    void process(Context& ctx, Prospects entries, int32_t event, std::optional<bool> forced_return);
    void start(Context& ctx, const Prospective& p, bool return_to_previous);
    void start_global(Context& ctx, Prospective p);
    void end_transitions(Context& ctx, int32_t index);
    void end_all_transitions(Context& ctx) {
        end_transitions(ctx, static_cast<int32_t>(m_transitions.size()) - 1);
    }
    void retire_transitions(Context& ctx);
    void echo_and_delayed(Context& ctx);

    const StateMachine* m_template = nullptr; // set on clones
    int32_t m_current_index = 0;
    bool m_entered = false;
    int32_t m_next_start_override = -1;
    std::vector<ActiveTransition> m_transitions; // oldest first
    // Effects that ended. The root still keys their records by these pointers until its next collection;
    // freeing them now would let a new effect take the same address and be mistaken for the old one.
    std::vector<std::shared_ptr<BlendingTransitionEffect>> m_retired;
    std::vector<DelayedTransition> m_delayed;
    // Per transition of the current state: bit 0 in the trigger interval, bit 1 in the initiate interval.
    std::vector<uint8_t> m_state_flags;
    std::vector<uint8_t> m_wildcard_flags;
    bool m_state_or_transition_changed = false;
    bool m_echo_next_update = false;
};

} // namespace spud::anim
