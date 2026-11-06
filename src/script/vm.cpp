#include "script/vm_internal.hpp"

#include "core/binary.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>

namespace spud::script {

namespace {

constexpr int k_slice = 100;           // instructions per dispatch (2.2)
constexpr size_t k_message_ring = 1024; // function messages (1.5)
constexpr size_t k_suspended_ring = 128;
constexpr size_t k_overstress = 100;
constexpr int64_t k_max_wait_ms = 86400000; // 3.3
constexpr int k_min_update_ms = 33;         // 3.4
constexpr float k_float_tolerance = 0.0001f;
constexpr size_t k_string_limit = 1023;

using Timepoint = std::chrono::steady_clock::time_point;

double elapsed_ms(Timepoint start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
    }
    return r;
}

uint64_t key(StringId state, StringId name) {
    return uint64_t{state} << 32 | name;
}

} // namespace

// ---- construction, strings, classes ---------------------------------------------------------------------------------

VirtualMachine::VirtualMachine(const oss::Scripts& scripts, Host& host, Settings settings)
    : m_scripts(scripts), m_host(host), m_settings(settings), m_queues(std::make_unique<Queues>()) {
    m_strings.emplace_back();
    m_string_ids.emplace("", 0);
    for (const oss::Script& s : m_scripts.scripts) {
        m_class_index.emplace(intern_offset(s.name), &s);
    }
}

VirtualMachine::~VirtualMachine() = default;

StringId VirtualMachine::intern(std::string_view s) {
    std::string k = lower(s);
    if (auto it = m_string_ids.find(k); it != m_string_ids.end()) {
        return it->second;
    }
    const StringId id = static_cast<StringId>(m_strings.size());
    m_strings.emplace_back(s);
    m_string_ids.emplace(std::move(k), id);
    return id;
}

StringId VirtualMachine::intern_offset(uint32_t offset) {
    if (offset == oss::k_none) {
        return 0;
    }
    if (auto it = m_offset_ids.find(offset); it != m_offset_ids.end()) {
        return it->second;
    }
    const StringId id = intern(m_scripts.string(offset));
    m_offset_ids.emplace(offset, id);
    return id;
}

bool Class::derives_from(const Class* other) const {
    for (const Class* c = this; c; c = c->parent) {
        if (c == other) {
            return true;
        }
    }
    return false;
}

Class* VirtualMachine::find_class(std::string_view name) {
    return find_class(intern(name));
}

Class* VirtualMachine::find_class(StringId name) {
    if (auto it = m_classes.find(name); it != m_classes.end()) {
        return it->second.get();
    }
    auto it = m_class_index.find(name);
    return it == m_class_index.end() ? nullptr : link(*it->second);
}

Value VirtualMachine::parse_type(StringId type) {
    std::string_view t = text(type);
    Value v;
    const bool array = t.ends_with("[]");
    if (array) {
        t.remove_suffix(2);
    }
    const std::string k = lower(t);
    Tag tag = Tag::object;
    if (k == "none" || k.empty()) {
        tag = Tag::none;
    } else if (k == "int") {
        tag = Tag::integer;
    } else if (k == "float") {
        tag = Tag::real;
    } else if (k == "string") {
        tag = Tag::string;
    } else if (k == "bool") {
        tag = Tag::boolean;
    }
    const Class* cls = tag == Tag::object ? find_class(t) : nullptr;
    if (array) {
        v.tag = Tag::array;
        v.element = tag;
        v.type = cls;
        return v;
    }
    v.tag = tag;
    v.type = cls;
    if (tag == Tag::object) {
        v.object = nullptr;
    }
    return v;
}

Function* VirtualMachine::make_function(Class* c, const oss::Function& f, StringId state) {
    auto fn = std::make_unique<Function>();
    fn->name = intern_offset(f.name);
    fn->state = state;
    fn->owner = c;
    fn->code = &f;
    fn->parameters = f.parameter_count;
    fn->global = (f.flags & oss::function_global) != 0;
    fn->native = (f.flags & oss::function_native) != 0;
    fn->return_default = parse_type(intern_offset(f.return_type));
    fn->slot_defaults.reserve(f.parameter_count + f.local_count);
    for (uint32_t i = 0; i < f.parameter_count + f.local_count; ++i) {
        fn->slot_defaults.push_back(parse_type(intern_offset(m_scripts.slots[f.first_slot + i].type)));
    }
    if (fn->native) {
        const uint64_t k = key(c->name, fn->name);
        if (auto it = m_queues->natives.find(k); it != m_queues->natives.end()) {
            fn->bound = it->second;
            const uint32_t flags = m_queues->native_flags[k];
            fn->callable_from_tasklets = (flags & native_tasklet) != 0;
            fn->latent = (flags & native_latent) != 0;
        }
    }
    Function* raw = fn.get();
    c->functions.emplace(key(state, raw->name), std::move(fn));
    return raw;
}

Class* VirtualMachine::link(const oss::Script& s) {
    const StringId name = intern_offset(s.name);
    auto owned = std::make_unique<Class>();
    Class* c = owned.get();
    m_classes.emplace(name, std::move(owned));
    c->name = name;
    c->script = &s;
    c->auto_state = intern_offset(s.auto_state);
    if (s.parent != oss::k_none) {
        c->parent = find_class(intern_offset(s.parent));
    }
    c->variable_base = c->parent ? c->parent->variable_count : 0;
    c->variable_count = c->variable_base + s.variable_count;
    for (uint32_t i = 0; i < s.variable_count; ++i) {
        const oss::Variable& v = m_scripts.variables[s.first_variable + i];
        Value d = default_of(parse_type(intern_offset(v.type)));
        switch (v.initial.kind) {
        case oss::OperandKind::integer: d.integer = static_cast<int32_t>(v.initial.value); break;
        case oss::OperandKind::real: std::memcpy(&d.real, &v.initial.value, 4); break;
        case oss::OperandKind::boolean: d.boolean = v.initial.value != 0; break;
        case oss::OperandKind::string: d.string = intern_offset(v.initial.value); break;
        default: break;
        }
        c->variable_defaults.push_back(d);
        c->variable_names.push_back(intern_offset(v.name));
    }
    for (uint32_t i = 0; i < s.state_count; ++i) {
        const oss::State& st = m_scripts.states[s.first_state + i];
        const StringId state = intern_offset(st.name);
        for (uint32_t f = 0; f < st.function_count; ++f) {
            make_function(c, m_scripts.functions[st.first_function + f], state);
        }
    }
    for (uint32_t i = 0; i < s.property_count; ++i) {
        const oss::Property& p = m_scripts.properties[s.first_property + i];
        Property prop;
        prop.type = parse_type(intern_offset(p.type));
        prop.read_only = !(p.flags & oss::property_write);
        auto accessor = [&](uint32_t index) -> const Function* {
            auto fn = std::make_unique<Function>();
            const oss::Function& f = m_scripts.functions[index];
            fn->name = intern_offset(f.name);
            fn->owner = c;
            fn->code = &f;
            fn->parameters = f.parameter_count;
            fn->return_default = parse_type(intern_offset(f.return_type));
            for (uint32_t k = 0; k < f.parameter_count + f.local_count; ++k) {
                fn->slot_defaults.push_back(parse_type(intern_offset(m_scripts.slots[f.first_slot + k].type)));
            }
            fn->accessor = true;
            c->accessors.push_back(std::move(fn));
            return c->accessors.back().get();
        };
        if (p.flags & oss::property_auto) {
            prop.automatic = true;
            prop.auto_variable = static_cast<int>(p.auto_variable);
            for (const bool setter : {false, true}) {
                auto fn = std::make_unique<Function>();
                fn->name = intern_offset(p.name);
                fn->owner = c;
                fn->auto_variable = prop.auto_variable;
                fn->auto_setter = setter;
                fn->parameters = setter ? 1 : 0;
                if (setter) {
                    fn->slot_defaults.push_back(prop.type);
                }
                fn->return_default = setter ? Value{} : prop.type;
                c->accessors.push_back(std::move(fn));
                (setter ? prop.setter : prop.getter) = c->accessors.back().get();
            }
        } else {
            if (p.getter != oss::k_none) {
                prop.getter = accessor(p.getter);
            }
            if (p.setter != oss::k_none) {
                prop.setter = accessor(p.setter);
            }
        }
        c->properties.emplace(intern_offset(p.name), std::move(prop));
    }
    return c;
}

