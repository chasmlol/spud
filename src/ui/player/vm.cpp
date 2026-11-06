#include "ui/player/vm.hpp"

#include "ui/player/movie.hpp"

#include "core/log.hpp"
#include "core/platform.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <set>

namespace spud::ui::player {

namespace {

constexpr int k_max_depth = 256;           // Flash's default recursion limit
constexpr size_t k_max_instructions = 20'000'000; // a runaway loop, not a script

// DefineFunction2 flags (SWF 10 spec, DefineFunction2), as the importer stores them (first byte low).
enum : uint16_t {
    f2_preload_this = 1 << 0,
    f2_suppress_this = 1 << 1,
    f2_preload_arguments = 1 << 2,
    f2_suppress_arguments = 1 << 3,
    f2_preload_super = 1 << 4,
    f2_suppress_super = 1 << 5,
    f2_preload_root = 1 << 6,
    f2_preload_parent = 1 << 7,
    f2_preload_global = 1 << 8,
};

// GetProperty / SetProperty indices (SWF spec, ActionGetProperty).
constexpr const char* k_property_names[] = {
    "_x",           "_y",       "_xscale",  "_yscale",  "_currentframe", "_totalframes", "_alpha",
    "_visible",     "_width",   "_height",  "_rotation", "_target",      "_framesloaded", "_name",
    "_droptarget",  "_url",     "_highquality", "_focusrect", "_soundbuftime", "_quality", "_xmouse",
    "_ymouse",
};

bool is_index(std::string_view s, size_t& out) {
    if (s.empty() || s.size() > 9) {
        return false;
    }
    size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + size_t(c - '0');
    }
    if (s.size() > 1 && s[0] == '0') {
        return false;
    }
    out = v;
    return true;
}

} // namespace

struct Vm::Frame {
    const MovieDef* movie = nullptr;
    const osu::Function* fn = nullptr;
    std::vector<Value> registers;
    std::vector<Value> stack;
    Value self;
    Object* activation = nullptr; // locals (null for blocks)
    std::shared_ptr<Scope> scope; // innermost first
    DisplayObject* target = nullptr;
    Object* callee = nullptr;
    Object* home = nullptr;
    const std::vector<Value>* args = nullptr;
};

Vm::Vm() { install_core(); }
Vm::~Vm() = default;

void Vm::error(std::string message) {
    log::debug("ui script: {}", message);
    if (errors.size() < 1000) {
        errors.push_back(std::move(message));
    }
}

// ---- objects -------------------------------------------------------------------------------------

Object* Vm::make_object(Object* proto) { return heap.make(ObjKind::object, proto ? proto : object_proto); }

Object* Vm::make_array(std::vector<Value> items) {
    Object* a = heap.make(ObjKind::array, array_proto);
    a->elements = std::move(items);
    return a;
}

Object* Vm::make_function(NativeFn fn, Object* prototype_object) {
    Object* f = heap.make(ObjKind::function, function_proto);
    f->native = std::move(fn);
    if (prototype_object) {
        define(f, "prototype", Value::object(prototype_object));
        define(prototype_object, "constructor", Value::object(f));
    }
    return f;
}

Object* Vm::make_script_function(const MovieDef* movie, uint32_t index, std::shared_ptr<Scope> scope,
                                 DisplayObject* target) {
    Object* f = heap.make(ObjKind::function, function_proto);
    f->script = std::make_unique<ScriptFunction>();
    f->script->movie = movie;
    f->script->index = index;
    f->script->scope = std::move(scope);
    f->script->target = target;
    Object* proto = make_object();
    define(f, "prototype", Value::object(proto));
    define(proto, "constructor", Value::object(f));
    return f;
}

void Vm::define(Object* o, std::string_view name, Value v, uint8_t flags) {
    Property& p = o->props.insert(name);
    p.value = std::move(v);
    p.getter = p.setter = nullptr;
    p.flags = flags;
}

void Vm::define_method(Object* o, std::string_view name, NativeFn fn) {
    define(o, name, Value::object(make_function(std::move(fn))));
}

void Vm::install_core() {
    object_proto = heap.make(ObjKind::object, nullptr);
    function_proto = heap.make(ObjKind::object, object_proto);
    global = heap.make(ObjKind::object, object_proto);
    array_proto = heap.make(ObjKind::object, object_proto);
    string_proto = heap.make(ObjKind::object, object_proto);
    number_proto = heap.make(ObjKind::object, object_proto);
    boolean_proto = heap.make(ObjKind::object, object_proto);
    install_core_library(*this);
}

// ---- conversions ---------------------------------------------------------------------------------

std::string Vm::number_to_string(double d, int radix) {
    if (std::isnan(d)) {
        return "NaN";
    }
    if (std::isinf(d)) {
        return d > 0 ? "Infinity" : "-Infinity";
    }
    if (d == 0) {
        return "0";
    }
    if (radix != 10) {
        // Integers only, as Flash's Number.toString(radix).
        int64_t v = static_cast<int64_t>(d);
        const bool neg = v < 0;
        uint64_t u = neg ? uint64_t(-v) : uint64_t(v);
        std::string s;
        do {
            const int digit = int(u % uint64_t(radix));
            s += char(digit < 10 ? '0' + digit : 'a' + digit - 10);
            u /= uint64_t(radix);
        } while (u);
        if (neg) {
            s += '-';
        }
        return {s.rbegin(), s.rend()};
    }
    // ECMA-262 9.8.1: the shortest round-tripping digits, at most 15 significant for AVM1.
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::general, 15);
    std::string s(buf, r.ptr);
    // to_chars general uses e+NN; Flash writes e+NN too, without leading zeros in the exponent.
    if (auto e = s.find('e'); e != std::string::npos) {
        std::string mant = s.substr(0, e), exp = s.substr(e + 1);
        const char sign = exp[0];
        exp = exp.substr(1);
        while (exp.size() > 1 && exp[0] == '0') {
            exp.erase(0, 1);
        }
        s = mant + "e" + sign + exp;
    }
    return s;
}

