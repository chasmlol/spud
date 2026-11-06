#pragma once

// The ActionScript 2 virtual machine: runs .osu functions (AVM1 semantics, docs/formats/osu.md) over the object
// model in object.hpp. Display objects plug in through VmHooks; the Player owns the VM.

#include "ui/player/object.hpp"

#include "formats/osu.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace spud::ui::player {

class MovieDef;

// One link of a scope chain: an activation object, a timeline clip or the global object.
struct Scope {
    Object* object = nullptr;
    std::shared_ptr<Scope> parent;
};

// What the VM asks of the display list.
class VmHooks {
public:
    virtual ~VmHooks() = default;
    // Display properties and children of clips, text fields and buttons (_x, instance names, ...).
    virtual bool get_display(Object* o, std::string_view name, Value& out) = 0;
    virtual bool set_display(Object* o, std::string_view name, const Value& v) = 0;
    virtual Object* root_object() = 0; // _root / _level0
    virtual Object* parent_object(DisplayObject* d) = 0;
    virtual std::string target_path(DisplayObject* d) = 0;
    // Timeline control for the frame ops (GotoFrame, Play, ...).
    virtual void goto_frame(DisplayObject* d, uint32_t frame, bool play) = 0; // 0-based
    virtual bool goto_label(DisplayObject* d, std::string_view label, bool play) = 0;
    virtual void play(DisplayObject* d, bool playing) = 0;
    virtual void step(DisplayObject* d, int direction) = 0;
    virtual DisplayObject* resolve_target(DisplayObject* from, std::string_view path) = 0;
    virtual Object* display_object(DisplayObject* d) = 0;
    // A removed clip's object -> the live clip now at its path, or null.
    virtual Object* revive(Object* dead) = 0;
};

class Vm {
public:
    Vm();
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    Heap heap;
    Object* global = nullptr;
    Object* object_proto = nullptr;
    Object* function_proto = nullptr;
    Object* array_proto = nullptr;
    Object* string_proto = nullptr;
    Object* number_proto = nullptr;
    Object* boolean_proto = nullptr;
    VmHooks* hooks = nullptr;
    int swf_version = 15;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    double fixed_time_ms = -1; // >= 0: getTimer() returns this (scripted runs are reproducible)
    std::mt19937 random{0x05ca1e};
    std::vector<std::string> errors; // script problems, for logs and tests
    // Also note calls and constructions of undefined names. Flash ignores them; most are the movies' own (code
    // shared between menus that reaches for members a menu lacks), the rest point at missing library.
    bool report_missing = false;
    std::vector<std::string> notes;
    void note(std::string message) {
        if (notes.size() < 1000) {
            notes.push_back(std::move(message));
        }
    }

    // ---- objects -----------------------------------------------------------------------------
    Object* make_object(Object* proto = nullptr);
    Object* make_array(std::vector<Value> items = {});
    Object* make_function(NativeFn fn, Object* prototype_object = nullptr);
    Object* make_script_function(const MovieDef* movie, uint32_t index, std::shared_ptr<Scope> scope,
                                 DisplayObject* target);
    void define(Object* o, std::string_view name, Value v, uint8_t flags = prop_dont_enum);
    void define_method(Object* o, std::string_view name, NativeFn fn);

    // ---- conversions (ECMA-262 3rd edition, with AVM1's SWF 7+ rules) --------------------------
    double to_number(const Value& v);
    String to_string(const Value& v);
    bool to_boolean(const Value& v);
    Value to_primitive(const Value& v, bool prefer_string = false);
    Object* to_object(const Value& v); // boxes primitives; null for null/undefined
    int32_t to_int32(const Value& v);
    static std::string number_to_string(double d, int radix = 10);
    std::string type_of(const Value& v);

    // ---- properties ----------------------------------------------------------------------------
    Value get(const Value& target, std::string_view name);
    void set(const Value& target, std::string_view name, const Value& v);
    bool has(Object* o, std::string_view name); // own or inherited (or a display property/child)
    bool remove(Object* o, std::string_view name);
    // Where `name` is found along the prototype chain (for `super`), or null.
    Object* holder(Object* o, std::string_view name);
    std::vector<std::string> enumerate(Object* o); // for..in order

    // ---- calls ---------------------------------------------------------------------------------
    // `home`: the object the method was found on (for `super`).
    Value call(const Value& fn, const Value& self, const std::vector<Value>& args, Object* home = nullptr);
    Value call_method(const Value& target, std::string_view name, const std::vector<Value>& args);
    Value construct(const Value& ctor, const std::vector<Value>& args);
    bool instance_of(const Value& v, Object* ctor);

    // A frame-action, init-action or clip-event block, on `target`'s timeline.
    void run_block(const MovieDef* movie, uint32_t function, DisplayObject* target);

    // Global-variable conveniences for built-ins.
    Value global_get(std::string_view name) { return get(Value::object(global), name); }

    void error(std::string message);

private:
    struct Frame;
    Value execute(Frame& f);
    Value get_variable(Frame& f, std::string_view name);
    void set_variable(Frame& f, std::string_view name, const Value& v);
    Value resolve_path(Frame& f, std::string_view path, bool& found);
    Object* make_super(const Value& self, Object* home);
    void install_core();

    int m_depth = 0; // call depth, against runaway recursion
};

// Comparisons (ECMA-262 11.9.3 / 11.9.6 / 11.8.5); `undefined` result of the relational comparison -> false.
bool abstract_equals(Vm& vm, const Value& a, const Value& b);
bool strict_equals(const Value& a, const Value& b);
bool abstract_less(Vm& vm, const Value& a, const Value& b, bool& undefined_result);

// Core library (builtins_core.cpp): Object, Function, Array, String, Number, Boolean, Math and the global
// functions (ASSetPropFlags, isNaN, parseInt, ...).
void install_core_library(Vm& vm);

} // namespace spud::ui::player