void VirtualMachine::mark_stub(std::string_view script, std::string_view name, std::string full) {
    m_stubs[key(intern(script), intern(name))] = std::move(full);
}

// With how many call sites the game's scripts have for that name, most first; none that nothing calls.
std::vector<std::string> VirtualMachine::stub_natives() const {
    std::unordered_map<std::string, uint32_t> calls;
    for (const oss::Instruction& in : m_scripts.instructions) {
        const uint32_t at = in.op == oss::Op::call_method || in.op == oss::Op::call_parent ? 0 : in.op == oss::Op::call_static ? 1 : ~0u;
        if (at != ~0u && at < in.operand_count) {
            ++calls[lower(m_scripts.string(m_scripts.operands[in.first_operand + at].value))];
        }
    }
    std::vector<std::pair<uint32_t, std::string>> found;
    for (const auto& [k, name] : m_stubs) {
        const auto it = calls.find(lower(name.substr(name.find('.') + 1)));
        if (it != calls.end()) {
            found.emplace_back(it->second, name);
        }
    }
    std::ranges::sort(found, std::greater<>());
    std::vector<std::string> out;
    for (const auto& [count, name] : found) {
        out.push_back(std::format("{} {}", count, name));
    }
    return out;
}

void VirtualMachine::bind_native(std::string_view script, std::string_view name, NativeFunction fn, uint32_t flags) {
    const StringId cls = intern(script);
    const StringId id = intern(name);
    const uint64_t k = key(cls, id);
    m_stubs.erase(k);
    m_queues->natives[k] = fn;
    m_queues->native_flags[k] = flags;
    if (auto it = m_classes.find(cls); it != m_classes.end()) {
        for (auto& [fk, f] : it->second->functions) {
            if (f->name == id && f->native) {
                f->bound = fn;
                f->callable_from_tasklets = (flags & native_tasklet) != 0;
                f->latent = (flags & native_latent) != 0;
            }
        }
    }
}

bool VirtualMachine::has_native(std::string_view script, std::string_view name) {
    return m_queues->natives.contains(key(intern(script), intern(name)));
}

// 4.2: the state through the whole chain, then the empty state through the whole chain.
const Function* VirtualMachine::find_method(const Class* from, StringId state, StringId name) const {
    if (state != 0) {
        for (const Class* c = from; c; c = c->parent) {
            if (auto it = c->functions.find(key(state, name)); it != c->functions.end()) {
                return it->second.get();
            }
        }
    }
    for (const Class* c = from; c; c = c->parent) {
        if (auto it = c->functions.find(key(0, name)); it != c->functions.end()) {
            return it->second.get();
        }
    }
    return nullptr;
}

// ---- values ---------------------------------------------------------------------------------------------------------

Value VirtualMachine::default_of(const Value& type) const {
    Value v;
    v.tag = type.tag;
    v.element = type.element;
    v.type = type.type;
    if (type.tag == Tag::object) {
        v.object = nullptr;
    }
    return v;
}

bool VirtualMachine::is_true(const Value& v) const {
    switch (v.tag) {
    case Tag::none: return false;
    case Tag::object: return v.object != nullptr;
    case Tag::string: return v.string != 0 && !m_strings[v.string].empty();
    case Tag::integer: return v.integer != 0;
    case Tag::real: return std::fabs(v.real) >= k_float_tolerance;
    case Tag::boolean: return v.boolean;
    case Tag::array: return v.array && !v.array->values.empty();
    }
    return false;
}

std::string VirtualMachine::to_string(const Value& v) {
    switch (v.tag) {
    case Tag::none: return "None";
    case Tag::object:
        if (!v.object) {
            return "None";
        }
        return std::format("[{} <{}>]", text(v.object->type->name), m_host.handle_text(v.object->handle));
    case Tag::string: return std::string(text(v.string));
    case Tag::integer: return std::to_string(v.integer);
    case Tag::real: {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%f", v.real);
        return buffer;
    }
    case Tag::boolean: return v.boolean ? "True" : "False";
    case Tag::array: {
        if (!v.array || v.array->values.empty()) {
            return "[]";
        }
        std::string s = "[";
        for (size_t i = 0; i < v.array->values.size(); ++i) {
            const Value& e = v.array->values[i];
            std::string item = e.tag == Tag::string ? "\"" + std::string(text(e.string)) + "\"" : to_string(e);
            if (s.size() + item.size() + 5 > 1024) {
                s += "...";
                break;
            }
            s += (i ? ", " : "") + item;
        }
        return s + "]";
    }
    }
    return {};
}

Value VirtualMachine::cast_object(const Value& target, const Value& a) {
    Value none = default_of(target);
    if (a.tag != Tag::object || !a.object) {
        return none;
    }
    if (a.object->type->derives_from(target.type)) {
        return Value::of_object(a.object, target.type);
    }
    if (Object* o = object_for(a.object->handle, const_cast<Class*>(target.type))) {
        return Value::of_object(o, target.type);
    }
    return none;
}

// 2.5. A tag-None result means "Cannot cast".
Value VirtualMachine::cast(Stack& s, const Value& target, const Value& a) {
    if (target.same_tag(a)) {
        return a;
    }
    switch (target.tag) {
    case Tag::none: return {};
    case Tag::object: return cast_object(target, a);
    case Tag::string: {
        std::string t = to_string(a);
        if (t.size() > k_string_limit) {
            t.resize(k_string_limit);
        }
        return Value::of_string(intern(t));
    }
    case Tag::integer:
        switch (a.tag) {
        case Tag::string: return Value::of(static_cast<int32_t>(std::strtol(std::string(text(a.string)).c_str(), nullptr, 10)));
        case Tag::real:
            if (std::isnan(a.real) || a.real >= 2147483648.0f || a.real < -2147483648.0f) {
                return Value::of(INT32_MIN);
            }
            return Value::of(static_cast<int32_t>(a.real));
        case Tag::boolean: return Value::of(a.boolean ? 1 : 0);
        default: break;
        }
        break;
    case Tag::real:
        switch (a.tag) {
        case Tag::string: return Value::of(static_cast<float>(std::strtod(std::string(text(a.string)).c_str(), nullptr)));
        case Tag::integer: return Value::of(static_cast<float>(a.integer));
        case Tag::boolean: return Value::of(a.boolean ? 1.0f : 0.0f);
        default: break;
        }
        break;
    case Tag::boolean: return Value::of(is_true(a));
    default: break;
    }
    log_error(s.id, std::format("Cannot cast from {} to {}", to_string(a), to_string(target)));
    return {};
}

int VirtualMachine::compare(const Value& a, const Value& b) {
    switch (a.tag) {
    case Tag::integer: return static_cast<int32_t>(static_cast<uint32_t>(a.integer) - static_cast<uint32_t>(b.integer));
    case Tag::real: {
        const float d = a.real - b.real;
        return std::fabs(d) <= k_float_tolerance ? 0 : d < 0 ? -1 : 1;
    }
    case Tag::string: return _stricmp(std::string(text(a.string)).c_str(), std::string(text(b.string)).c_str());
    case Tag::boolean: return a.boolean == b.boolean ? 0 : -1;
    case Tag::object:
        if (a.object == b.object) {
            return 0;
        }
        if (!a.object || !b.object) {
            return -1;
        }
        return a.object->handle != k_no_handle && a.object->handle == b.object->handle ? 0 : -1;
    case Tag::array: {
        if (!a.array && !b.array) {
            return 0;
        }
        if (!a.array || !b.array) {
            return -1;
        }
        if (a.array->values.size() != b.array->values.size()) {
            return -1;
        }
        for (size_t i = 0; i < a.array->values.size(); ++i) {
            if (const int c = compare(a.array->values[i], b.array->values[i])) {
                return c;
            }
        }
        return 0;
    }
    default: return 0;
    }
}

// ---- operands -------------------------------------------------------------------------------------------------------