double Vm::to_number(const Value& v) {
    switch (v.type) {
    case Type::undefined:
        return std::numeric_limits<double>::quiet_NaN(); // SWF 7+
    case Type::null:
        return std::numeric_limits<double>::quiet_NaN();
    case Type::boolean:
        return v.b ? 1.0 : 0.0;
    case Type::number:
        return v.n;
    case Type::string: {
        std::string_view s = *v.s;
        const size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string_view::npos) {
            return s.empty() ? std::numeric_limits<double>::quiet_NaN() : 0.0;
        }
        s = s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            int64_t x = 0;
            auto r = std::from_chars(s.data() + 2, s.data() + s.size(), x, 16);
            return r.ptr == s.data() + s.size() ? double(x) : std::numeric_limits<double>::quiet_NaN();
        }
        if (s == "Infinity" || s == "+Infinity") {
            return std::numeric_limits<double>::infinity();
        }
        if (s == "-Infinity") {
            return -std::numeric_limits<double>::infinity();
        }
        double d = 0;
        const char* begin = s.data() + (s[0] == '+' ? 1 : 0);
        auto r = std::from_chars(begin, s.data() + s.size(), d);
        if (r.ec != std::errc() || r.ptr != s.data() + s.size()) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return d;
    }
    case Type::object:
        return to_number(to_primitive(v));
    }
    return 0;
}

String Vm::to_string(const Value& v) {
    switch (v.type) {
    case Type::undefined:
        return make_string("undefined");
    case Type::null:
        return make_string("null");
    case Type::boolean:
        return make_string(v.b ? "true" : "false");
    case Type::number:
        return make_string(number_to_string(v.n));
    case Type::string:
        return v.s;
    case Type::object: {
        const Value p = to_primitive(v, true);
        if (p.type == Type::object) {
            return make_string(p.o->is_function() ? "[type Function]" : "[object Object]");
        }
        return to_string(p);
    }
    }
    return empty_string();
}

bool Vm::to_boolean(const Value& v) {
    switch (v.type) {
    case Type::undefined:
    case Type::null:
        return false;
    case Type::boolean:
        return v.b;
    case Type::number:
        return !(v.n == 0 || std::isnan(v.n));
    case Type::string:
        return !v.s->empty(); // SWF 7+: non-empty strings are true
    case Type::object:
        return true;
    }
    return false;
}

Value Vm::to_primitive(const Value& v, bool prefer_string) {
    if (v.type != Type::object) {
        return v;
    }
    Object* o = v.o;
    if (o->kind == ObjKind::string || o->kind == ObjKind::number || o->kind == ObjKind::boolean) {
        if (!prefer_string || o->kind == ObjKind::string) {
            return o->primitive;
        }
    }
    const char* order[2] = {prefer_string ? "toString" : "valueOf", prefer_string ? "valueOf" : "toString"};
    for (const char* name : order) {
        const Value fn = get(v, name);
        if (fn.is_object() && fn.o->is_function()) {
            const Value r = call(fn, v, {});
            if (r.type != Type::object) {
                return r;
            }
        }
    }
    return v;
}

Object* Vm::to_object(const Value& v) {
    switch (v.type) {
    case Type::object:
        return v.o;
    case Type::string: {
        Object* o = heap.make(ObjKind::string, string_proto);
        o->primitive = v;
        return o;
    }
    case Type::number: {
        Object* o = heap.make(ObjKind::number, number_proto);
        o->primitive = v;
        return o;
    }
    case Type::boolean: {
        Object* o = heap.make(ObjKind::boolean, boolean_proto);
        o->primitive = v;
        return o;
    }
    default:
        return nullptr;
    }
}

int32_t Vm::to_int32(const Value& v) {
    const double d = to_number(v);
    if (std::isnan(d) || std::isinf(d)) {
        return 0;
    }
    const double t = std::fmod(std::trunc(d), 4294967296.0);
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(t < 0 ? t + 4294967296.0 : t)));
}

std::string Vm::type_of(const Value& v) {
    switch (v.type) {
    case Type::undefined: return "undefined";
    case Type::null: return "null";
    case Type::boolean: return "boolean";
    case Type::number: return "number";
    case Type::string: return "string";
    case Type::object:
        if (v.o->is_function()) {
            return "function";
        }
        if (v.o->kind == ObjKind::clip) {
            return "movieclip";
        }
        return "object";
    }
    return "undefined";
}

// ---- comparisons ---------------------------------------------------------------------------------

bool strict_equals(const Value& a, const Value& b) {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
    case Type::undefined:
    case Type::null: return true;
    case Type::boolean: return a.b == b.b;
    case Type::number: return a.n == b.n;
    case Type::string: return *a.s == *b.s;
    case Type::object: return a.o == b.o;
    }
    return false;
}

bool abstract_equals(Vm& vm, const Value& a, const Value& b) {
    if (a.type == b.type) {
        return strict_equals(a, b);
    }
    if (a.is_null_or_undefined() && b.is_null_or_undefined()) {
        return true;
    }
    if (a.is_null_or_undefined() || b.is_null_or_undefined()) {
        return false;
    }
    if (a.type == Type::number && b.type == Type::string) {
        return a.n == vm.to_number(b);
    }
    if (a.type == Type::string && b.type == Type::number) {
        return vm.to_number(a) == b.n;
    }
    if (a.type == Type::boolean) {
        return abstract_equals(vm, Value::number(a.b ? 1 : 0), b);
    }
    if (b.type == Type::boolean) {
        return abstract_equals(vm, a, Value::number(b.b ? 1 : 0));
    }
    if ((a.type == Type::number || a.type == Type::string) && b.type == Type::object) {
        return abstract_equals(vm, a, vm.to_primitive(b));
    }
    if (a.type == Type::object && (b.type == Type::number || b.type == Type::string)) {
        return abstract_equals(vm, vm.to_primitive(a), b);
    }
    return false;
}

bool abstract_less(Vm& vm, const Value& a, const Value& b, bool& undefined_result) {
    undefined_result = false;
    const Value pa = vm.to_primitive(a), pb = vm.to_primitive(b);
    if (pa.type == Type::string && pb.type == Type::string) {
        return *pa.s < *pb.s;
    }
    const double x = vm.to_number(pa), y = vm.to_number(pb);
    if (std::isnan(x) || std::isnan(y)) {
        undefined_result = true;
        return false;
    }
    return x < y;
}

