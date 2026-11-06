// The core ActionScript 2 library: Object, Function, Array, String, Number, Boolean, Math and the global
// functions, per the ActionScript 2.0 Language Reference (Adobe, Flash 8). Display classes are in
// builtins_display.cpp.

#include "ui/player/vm.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <sstream>

namespace spud::ui::player {

namespace {

// Strings are UTF-8 in the VM; ActionScript indexes UTF-16 code units.
std::u16string to_u16(std::string_view s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = uint8_t(s[i]);
        size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) {
            c = ((c & 7) << 18) | ((uint8_t(s[i + 1]) & 63) << 12) | ((uint8_t(s[i + 2]) & 63) << 6) | (uint8_t(s[i + 3]) & 63);
            n = 4;
        } else if (c >= 0xE0 && i + 2 < s.size()) {
            c = ((c & 15) << 12) | ((uint8_t(s[i + 1]) & 63) << 6) | (uint8_t(s[i + 2]) & 63);
            n = 3;
        } else if (c >= 0xC0 && i + 1 < s.size()) {
            c = ((c & 31) << 6) | (uint8_t(s[i + 1]) & 63);
            n = 2;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out += char16_t(0xD800 + (c >> 10));
            out += char16_t(0xDC00 + (c & 0x3FF));
        } else {
            out += char16_t(c);
        }
        i += n;
    }
    return out;
}

std::string to_u8(std::u16string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 63));
            out += char(0x80 | (c & 63));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 63));
            out += char(0x80 | ((c >> 6) & 63));
            out += char(0x80 | (c & 63));
        }
    }
    return out;
}

double integer_arg(Vm& vm, const Value& v, double fallback) {
    if (v.is_undefined()) {
        return fallback;
    }
    const double d = vm.to_number(v);
    return std::isnan(d) ? 0 : std::trunc(d);
}

std::u16string self_string(NativeCall& c) {
    const Value s = c.self.is_object() && c.self.o->kind == ObjKind::string ? c.self.o->primitive : c.self;
    return to_u16(*c.vm.to_string(s));
}

std::string join(Vm& vm, Object* a, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < a->elements.size(); ++i) {
        if (i) {
            out += sep;
        }
        if (!a->elements[i].is_null_or_undefined()) {
            out += *vm.to_string(a->elements[i]);
        }
    }
    return out;
}

// Array.sort / sortOn option flags (ActionScript 2.0 Language Reference, Array).
enum : int { sort_case_insensitive = 1, sort_descending = 2, sort_unique = 4, sort_return_indexed = 8, sort_numeric = 16 };

Value sort_array(Vm& vm, Object* a, const std::function<int(const Value&, const Value&)>& cmp, int options) {
    std::vector<size_t> order(a->elements.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        const int r = cmp(a->elements[x], a->elements[y]);
        return (options & sort_descending) ? r > 0 : r < 0;
    });
    if (options & sort_unique) {
        for (size_t i = 1; i < order.size(); ++i) {
            if (cmp(a->elements[order[i - 1]], a->elements[order[i]]) == 0) {
                return Value::number(0);
            }
        }
    }
    if (options & sort_return_indexed) {
        std::vector<Value> idx;
        for (size_t i : order) {
            idx.push_back(Value::number(double(i)));
        }
        return Value::object(vm.make_array(std::move(idx)));
    }
    std::vector<Value> sorted;
    for (size_t i : order) {
        sorted.push_back(a->elements[i]);
    }
    a->elements = std::move(sorted);
    return Value::object(a);
}

int compare_values(Vm& vm, const Value& x, const Value& y, int options) {
    if (options & sort_numeric) {
        const double a = vm.to_number(x), b = vm.to_number(y);
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    std::u16string a = to_u16(*vm.to_string(x)), b = to_u16(*vm.to_string(y));
    if (options & sort_case_insensitive) {
        for (auto* s : {&a, &b}) {
            for (auto& ch : *s) {
                if (ch < 128) {
                    ch = char16_t(std::tolower(int(ch)));
                }
            }
        }
    }
    return a < b ? -1 : (a > b ? 1 : 0);
}

} // namespace