bool VirtualMachine::read(Stack& s, const oss::Operand& o, Value& out) {
    Frame& f = s.frames.back();
    switch (o.kind) {
    case oss::OperandKind::none:
    case oss::OperandKind::discard: out = {}; return true;
    case oss::OperandKind::slot: out = f.slots[o.value]; return true;
    case oss::OperandKind::member:
        if (!f.object) {
            return false;
        }
        out = f.object->variables[f.function->owner->variable_base + o.value];
        return true;
    case oss::OperandKind::self: out = f.self; return true;
    case oss::OperandKind::state:
        if (!f.object) {
            log_error(s.id, "Static functions cannot retrieve object state");
            return false;
        }
        out = Value::of_string(f.object->state);
        return true;
    case oss::OperandKind::string: out = Value::of_string(intern_offset(o.value)); return true;
    case oss::OperandKind::integer: out = Value::of(static_cast<int32_t>(o.value)); return true;
    case oss::OperandKind::real: {
        float v;
        std::memcpy(&v, &o.value, 4);
        out = Value::of(v);
        return true;
    }
    case oss::OperandKind::boolean: out = Value::of(o.value != 0); return true;
    case oss::OperandKind::name:
        if (const Value* inherited = inherited_variable(f.object, f.function->owner, o.value)) {
            out = *inherited;
            return true;
        }
        log_error(s.id, std::format("Argument variable \"{}\" was not successfully looked up", m_scripts.string(o.value)));
        out = {};
        return true;
    }
    return false;
}

// A child's code naming a parent's variable (Survival's needs read their base's auto properties that way): the importer
// knows one script at a time, so it left the name.
Value* VirtualMachine::inherited_variable(Object* object, const Class* owner, uint32_t name) {
    if (!object) {
        return nullptr;
    }
    const StringId id = intern_offset(name);
    for (const Class* c = owner->parent; c; c = c->parent) {
        for (size_t i = 0; i < c->variable_names.size(); ++i) {
            if (c->variable_names[i] == id) {
                return &object->variables[c->variable_base + i];
            }
        }
    }
    return nullptr;
}

Value* VirtualMachine::slot(Stack& s, const oss::Operand& o) {
    Frame& f = s.frames.back();
    switch (o.kind) {
    case oss::OperandKind::slot: return &f.slots[o.value];
    case oss::OperandKind::member:
        if (!f.object) {
            log_error(s.id, "Static functions cannot set member variables");
            return nullptr;
        }
        return &f.object->variables[f.function->owner->variable_base + o.value];
    case oss::OperandKind::name:
        if (Value* inherited = inherited_variable(f.object, f.function->owner, o.value)) {
            return inherited;
        }
        log_error(s.id, std::format("Attempting to manipulate variable \"{}\" was not successfully looked up",
                                    m_scripts.string(o.value)));
        return nullptr;
    default: return nullptr;
    }
}

// 2.4.
void VirtualMachine::store_into(Stack& s, Value& d, const Value& v, bool assign, StringId name) {
    if (d.same_tag(v)) {
        d = v;
    } else if (v.tag == Tag::none) {
        if (d.tag != Tag::object && d.tag != Tag::array) {
            m_host.log(1, std::format("warning: Assigning None to a non-object variable named \"{}\"", text(name)));
        }
        d = default_of(d);
    } else if (assign) {
        m_host.log(1, std::format("warning: Mismatched types assigning to variable named \"{}\" - assigning None", text(name)));
        d = default_of(d);
    } else {
        log_error(s.id, std::format("Mismatched types assigning to variable named \"{}\"", text(name)));
        d = v;
    }
}

void VirtualMachine::store(Stack& s, const oss::Operand& dest, const Value& v, bool assign) {
    Frame& f = s.frames.back();
    switch (dest.kind) {
    case oss::OperandKind::discard:
    case oss::OperandKind::none: return;
    case oss::OperandKind::self: log_error(s.id, "Self variable is read-only, cannot assign a value to it"); return;
    case oss::OperandKind::state:
        if (!f.object) {
            log_error(s.id, "Static functions cannot set state");
        } else if (v.tag != Tag::string) {
            log_error(s.id, "Cannot set an object to a non-string state");
        } else {
            f.object->state = v.string;
        }
        return;
    default: break;
    }
    if (Value* d = slot(s, dest)) {
        StringId name = 0;
        if (dest.kind == oss::OperandKind::slot && f.function->code) {
            name = intern_offset(m_scripts.slots[f.function->code->first_slot + dest.value].name);
        } else if (dest.kind == oss::OperandKind::member) {
            name = f.function->owner->variable_names[dest.value];
        }
        store_into(s, *d, v, assign, name);
    }
}

void VirtualMachine::store_return(Stack& s) {
    Frame& f = s.frames.back();
    const oss::Instruction& ins = m_scripts.instructions[f.function->code->first_instruction + f.ip];
    const oss::Operand* ops = &m_scripts.operands[ins.first_operand];
    switch (ins.op) {
    case oss::Op::call_method:
    case oss::Op::call_static:
    case oss::Op::prop_get: store(s, ops[2], s.ret, false); break;
    case oss::Op::call_parent: store(s, ops[1], s.ret, false); break;
    default: break;
    }
    ++f.ip;
}

// ---- stacks and queues ----------------------------------------------------------------------------------------------

Stack& VirtualMachine::new_stack(StackType type) {
    auto owned = std::make_unique<Stack>();
    Stack& s = *owned;
    s.id = m_next_stack++;
    s.type = type;
    m_stacks.emplace(s.id, std::move(owned));
    return s;
}

void VirtualMachine::post_call(Stack& s) {
    auto& q = *m_queues;
    if (q.messages.size() < k_message_ring) {
        q.messages.push_back({s.id, false, true});
    } else {
        q.overflow.push_back({s.id, false, true});
    }
}

void VirtualMachine::post_return(Stack& s, bool pop) {
    auto& q = *m_queues;
    if (q.messages.size() < k_message_ring) {
        q.messages.push_back({s.id, true, pop});
    } else {
        q.overflow.push_back({s.id, true, pop});
    }
}

void VirtualMachine::suspend(Stack& s, StackState state) {
    s.state = state;
    auto& side = m_queues->sides[m_queues->write];
    if (side.ring.size() < k_suspended_ring) {
        side.ring.push_back(s.id);
    } else {
        side.overflow.push_back(s.id);
    }
}

size_t VirtualMachine::suspended_stacks() const {
    return m_queues->sides[0].size() + m_queues->sides[1].size();
}

void VirtualMachine::ready(Stack& s) {
    if (!s.queued) {
        s.queued = true;
        m_ready.push_back(s.id);
    }
}

bool VirtualMachine::try_lock(Stack& s, Object* o) {
    if (o->lock_owner != 0 && o->lock_owner != s.id) {
        return false;
    }
    if (o->reset_pending) {
        return false;
    }
    o->lock_owner = s.id;
    s.held = o;
    return true;
}

void VirtualMachine::unlock(Stack& s) {
    if (s.held) {
        s.held->lock_owner = 0;
        s.held = nullptr;
    }
}

void VirtualMachine::finish_stack(Stack& s) {
    s.state = StackState::finished;
    unlock(s);
    const Value result = s.ret;
    auto callback = std::move(s.callback);
    const StackType type = s.type;
    Object* object = s.init_object;
    m_stacks.erase(s.id);
    if (callback) {
        callback(result);
    }
    if (type == StackType::property_init) {
        property_done(object);
    } else if (type == StackType::init_event) {
        object->initialised = true;
    }
}

uint32_t VirtualMachine::frame_bytes(const Function& f) const {
    return std::max<uint32_t>(static_cast<uint32_t>(16 * (f.slot_defaults.size() + 4)), m_settings.min_memory_page);
}

bool VirtualMachine::push_frame(Stack& s, const Function& f, Value self, std::span<const Value> args) {
    const size_t slots = f.slot_defaults.size();
    const uint32_t bytes = frame_bytes(f);
    if (m_stack_memory + bytes > m_settings.max_allocated_memory) {
        return false;
    }
    m_stack_memory += bytes;
    Frame frame;
    frame.function = &f;
    frame.object = self.tag == Tag::object ? self.object : nullptr;
    frame.self = std::move(self);
    if (frame.object) {
        frame.self.type = f.owner;
    }
    frame.bytes = bytes;
    frame.slots.reserve(slots);
    for (size_t i = 0; i < slots; ++i) {
        frame.slots.push_back(i < args.size() ? args[i] : default_of(f.slot_defaults[i]));
    }
    s.frames.push_back(std::move(frame));
    return true;
}