// ---- properties ----------------------------------------------------------------------------------

Object* Vm::holder(Object* o, std::string_view name) {
    for (int guard = 0; o && guard < 256; o = o->proto, ++guard) {
        if (o->props.find(name)) {
            return o;
        }
    }
    return nullptr;
}

bool Vm::has(Object* o, std::string_view name) {
    if (!o) {
        return false;
    }
    if (o->kind == ObjKind::clip && !o->display && hooks) {
        o = hooks->revive(o);
        if (!o) {
            return false;
        }
    }
    if (holder(o, name)) {
        return true;
    }
    Value tmp;
    if (o->display && hooks && hooks->get_display(o, name, tmp)) {
        return true;
    }
    if (o->kind == ObjKind::array) {
        size_t i;
        return name == "length" || (is_index(name, i) && i < o->elements.size());
    }
    return false;
}

Value Vm::get(const Value& target, std::string_view name) {
    if (target.type == Type::string) {
        if (name == "length") {
            // Character count in UTF-16 units, as Flash counts.
            size_t n = 0;
            for (unsigned char c : *target.s) {
                n += (c & 0xC0) != 0x80;
            }
            return Value::number(double(n));
        }
        const Value r = get(Value::object(string_proto), name);
        return r;
    }
    if (target.type == Type::number) {
        return get(Value::object(number_proto), name);
    }
    if (target.type == Type::boolean) {
        return get(Value::object(boolean_proto), name);
    }
    if (target.type != Type::object) {
        return {};
    }
    Object* o = target.o;
    if (o->kind == ObjKind::clip && !o->display && hooks) {
        // A reference to a removed clip reaches whatever clip is at its path now, or nothing.
        Object* live = hooks->revive(o);
        return live ? get(Value::object(live), name) : Value{};
    }
    if (o->kind == ObjKind::super) {
        if (name == "__proto__") {
            return Value::object(o->super_proto);
        }
        Object* h = holder(o->super_proto, name);
        if (!h) {
            return {};
        }
        const Property* p = h->props.find(name);
        if (p->getter) {
            return call(Value::object(p->getter), o->super_this, {}, h);
        }
        return p->value;
    }
    if (name == "__proto__") {
        return Value::object(o->proto);
    }
    if (o->kind == ObjKind::array) {
        size_t i;
        if (name == "length") {
            return Value::number(double(o->elements.size()));
        }
        if (is_index(name, i)) {
            return i < o->elements.size() ? o->elements[i] : Value{};
        }
    }
    // Own properties first, then a display object's children and display properties, then the prototype chain.
    if (const Property* p = o->props.find(name)) {
        if (p->getter) {
            return call(Value::object(p->getter), target, {}, o);
        }
        return p->value;
    }
    if (o->display && hooks) {
        Value out;
        if (hooks->get_display(o, name, out)) {
            return out;
        }
    }
    for (Object* h = o->proto; h; h = h->proto) {
        if (const Property* p = h->props.find(name)) {
            if (p->getter) {
                return call(Value::object(p->getter), target, {}, h);
            }
            return p->value;
        }
    }
    return {};
}

void Vm::set(const Value& target, std::string_view name, const Value& v) {
    if (target.type != Type::object) {
        return; // properties on primitives are dropped
    }
    Object* o = target.o;
    if (o->kind == ObjKind::clip && !o->display && hooks) {
        if (Object* live = hooks->revive(o)) {
            set(Value::object(live), name, v);
        }
        return;
    }
    if (o->kind == ObjKind::super) {
        set(o->super_this, name, v);
        return;
    }
    if (name == "__proto__") {
        o->proto = v.object_or_null();
        return;
    }
    if (o->kind == ObjKind::array) {
        size_t i;
        if (name == "length") {
            const double n = to_number(v);
            if (n >= 0 && n < 1e8) {
                o->elements.resize(size_t(n));
            }
            return;
        }
        if (is_index(name, i)) {
            if (i < 100'000'000) {
                if (i >= o->elements.size()) {
                    o->elements.resize(i + 1);
                }
                o->elements[i] = v;
            }
            return;
        }
    }
    if (Property* p = o->props.find(name)) {
        if (p->setter) {
            call(Value::object(p->setter), target, {v}, o);
            return;
        }
        if (!p->getter && !(p->flags & prop_read_only)) {
            p->value = v;
        }
        return;
    }
    if (o->display && hooks && hooks->set_display(o, name, v)) {
        return;
    }
    // An inherited setter (addProperty on a prototype) applies to instances.
    for (Object* h = o->proto; h; h = h->proto) {
        if (Property* p = h->props.find(name)) {
            if (p->setter) {
                call(Value::object(p->setter), target, {v}, h);
                return;
            }
            if (p->getter || (p->flags & prop_read_only)) {
                return;
            }
            break;
        }
    }
    o->props.insert(name).value = v;
}

bool Vm::remove(Object* o, std::string_view name) {
    if (!o) {
        return false;
    }
    if (const Property* p = o->props.find(name)) {
        if (p->flags & prop_dont_delete) {
            return false;
        }
        return o->props.erase(name);
    }
    return false;
}

std::vector<std::string> Vm::enumerate(Object* o) {
    // Most recently added first, own properties before inherited ones (as Flash Player enumerates).
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (Object* h = o; h; h = h->proto) {
        std::vector<std::string> level;
        h->props.for_each([&](const std::string& k, const Property& p) {
            if (!seen.count(k)) {
                seen.insert(k);
                if (!(p.flags & prop_dont_enum)) {
                    level.push_back(k);
                }
            }
        });
        out.insert(out.end(), level.rbegin(), level.rend());
        if (h == o && o->kind == ObjKind::array) {
            for (size_t i = o->elements.size(); i-- > 0;) {
                out.push_back(std::to_string(i));
            }
        }
    }
    return out;
}