void install_core_library(Vm& vm) {
    Object* g = vm.global;
    auto method = [&](Object* o, std::string_view name, NativeFn fn) { vm.define_method(o, name, std::move(fn)); };

    // ---- Object --------------------------------------------------------------------------------
    Object* object_ctor = vm.make_function([](NativeCall& c) -> Value {
        if (!c.args.empty() && !c.args[0].is_null_or_undefined()) {
            return Value::object(c.vm.to_object(c.args[0]));
        }
        if (c.self.is_object() && c.self.o->kind == ObjKind::object) {
            return c.self;
        }
        return Value::object(c.vm.make_object());
    });
    vm.define(object_ctor, "prototype", Value::object(vm.object_proto), prop_dont_enum | prop_dont_delete);
    vm.define(vm.object_proto, "constructor", Value::object(object_ctor));
    vm.define(g, "Object", Value::object(object_ctor));
    method(vm.object_proto, "toString", [](NativeCall& c) -> Value {
        return Value::string(c.self.is_object() && c.self.o->is_function() ? "[type Function]" : "[object Object]");
    });
    method(vm.object_proto, "valueOf", [](NativeCall& c) -> Value { return c.self; });
    method(vm.object_proto, "hasOwnProperty", [](NativeCall& c) -> Value {
        Object* o = c.self.object_or_null();
        const auto n = c.vm.to_string(c.arg(0));
        return Value::boolean(o && (o->props.find(*n) != nullptr));
    });
    method(vm.object_proto, "isPropertyEnumerable", [](NativeCall& c) -> Value {
        Object* o = c.self.object_or_null();
        const auto n = c.vm.to_string(c.arg(0));
        const Property* p = o ? o->props.find(*n) : nullptr;
        return Value::boolean(p && !(p->flags & prop_dont_enum));
    });
    method(vm.object_proto, "isPrototypeOf", [](NativeCall& c) -> Value {
        Object* proto = c.self.object_or_null();
        for (Object* o = c.arg(0).object_or_null(); o && proto; o = o->proto) {
            if (o->proto == proto) {
                return Value::boolean(true);
            }
        }
        return Value::boolean(false);
    });
    method(vm.object_proto, "addProperty", [](NativeCall& c) -> Value {
        Object* o = c.self.object_or_null();
        const auto n = c.vm.to_string(c.arg(0));
        Object* getter = c.arg(1).object_or_null();
        Object* setter = c.arg(2).object_or_null();
        if (!o || n->empty() || !getter || !getter->is_function()) {
            return Value::boolean(false);
        }
        Property& p = o->props.insert(*n);
        p.value = {};
        p.getter = getter;
        p.setter = setter && setter->is_function() ? setter : nullptr;
        return Value::boolean(true);
    });
    method(vm.object_proto, "watch", [](NativeCall&) -> Value { return Value::boolean(false); });
    method(vm.object_proto, "unwatch", [](NativeCall&) -> Value { return Value::boolean(false); });
    // Object.registerClass(symbol, class): the player looks the pair up when it creates a clip of that symbol.
    Object* registry = vm.make_object();
    vm.define(g, "__registeredClasses", Value::object(registry));
    vm.define_method(object_ctor, "registerClass", [registry](NativeCall& c) -> Value {
        const auto name = c.vm.to_string(c.arg(0));
        if (c.arg(1).is_null_or_undefined()) {
            c.vm.remove(registry, *name);
        } else {
            registry->props.insert(*name).value = c.arg(1);
        }
        return Value::boolean(true);
    });

    // ---- Function ------------------------------------------------------------------------------
    Object* function_ctor = vm.make_function([](NativeCall&) -> Value { return {}; });
    vm.define(function_ctor, "prototype", Value::object(vm.function_proto), prop_dont_enum | prop_dont_delete);
    vm.define(vm.function_proto, "constructor", Value::object(function_ctor));
    vm.define(g, "Function", Value::object(function_ctor));
    method(vm.function_proto, "call", [](NativeCall& c) -> Value {
        std::vector<Value> rest(c.args.size() > 1 ? c.args.begin() + 1 : c.args.end(), c.args.end());
        return c.vm.call(c.self, c.arg(0), rest);
    });
    method(vm.function_proto, "apply", [](NativeCall& c) -> Value {
        std::vector<Value> rest;
        if (Object* a = c.arg(1).object_or_null()) {
            if (a->kind == ObjKind::array) {
                rest = a->elements;
            } else {
                const double n = c.vm.to_number(c.vm.get(c.arg(1), "length"));
                for (size_t i = 0; i < size_t(std::isnan(n) ? 0 : n) && i < 4096; ++i) {
                    rest.push_back(c.vm.get(c.arg(1), std::to_string(i)));
                }
            }
        }
        return c.vm.call(c.self, c.arg(0), rest);
    });
    method(vm.function_proto, "toString", [](NativeCall&) -> Value { return Value::string("[type Function]"); });

    // ---- Array ---------------------------------------------------------------------------------
    Object* array_ctor = vm.make_function([](NativeCall& c) -> Value {
        std::vector<Value> items;
        if (c.args.size() == 1 && c.args[0].is_number()) {
            const double n = c.args[0].n;
            if (n >= 0 && n < 1e7) {
                items.resize(size_t(n));
            }
        } else {
            items = c.args;
        }
        return Value::object(c.vm.make_array(std::move(items)));
    });
    array_ctor->native_construct = array_ctor->native;
    vm.define(array_ctor, "prototype", Value::object(vm.array_proto), prop_dont_enum | prop_dont_delete);
    vm.define(vm.array_proto, "constructor", Value::object(array_ctor));
    vm.define(g, "Array", Value::object(array_ctor));
    for (auto [name, v] : {std::pair{"CASEINSENSITIVE", 1}, {"DESCENDING", 2}, {"UNIQUESORT", 4},
                           {"RETURNINDEXEDARRAY", 8}, {"NUMERIC", 16}}) {
        vm.define(array_ctor, name, Value::number(v));
    }
    auto self_array = [](NativeCall& c) -> Object* {
        Object* o = c.self.object_or_null();
        return o && o->kind == ObjKind::array ? o : nullptr;
    };
    method(vm.array_proto, "push", [self_array](NativeCall& c) -> Value {
        Object* a = self_array(c);
        if (!a) {
            return {};
        }
        a->elements.insert(a->elements.end(), c.args.begin(), c.args.end());
        return Value::number(double(a->elements.size()));
    });
    method(vm.array_proto, "pop", [self_array](NativeCall& c) -> Value {
        Object* a = self_array(c);
        if (!a || a->elements.empty()) {
            return {};
        }
        Value v = a->elements.back();
        a->elements.pop_back();
        return v;
    });
    method(vm.array_proto, "shift", [self_array](NativeCall& c) -> Value {
        Object* a = self_array(c);
        if (!a || a->elements.empty()) {
            return {};
        }
        Value v = a->elements.front();
        a->elements.erase(a->elements.begin());
        return v;
    });
    method(vm.array_proto, "unshift", [self_array](NativeCall& c) -> Value {
        Object* a = self_array(c);
        if (!a) {
            return {};
        }
        a->elements.insert(a->elements.begin(), c.args.begin(), c.args.end());
        return Value::number(double(a->elements.size()));
    });
    method(vm.array_proto, "slice", [self_array](NativeCall& c) -> Value {
        Object* a = self_array(c);
        if (!a) {
            return {};
        }
        const double n = double(a->elements.size());
        double s = integer_arg(c.vm, c.arg(0), 0), e = integer_arg(c.vm, c.arg(1), n);
        if (s < 0) s = std::max(0.0, n + s);
        if (e < 0) e = std::max(0.0, n + e);
        s = std::min(s, n);
        e = std::min(e, n);
        std::vector<Value> out;
        for (size_t i = size_t(s); i < size_t(e); ++i) {
            out.push_back(a->elements[i]);
        }
        return Value::object(c.vm.make_array(std::move(out)));
    });
    method(vm.array_proto, "splice", [self_array](NativeCall& c) -> Value {
        Object* a = self_array(c);
        if (!a || c.args.empty()) {
            return {};
        }
        const double n = double(a->elements.size());
        double s = integer_arg(c.vm, c.arg(0), 0);
        if (s < 0) s = std::max(0.0, n + s);
        s = std::min(s, n);
        double del = c.args.size() > 1 ? integer_arg(c.vm, c.arg(1), n - s) : n - s;
        del = std::clamp(del, 0.0, n - s);
        std::vector<Value> removed(a->elements.begin() + size_t(s), a->elements.begin() + size_t(s + del));
        a->elements.erase(a->elements.begin() + size_t(s), a->elements.begin() + size_t(s + del));
        if (c.args.size() > 2) {
            a->elements.insert(a->elements.begin() + size_t(s), c.args.begin() + 2, c.args.end());
        }
        return Value::object(c.vm.make_array(std::move(removed)));
    });
    method(vm.array_proto, "concat", [self_array](NativeCall& c) -> Value {
        Object* a = self_array(c);
        std::vector<Value> out = a ? a->elements : std::vector<Value>{};
        for (const auto& v : c.args) {
            if (v.is_object() && v.o->kind == ObjKind::array) {
                out.insert(out.end(), v.o->elements.begin(), v.o->elements.end());
            } else {
                out.push_back(v);
            }
        }
        return Value::object(c.vm.make_array(std::move(out)));
    });
    method(vm.array_proto, "join", [self_array](NativeCall& c) -> Value {
        Object* a = self_array(c);
        const std::string sep = c.arg(0).is_undefined() ? "," : *c.vm.to_string(c.arg(0));
        return Value::string(a ? join(c.vm, a, sep) : std::string());
    });
    method(vm.array_proto, "toString", [self_array](NativeCall& c) -> Value {
        Object* a = self_array(c);
        return Value::string(a ? join(c.vm, a, ",") : std::string());
    });
    method(vm.array_proto, "reverse", [self_array](NativeCall& c) -> Value {
        Object* a = self_array(c);
        if (a) {
            std::reverse(a->elements.begin(), a->elements.end());
        }
        return c.self;
    });
    method(vm.array_proto, "sort", [self_array](NativeCall& c) -> Value {
        Object* a = self_array(c);
        if (!a) {
            return {};
        }
        Value fn;
        int options = 0;
        if (c.arg(0).is_object() && c.arg(0).o->is_function()) {
            fn = c.arg(0);
            options = int(c.vm.to_number(c.arg(1)));
        } else if (!c.arg(0).is_undefined()) {
            options = int(c.vm.to_number(c.arg(0)));
        }
        Vm& vm2 = c.vm;
        if (fn.is_object()) {
            return sort_array(vm2, a, [&](const Value& x, const Value& y) {
                const double r = vm2.to_number(vm2.call(fn, Value{}, {x, y}));
                return r < 0 ? -1 : (r > 0 ? 1 : 0);
            }, options & ~0);
        }
        return sort_array(vm2, a, [&](const Value& x, const Value& y) { return compare_values(vm2, x, y, options); },
                          options);
    });
    method(vm.array_proto, "sortOn", [self_array](NativeCall& c) -> Value {
        Object* a = self_array(c);
        if (!a) {
            return {};
        }
        std::vector<std::string> fields;
        std::vector<int> field_options;
        if (Object* f = c.arg(0).object_or_null(); f && f->kind == ObjKind::array) {
            for (const auto& v : f->elements) {
                fields.push_back(*c.vm.to_string(v));
            }
        } else {
            fields.push_back(*c.vm.to_string(c.arg(0)));
        }
        int options = 0;
        if (Object* o = c.arg(1).object_or_null(); o && o->kind == ObjKind::array) {
            for (const auto& v : o->elements) {
                field_options.push_back(int(c.vm.to_number(v)));
            }
        } else if (!c.arg(1).is_undefined()) {
            options = int(c.vm.to_number(c.arg(1)));
        }
        Vm& vm2 = c.vm;
        return sort_array(vm2, a, [&](const Value& x, const Value& y) {
            for (size_t i = 0; i < fields.size(); ++i) {
                const int opt = i < field_options.size() ? field_options[i] : options;
                int r = compare_values(vm2, vm2.get(x, fields[i]), vm2.get(y, fields[i]), opt);
                if (i < field_options.size() && (opt & sort_descending)) {
                    r = -r;
                }
                if (r != 0) {
                    return r;
                }
            }
            return 0;
        }, field_options.empty() ? options : (options & ~sort_descending));
    });

    // ---- String --------------------------------------------------------------------------------
    Object* string_ctor = vm.make_function([](NativeCall& c) -> Value {
        return Value::string(c.args.empty() ? empty_string() : c.vm.to_string(c.args[0]));
    });
    string_ctor->native_construct = [](NativeCall& c) -> Value {
        Object* o = c.vm.to_object(Value::string(c.args.empty() ? empty_string() : c.vm.to_string(c.args[0])));
        return Value::object(o);
    };
    vm.define(string_ctor, "prototype", Value::object(vm.string_proto), prop_dont_enum | prop_dont_delete);
    vm.define(vm.string_proto, "constructor", Value::object(string_ctor));
    vm.define(g, "String", Value::object(string_ctor));
    vm.define_method(string_ctor, "fromCharCode", [](NativeCall& c) -> Value {
        std::u16string s;
        for (const auto& v : c.args) {
            s += char16_t(uint16_t(c.vm.to_int32(v)));
        }
        return Value::string(to_u8(s));
    });
    method(vm.string_proto, "toString", [](NativeCall& c) -> Value { return Value::string(to_u8(self_string(c))); });
    method(vm.string_proto, "valueOf", [](NativeCall& c) -> Value { return Value::string(to_u8(self_string(c))); });
    method(vm.string_proto, "charAt", [](NativeCall& c) -> Value {
        const auto s = self_string(c);
        const double i = integer_arg(c.vm, c.arg(0), 0);
        return Value::string(i >= 0 && i < double(s.size()) ? to_u8(s.substr(size_t(i), 1)) : std::string());
    });
    method(vm.string_proto, "charCodeAt", [](NativeCall& c) -> Value {
        const auto s = self_string(c);
        const double i = integer_arg(c.vm, c.arg(0), 0);
        return Value::number(i >= 0 && i < double(s.size()) ? double(s[size_t(i)]) : std::nan(""));
    });
    method(vm.string_proto, "concat", [](NativeCall& c) -> Value {
        std::string s = to_u8(self_string(c));
        for (const auto& v : c.args) {
            s += *c.vm.to_string(v);
        }
        return Value::string(s);
    });
    method(vm.string_proto, "indexOf", [](NativeCall& c) -> Value {
        const auto s = self_string(c), f = to_u16(*c.vm.to_string(c.arg(0)));
        const double from = std::max(0.0, integer_arg(c.vm, c.arg(1), 0));
        const size_t r = s.find(f, size_t(from));
        return Value::number(r == std::u16string::npos ? -1.0 : double(r));
    });
    method(vm.string_proto, "lastIndexOf", [](NativeCall& c) -> Value {
        const auto s = self_string(c), f = to_u16(*c.vm.to_string(c.arg(0)));
        const double from = integer_arg(c.vm, c.arg(1), double(s.size()));
        const size_t r = from < 0 ? std::u16string::npos : s.rfind(f, size_t(from));
        return Value::number(r == std::u16string::npos ? -1.0 : double(r));
    });
    method(vm.string_proto, "slice", [](NativeCall& c) -> Value {
        const auto s = self_string(c);
        const double n = double(s.size());
        double a = integer_arg(c.vm, c.arg(0), 0), b = integer_arg(c.vm, c.arg(1), n);
        if (a < 0) a = std::max(0.0, n + a);
        if (b < 0) b = std::max(0.0, n + b);
        a = std::min(a, n);
        b = std::min(b, n);
        return Value::string(b > a ? to_u8(s.substr(size_t(a), size_t(b - a))) : std::string());
    });
    method(vm.string_proto, "substring", [](NativeCall& c) -> Value {
        const auto s = self_string(c);
        const double n = double(s.size());
        double a = std::clamp(integer_arg(c.vm, c.arg(0), 0), 0.0, n);
        double b = std::clamp(integer_arg(c.vm, c.arg(1), n), 0.0, n);
        if (a > b) std::swap(a, b);
        return Value::string(to_u8(s.substr(size_t(a), size_t(b - a))));
    });
    method(vm.string_proto, "substr", [](NativeCall& c) -> Value {
        const auto s = self_string(c);
        const double n = double(s.size());
        double a = integer_arg(c.vm, c.arg(0), 0);
        if (a < 0) a = std::max(0.0, n + a);
        a = std::min(a, n);
        const double len = std::clamp(integer_arg(c.vm, c.arg(1), n - a), 0.0, n - a);
        return Value::string(to_u8(s.substr(size_t(a), size_t(len))));
    });
    method(vm.string_proto, "split", [](NativeCall& c) -> Value {
        const auto s = self_string(c);
        std::vector<Value> parts;
        if (c.arg(0).is_undefined()) {
            parts.push_back(Value::string(to_u8(s)));
        } else {
            const auto sep = to_u16(*c.vm.to_string(c.arg(0)));
            const double limit = c.arg(1).is_undefined() ? 1e9 : c.vm.to_number(c.arg(1));
            if (sep.empty()) {
                for (size_t i = 0; i < s.size() && double(parts.size()) < limit; ++i) {
                    parts.push_back(Value::string(to_u8(s.substr(i, 1))));
                }
            } else {
                size_t start = 0;
                for (;;) {
                    if (double(parts.size()) >= limit) {
                        break;
                    }
                    const size_t at = s.find(sep, start);
                    parts.push_back(Value::string(to_u8(s.substr(start, at == std::u16string::npos ? std::u16string::npos : at - start))));
                    if (at == std::u16string::npos) {
                        break;
                    }
                    start = at + sep.size();
                }
            }
        }
        return Value::object(c.vm.make_array(std::move(parts)));
    });
    auto change_case = [](bool upper) {
        return [upper](NativeCall& c) -> Value {
            auto s = self_string(c);
            for (auto& ch : s) {
                if (ch < 128) {
                    ch = char16_t(upper ? std::toupper(int(ch)) : std::tolower(int(ch)));
                } else if (upper && ch >= 0xE0 && ch <= 0xFE && ch != 0xF7) {
                    ch = char16_t(ch - 0x20); // Latin-1 letters
                } else if (!upper && ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) {
                    ch = char16_t(ch + 0x20);
                }
            }
            return Value::string(to_u8(s));
        };
    };
    method(vm.string_proto, "toUpperCase", change_case(true));
    method(vm.string_proto, "toLowerCase", change_case(false));

    // ---- Number, Boolean -------------------------------------------------------------------------
    Object* number_ctor = vm.make_function([](NativeCall& c) -> Value {
        return Value::number(c.args.empty() ? 0.0 : c.vm.to_number(c.args[0]));
    });
    number_ctor->native_construct = [](NativeCall& c) -> Value {
        return Value::object(c.vm.to_object(Value::number(c.args.empty() ? 0.0 : c.vm.to_number(c.args[0]))));
    };
    vm.define(number_ctor, "prototype", Value::object(vm.number_proto), prop_dont_enum | prop_dont_delete);
    vm.define(vm.number_proto, "constructor", Value::object(number_ctor));
    vm.define(g, "Number", Value::object(number_ctor));
    vm.define(number_ctor, "MAX_VALUE", Value::number(std::numeric_limits<double>::max()));
    vm.define(number_ctor, "MIN_VALUE", Value::number(std::numeric_limits<double>::denorm_min()));
    vm.define(number_ctor, "NaN", Value::number(std::nan("")));
    vm.define(number_ctor, "NEGATIVE_INFINITY", Value::number(-std::numeric_limits<double>::infinity()));
    vm.define(number_ctor, "POSITIVE_INFINITY", Value::number(std::numeric_limits<double>::infinity()));
    auto self_number = [](NativeCall& c) {
        const Value v = c.self.is_object() && c.self.o->kind == ObjKind::number ? c.self.o->primitive : c.self;
        return c.vm.to_number(v);
    };
    method(vm.number_proto, "toString", [self_number](NativeCall& c) -> Value {
        const int radix = c.arg(0).is_undefined() ? 10 : int(c.vm.to_number(c.arg(0)));
        return Value::string(Vm::number_to_string(self_number(c), radix >= 2 && radix <= 36 ? radix : 10));
    });
    method(vm.number_proto, "valueOf", [self_number](NativeCall& c) -> Value { return Value::number(self_number(c)); });

    Object* boolean_ctor = vm.make_function([](NativeCall& c) -> Value {
        return Value::boolean(!c.args.empty() && c.vm.to_boolean(c.args[0]));
    });
    boolean_ctor->native_construct = [](NativeCall& c) -> Value {
        return Value::object(c.vm.to_object(Value::boolean(!c.args.empty() && c.vm.to_boolean(c.args[0]))));
    };
    vm.define(boolean_ctor, "prototype", Value::object(vm.boolean_proto), prop_dont_enum | prop_dont_delete);
    vm.define(vm.boolean_proto, "constructor", Value::object(boolean_ctor));
    vm.define(g, "Boolean", Value::object(boolean_ctor));
    auto self_bool = [](NativeCall& c) {
        const Value v = c.self.is_object() && c.self.o->kind == ObjKind::boolean ? c.self.o->primitive : c.self;
        return c.vm.to_boolean(v);
    };
    method(vm.boolean_proto, "toString", [self_bool](NativeCall& c) -> Value { return Value::string(self_bool(c) ? "true" : "false"); });
    method(vm.boolean_proto, "valueOf", [self_bool](NativeCall& c) -> Value { return Value::boolean(self_bool(c)); });

    // ---- Math ------------------------------------------------------------------------------------
    Object* math = vm.make_object();
    vm.define(g, "Math", Value::object(math));
    auto unary = [&](std::string_view name, double (*fn)(double)) {
        vm.define_method(math, name, [fn](NativeCall& c) -> Value { return Value::number(fn(c.vm.to_number(c.arg(0)))); });
    };
    unary("abs", [](double x) { return std::abs(x); });
    unary("acos", [](double x) { return std::acos(x); });
    unary("asin", [](double x) { return std::asin(x); });
    unary("atan", [](double x) { return std::atan(x); });
    unary("ceil", [](double x) { return std::ceil(x); });
    unary("cos", [](double x) { return std::cos(x); });
    unary("exp", [](double x) { return std::exp(x); });
    unary("floor", [](double x) { return std::floor(x); });
    unary("log", [](double x) { return std::log(x); });
    unary("round", [](double x) { return std::floor(x + 0.5); });
    unary("sin", [](double x) { return std::sin(x); });
    unary("sqrt", [](double x) { return std::sqrt(x); });
    unary("tan", [](double x) { return std::tan(x); });
    vm.define_method(math, "atan2", [](NativeCall& c) -> Value {
        return Value::number(std::atan2(c.vm.to_number(c.arg(0)), c.vm.to_number(c.arg(1))));
    });
    vm.define_method(math, "pow", [](NativeCall& c) -> Value {
        return Value::number(std::pow(c.vm.to_number(c.arg(0)), c.vm.to_number(c.arg(1))));
    });
    vm.define_method(math, "max", [](NativeCall& c) -> Value {
        if (c.args.empty()) {
            return Value::number(-std::numeric_limits<double>::infinity());
        }
        double r = c.vm.to_number(c.args[0]);
        for (size_t i = 1; i < c.args.size(); ++i) {
            const double v = c.vm.to_number(c.args[i]);
            if (std::isnan(v) || std::isnan(r)) {
                return Value::number(std::nan(""));
            }
            r = std::max(r, v);
        }
        return Value::number(r);
    });
    vm.define_method(math, "min", [](NativeCall& c) -> Value {
        if (c.args.empty()) {
            return Value::number(std::numeric_limits<double>::infinity());
        }
        double r = c.vm.to_number(c.args[0]);
        for (size_t i = 1; i < c.args.size(); ++i) {
            const double v = c.vm.to_number(c.args[i]);
            if (std::isnan(v) || std::isnan(r)) {
                return Value::number(std::nan(""));
            }
            r = std::min(r, v);
        }
        return Value::number(r);
    });
    vm.define_method(math, "random", [](NativeCall& c) -> Value {
        return Value::number(double(c.vm.random() % 1000000u) / 1000000.0);
    });
    for (auto [name, v] : {std::pair{"E", std::numbers::e}, {"LN10", std::numbers::ln10}, {"LN2", std::numbers::ln2},
                           {"LOG10E", std::numbers::log10e}, {"LOG2E", std::numbers::log2e}, {"PI", std::numbers::pi},
                           {"SQRT1_2", 1.0 / std::numbers::sqrt2}, {"SQRT2", std::numbers::sqrt2}}) {
        vm.define(math, name, Value::number(v), prop_dont_enum | prop_dont_delete | prop_read_only);
    }

    // ---- global functions ------------------------------------------------------------------------
    vm.define(g, "NaN", Value::number(std::nan("")));
    vm.define(g, "Infinity", Value::number(std::numeric_limits<double>::infinity()));
    vm.define_method(g, "isNaN", [](NativeCall& c) -> Value { return Value::boolean(std::isnan(c.vm.to_number(c.arg(0)))); });
    vm.define_method(g, "isFinite", [](NativeCall& c) -> Value { return Value::boolean(std::isfinite(c.vm.to_number(c.arg(0)))); });
    vm.define_method(g, "parseFloat", [](NativeCall& c) -> Value {
        const std::string s = *c.vm.to_string(c.arg(0));
        const char* p = s.c_str();
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            ++p;
        }
        char* end = nullptr;
        const double d = std::strtod(p, &end);
        return Value::number(end == p ? std::nan("") : d);
    });
    vm.define_method(g, "parseInt", [](NativeCall& c) -> Value {
        std::string s = *c.vm.to_string(c.arg(0));
        size_t i = s.find_first_not_of(" \t\r\n");
        if (i == std::string::npos) {
            return Value::number(std::nan(""));
        }
        bool neg = false;
        if (s[i] == '-' || s[i] == '+') {
            neg = s[i] == '-';
            ++i;
        }
        int radix = c.arg(1).is_undefined() ? 0 : int(c.vm.to_number(c.arg(1)));
        if ((radix == 0 || radix == 16) && i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            radix = 16;
            i += 2;
        } else if (radix == 0 && i + 1 < s.size() && s[i] == '0') {
            radix = 8; // AS2: a leading 0 means octal
        }
        if (radix == 0) {
            radix = 10;
        }
        if (radix < 2 || radix > 36) {
            return Value::number(std::nan(""));
        }
        double v = 0;
        bool any = false;
        for (; i < s.size(); ++i) {
            const char ch = char(std::tolower(uint8_t(s[i])));
            const int d = ch >= '0' && ch <= '9' ? ch - '0' : (ch >= 'a' && ch <= 'z' ? ch - 'a' + 10 : 99);
            if (d >= radix) {
                break;
            }
            v = v * radix + d;
            any = true;
        }
        return Value::number(any ? (neg ? -v : v) : std::nan(""));
    });
    vm.define_method(g, "escape", [](NativeCall& c) -> Value {
        const std::string s = *c.vm.to_string(c.arg(0));
        std::string out;
        for (unsigned char ch : s) {
            if (std::isalnum(ch)) {
                out += char(ch);
            } else {
                out += std::format("%{:02X}", ch);
            }
        }
        return Value::string(out);
    });
    vm.define_method(g, "unescape", [](NativeCall& c) -> Value {
        const std::string s = *c.vm.to_string(c.arg(0));
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                out += s[i];
            }
        }
        return Value::string(out);
    });
    vm.define_method(g, "trace", [](NativeCall& c) -> Value {
        c.vm.error("trace: " + *c.vm.to_string(c.arg(0)));
        return {};
    });
    // ASSetPropFlags(object, names | null, set, clear): bit 1 DontEnum, 2 DontDelete, 4 ReadOnly.
    vm.define_method(g, "ASSetPropFlags", [](NativeCall& c) -> Value {
        Object* o = c.arg(0).object_or_null();
        if (!o) {
            return {};
        }
        const int set = c.vm.to_int32(c.arg(2)), clear = c.vm.to_int32(c.arg(3));
        std::vector<std::string> names;
        if (c.arg(1).is_null()) {
            o->props.for_each([&](const std::string& k, const Property&) { names.push_back(k); });
        } else if (Object* a = c.arg(1).object_or_null(); a && a->kind == ObjKind::array) {
            for (const auto& v : a->elements) {
                names.push_back(*c.vm.to_string(v));
            }
        } else {
            std::stringstream ss(*c.vm.to_string(c.arg(1)));
            for (std::string n; std::getline(ss, n, ',');) {
                names.push_back(n);
            }
        }
        for (const auto& n : names) {
            if (Property* p = o->props.find(n)) {
                p->flags = uint8_t((p->flags & ~clear) | set) & 7;
            }
        }
        return {};
    });
}

} // namespace spud::ui::player