bool VirtualMachine::check_arguments(Stack& s, const Function& f, std::vector<Value>& args) {
    if (args.size() != f.parameters) {
        log_error(s.id, std::format("Incorrect number of arguments passed. Expected {}, got {}. ", f.parameters, args.size()));
        return false;
    }
    for (uint32_t i = 0; i < f.parameters; ++i) {
        const Value& type = f.slot_defaults[i];
        Value& a = args[i];
        if (a.tag == Tag::none) {
            if (type.tag == Tag::object || type.tag == Tag::array) {
                a = default_of(type);
            } else {
                m_host.log(1, std::format("warning: Passing NONE to non-object argument {}. ", i));
            }
        } else if (!a.same_tag(type)) {
            log_error(s.id, std::format("Type mismatch for argument {}. ", i));
            return false;
        }
    }
    return true;
}

VirtualMachine::Flow VirtualMachine::wait_for(Stack& s, bool from_tasklet, StackState state) {
    if (state == StackState::waiting_call) {
        if (from_tasklet) {
            post_call(s);
        } else {
            suspend(s, state);
        }
    } else if (from_tasklet) {
        post_return(s, state == StackState::waiting_return);
    } else {
        suspend(s, state);
    }
    return Flow::left;
}

// 2.7.
VirtualMachine::Flow VirtualMachine::perform_call(Stack& s, bool from_tasklet) {
    Request& r = s.pending;
    s.state = StackState::running;
    Object* target = r.self.tag == Tag::object ? r.self.object : nullptr;
    const bool is_static = r.kind == CallKind::global;
    auto fail = [&](std::string_view message) {
        log_error(s.id, message);
        s.ret = {};
        return return_from_frame(s, false, from_tasklet);
    };
    if (target && !target->valid) {
        return fail(std::format("{}.{}() being called on an invalid object, aborting function call",
                                text(target->type->name), text(r.name)));
    }
    if (target && (!target->constructed || (!target->initialised && s.init_object != target))) {
        return wait_for(s, from_tasklet, StackState::waiting_call);
    }
    if (!is_static && !target) {
        return fail(std::format("Cannot call {}() on a None object, aborting function call", text(r.name)));
    }
    if (target != s.held) {
        unlock(s);
        if (target && !try_lock(s, target)) {
            return wait_for(s, from_tasklet, StackState::waiting_call);
        }
    }

    const Function* f = nullptr;
    switch (r.kind) {
    case CallKind::method:
        f = find_method(target->type, target->state, r.name);
        if (!f) {
            return fail(std::format("Method {} not found on {}. Aborting call and returning None", text(r.name),
                                    text(target->type->name)));
        }
        break;
    case CallKind::parent:
        f = r.search ? find_method(r.search, target->state, r.name) : nullptr;
        if (!f) {
            return fail(std::format("Method {} not found on {}. Aborting call and returning None", text(r.name),
                                    text(target->type->name)));
        }
        break;
    case CallKind::global:
        if (r.search) {
            if (auto it = r.search->functions.find(key(0, r.name)); it != r.search->functions.end()) {
                f = it->second.get();
            }
        }
        if (!f) {
            return fail(std::format("Static function {} not found on object {}. Aborting call and returning None",
                                    text(r.name), r.search ? text(r.search->name) : "None"));
        }
        break;
    case CallKind::property_get:
    case CallKind::property_set: {
        const Property* p = nullptr;
        for (const Class* c = target->type; c && !p; c = c->parent) {
            if (auto it = c->properties.find(r.name); it != c->properties.end()) {
                p = &it->second;
            }
        }
        if (!p) {
            return fail(std::format("Property {} not found on {}. Aborting call and returning None", text(r.name),
                                    text(target->type->name)));
        }
        f = r.kind == CallKind::property_get ? p->getter : p->setter;
        if (!f) {
            return fail(std::format("Property {} for property {} not found on {}. Aborting call and returning None",
                                    r.kind == CallKind::property_get ? "getter" : "setter", text(r.name),
                                    text(target->type->name)));
        }
        break;
    }
    }

    if (f->native && !f->callable_from_tasklets && from_tasklet) {
        post_call(s);
        return Flow::left;
    }
    if (!check_arguments(s, *f, r.args)) {
        s.ret = {};
        return return_from_frame(s, false, from_tasklet);
    }
    if (f->auto_variable >= 0) {
        Value& variable = target->variables[f->owner->variable_base + f->auto_variable];
        s.ret = {};
        if (f->auto_setter) {
            store_into(s, variable, r.args[0], false, r.name);
        } else {
            s.ret = variable;
        }
        return return_from_frame(s, false, from_tasklet);
    }
    if (!push_frame(s, *f, r.self, r.args)) {
        suspend(s, StackState::waiting_memory);
        return Flow::left;
    }
    if (!f->native) {
        if (from_tasklet) {
            return Flow::entered;
        }
        ready(s);
        return Flow::left;
    }

    Frame& frame = s.frames.back();
    if (!f->bound) {
        log_error(s.id, std::format("error: Unbound native function \"{}\" called", text(f->name)));
        s.ret = {};
        return return_from_frame(s, true, from_tasklet);
    }
    NativeCall call{*this, s.id, frame.object, std::span<Value>(frame.slots.data(), f->parameters), default_of(f->return_default)};
    if (f->latent) {
        // 3.2.
        s.state = StackState::waiting_latent;
        s.in_latent = true;
        unlock(s);
        const StackId id = s.id;
        f->bound(call);
        if (!call.started) {
            log_error(id, std::format("Latent function {} did not successfully start - aborting call", text(f->name)));
            return_from_latent(id, {});
        }
        return Flow::left;
    }
    f->bound(call);
    s.ret = std::move(call.result);
    if (s.ret.tag == Tag::object && f->return_default.tag == Tag::object) {
        s.ret.type = f->return_default.type;
    }
    return return_from_frame(s, true, from_tasklet);
}

// 2.8.
VirtualMachine::Flow VirtualMachine::return_from_frame(Stack& s, bool pop, bool from_tasklet) {
    if (from_tasklet && pop && s.frames.size() == 1) {
        post_return(s, true);
        return Flow::left;
    }
    const size_t remaining = s.frames.size() - (pop ? 1 : 0);
    Object* caller = remaining > 0 ? s.frames[remaining - 1].object : nullptr;
    const StackState waiting = pop ? StackState::waiting_return : StackState::waiting_return_no_pop;
    if (caller && !caller->initialised && s.init_object != caller) {
        return wait_for(s, from_tasklet, waiting);
    }
    if (remaining > 0 && caller != s.held) {
        unlock(s);
        if (caller && !try_lock(s, caller)) {
            return wait_for(s, from_tasklet, waiting);
        }
    }
    if (pop) {
        m_stack_memory -= s.frames.back().bytes;
        s.frames.pop_back();
    }
    s.state = StackState::running;
    if (!s.frames.empty()) {
        if (from_tasklet) {
            return Flow::done;
        }
        s.resume = true;
        ready(s);
        return Flow::left;
    }
    finish_stack(s);
    return Flow::left;
}

void VirtualMachine::return_from_latent(StackId id, Value value) {
    auto it = m_stacks.find(id);
    if (it == m_stacks.end()) {
        m_host.log(2, std::format("error: Received a latent return for stack {}, but stack is no longer running - ignoring.", id));
        return;
    }
    Stack& s = *it->second;
    if (!s.in_latent) {
        m_host.log(2, std::format("error: Received a latent return for stack {}, but stack is not waiting on a latent - ignoring.", id));
        return;
    }
    s.in_latent = false;
    s.ret = std::move(value);
    return_from_frame(s, true, false);
}

// ---- the interpreter ------------------------------------------------------------------------------------------------