bool Vm::instance_of(const Value& v, Object* ctor) {
    if (!v.is_object() || !ctor) {
        return false;
    }
    const Value proto = get(Value::object(ctor), "prototype");
    if (!proto.is_object()) {
        return false;
    }
    for (Object* p = v.o->proto; p; p = p->proto) {
        if (p == proto.o) {
            return true;
        }
        // Interfaces implemented by a class in the chain.
        const Property* impl = p->props.find("__implements__");
        if (impl && impl->value.is_object()) {
            for (const auto& i : impl->value.o->elements) {
                if (i.is_object() && i.o == ctor) {
                    return true;
                }
            }
        }
    }
    return false;
}

// ---- calls ---------------------------------------------------------------------------------------

Object* Vm::make_super(const Value& self, Object* home) {
    Object* s = heap.make(ObjKind::super, nullptr);
    s->super_this = self;
    s->super_home = home;
    s->super_proto = home ? home->proto : nullptr;
    return s;
}

Value Vm::call(const Value& fn, const Value& self, const std::vector<Value>& args, Object* home) {
    if (!fn.is_object()) {
        return {};
    }
    Object* f = fn.o;
    if (f->kind == ObjKind::super) {
        // super(...): the base class constructor, which Extends stored on the subclass prototype.
        if (!f->super_home) {
            return {};
        }
        const Value ctor = get(Value::object(f->super_home), "__constructor__");
        return call(ctor, f->super_this, args, f->super_proto);
    }
    if (!f->is_function()) {
        return {};
    }
    if (m_depth >= k_max_depth) {
        error("256 levels of recursion were exceeded");
        return {};
    }
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& x) : d(x) { ++d; }
        ~DepthGuard() { --d; }
    } guard(m_depth);
    if (f->native) {
        NativeCall c{*this, self, args, f};
        return f->native(c);
    }
    if (!f->script) {
        return {};
    }
    const auto* movie = static_cast<const MovieDef*>(f->script->movie);
    const osu::Movie& m = movie->data();
    const osu::Function& def = m.functions[f->script->index];
    const uint16_t flags = def.flags;
    const bool f2 = def.kind == osu::FunctionKind::function2;
    if (!home && self.is_object() && (!f2 || !(flags & f2_suppress_super))) {
        // Called without the object it was found on (Function.apply / call: GameDelegate.receiveResponse applies
        // the movie's callbacks this way): super still means the base of the prototype in this's chain that holds
        // the function, as when the method is called by name (ContainerMenu.UpdateItemCardInfo's super call).
        for (Object* o = self.o; o && !home; o = o->proto) {
            o->props.for_each([&](std::string_view, const Property& p) {
                if (!home && p.value.is_object() && p.value.o == f) {
                    home = o;
                }
            });
        }
    }
    Frame fr;
    fr.movie = movie;
    fr.fn = &def;
    fr.self = self;
    fr.callee = f;
    fr.home = home;
    fr.args = &args;
    fr.target = f->script->target;
    fr.activation = heap.make(ObjKind::object, nullptr);
    fr.scope = std::make_shared<Scope>(Scope{fr.activation, f->script->scope});
    fr.registers.resize(f2 ? std::max<size_t>(def.registers, 1) : 4);
    Object* arguments = nullptr;
    if (!f2 || !(flags & f2_suppress_arguments)) {
        arguments = make_array(args);
        define(arguments, "callee", Value::object(f));
    }
    if (f2) {
        uint32_t reg = 1;
        auto preload = [&](uint16_t bit, const Value& v) {
            if ((flags & bit) && reg < fr.registers.size()) {
                fr.registers[reg++] = v;
            }
        };
        preload(f2_preload_this, self);
        preload(f2_preload_arguments, Value::object(arguments));
        if ((flags & f2_preload_super) && reg < fr.registers.size()) {
            fr.registers[reg++] = Value::object(make_super(self, home));
        }
        preload(f2_preload_root, Value::object(hooks ? hooks->root_object() : nullptr));
        if ((flags & f2_preload_parent) && reg < fr.registers.size()) {
            fr.registers[reg++] = Value::object(hooks ? hooks->parent_object(fr.target) : nullptr);
        }
        preload(f2_preload_global, Value::object(global));
        if (!(flags & f2_suppress_this) && !(flags & f2_preload_this)) {
            define(fr.activation, "this", self, 0);
        }
        if (arguments && !(flags & f2_preload_arguments)) {
            define(fr.activation, "arguments", Value::object(arguments), 0);
        }
        if (!(flags & f2_suppress_super) && !(flags & f2_preload_super)) {
            define(fr.activation, "super", Value::object(make_super(self, home)), 0);
        }
    } else {
        define(fr.activation, "this", self, 0);
        define(fr.activation, "arguments", Value::object(arguments), 0);
        define(fr.activation, "super", Value::object(make_super(self, home)), 0);
    }
    for (uint32_t i = 0; i < def.param_count; ++i) {
        const osu::Param& p = m.params[def.first_param + i];
        const Value a = i < args.size() ? args[i] : Value{};
        if (f2 && p.reg != 0 && p.reg < fr.registers.size()) {
            fr.registers[p.reg] = a;
        } else {
            define(fr.activation, movie->string(p.name), a, 0);
        }
    }
    return execute(fr);
}

namespace {

// SPUD_UI_TRACE=calls:<text>[|<text>...] logs each method call whose name or target path contains one of the
// texts, and whether the method exists.
const std::vector<std::string>& trace_calls() {
    static const std::vector<std::string> filters = [] {
        std::vector<std::string> out;
        const std::string v = platform::env("SPUD_UI_TRACE").value_or("");
        if (v.starts_with("calls:")) {
            for (size_t at = 6; at <= v.size();) {
                const size_t bar = std::min(v.find('|', at), v.size());
                if (bar > at) {
                    out.push_back(v.substr(at, bar - at));
                }
                at = bar + 1;
            }
        }
        return out;
    }();
    return filters;
}

} // namespace

