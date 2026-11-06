#pragma once

// ActionScript 2 values and objects for the movie player (src/ui/player). Semantics follow the ActionScript 2.0
// Language Reference and the AVM1 chapter of Adobe's SWF 10 specification.
//
// Objects live in a Heap and are collected by mark and sweep between frames, never while script runs.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::ui::player {

class Object;
class Vm;
class DisplayObject;

using String = std::shared_ptr<const std::string>;
String make_string(std::string_view s);
const String& empty_string();

enum class Type : uint8_t { undefined, null, boolean, number, string, object };

struct Value {
    Type type = Type::undefined;
    bool b = false;
    double n = 0;
    String s;
    Object* o = nullptr;

    static Value undefined() { return {}; }
    static Value null() {
        Value v;
        v.type = Type::null;
        return v;
    }
    static Value boolean(bool x) {
        Value v;
        v.type = Type::boolean;
        v.b = x;
        return v;
    }
    static Value number(double x) {
        Value v;
        v.type = Type::number;
        v.n = x;
        return v;
    }
    static Value string(String x) {
        Value v;
        v.type = Type::string;
        v.s = std::move(x);
        return v;
    }
    static Value string(std::string_view x) { return string(make_string(x)); }
    static Value object(Object* x) {
        if (!x) {
            return null();
        }
        Value v;
        v.type = Type::object;
        v.o = x;
        return v;
    }

    bool is_undefined() const { return type == Type::undefined; }
    bool is_null() const { return type == Type::null; }
    bool is_null_or_undefined() const { return type == Type::undefined || type == Type::null; }
    bool is_object() const { return type == Type::object; }
    bool is_string() const { return type == Type::string; }
    bool is_number() const { return type == Type::number; }
    Object* object_or_null() const { return type == Type::object ? o : nullptr; }
};

enum PropFlag : uint8_t {
    prop_dont_enum = 1 << 0,
    prop_dont_delete = 1 << 1,
    prop_read_only = 1 << 2,
};

struct Property {
    Value value;
    Object* getter = nullptr; // addProperty
    Object* setter = nullptr;
    uint8_t flags = 0;
};

// Insertion-ordered properties with a hash index.
class PropertyMap {
public:
    Property* find(std::string_view key);
    const Property* find(std::string_view key) const;
    Property& insert(std::string_view key); // existing or new
    bool erase(std::string_view key);
    // In insertion order.
    template <typename F>
    void for_each(F&& f) const {
        for (const auto& e : m_entries) {
            if (e.alive) {
                f(e.key, e.prop);
            }
        }
    }
    size_t size() const { return m_index.size(); }

private:
    struct Hash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };
    struct Entry {
        std::string key;
        Property prop;
        bool alive = true;
    };
    void compact();

    std::vector<Entry> m_entries;
    std::unordered_map<std::string, size_t, Hash, std::equal_to<>> m_index;
    size_t m_dead = 0;
};

enum class ObjKind : uint8_t {
    object, function, array, clip, text_field, button, boolean, number, string, text_format, super, other
};

// What a native function gets: the VM, `this` and the arguments.
struct NativeCall {
    Vm& vm;
    Value self;
    const std::vector<Value>& args;
    Object* callee;
    Value arg(size_t i) const { return i < args.size() ? args[i] : Value{}; }
};
using NativeFn = std::function<Value(NativeCall&)>;

struct Scope; // captured scope chain (vm.hpp)

// A bytecode function: which movie and function, and what it closed over.
struct ScriptFunction {
    const void* movie = nullptr; // MovieDef
    uint32_t index = 0;
    std::shared_ptr<Scope> scope;
    DisplayObject* target = nullptr; // the timeline the function was defined on
};

class Object {
public:
    explicit Object(ObjKind k) : kind(k) {}
    virtual ~Object() = default;

    ObjKind kind;
    Object* proto = nullptr; // __proto__
    PropertyMap props;
    bool marked = false;

    // Functions
    std::unique_ptr<ScriptFunction> script;
    NativeFn native;
    NativeFn native_construct; // when called with `new`, if different
    bool is_function() const { return kind == ObjKind::function; }

    // Arrays
    std::vector<Value> elements;

    // Wrapped primitives (Boolean, Number, String objects)
    Value primitive;

    // Display objects (clips, text fields, buttons)
    DisplayObject* display = nullptr;
    // A removed clip: references to it resolve by this target path (AVM1 clip references are paths).
    std::string dead_path;

    // Super objects: the `this` they forward to, the prototype the running method was found on (its
    // __constructor__ is the base class) and the one above it, where super.method() looks.
    Value super_this;
    Object* super_home = nullptr;
    Object* super_proto = nullptr;

    // Native state of built-in classes (TextFormat, MovieClipLoader, ...), and GC tracing for it.
    std::shared_ptr<void> native_data;
    std::function<void(std::vector<Object*>&)> trace_native;
};

class Heap {
public:
    Object* make(ObjKind kind, Object* proto);
    size_t live() const { return m_objects.size(); }
    size_t allocated_since_collect() const { return m_since; }
    // Marks from `roots` (and everything they reach), frees the rest.
    void collect(const std::vector<Object*>& roots);

private:
    std::vector<std::unique_ptr<Object>> m_objects;
    size_t m_since = 0;
};

} // namespace spud::ui::player
