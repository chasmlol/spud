#pragma once

// The Papyrus virtual machine.
//
// One thread runs everything, so the game's parallel tasklets become a serial pass over the ready list; what scripts
// can observe (3.1) is kept. The game side (forms, time, logs) comes in through Host, natives through bind_native().

#include "formats/oss.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud::script {

class VirtualMachine;
struct Class;
struct Object;
struct Stack;

using StringId = uint32_t; // interned, case-insensitively (2.1): equal ids are equal strings
using StackId = uint32_t;

// A handle: low 32 bits form ID, bits 32..47 alias or 0xFFFF for a plain form, bits 48+ a kind (5.4).
using Handle = uint64_t;
inline constexpr Handle k_no_handle = 0;
constexpr Handle form_handle(uint32_t form_id) {
    return uint64_t{0xFFFF} << 32 | form_id;
}
constexpr uint32_t handle_form(Handle h) {
    return static_cast<uint32_t>(h);
}
inline constexpr uint32_t k_handle_ref_alias = 1;
inline constexpr uint32_t k_handle_location_alias = 2;
inline constexpr uint32_t k_handle_active_effect = 3;
inline constexpr uint32_t k_handle_inventory = 4;
constexpr Handle alias_handle(uint32_t quest, uint32_t alias, bool location) {
    return uint64_t{location ? k_handle_location_alias : k_handle_ref_alias} << 48 | uint64_t{alias & 0xFFFF} << 32 | quest;
}
// A running magic effect: the form slot holds its run's id.
constexpr Handle active_effect_handle(uint32_t id) {
    return uint64_t{k_handle_active_effect} << 48 | uint64_t{0xFFFF} << 32 | id;
}
// A scripted item in an inventory: the base form and the item's id.
constexpr Handle inventory_handle(uint32_t base, uint32_t id) {
    return uint64_t{k_handle_inventory} << 48 | uint64_t{id & 0xFFFF} << 32 | base;
}
constexpr uint32_t handle_kind(Handle h) {
    return static_cast<uint32_t>(h >> 48);
}
constexpr uint32_t handle_alias(Handle h) {
    return static_cast<uint32_t>(h >> 32) & 0xFFFF;
}

enum class Tag : uint8_t { none, object, string, integer, real, boolean, array };

struct Array;

// A variable's value and tag. An object or array value without a target is that type's None (2.1).
struct Value {
    Tag tag = Tag::none;
    Tag element = Tag::none;     // arrays
    const Class* type = nullptr; // objects and object arrays: the tag's class
    union {
        int32_t integer = 0;
        float real;
        bool boolean;
        StringId string;
        Object* object;
    };
    std::shared_ptr<Array> array;

    static Value of(int32_t v) {
        Value r;
        r.tag = Tag::integer;
        r.integer = v;
        return r;
    }
    static Value of(float v) {
        Value r;
        r.tag = Tag::real;
        r.real = v;
        return r;
    }
    static Value of(bool v) {
        Value r;
        r.tag = Tag::boolean;
        r.boolean = v;
        return r;
    }
    static Value of_string(StringId v) {
        Value r;
        r.tag = Tag::string;
        r.string = v;
        return r;
    }
    static Value of_object(Object* o, const Class* type) {
        Value r;
        r.tag = Tag::object;
        r.type = type;
        r.object = o;
        return r;
    }
    bool same_tag(const Value& o) const { return tag == o.tag && element == o.element && type == o.type; }
};

struct Array {
    Value element_default;
    std::vector<Value> values;
};

struct Function {
    StringId name = 0;
    StringId state = 0;
    Class* owner = nullptr;
    const oss::Function* code = nullptr; // null for auto property accessors
    std::vector<Value> slot_defaults;    // parameters, then locals
    Value return_default;
    uint32_t parameters = 0;
    bool global = false;
    bool native = false;
    // Natives (7.2, 3.2).
    std::function<void(struct NativeCall&)> bound;
    bool callable_from_tasklets = false;
    bool latent = false;
    // Auto property accessors.
    int auto_variable = -1;
    bool auto_setter = false;
    bool accessor = false; // a property's get or set function
};

struct Property {
    const Function* getter = nullptr;
    const Function* setter = nullptr;
    bool automatic = false;
    int auto_variable = -1;
    bool read_only = false;
    Value type;
};

struct Class {
    StringId name = 0;
    Class* parent = nullptr;
    const oss::Script* script = nullptr;
    uint32_t variable_base = 0;  // this class's variables start here in an object
    uint32_t variable_count = 0; // the chain's total
    StringId auto_state = 0;
    std::vector<Value> variable_defaults; // this class's, default then initial value
    std::vector<StringId> variable_names;
    // (state, name) -> function: 4.2.
    std::unordered_map<uint64_t, std::unique_ptr<Function>> functions;
    std::unordered_map<StringId, Property> properties;
    std::vector<std::unique_ptr<Function>> accessors;