Value Vm::call_method(const Value& target_in, std::string_view name, const std::vector<Value>& args) {
    if (const auto& filters = trace_calls(); !filters.empty()) {
        const std::string path = target_in.is_object() && target_in.o->display && hooks
                                     ? hooks->target_path(target_in.o->display)
                                     : std::string(type_of(target_in));
        if (std::ranges::any_of(filters, [&](const std::string& f) {
                return name.find(f) != std::string_view::npos || path.find(f) != std::string::npos;
            })) {
            const Value fn = get(target_in, name);
            std::string shown;
            for (const Value& a : args) {
                // Primitives as text; objects only by type (their toString could run script).
                shown += (shown.empty() ? "" : ", ") +
                         (a.is_object() ? std::string(type_of(a)) : *to_string(a));
            }
            log::info("ui call: {}.{}({}){}", path, name, shown,
                      fn.is_object() && fn.o->is_function() ? "" : " - no such method");
        }
    }
    Value target = target_in;
    if (target.is_object() && target.o->kind == ObjKind::clip && !target.o->display && hooks) {
        Object* live = hooks->revive(target.o);
        if (!live) {
            return {};
        }
        target = Value::object(live);
    }
    const Value fn = get(target, name);
    Object* home = target.is_object() ? holder(target.o->kind == ObjKind::super ? target.o->super_proto : target.o, name)
                                      : nullptr;
    const Value self = target.is_object() && target.o->kind == ObjKind::super ? target.o->super_this : target;
    return call(fn, self, args, home);
}

Value Vm::construct(const Value& ctor, const std::vector<Value>& args) {
    if (!ctor.is_object() || !ctor.o->is_function()) {
        return {};
    }
    Object* f = ctor.o;
    if (f->native_construct) {
        NativeCall c{*this, Value{}, args, f};
        return f->native_construct(c);
    }
    const Value proto = get(ctor, "prototype");
    Object* o = make_object(proto.is_object() ? proto.o : object_proto);
    define(o, "__constructor__", ctor);
    if (f->script && swf_version >= 6) {
        define(o, "constructor", ctor);
    }
    const Value r = call(ctor, Value::object(o), args, proto.object_or_null());
    if (f->native && r.is_object()) {
        return r; // built-in constructors make their own objects (Array, ...)
    }
    return Value::object(o);
}

void Vm::run_block(const MovieDef* movie, uint32_t function, DisplayObject* target) {
    const osu::Function& def = movie->data().functions[function];
    Frame fr;
    fr.movie = movie;
    fr.fn = &def;
    fr.target = target;
    Object* clip = hooks ? hooks->display_object(target) : nullptr;
    fr.self = Value::object(clip);
    fr.scope = std::make_shared<Scope>(Scope{clip, nullptr});
    fr.registers.resize(4);
    execute(fr);
}

// ---- variables -----------------------------------------------------------------------------------

Value Vm::resolve_path(Frame& f, std::string_view path, bool& found) {
    found = false;
    // Dot paths ("a.b.c") and slash paths ("/a/b:var") resolve from the current target.
    std::string p(path);
    std::replace(p.begin(), p.end(), '/', '.');
    std::string var;
    if (auto colon = p.rfind(':'); colon != std::string::npos) {
        var = p.substr(colon + 1);
        p = p.substr(0, colon);
    }
    Value cur;
    size_t start = 0;
    if (p.starts_with(".")) {
        cur = Value::object(hooks ? hooks->root_object() : nullptr);
        start = 1;
    }
    bool first = cur.is_undefined();
    while (start <= p.size()) {
        size_t dot = p.find('.', start);
        if (dot == std::string::npos) {
            dot = p.size();
        }
        const std::string part = p.substr(start, dot - start);
        start = dot + 1;
        if (part.empty()) {
            if (dot == p.size()) {
                break;
            }
            continue;
        }
        if (first) {
            cur = get_variable(f, part);
            first = false;
        } else if (part == "..") {
            cur = Value::object(cur.is_object() && cur.o->display && hooks ? hooks->parent_object(cur.o->display) : nullptr);
        } else {
            cur = get(cur, part);
        }
        if (cur.is_undefined()) {
            return {};
        }
        if (dot == p.size()) {
            break;
        }
    }
    if (!var.empty()) {
        cur = get(cur, var);
    }
    found = true;
    return cur;
}

Value Vm::get_variable(Frame& f, std::string_view name) {
    if (name.find_first_of(".:/") != std::string_view::npos && name != "." && name != "..") {
        bool found = false;
        Value v = resolve_path(f, name, found);
        if (found) {
            return v;
        }
    }
    if (name == "this") {
        if (f.activation) {
            if (const Property* p = f.activation->props.find("this")) {
                return p->value;
            }
        }
        return f.self;
    }
    if (name == "_global") {
        return Value::object(global);
    }
    if (name == "_root" || name == "_level0") {
        return Value::object(hooks ? hooks->root_object() : nullptr);
    }
    if (name == "_parent") {
        return Value::object(hooks ? hooks->parent_object(f.target) : nullptr);
    }
    for (Scope* s = f.scope.get(); s; s = s->parent.get()) {
        if (s->object && has(s->object, name)) {
            return get(Value::object(s->object), name);
        }
    }
    if (has(global, name)) {
        return get(Value::object(global), name);
    }
    return {};
}

void Vm::set_variable(Frame& f, std::string_view name, const Value& v) {
    if (name.find_first_of(".:/") != std::string_view::npos) {
        std::string p(name);
        std::replace(p.begin(), p.end(), '/', '.');
        size_t cut = p.rfind(':');
        if (cut == std::string::npos) {
            cut = p.rfind('.');
        }
        if (cut != std::string::npos && cut > 0) {
            bool found = false;
            const Value holder_v = resolve_path(f, p.substr(0, cut), found);
            if (found && holder_v.is_object()) {
                set(holder_v, p.substr(cut + 1), v);
            }
            return;
        }
    }
    for (Scope* s = f.scope.get(); s; s = s->parent.get()) {
        if (s->object && has(s->object, name)) {
            set(Value::object(s->object), name, v);
            return;
        }
    }
    // Undeclared variables land on the timeline the code runs on.
    Object* clip = hooks ? hooks->display_object(f.target) : nullptr;
    if (clip) {
        set(Value::object(clip), name, v);
    } else {
        set(Value::object(global), name, v);
    }
}