VirtualMachine::Flow VirtualMachine::do_call(Stack& s, const oss::Instruction& ins) {
    const oss::Operand* ops = &m_scripts.operands[ins.first_operand];
    Frame& f = s.frames.back();
    Request r;
    size_t first_arg = 0;
    switch (ins.op) {
    case oss::Op::call_method:
        r.kind = CallKind::method;
        r.name = intern_offset(ops[0].value);
        read(s, ops[1], r.self);
        first_arg = 3;
        break;
    case oss::Op::call_parent:
        r.kind = CallKind::parent;
        r.name = intern_offset(ops[0].value);
        r.self = f.self;
        r.search = f.function->owner->parent;
        first_arg = 2;
        break;
    case oss::Op::call_static:
        r.kind = CallKind::global;
        r.search = find_class(intern_offset(ops[0].value));
        r.name = intern_offset(ops[1].value);
        first_arg = 3;
        break;
    case oss::Op::prop_get:
        r.kind = CallKind::property_get;
        r.name = intern_offset(ops[0].value);
        read(s, ops[1], r.self);
        first_arg = 3;
        break;
    case oss::Op::prop_set:
        r.kind = CallKind::property_set;
        r.name = intern_offset(ops[0].value);
        read(s, ops[1], r.self);
        first_arg = 2;
        break;
    default: break;
    }
    r.args.resize(ins.operand_count - first_arg);
    for (size_t i = first_arg; i < ins.operand_count; ++i) {
        read(s, ops[i], r.args[i - first_arg]);
    }
    s.pending = std::move(r);
    return perform_call(s, true);
}