    bool derives_from(const Class* other) const;
};

// One script instance. Several may share a handle.
struct Object {
    Class* type = nullptr;
    Handle handle = k_no_handle;
    StringId state = 0;
    std::vector<Value> variables;
    StackId lock_owner = 0;
    uint32_t properties_left = 0;
    bool constructed = false;
    bool initialised = false;
    bool valid = true;
    bool reset_pending = false;
    std::vector<uint32_t> pending_properties; // indices into attached_properties, until the policy is ready
    uint32_t attachment = oss::k_none;
};

enum class StackState { running, finished, waiting_memory, waiting_latent, waiting_call, waiting_return,
                        waiting_return_no_pop, retry_return_no_pop, retry_call };
enum class StackType { normal, property_init, init_event };

// The arguments a native sees (7.3).
struct NativeCall {
    VirtualMachine& vm;
    StackId stack;
    Object* self; // null for static natives
    std::span<Value> args;
    Value result;      // void leaves None
    bool started = true; // latent natives: false returns None at once (3.2)
};
using NativeFunction = std::function<void(NativeCall&)>;

enum NativeFlag : uint32_t { native_tasklet = 1, native_latent = 2 };

// What the VM needs from the game.
class Host {
public:
    virtual ~Host() = default;
    // Severity: 0 info, 1 warning, 2 error, 3 fatal (7.4).
    virtual void log(int severity, std::string_view text) = 0;
    // The script class of a form's native type ("Actor"), or "" when the form doesn't exist.
    virtual std::string_view form_class(uint32_t form_id) = 0;
    // The native class for any handle: aliases have their own.
    virtual std::string_view handle_class(Handle handle) { return form_class(handle_form(handle)); }
    // A placed reference's base form, 0 for anything else.
    virtual uint32_t reference_base(uint32_t form_id) {
        (void)form_id;
        return 0;
    }
    // Text for a handle in errors and ToString (5.4).
    virtual std::string handle_text(Handle handle) = 0;
    // VM update's clocks (1.2): whether the game is in menu mode, and days passed.
    virtual bool in_menu_mode() = 0;
    virtual double game_days_passed() = 0;
    virtual bool player_sleeping_or_waiting() { return false; }
};

struct Settings {
    float update_budget_ms = 1.2f;              // fUpdateBudgetMS
    uint32_t min_memory_page = 128;             // iMinMemoryPageSize
    uint32_t max_allocated_memory = 153600;     // iMaxAllocatedMemoryBytes
};

class VirtualMachine {
public:
    VirtualMachine(const oss::Scripts& scripts, Host& host, Settings settings = {});
    ~VirtualMachine();

    // ---- natives ---------------------------------------------------------------------------------------------
    // Natives the game declares and nothing here answers: a stub stands in, until bind_native replaces it.
    void mark_stub(std::string_view script, std::string_view name, std::string full);
    std::vector<std::string> stub_natives() const;
    void bind_native(std::string_view script, std::string_view name, NativeFunction fn, uint32_t flags = 0);
    bool has_native(std::string_view script, std::string_view name);

    // ---- strings, classes, objects ---------------------------------------------------------------------------
    StringId intern(std::string_view s);
    std::string_view text(StringId id) const { return m_strings[id]; }
    Class* find_class(std::string_view name);
    Class* find_class(StringId name);

    // The objects bound to a handle, in attach order (5.1).
    std::span<Object* const> objects(Handle handle) const;
    // Binds the scripts a form's record attaches (5.1), once.
    void bind_scripts(uint32_t form_id);
    // A quest alias's scripts, on its handle.
    void bind_scripts(uint32_t form_id, int32_t alias, Handle handle);
    bool has_scripts(uint32_t form_id) const;
    // A script variable of the objects on a handle, by name.
    const Value* find_variable(Handle handle, std::string_view name);
    // Creates and binds one object.
    Object* bind(Handle handle, Class* type);
    // The object of `type` for a handle: bound, or created for a native type (CastObject step 3). Null for neither.
    Object* object_for(Handle handle, Class* type);
    Value form_value(uint32_t form_id, std::string_view type);
    void unbind(Handle handle);
    void reset(Object* object); // 5.3

    // ---- calls and events ------------------------------------------------------------------------------------
    using Callback = std::function<void(const Value&)>;
    void send_event(Handle handle, std::string_view name, std::vector<Value> args = {});
    void call_method(Object* object, std::string_view name, std::vector<Value> args = {}, Callback callback = {});
    void call_static(std::string_view script, std::string_view name, std::vector<Value> args = {},
                     Callback callback = {});

    // ---- latent natives --------------------------------------------------------------------------------------
    void return_from_latent(StackId stack, Value value);
    // Waits (3.3): the stack is woken with None once the clock passes the target.
    enum class Clock { gameplay, real, game };
    void wait(StackId stack, Clock clock, int64_t target);
    int64_t clock(Clock which) const;
    // Update registrations (3.4).
    void register_update(Handle handle, float seconds, bool single);
    void register_game_time_update(Handle handle, float hours, bool single);
    void unregister_update(Handle handle);
    void unregister_game_time_update(Handle handle);