// ---- the interpreter -----------------------------------------------------------------------------

namespace {

// " in <movie> #function @instruction", for diagnostics.
template <typename F>
std::string where(const F& f, uint32_t pc) {
    return std::format(" in {} #{} @{}", f.movie->url(), uint32_t(f.fn - f.movie->data().functions.data()), pc - 1);
}

} // namespace

Value Vm::execute(Frame& f) {
    const osu::Movie& m = f.movie->data();
    const osu::Instruction* code = m.instructions.data() + f.fn->first_instruction;
    const uint32_t count = f.fn->instruction_count;
    auto& st = f.stack;
    auto pop = [&]() -> Value {
        if (st.empty()) {
            return {};
        }
        Value v = std::move(st.back());
        st.pop_back();
        return v;
    };
    auto pop_args = [&](std::vector<Value>& out) {
        const double n = to_number(pop());
        const size_t k = std::isnan(n) || n < 0 ? 0 : std::min<size_t>(size_t(n), st.size());
        out.clear();
        for (size_t i = 0; i < k; ++i) {
            out.push_back(pop());
        }
    };
    auto str = [&](const Value& v) { return to_string(v); };
    std::vector<Value> args;
    size_t executed = 0;
    for (uint32_t pc = 0; pc < count;) {
        if (++executed > k_max_instructions) {
            error("script stopped: too many instructions (an endless loop?)");
            return {};
        }
        const osu::Instruction& ins = code[pc++];
        switch (ins.op) {
        case 0x04: hooks->step(f.target, 1); break;
        case 0x05: hooks->step(f.target, -1); break;
        case 0x06: hooks->play(f.target, true); break;
        case 0x07: hooks->play(f.target, false); break;
        case 0x08: case 0x09: break; // quality, sounds
        case 0x0A: { const double b = to_number(pop()), a = to_number(pop()); st.push_back(Value::number(a + b)); break; }
        case 0x0B: { const double b = to_number(pop()), a = to_number(pop()); st.push_back(Value::number(a - b)); break; }
        case 0x0C: { const double b = to_number(pop()), a = to_number(pop()); st.push_back(Value::number(a * b)); break; }
        case 0x0D: { const double b = to_number(pop()), a = to_number(pop()); st.push_back(Value::number(a / b)); break; }
        case 0x0E: { const double b = to_number(pop()), a = to_number(pop()); st.push_back(Value::boolean(a == b)); break; }
        case 0x0F: { const double b = to_number(pop()), a = to_number(pop()); st.push_back(Value::boolean(a < b)); break; }
        case 0x10: { const bool b = to_boolean(pop()), a = to_boolean(pop()); st.push_back(Value::boolean(a && b)); break; }
        case 0x11: { const bool b = to_boolean(pop()), a = to_boolean(pop()); st.push_back(Value::boolean(a || b)); break; }
        case 0x12: st.push_back(Value::boolean(!to_boolean(pop()))); break;
        case 0x13: { const auto b = str(pop()), a = str(pop()); st.push_back(Value::boolean(*a == *b)); break; }
        case 0x14: case 0x31: { const Value s = Value::string(str(pop())); st.push_back(get(s, "length")); break; }
        case 0x15: case 0x35: { // StringExtract(string, index 1-based, count)
            const double n = to_number(pop()), i = to_number(pop());
            const auto s = str(pop());
            const size_t start = i < 1 || std::isnan(i) ? 0 : size_t(i - 1);
            st.push_back(Value::string(start < s->size() ? s->substr(start, std::isnan(n) || n < 0 ? std::string::npos : size_t(n)) : std::string()));
            break;
        }
        case 0x17: pop(); break;
        case 0x18: { const double d = to_number(pop()); st.push_back(Value::number(std::isnan(d) ? 0 : std::trunc(d))); break; }
        case 0x1C: { const auto n = str(pop()); st.push_back(get_variable(f, *n)); break; }
        case 0x1D: { const Value v = pop(); const auto n = str(pop()); set_variable(f, *n, v); break; }
        case 0x20: { // SetTarget2
            const Value t = pop();
            DisplayObject* d = t.is_object() && t.o->display ? t.o->display : hooks->resolve_target(f.target, *str(t));
            if (d) {
                f.target = d;
            }
            break;
        }
        case 0x21: { const auto b = str(pop()), a = str(pop()); st.push_back(Value::string(*a + *b)); break; }
        case 0x22: { // GetProperty(target, index)
            const double idx = to_number(pop());
            const Value t = pop();
            DisplayObject* d = t.is_object() && t.o->display ? t.o->display : (str(t)->empty() ? f.target : hooks->resolve_target(f.target, *str(t)));
            const size_t i = std::isnan(idx) ? 999 : size_t(idx);
            st.push_back(d && i < std::size(k_property_names) ? get(Value::object(hooks->display_object(d)), k_property_names[i]) : Value{});
            break;
        }
        case 0x23: { // SetProperty(target, index, value)
            const Value v = pop();
            const double idx = to_number(pop());
            const Value t = pop();
            DisplayObject* d = t.is_object() && t.o->display ? t.o->display : (str(t)->empty() ? f.target : hooks->resolve_target(f.target, *str(t)));
            const size_t i = std::isnan(idx) ? 999 : size_t(idx);
            if (d && i < std::size(k_property_names)) {
                set(Value::object(hooks->display_object(d)), k_property_names[i], v);
            }
            break;
        }
        case 0x26: log::debug("ui trace: {}", *str(pop())); break;
        case 0x29: { const auto b = str(pop()), a = str(pop()); st.push_back(Value::boolean(*a < *b)); break; }
        case 0x2B: { // CastOp(constructor, object)
            const Value obj = pop(), ctor = pop();
            st.push_back(ctor.is_object() && instance_of(obj, ctor.o) ? obj : Value::null());
            break;
        }
        case 0x2C: { // ImplementsOp(constructor, count, interfaces...)
            const Value ctor = pop();
            pop_args(args);
            if (ctor.is_object()) {
                const Value proto = get(ctor, "prototype");
                if (proto.is_object()) {
                    define(proto.o, "__implements__", Value::object(make_array(args)));
                }
            }
            break;
        }
        case 0x30: { const double n = to_number(pop()); st.push_back(Value::number(n > 0 ? double(random() % uint32_t(n)) : 0)); break; }
        case 0x32: case 0x36: { const auto s = str(pop()); st.push_back(Value::number(s->empty() ? 0 : double(uint8_t((*s)[0])))); break; }
        case 0x33: case 0x37: { const double c = to_number(pop()); st.push_back(Value::string(std::string(1, char(int(c))))); break; }
        case 0x34: {
            const double ms = fixed_time_ms >= 0 ? fixed_time_ms
                              : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            st.push_back(Value::number(std::floor(ms)));
            break;
        }
        case 0x3A: { const auto n = str(pop()); const Value o = pop(); st.push_back(Value::boolean(remove(o.object_or_null(), *n))); break; }
        case 0x3B: { // Delete2(name): from the scope chain
            const auto n = str(pop());
            bool done = false;
            for (Scope* s = f.scope.get(); s && !done; s = s->parent.get()) {
                if (s->object && s->object->props.find(*n)) {
                    done = remove(s->object, *n);
                }
            }
            st.push_back(Value::boolean(done));
            break;
        }
        case 0x3C: { // DefineLocal(name, value)
            const Value v = pop();
            const auto n = str(pop());
            if (f.activation) {
                f.activation->props.insert(*n).value = v;
            } else {
                set_variable(f, *n, v);
            }
            break;
        }
        case 0x3D: { // CallFunction(name, count, args...)
            const auto n = str(pop());
            pop_args(args);
            const Value fn = get_variable(f, *n);
            if (report_missing && !(fn.is_object() && fn.o->is_function())) {
                note(std::format("call of undefined function {}{}", *n, where(f, pc)));
            }
            const Value self = Value::object(hooks->display_object(f.target));
            st.push_back(call(fn, self, args));
            break;
        }
        case 0x3E: return pop();
        case 0x3F: { const double b = to_number(pop()), a = to_number(pop()); st.push_back(Value::number(std::fmod(a, b))); break; }
        case 0x40: { // NewObject(name, count, args...)
            const auto n = str(pop());
            pop_args(args);
            const Value ctor = get_variable(f, *n);
            if (report_missing && !(ctor.is_object() && ctor.o->is_function())) {
                note(std::format("new of undefined class {}{}", *n, where(f, pc)));
            }
            st.push_back(construct(ctor, args));
            break;
        }
        case 0x41: { // DefineLocal2(name)
            const auto n = str(pop());
            if (f.activation) {
                if (!f.activation->props.find(*n)) {
                    f.activation->props.insert(*n);
                }
            } else if (Object* clip = hooks->display_object(f.target); clip && !has(clip, *n)) {
                set(Value::object(clip), *n, Value{});
            }
            break;
        }
        case 0x42: { // InitArray(count, items...)
            pop_args(args);
            st.push_back(Value::object(make_array(args)));
            break;
        }
        case 0x43: { // InitObject(count, (value, name)...)
            const double n = to_number(pop());
            Object* o = make_object();
            const size_t k = std::isnan(n) || n < 0 ? 0 : size_t(n);
            for (size_t i = 0; i < k && st.size() >= 2; ++i) {
                const Value v = pop();
                const auto name = str(pop());
                set(Value::object(o), *name, v);
            }
            st.push_back(Value::object(o));
            break;
        }
        case 0x44: st.push_back(Value::string(type_of(pop()))); break;
        case 0x45: { // TargetPath
            const Value v = pop();
            st.push_back(v.is_object() && v.o->display ? Value::string(hooks->target_path(v.o->display)) : Value{});
            break;
        }
        case 0x46: case 0x55: { // Enumerate(name) / Enumerate2(object)
            const Value v = pop();
            Object* o = ins.op == 0x55 ? to_object(v) : to_object(get_variable(f, *str(v)));
            st.push_back(Value::null());
            if (o) {
                for (const auto& k : enumerate(o)) {
                    st.push_back(Value::string(k));
                }
            }
            break;
        }
        case 0x47: { // Add2
            const Value b = to_primitive(pop()), a = to_primitive(pop());
            if (a.is_string() || b.is_string()) {
                st.push_back(Value::string(*to_string(a) + *to_string(b)));
            } else {
                st.push_back(Value::number(to_number(a) + to_number(b)));
            }
            break;
        }
        case 0x48: { const Value b = pop(), a = pop(); bool u; const bool r = abstract_less(*this, a, b, u); st.push_back(u ? Value{} : Value::boolean(r)); break; }
        case 0x49: { const Value b = pop(), a = pop(); st.push_back(Value::boolean(abstract_equals(*this, a, b))); break; }
        case 0x4A: st.push_back(Value::number(to_number(pop()))); break;
        case 0x4B: st.push_back(Value::string(str(pop()))); break;
        case 0x4C: st.push_back(st.empty() ? Value{} : st.back()); break;
        case 0x4D: if (st.size() >= 2) { std::swap(st[st.size() - 1], st[st.size() - 2]); } break;
        case 0x4E: { const auto n = str(pop()); const Value o = pop(); st.push_back(get(o, *n)); break; }
        case 0x4F: { const Value v = pop(); const auto n = str(pop()); const Value o = pop(); set(o, *n, v); break; }
        case 0x50: st.push_back(Value::number(to_number(pop()) + 1)); break;
        case 0x51: st.push_back(Value::number(to_number(pop()) - 1)); break;
        case 0x52: { // CallMethod(name, object, count, args...)
            const Value n = pop(), o = pop();
            pop_args(args);
            if (n.is_undefined() || (n.is_string() && n.s->empty())) {
                st.push_back(call(o, Value{}, args));
            } else {
                if (report_missing) {
                    const Value fn = get(o, *str(n));
                    if (!(fn.is_object() && fn.o->is_function())) {
                        note(std::format("call of undefined method {} on {}{}", *str(n),
                                          o.is_object() && o.o->display ? hooks->target_path(o.o->display) : type_of(o),
                                          where(f, pc)));
                    }
                }
                st.push_back(call_method(o, *str(n), args));
            }
            break;
        }
        case 0x53: { // NewMethod(name, object, count, args...)
            const Value n = pop(), o = pop();
            pop_args(args);
            const Value ctor = n.is_undefined() || (n.is_string() && n.s->empty()) ? o : get(o, *str(n));
            if (report_missing && !(ctor.is_object() && ctor.o->is_function())) {
                note(std::format("new of undefined class {}{}", *str(n), where(f, pc)));
            }
            st.push_back(construct(ctor, args));
            break;
        }
        case 0x54: { // InstanceOf(object, constructor)
            const Value ctor = pop(), obj = pop();
            st.push_back(Value::boolean(ctor.is_object() && instance_of(obj, ctor.o)));
            break;
        }
        case 0x60: { const int32_t b = to_int32(pop()), a = to_int32(pop()); st.push_back(Value::number(a & b)); break; }
        case 0x61: { const int32_t b = to_int32(pop()), a = to_int32(pop()); st.push_back(Value::number(a | b)); break; }
        case 0x62: { const int32_t b = to_int32(pop()), a = to_int32(pop()); st.push_back(Value::number(a ^ b)); break; }
        case 0x63: { const int32_t b = to_int32(pop()), a = to_int32(pop()); st.push_back(Value::number(int32_t(uint32_t(a) << (b & 31)))); break; }
        case 0x64: { const int32_t b = to_int32(pop()), a = to_int32(pop()); st.push_back(Value::number(a >> (b & 31))); break; }
        case 0x65: { const int32_t b = to_int32(pop()), a = to_int32(pop()); st.push_back(Value::number(double(uint32_t(a) >> (b & 31)))); break; }
        case 0x66: { const Value b = pop(), a = pop(); st.push_back(Value::boolean(strict_equals(a, b))); break; }
        case 0x67: { const Value b = pop(), a = pop(); bool u; const bool r = abstract_less(*this, b, a, u); st.push_back(u ? Value{} : Value::boolean(r)); break; }
        case 0x68: { const auto b = str(pop()), a = str(pop()); st.push_back(Value::boolean(*a > *b)); break; }
        case 0x69: { // Extends(subclass, superclass)
            const Value super_ctor = pop(), sub = pop();
            if (sub.is_object() && super_ctor.is_object()) {
                const Value sp = get(super_ctor, "prototype");
                Object* proto = heap.make(ObjKind::object, sp.object_or_null());
                define(proto, "__constructor__", super_ctor);
                set(sub, "prototype", Value::object(proto));
            }
            break;
        }
        case 0x81: hooks->goto_frame(f.target, ins.arg, false); break;
        case 0x87: if (ins.arg < f.registers.size()) { f.registers[ins.arg] = st.empty() ? Value{} : st.back(); } break;
        case 0x8B: { // SetTarget(name)
            const std::string_view t = f.movie->string(ins.arg);
            DisplayObject* d = t.empty() ? nullptr : hooks->resolve_target(f.target, t);
            if (t.empty()) {
                // back to the original target: the clip of the block
            } else if (d) {
                f.target = d;
            }
            break;
        }
        case 0x8C: hooks->goto_label(f.target, f.movie->string(ins.arg), false); break;
        case 0x96: { // Push
            const osu::PushValue* v = m.values.data() + ins.arg;
            for (uint32_t i = 0; i < ins.count; ++i, ++v) {
                switch (v->kind) {
                case osu::ValueKind::string: st.push_back(Value::string(f.movie->string(v->u))); break;
                case osu::ValueKind::number: st.push_back(Value::number(v->number)); break;
                case osu::ValueKind::null: st.push_back(Value::null()); break;
                case osu::ValueKind::undefined: st.push_back(Value{}); break;
                case osu::ValueKind::reg: st.push_back(v->u < f.registers.size() ? f.registers[v->u] : Value{}); break;
                case osu::ValueKind::boolean: st.push_back(Value::boolean(v->u != 0)); break;
                }
            }
            break;
        }
        case 0x99: pc = ins.arg; break;
        case 0x9A: pop(); pop(); break; // GetURL2: no browser
        case 0x9B: case 0x8E: { // DefineFunction / DefineFunction2
            Object* fn = make_script_function(f.movie, ins.arg, f.scope, f.target);
            const osu::Function& child = m.functions[ins.arg];
            if (child.name == osu::k_none || f.movie->string(child.name).empty()) {
                st.push_back(Value::object(fn));
            } else if (f.activation) {
                f.activation->props.insert(f.movie->string(child.name)).value = Value::object(fn);
            } else {
                set_variable(f, f.movie->string(child.name), Value::object(fn));
            }
            break;
        }
        case 0x9D: if (to_boolean(pop())) { pc = ins.arg; } break;
        case 0x9E: pop(); break; // Call (frame actions by name): unused by the game's movies
        case 0x9F: { // GotoFrame2(frame or label, play flag, scene bias)
            const Value v = pop();
            if (v.is_string()) {
                const std::string& s = *v.s;
                const size_t colon = s.rfind(':');
                DisplayObject* d = f.target;
                std::string frame = s;
                if (colon != std::string::npos) {
                    d = hooks->resolve_target(f.target, s.substr(0, colon));
                    frame = s.substr(colon + 1);
                }
                double n = to_number(Value::string(frame));
                if (!std::isnan(n)) {
                    hooks->goto_frame(d, uint32_t(std::max(0.0, n - 1 + ins.arg)), ins.count != 0);
                } else {
                    hooks->goto_label(d, frame, ins.count != 0);
                }
            } else {
                const double n = to_number(v);
                if (!std::isnan(n)) {
                    hooks->goto_frame(f.target, uint32_t(std::max(0.0, n - 1 + ins.arg)), ins.count != 0);
                }
            }
            break;
        }
        default:
            error(std::format("unsupported action 0x{:02x}", ins.op));
            break;
        }
    }
    return {};
}

} // namespace spud::ui::player