// 2.2, 2.3.
void VirtualMachine::run_slice(Stack& s) {
    if (s.resume) {
        s.resume = false;
        store_return(s);
    }
    for (int count = 0; count < k_slice; ++count) {
        Frame* f = &s.frames.back();
        const oss::Function& code = *f->function->code;
        if (f->ip >= code.instruction_count) {
            s.ret = {};
            const Flow flow = return_from_frame(s, true, true);
            if (flow == Flow::left) {
                return;
            }
            store_return(s);
            continue;
        }
        const oss::Instruction& ins = m_scripts.instructions[code.first_instruction + f->ip];
        const oss::Operand* ops = &m_scripts.operands[ins.first_operand];
        Value a, b;
        auto integers = [&](const char* message) {
            const bool ok = read(s, ops[1], a) && read(s, ops[2], b);
            if (ok && a.tag == Tag::integer && b.tag == Tag::integer) {
                return true;
            }
            if (ok) {
                log_error(s.id, message);
            }
            return false;
        };
        auto floats = [&](const char* message) {
            const bool ok = read(s, ops[1], a) && read(s, ops[2], b);
            if (ok && a.tag == Tag::real && b.tag == Tag::real) {
                return true;
            }
            if (ok) {
                log_error(s.id, message);
            }
            return false;
        };
        auto wrap = [](auto fn, int32_t x, int32_t y) {
            return static_cast<int32_t>(fn(static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
        };
        uint32_t next = f->ip + 1;
        switch (ins.op) {
        case oss::Op::nop: break;
        case oss::Op::iadd:
            if (integers("Cannot do integer addition on non-integers")) {
                store(s, ops[0], Value::of(wrap(std::plus<uint32_t>{}, a.integer, b.integer)), false);
            }
            break;
        case oss::Op::isub:
            if (integers("Cannot do integer subtraction on non-integers")) {
                store(s, ops[0], Value::of(wrap(std::minus<uint32_t>{}, a.integer, b.integer)), false);
            }
            break;
        case oss::Op::imul:
            if (integers("Cannot do integer multiplication on non-integers")) {
                store(s, ops[0], Value::of(wrap(std::multiplies<uint32_t>{}, a.integer, b.integer)), false);
            }
            break;
        case oss::Op::idiv:
        case oss::Op::imod:
            if (integers("Cannot do integer division on non-integers")) {
                if (b.integer == 0) {
                    log_error(s.id, "Cannot divide by zero");
                } else if (b.integer == -1) {
                    // The game's x86 divide faults on INT_MIN / -1 (2.3); a defined result here.
                    store(s, ops[0], Value::of(ins.op == oss::Op::idiv ? wrap(std::minus<uint32_t>{}, 0, a.integer) : 0), false);
                } else {
                    store(s, ops[0], Value::of(ins.op == oss::Op::idiv ? a.integer / b.integer : a.integer % b.integer), false);
                }
            }
            break;
        case oss::Op::fadd:
            if (floats("Cannot do float addition on non-floats")) {
                store(s, ops[0], Value::of(a.real + b.real), false);
            }
            break;
        case oss::Op::fsub:
            if (floats("Cannot do float subtraction on non-floats")) {
                store(s, ops[0], Value::of(a.real - b.real), false);
            }
            break;
        case oss::Op::fmul:
            if (floats("Cannot do float multiplication on non-floats")) {
                store(s, ops[0], Value::of(a.real * b.real), false);
            }
            break;
        case oss::Op::fdiv:
            if (floats("Cannot do float division on non-floats")) {
                if (std::fabs(b.real) < k_float_tolerance) {
                    log_error(s.id, "Cannot divide by zero");
                } else {
                    store(s, ops[0], Value::of(a.real / b.real), false);
                }
            }
            break;
        case oss::Op::bool_not:
            if (read(s, ops[1], a)) {
                store(s, ops[0], Value::of(!is_true(a)), false);
            }
            break;
        case oss::Op::ineg:
            if (read(s, ops[1], a)) {
                if (a.tag == Tag::integer) {
                    store(s, ops[0], Value::of(wrap(std::minus<uint32_t>{}, 0, a.integer)), false);
                } else {
                    log_error(s.id, "Cannot do integer negation on a non-integer");
                }
            }
            break;
        case oss::Op::fneg:
            if (read(s, ops[1], a)) {
                if (a.tag == Tag::real) {
                    store(s, ops[0], Value::of(-a.real), false);
                } else {
                    log_error(s.id, "Cannot do float negation on a non-float");
                }
            }
            break;
        case oss::Op::assign:
            if (!read(s, ops[1], a)) {
                a = {};
            }
            store(s, ops[0], a, true);
            break;
        case oss::Op::cast: {
            if (!read(s, ops[1], a)) {
                a = {};
            }
            if (ops[0].kind == oss::OperandKind::discard) {
                break;
            }
            if (Value* d = slot(s, ops[0])) {
                *d = cast(s, *d, a);
            }
            break;
        }
        case oss::Op::cmp_eq:
            if (read(s, ops[1], a) && read(s, ops[2], b)) {
                auto null = [](const Value& v) {
                    return v.tag == Tag::none || (v.tag == Tag::object && !v.object) || (v.tag == Tag::array && !v.array);
                };
                if (a.same_tag(b)) {
                    store(s, ops[0], Value::of(compare(a, b) == 0), false);
                } else if (a.tag == Tag::none || b.tag == Tag::none) {
                    store(s, ops[0], Value::of(null(a) && null(b)), false);
                } else {
                    log_error(s.id, "Cannot compare non-identical types");
                }
            }
            break;
        case oss::Op::cmp_lt:
        case oss::Op::cmp_le:
        case oss::Op::cmp_gt:
        case oss::Op::cmp_ge:
            if (read(s, ops[1], a) && read(s, ops[2], b)) {
                const bool less = ins.op == oss::Op::cmp_lt || ins.op == oss::Op::cmp_le;
                const char* word = less ? "less than" : "greater than";
                if (a.tag == Tag::none || b.tag == Tag::none) {
                    store(s, ops[0], Value::of(false), false);
                } else if (!a.same_tag(b)) {
                    log_error(s.id, "Cannot compare non-identical types");
                } else if (a.tag == Tag::array) {
                    log_error(s.id, std::format("Arrays cannot be {} another", word));
                } else if (a.tag == Tag::boolean) {
                    log_error(s.id, std::format("Boolean values cannot be {} another", word));
                } else if (a.tag == Tag::object) {
                    log_error(s.id, std::format("Object values cannot be {} another", word));
                } else {
                    const int c = compare(a, b);
                    const bool r = ins.op == oss::Op::cmp_lt   ? c < 0
                                   : ins.op == oss::Op::cmp_le ? c <= 0
                                   : ins.op == oss::Op::cmp_gt ? c > 0
                                                               : c >= 0;
                    store(s, ops[0], Value::of(r), false);
                }
            }
            break;
        case oss::Op::jmp: next = ops[0].value; break;
        case oss::Op::jmpt:
            if (read(s, ops[0], a) && is_true(a)) {
                next = ops[1].value;
            }
            break;
        case oss::Op::jmpf:
            if (read(s, ops[0], a) && !is_true(a)) {
                next = ops[1].value;
            }
            break;
        case oss::Op::call_method:
        case oss::Op::call_parent:
        case oss::Op::call_static:
        case oss::Op::prop_get:
        case oss::Op::prop_set: {
            const Flow flow = do_call(s, ins);
            if (flow == Flow::left) {
                return;
            }
            if (flow == Flow::done) {
                store_return(s);
            }
            continue;
        }
        case oss::Op::ret: {
            if (!read(s, ops[0], s.ret)) {
                s.ret = {};
            }
            const Flow flow = return_from_frame(s, true, true);
            if (flow == Flow::left) {
                return;
            }
            store_return(s);
            continue;
        }
        case oss::Op::strcat:
            if (read(s, ops[1], a) && read(s, ops[2], b)) {
                if (a.tag == Tag::string && b.tag == Tag::string) {
                    std::string t(text(a.string));
                    t += text(b.string);
                    store(s, ops[0], Value::of_string(intern(t)), false);
                } else {
                    log_error(s.id, "Cannot concatenate non-strings");
                }
            }
            break;
        case oss::Op::array_create: {
            if (!read(s, ops[1], a)) {
                break;
            }
            Value* d = slot(s, ops[0]);
            if (a.tag != Tag::integer) {
                log_error(s.id, "Array size must be an integer");
            } else if (a.integer <= 0) {
                log_error(s.id, std::format("Array size of {} must be greater then 0", a.integer));
            } else if (a.integer > 128) {
                log_error(s.id, std::format("Array size of {} is too large, must be 128 or smaller", a.integer));
            } else if (!d || d->tag != Tag::array) {
                log_error(s.id, "Cannot create an array into a non-array variable");
            } else {
                auto array = std::make_shared<Array>();
                array->element_default.tag = d->element;
                array->element_default.type = d->type;
                if (d->element == Tag::object) {
                    array->element_default.object = nullptr;
                }
                array->values.assign(static_cast<size_t>(a.integer), array->element_default);
                d->array = std::move(array);
            }
            break;
        }
        case oss::Op::array_length:
            if (read(s, ops[1], a)) {
                if (a.tag == Tag::array) {
                    store(s, ops[0], Value::of(a.array ? static_cast<int32_t>(a.array->values.size()) : 0), false);
                } else {
                    log_error(s.id, "Cannot get the length of of a non-array variable");
                }
            }
            break;
        case oss::Op::array_get:
        case oss::Op::array_set: {
            const bool get = ins.op == oss::Op::array_get;
            const oss::Operand& array = get ? ops[1] : ops[0];
            const oss::Operand& index = get ? ops[2] : ops[1];
            if (!read(s, array, a) || !read(s, index, b)) {
                break;
            }
            if (b.tag != Tag::integer) {
                log_error(s.id, "Array element index must be an integer");
            } else if (a.tag != Tag::array) {
                log_error(s.id, "Variable isn't an array and cannot be indexed");
                log_error(s.id, "Cannot access an element of a non-array variable");
            } else if (!a.array) {
                log_error(s.id, "Cannot access an element of a None array");
            } else if (b.integer < 0 || static_cast<size_t>(b.integer) >= a.array->values.size()) {
                log_error(s.id, std::format("Array index {} is out of range (0-{})", b.integer,
                                            static_cast<int64_t>(a.array->values.size()) - 1));
            } else if (get) {
                Value& element = a.array->values[static_cast<size_t>(b.integer)];
                if (ops[0].kind == oss::OperandKind::discard) {
                    break;
                }
                if (Value* d = slot(s, ops[0])) {
                    const Value r = cast(s, *d, element);
                    store(s, ops[0], r, false);
                }
            } else {
                Value v;
                if (read(s, ops[2], v)) {
                    store_into(s, a.array->values[static_cast<size_t>(b.integer)], v, false, 0);
                }
            }
            break;
        }
        case oss::Op::array_find:
        case oss::Op::array_rfind: {
            Value v, start;
            if (!read(s, ops[0], a) || !read(s, ops[2], v) || !read(s, ops[3], start)) {
                break;
            }
            if (start.tag != Tag::integer) {
                log_error(s.id, "Array start index must be an integer");
                break;
            }
            if (a.tag != Tag::array) {
                log_error(s.id, "Variable isn't an array and cannot be searched");
                break;
            }
            if (!a.array) {
                store(s, ops[1], Value::of(-1), false);
                break;
            }
            if (v.tag != Tag::none && !v.same_tag(a.array->element_default)) {
                log_error(s.id, "Type of element to find doesn't match array element type");
                break;
            }
            const auto size = static_cast<int32_t>(a.array->values.size());
            const bool forward = ins.op == oss::Op::array_find;
            int32_t i = start.integer;
            if (!forward && i == -1) {
                i = size - 1;
            }
            if (i < 0 || i >= size) {
                log_error(s.id, std::format("Array start index {} is out of range (0-{})", start.integer, size - 1));
                store(s, ops[1], Value::of(-1), false);
                break;
            }
            int32_t found = -1;
            for (; i >= 0 && i < size; i += forward ? 1 : -1) {
                const Value& e = a.array->values[static_cast<size_t>(i)];
                const bool match = v.tag == Tag::none ? (e.tag == Tag::object ? !e.object : !e.array) : compare(e, v) == 0;
                if (match) {
                    found = i;
                    break;
                }
            }
            store(s, ops[1], Value::of(found), false);
            break;
        }
        }
        s.frames.back().ip = next;
    }
    ready(s);
}

void VirtualMachine::dispatch() {
    if (m_frozen) {
        return;
    }
    std::deque<StackId> list;
    list.swap(m_ready);
    for (StackId id : list) {
        if (auto it = m_stacks.find(id); it != m_stacks.end()) {
            it->second->queued = false;
            run_slice(*it->second);
        }
    }
}

// ---- the update -----------------------------------------------------------------------------------------------------

int64_t VirtualMachine::clock(Clock which) const {
    return which == Clock::gameplay ? m_gameplay_clock : which == Clock::real ? m_real_clock : m_game_clock;
}

void VirtualMachine::wait(StackId stack, Clock which, int64_t target) {
    auto& heap = m_queues->waits[static_cast<int>(which)];
    heap.push_back({target, stack});
    std::push_heap(heap.begin(), heap.end(), std::greater<>{});
}

void VirtualMachine::register_update(Handle handle, float seconds, bool single) {
    const auto ms = static_cast<int64_t>(std::min<double>(seconds * 1000.0, static_cast<double>(k_max_wait_ms)));
    const int64_t interval = std::max<int64_t>(ms, k_min_update_ms);
    for (auto& u : m_queues->updates) {
        if (u.handle == handle) {
            u.interval = interval; // the single-shot flag stays (3.4)
            u.next = m_gameplay_clock + interval;
            return;
        }
    }
    m_queues->updates.push_back({handle, m_gameplay_clock + interval, interval, single});
}

void VirtualMachine::register_game_time_update(Handle handle, float hours, bool single) {
    const auto interval = static_cast<int64_t>(std::max(hours, 1.0f / 60.0f) * 41.666668f);
    for (auto& u : m_queues->game_updates) {
        if (u.handle == handle) {
            u.interval = interval;
            u.next = m_game_clock + interval;
            return;
        }
    }
    m_queues->game_updates.push_back({handle, m_game_clock + interval, interval, single});
}

void VirtualMachine::unregister_update(Handle handle) {
    std::erase_if(m_queues->updates, [&](const UpdateRegistration& u) { return u.handle == handle; });
}

void VirtualMachine::unregister_game_time_update(Handle handle) {
    std::erase_if(m_queues->game_updates, [&](const UpdateRegistration& u) { return u.handle == handle; });
}

void VirtualMachine::update_clocks(uint32_t delta_ms) {
    const int64_t delta = std::min<uint32_t>(delta_ms, 166);
    if (!m_frozen) {
        m_real_clock += delta;
        if (!m_host.in_menu_mode()) {
            m_gameplay_clock += delta;
        }
    }
    m_game_clock = static_cast<int64_t>(m_host.game_days_passed() * 1000.0);
}

void VirtualMachine::fire_timers() {
    for (int i = 0; i < 3; ++i) {
        auto& heap = m_queues->waits[i];
        const int64_t now = clock(static_cast<Clock>(i));
        while (!heap.empty() && heap.front().target <= now) {
            std::pop_heap(heap.begin(), heap.end(), std::greater<>{});
            const StackId id = heap.back().stack;
            heap.pop_back();
            return_from_latent(id, {});
        }
    }
    // Each list once per update, soonest first. A zero game-time interval would otherwise not end (3.4).
    auto fire = [this](std::vector<UpdateRegistration>& list, int64_t now, const char* event) {
        std::vector<UpdateRegistration> due;
        for (const auto& u : list) {
            if (u.next <= now) {
                due.push_back(u);
            }
        }
        std::ranges::stable_sort(due, {}, &UpdateRegistration::next);
        for (const auto& u : due) {
            if (u.single) {
                std::erase_if(list, [&](const UpdateRegistration& x) { return x.handle == u.handle; });
            } else {
                for (auto& x : list) {
                    if (x.handle == u.handle) {
                        x.next = now + x.interval;
                    }
                }
            }
            send_event(u.handle, event);
        }
    };
    fire(m_queues->updates, m_gameplay_clock, "OnUpdate");
    if (!m_host.player_sleeping_or_waiting()) {
        fire(m_queues->game_updates, m_game_clock, "OnUpdateGameTime");
    }
}

void VirtualMachine::update(uint32_t delta_ms) {
    const auto start = std::chrono::steady_clock::now();
    const double budget = m_settings.update_budget_ms * 0.9;
    update_clocks(delta_ms);
    if (!m_frozen) {
        fire_timers();
    }
    vm_update(std::max(0.1, 0.9 * (budget - elapsed_ms(start))));
}

void VirtualMachine::object_resets() {
    std::vector<Object*> keep;
    for (Object* o : m_reset_queue) {
        if (o->lock_owner != 0) {
            keep.push_back(o);
            continue;
        }
        o->reset_pending = false;
        o->initialised = false;
        init_variables(o);
        initial_state(o);
        apply_properties(o);
    }
    m_reset_queue = std::move(keep);
}

void VirtualMachine::function_messages(double budget_ms) {
    const auto start = std::chrono::steady_clock::now();
    auto& q = *m_queues;
    bool first = true;
    while (!q.messages.empty() || !q.overflow.empty()) {
        if (!first && elapsed_ms(start) >= budget_ms) {
            break;
        }
        first = false;
        Message m{};
        if (!q.messages.empty()) {
            m = q.messages.front();
            q.messages.pop_front();
        } else {
            m = q.overflow.back();
            q.overflow.pop_back();
        }
        auto it = m_stacks.find(m.stack);
        if (it == m_stacks.end()) {
            continue;
        }
        if (m.is_return) {
            return_from_frame(*it->second, m.pop, false);
        } else {
            perform_call(*it->second, false);
        }
    }
}

void VirtualMachine::retry(Stack& s) {
    switch (s.state) {
    case StackState::waiting_memory:
    case StackState::waiting_call:
    case StackState::retry_call: perform_call(s, false); break;
    case StackState::waiting_return: return_from_frame(s, true, false); break;
    case StackState::waiting_return_no_pop:
    case StackState::retry_return_no_pop: return_from_frame(s, false, false); break;
    default: break;
    }
}

void VirtualMachine::suspended_stage(double budget_ms) {
    auto start = std::chrono::steady_clock::now();
    auto& q = *m_queues;
    m_overstressed = suspended_stacks() > k_overstress;
    const int read_side = q.write;
    q.write = 1 - q.write;
    auto& side = q.sides[read_side];
    while (side.size() != 0 && elapsed_ms(start) < budget_ms) {
        StackId id = 0;
        if (!side.ring.empty()) {
            id = side.ring.front();
            side.ring.pop_front();
            if (m_overstressed) {
                start = std::chrono::steady_clock::now();
            }
        } else if (m_overstressed) {
            id = side.overflow.front();
            side.overflow.erase(side.overflow.begin());
        } else {
            id = side.overflow.back();
            side.overflow.pop_back();
        }
        if (auto it = m_stacks.find(id); it != m_stacks.end()) {
            retry(*it->second);
        }
    }
}

void VirtualMachine::vm_update(double budget) {
    if (m_frozen) {
        return;
    }
    object_resets();
    function_messages(budget * 0.45);
    suspended_stage(budget * 0.40);
}

// ---- binding and events ---------------------------------------------------------------------------------------------

std::span<Object* const> VirtualMachine::objects(Handle handle) const {
    auto it = m_bound.find(handle);
    return it == m_bound.end() ? std::span<Object* const>{} : std::span<Object* const>(it->second);
}

void VirtualMachine::init_variables(Object* o) {
    o->variables.resize(o->type->variable_count);
    std::vector<const Class*> chain;
    for (const Class* c = o->type; c; c = c->parent) {
        chain.push_back(c);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const Class* c = *it;
        std::copy(c->variable_defaults.begin(), c->variable_defaults.end(), o->variables.begin() + c->variable_base);
    }
}

void VirtualMachine::initial_state(Object* o) {
    o->state = 0;
    for (const Class* c = o->type; c; c = c->parent) {
        if (c->auto_state != 0) {
            o->state = c->auto_state;
            break;
        }
    }
}

Object* VirtualMachine::bind(Handle handle, Class* type) {
    auto owned = std::make_unique<Object>();
    Object* o = owned.get();
    m_objects.push_back(std::move(owned));
    o->type = type;
    o->handle = handle;
    init_variables(o);
    initial_state(o);
    o->constructed = true;
    m_bound[handle].push_back(o);
    return o;
}

Object* VirtualMachine::object_for(Handle handle, Class* type) {
    for (Object* o : objects(handle)) {
        if (o->type->derives_from(type)) {
            return o;
        }
    }
    if (handle == k_no_handle) {
        return nullptr;
    }
    // A form or reference nothing has touched yet: its record's scripts, and its base's on a reference.
    // (A magic effect's scripts belong to its running effects.)
    if (handle_kind(handle) == 0 && m_host.handle_class(handle) != "MagicEffect" && m_bound_bases.insert(handle).second) {
        const uint32_t form = handle_form(handle);
        bind_scripts(form);
        if (const uint32_t base = m_host.reference_base(form)) {
            bind_scripts(base, -1, handle);
        }
        for (Object* o : objects(handle)) {
            if (o->type->derives_from(type)) {
                return o;
            }
        }
    }
    Class* native = find_class(m_host.handle_class(handle));
    if (native && native->derives_from(type)) {
        Object* o = bind(handle, type);
        o->initialised = true;
        return o;
    }
    return nullptr;
}

Value VirtualMachine::form_value(uint32_t form_id, std::string_view type) {
    Class* c = find_class(type);
    Value none = Value::of_object(nullptr, c);
    if (!c || form_id == 0) {
        return none;
    }
    Object* o = object_for(form_handle(form_id), c);
    return o ? Value::of_object(o, c) : none;
}

const Value* VirtualMachine::find_variable(Handle handle, std::string_view name) {
    const StringId id = intern(name);
    for (Object* o : objects(handle)) {
        for (const Class* c = o->type; c; c = c->parent) {
            for (size_t i = 0; i < c->variable_names.size(); ++i) {
                if (c->variable_names[i] == id) {
                    return &o->variables[c->variable_base + i];
                }
            }
        }
    }
    return nullptr;
}

bool VirtualMachine::has_scripts(uint32_t form_id) const {
    const auto& list = m_scripts.attachments;
    const auto it = std::ranges::lower_bound(list, form_id, {}, &oss::Attachment::form_id);
    return it != list.end() && it->form_id == form_id;
}

void VirtualMachine::bind_scripts(uint32_t form_id) {
    if (m_bound_forms.emplace(form_id, true).second) {
        bind_scripts(form_id, -1, form_handle(form_id));
    }
}

void VirtualMachine::bind_scripts(uint32_t form_id, int32_t alias, Handle handle) {
    const auto& list = m_scripts.attachments;
    auto it = std::ranges::lower_bound(list, form_id, {}, &oss::Attachment::form_id);
    Class* native = find_class(m_host.handle_class(handle));
    for (; it != list.end() && it->form_id == form_id; ++it) {
        if (it->status == 3 || it->alias != alias) {
            continue;
        }
        Class* c = find_class(intern_offset(it->script));
        if (!c) {
            continue;
        }
        const auto bound = objects(handle);
        if (std::ranges::any_of(bound, [c](const Object* o) { return o->type == c; })) {
            continue;
        }
        // A script's native root only has to be something the handle's form is (a Form script on a MISC).
        bool fits = !native;
        for (const Class* p = c; p && !fits; p = p->parent) {
            fits = native->derives_from(p);
        }
        if (!fits && !c->derives_from(native)) {
            m_host.log(2, std::format("error: Unable to bind script {} to {} because their base types do not match",
                                      text(c->name), m_host.handle_text(handle)));
            continue;
        }
        Object* o = bind(handle, c);
        o->attachment = static_cast<uint32_t>(it - list.begin());
        apply_properties(o);
    }
}

Value VirtualMachine::value_of(const oss::AttachedValue& v, const Value& type) {
    switch (v.kind) {
    case oss::ValueKind::object:
        if (v.value != 0 && type.tag == Tag::object && type.type) {
            // An alias property names the quest and the alias; the class says which kind.
            Handle h = form_handle(v.value);
            if (v.alias >= 0) {
                h = alias_handle(v.value, static_cast<uint32_t>(v.alias), type.type->derives_from(find_class("LocationAlias")));
            }
            if (Object* o = object_for(h, const_cast<Class*>(type.type))) {
                return Value::of_object(o, type.type);
            }
        }
        return default_of(type);
    case oss::ValueKind::string: return Value::of_string(intern_offset(v.value));
    case oss::ValueKind::integer: return Value::of(static_cast<int32_t>(v.value));
    case oss::ValueKind::real: {
        float f;
        std::memcpy(&f, &v.value, 4);
        return Value::of(f);
    }
    case oss::ValueKind::boolean: return Value::of(v.value != 0);
    default: return {};
    }
}

// 5.2.
void VirtualMachine::apply_properties(Object* o) {
    if (o->attachment == oss::k_none) {
        queue_on_init(o);
        return;
    }
    const oss::Attachment& a = m_scripts.attachments[o->attachment];
    // A reference's script keeps what its base's has unless it says otherwise: the base's first.
    const oss::Attachment* inherited = nullptr;
    if (const uint32_t base = m_host.reference_base(a.form_id); base && a.alias == -1) {
        const auto& list = m_scripts.attachments;
        for (auto it = std::ranges::lower_bound(list, base, {}, &oss::Attachment::form_id); it != list.end() && it->form_id == base; ++it) {
            if (it->script == a.script && it->alias == -1) {
                inherited = &*it;
            }
        }
    }
    const uint32_t own = a.property_count, base_count = inherited ? inherited->property_count : 0;
    o->properties_left = own + base_count + 1; // one held until the loop ends, so OnInit waits for it
    for (uint32_t i = 0; i < own + base_count; ++i) {
        const oss::AttachedProperty& ap = i < base_count ? m_scripts.attached_properties[inherited->first_property + i]
                                                         : m_scripts.attached_properties[a.first_property + i - base_count];
        const StringId name = intern_offset(ap.name);
        const Property* p = nullptr;
        for (const Class* c = o->type; c && !p; c = c->parent) {
            if (auto it = c->properties.find(name); it != c->properties.end()) {
                p = &it->second;
                if (p->automatic) {
                    // The owning class gives the variable's place.
                    Value value;
                    if (ap.array) {
                        value = default_of(p->type);
                        value.array = std::make_shared<Array>();
                        value.array->element_default.tag = p->type.element;
                        value.array->element_default.type = p->type.type;
                        Value element = value.array->element_default;
                        element.element = Tag::none;
                        for (uint32_t k = 0; k < ap.value_count; ++k) {
                            value.array->values.push_back(value_of(m_scripts.attached_values[ap.first_value + k], element));
                        }
                    } else {
                        value = value_of(m_scripts.attached_values[ap.first_value], p->type);
                    }
                    o->variables[c->variable_base + static_cast<size_t>(p->auto_variable)] = std::move(value);
                    --o->properties_left;
                } else if (p->setter) {
                    Stack& s = new_stack(StackType::property_init);
                    s.init_object = o;
                    s.pending.kind = CallKind::property_set;
                    s.pending.self = Value::of_object(o, o->type);
                    s.pending.name = name;
                    s.pending.args.push_back(value_of(m_scripts.attached_values[ap.first_value], p->type));
                    post_call(s);
                } else {
                    m_host.log(1, std::format("warning: Property {} on object {} is read-only, property skipped.",
                                              text(name), text(o->type->name)));
                    --o->properties_left;
                }
            }
        }
        if (!p) {
            m_host.log(1, std::format("warning: {} does not have a property named {}, property skipped.",
                                      text(o->type->name), text(name)));
            --o->properties_left;
        }
    }
    property_done(o);
}

void VirtualMachine::property_done(Object* o) {
    if (--o->properties_left == 0) {
        queue_on_init(o);
    }
}

// 4.4.
void VirtualMachine::queue_on_init(Object* o) {
    const StringId name = intern("OnInit");
    const Function* f = find_method(o->type, o->state, name);
    if (!f || (f->code && f->code->instruction_count == 0)) {
        o->initialised = true;
        return;
    }
    Stack& s = new_stack(StackType::init_event);
    s.init_object = o;
    s.pending.kind = CallKind::method;
    s.pending.self = Value::of_object(o, o->type);
    s.pending.name = name;
    post_call(s);
}

void VirtualMachine::unbind(Handle handle) {
    if (auto it = m_bound.find(handle); it != m_bound.end()) {
        for (Object* o : it->second) {
            o->handle = k_no_handle;
        }
        m_bound.erase(it);
    }
    unregister_update(handle);
    unregister_game_time_update(handle);
}

void VirtualMachine::reset(Object* o) {
    if (!o->reset_pending) {
        o->reset_pending = true;
        m_reset_queue.push_back(o);
    }
}

// 4.3.
void VirtualMachine::call_method(Object* object, std::string_view name, std::vector<Value> args, Callback callback) {
    const StringId id = intern(name);
    const Function* f = find_method(object->type, object->state, id);
    if (!f) {
        return;
    }
    if (f->code && !f->native && f->code->instruction_count == 0) {
        if (callback) {
            callback({});
        }
        return;
    }
    Stack& s = new_stack(StackType::normal);
    s.callback = std::move(callback);
    s.pending.kind = CallKind::method;
    s.pending.self = Value::of_object(object, object->type);
    s.pending.name = id;
    s.pending.args = std::move(args);
    post_call(s);
}

void VirtualMachine::call_static(std::string_view script, std::string_view name, std::vector<Value> args, Callback callback) {
    Stack& s = new_stack(StackType::normal);
    s.callback = std::move(callback);
    s.pending.kind = CallKind::global;
    s.pending.search = find_class(script);
    s.pending.name = intern(name);
    s.pending.args = std::move(args);
    post_call(s);
}

void VirtualMachine::send_event(Handle handle, std::string_view name, std::vector<Value> args) {
    const auto bound = objects(handle);
    const std::vector<Object*> snapshot(bound.begin(), bound.end());
    for (Object* o : snapshot) {
        call_method(o, name, args);
    }
}

// ---- logging --------------------------------------------------------------------------------------------------------

void VirtualMachine::log_error(StackId id, std::string_view message, int severity) {
    std::string out = std::format("{}: {}\nstack:", severity >= 2 ? "error" : "warning", message);
    if (auto it = m_stacks.find(id); it != m_stacks.end()) {
        const auto& frames = it->second->frames;
        for (auto f = frames.rbegin(); f != frames.rend(); ++f) {
            std::string where = f->object ? std::format("[{}].", m_host.handle_text(f->object->handle)) : std::string();
            const std::string file = f->function->native ? "<native>" : std::format("{}.psc", text(f->function->owner->name));
            std::string line = std::format("\n\t{}{}.{}() - \"{}\" Line ?", where, text(f->function->owner->name),
                                           text(f->function->name), file);
            if (out.size() + line.size() + 20 > 1024) {
                out += "\n\t<truncated stack>";
                break;
            }
            out += line;
        }
    }
    m_host.log(severity, out);
}

} // namespace spud::script