    // ---- the frame -------------------------------------------------------------------------------------------
    // The running frame's two dispatches (1.3): update(delta) then dispatch(), later dispatch() again. A paused frame
    // does update(delta) and one dispatch() on the main thread.
    void update(uint32_t delta_ms);
    void dispatch();
    void set_frozen(bool frozen) { m_frozen = frozen; }

    // ---- diagnostics and tests -------------------------------------------------------------------------------
    size_t running_stacks() const { return m_stacks.size(); }
    size_t suspended_stacks() const;
    std::string to_string(const Value& v);
    void log_error(StackId stack, std::string_view text, int severity = 2);

    // Our own save, between frames: everything 6.1 lists. Waits and updates go as offsets from the clocks.
    std::vector<std::byte> save();
    void load(std::span<const std::byte> bytes);

    const oss::Scripts& scripts() const { return m_scripts; }
    Host& host() { return m_host; }

private:
    std::unordered_map<uint64_t, std::string> m_stubs;
    enum class Flow { done, entered, left };
    struct Timer;
    struct Message;
    struct UpdateRegistration;
    struct Queues;

    Class* link(const oss::Script& script);
    Value parse_type(StringId type);
    Function* make_function(Class* c, const oss::Function& f, StringId state);
    const Function* find_method(const Class* from, StringId state, StringId name) const;

    Stack& new_stack(StackType type);
    void finish_stack(Stack& s);
    void post_call(Stack& s);
    void post_return(Stack& s, bool pop);
    void suspend(Stack& s, StackState state);

    struct Saver;
    struct Loader;
    uint32_t frame_bytes(const Function& f) const;
    Flow perform_call(Stack& s, bool from_tasklet);
    Flow return_from_frame(Stack& s, bool pop, bool from_tasklet);
    Flow wait_for(Stack& s, bool from_tasklet, StackState state);
    bool push_frame(Stack& s, const Function& f, Value self, std::span<const Value> args);
    bool check_arguments(Stack& s, const Function& f, std::vector<Value>& args);

    void ready(Stack& s);
    void run_slice(Stack& s);
    bool read(Stack& s, const oss::Operand& o, Value& out);
    Value* slot(Stack& s, const oss::Operand& o);
    Value* inherited_variable(Object* object, const Class* owner, uint32_t name);
    void store(Stack& s, const oss::Operand& dest, const Value& v, bool assign);
    void store_into(Stack& s, Value& d, const Value& v, bool assign, StringId name);
    Value cast(Stack& s, const Value& target, const Value& a);
    Value cast_object(const Value& target, const Value& a);
    Value default_of(const Value& type) const;
    bool is_true(const Value& v) const;
    int compare(const Value& a, const Value& b);
    Flow do_call(Stack& s, const oss::Instruction& ins);
    void store_return(Stack& s);

    void update_clocks(uint32_t delta_ms);
    void fire_timers();
    void vm_update(double budget_ms);
    void object_resets();
    void function_messages(double budget_ms);
    void suspended_stage(double budget_ms);
    void retry(Stack& s);

    void apply_properties(Object* o);
    void queue_on_init(Object* o);
    void property_done(Object* o);
    bool try_lock(Stack& s, Object* o);
    void unlock(Stack& s);
    void initial_state(Object* o);
    void init_variables(Object* o);

    Value value_of(const oss::AttachedValue& v, const Value& type);

    const oss::Scripts& m_scripts;
    Host& m_host;
    Settings m_settings;

    std::vector<std::string> m_strings;
    std::unordered_map<std::string, StringId> m_string_ids; // lower-case key
    std::unordered_map<uint32_t, StringId> m_offset_ids;
    StringId intern_offset(uint32_t offset);

    std::unordered_map<StringId, std::unique_ptr<Class>> m_classes;
    std::unordered_map<StringId, const oss::Script*> m_class_index;
    std::vector<std::unique_ptr<Object>> m_objects;
    std::unordered_map<Handle, std::vector<Object*>> m_bound;
    std::unordered_map<uint32_t, bool> m_bound_forms;
    std::unordered_set<Handle> m_bound_bases; // handles object_for looked up scripts for

    std::unordered_map<StackId, std::unique_ptr<Stack>> m_stacks;
    StackId m_next_stack = 1;
    std::deque<StackId> m_ready;
    std::unique_ptr<Queues> m_queues;
    std::vector<Object*> m_reset_queue;
    uint64_t m_stack_memory = 0;

    int64_t m_gameplay_clock = 0;
    int64_t m_real_clock = 0;
    int64_t m_game_clock = 0;
    bool m_frozen = false;
    bool m_overstressed = false;
};

} // namespace spud::script
