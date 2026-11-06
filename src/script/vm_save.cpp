// The VM's own save, between frames: everything 6.1 of lists. Slices end on instruction
// boundaries, so a stack is always in a state the next dispatch can pick up, and no freeze is needed.
//
// Layout: strings, clocks, objects, stacks, queues, waits and updates (as offsets from the clocks), then arrays.
// A function that no longer exists drops its stack; the game instead resumes from a copy kept in the save.

#include "script/vm_internal.hpp"

#include "core/binary.hpp"

#include <format>

namespace spud::script {

namespace {

constexpr uint32_t k_save_version = 1;

} // namespace

struct VirtualMachine::Saver {
    VirtualMachine& vm;
    BinaryWriter out;
    std::unordered_map<const Object*, uint32_t> object_ids;
    std::unordered_map<const Array*, uint32_t> array_ids;
    std::vector<std::shared_ptr<Array>> arrays;

    void string(std::string_view s) {
        out.write<uint32_t>(static_cast<uint32_t>(s.size()));
        out.write_string(s);
    }
    void object(const Object* o) { out.write<uint32_t>(o ? object_ids.at(o) + 1 : 0); }
    void type(const Class* c) { out.write<uint32_t>(c ? c->name : 0); }
    void value(const Value& v) {
        out.write<uint8_t>(static_cast<uint8_t>(v.tag));
        out.write<uint8_t>(static_cast<uint8_t>(v.element));
        type(v.type);
        switch (v.tag) {
        case Tag::object: object(v.object); break;
        case Tag::array: {
            uint32_t id = 0;
            if (v.array) {
                auto [it, added] = array_ids.emplace(v.array.get(), static_cast<uint32_t>(arrays.size()) + 1);
                if (added) {
                    arrays.push_back(v.array);
                }
                id = it->second;
            }
            out.write<uint32_t>(id);
            break;
        }
        case Tag::none: break;
        default: out.write<int32_t>(v.integer); break;
        }
    }
    void values(std::span<const Value> list) {
        out.write<uint32_t>(static_cast<uint32_t>(list.size()));
        for (const Value& v : list) {
            value(v);
        }
    }
    void function(const Function* f) {
        type(f->owner);
        out.write<uint32_t>(f->state);
        out.write<uint32_t>(f->name);
        out.write<uint8_t>(f->accessor ? 1 : 0);
    }
};

struct VirtualMachine::Loader {
    VirtualMachine& vm;
    BinaryReader in;
    std::vector<StringId> strings;
    std::vector<Object*> objects;
    std::unordered_map<uint32_t, std::shared_ptr<Array>> arrays;

    std::string_view string_at(uint32_t) = delete;
    std::string string() {
        const uint32_t n = in.read<uint32_t>();
        auto bytes = in.read_array<char>(n);
        return std::string(bytes.begin(), bytes.end());
    }
    StringId id() { return strings.at(in.read<uint32_t>()); }
    Object* object() {
        const uint32_t i = in.read<uint32_t>();
        return i ? objects.at(i - 1) : nullptr;
    }
    Class* type() {
        const StringId name = id();
        return name ? vm.find_class(name) : nullptr;
    }
    Value value() {
        Value v;
        v.tag = static_cast<Tag>(in.read<uint8_t>());
        v.element = static_cast<Tag>(in.read<uint8_t>());
        v.type = type();
        switch (v.tag) {
        case Tag::object: v.object = object(); break;
        case Tag::array: {
            const uint32_t i = in.read<uint32_t>();
            if (i) {
                auto& slot = arrays[i];
                if (!slot) {
                    slot = std::make_shared<Array>();
                }
                v.array = slot;
            }
            break;
        }
        case Tag::none: break;
        case Tag::string: v.string = strings.at(static_cast<uint32_t>(in.read<int32_t>())); break;
        default: v.integer = in.read<int32_t>(); break;
        }
        return v;
    }
    std::vector<Value> values() {
        std::vector<Value> list(in.read<uint32_t>());
        for (Value& v : list) {
            v = value();
        }
        return list;
    }
    const Function* function() {
        const Class* owner = type();
        const StringId state = id();
        const StringId name = id();
        const bool accessor = in.read<uint8_t>() != 0;
        if (!owner) {
            return nullptr;
        }
        if (accessor) {
            for (const auto& f : owner->accessors) {
                if (f->name == name) {
                    return f.get();
                }
            }
            return nullptr;
        }
        auto it = owner->functions.find(uint64_t{state} << 32 | name);
        return it == owner->functions.end() ? nullptr : it->second.get();
    }
};

std::vector<std::byte> VirtualMachine::save() {
    Saver s{*this, {}, {}, {}, {}};
    s.out.write<uint32_t>(k_save_version);
    s.out.write<uint32_t>(static_cast<uint32_t>(m_strings.size()));
    for (const std::string& t : m_strings) {
        s.string(t);
    }
    s.out.write<int64_t>(m_gameplay_clock);
    s.out.write<int64_t>(m_real_clock);
    s.out.write<int64_t>(m_game_clock);
    s.out.write<uint32_t>(m_next_stack);

    for (size_t i = 0; i < m_objects.size(); ++i) {
        s.object_ids.emplace(m_objects[i].get(), static_cast<uint32_t>(i));
    }
    s.out.write<uint32_t>(static_cast<uint32_t>(m_objects.size()));
    for (const auto& o : m_objects) {
        s.type(o->type);
        s.out.write<uint64_t>(o->handle);
        s.out.write<uint32_t>(o->state);
        s.out.write<uint32_t>(o->lock_owner);
        s.out.write<uint32_t>(o->properties_left);
        s.out.write<uint32_t>(o->attachment);
        s.out.write<uint8_t>(static_cast<uint8_t>(o->constructed | o->initialised << 1 | o->valid << 2 | o->reset_pending << 3));
    }
    for (const auto& o : m_objects) {
        s.values(o->variables);
    }

    s.out.write<uint32_t>(static_cast<uint32_t>(m_stacks.size()));
    for (const auto& [id, st] : m_stacks) {
        s.out.write<uint32_t>(id);
        s.out.write<uint8_t>(static_cast<uint8_t>(st->type));
        s.out.write<uint8_t>(static_cast<uint8_t>(st->state));
        s.out.write<uint8_t>(static_cast<uint8_t>(st->resume | st->queued << 1 | st->in_latent << 2));
        s.object(st->held);
        s.object(st->init_object);
        s.value(st->ret);
        s.out.write<uint8_t>(static_cast<uint8_t>(st->pending.kind));
        s.value(st->pending.self);
        s.type(st->pending.search);
        s.out.write<uint32_t>(st->pending.name);
        s.values(st->pending.args);
        s.out.write<uint32_t>(static_cast<uint32_t>(st->frames.size()));
        for (const Frame& f : st->frames) {
            s.function(f.function);
            s.value(f.self);
            s.object(f.object);
            s.out.write<uint32_t>(f.ip);
            s.values(f.slots);
        }
    }

    auto ids = [&](const auto& list) {
        s.out.write<uint32_t>(static_cast<uint32_t>(list.size()));
        for (StackId id : list) {
            s.out.write<uint32_t>(id);
        }
    };
    ids(m_ready);
    auto& q = *m_queues;
    auto messages = [&](const std::vector<Message>& list) {
        s.out.write<uint32_t>(static_cast<uint32_t>(list.size()));
        for (const Message& m : list) {
            s.out.write<uint32_t>(m.stack);
            s.out.write<uint8_t>(static_cast<uint8_t>(m.is_return | m.pop << 1));
        }
    };
    messages(q.messages_as_vector());
    messages(q.overflow);
    for (const auto& side : q.sides) {
        ids(side.ring);
        ids(side.overflow);
    }
    s.out.write<int32_t>(q.write);
    for (int i = 0; i < 3; ++i) {
        s.out.write<uint32_t>(static_cast<uint32_t>(q.waits[i].size()));
        for (const Timer& t : q.waits[i]) {
            s.out.write<int64_t>(t.target - clock(static_cast<Clock>(i)));
            s.out.write<uint32_t>(t.stack);
        }
    }
    auto updates = [&](const std::vector<UpdateRegistration>& list, int64_t now) {
        s.out.write<uint32_t>(static_cast<uint32_t>(list.size()));
        for (const auto& u : list) {
            s.out.write<uint64_t>(u.handle);
            s.out.write<int64_t>(u.next - now);
            s.out.write<int64_t>(u.interval);
            s.out.write<uint8_t>(u.single ? 1 : 0);
        }
    };
    updates(q.updates, m_gameplay_clock);
    updates(q.game_updates, m_game_clock);

    // Arrays last: writing elements can find more.
    for (size_t i = 0; i < s.arrays.size(); ++i) {
        s.out.write<uint32_t>(static_cast<uint32_t>(i) + 1);
        s.value(s.arrays[i]->element_default);
        s.values(s.arrays[i]->values);
    }
    s.out.write<uint32_t>(0);
    return s.out.take();
}

void VirtualMachine::load(std::span<const std::byte> bytes) {
    if (!m_objects.empty() || !m_stacks.empty()) {
        throw ParseError("VM save: load needs a fresh VM");
    }
    Loader l{*this, BinaryReader(bytes), {}, {}, {}};
    if (l.in.read<uint32_t>() != k_save_version) {
        throw ParseError("VM save: unknown version");
    }
    l.strings.resize(l.in.read<uint32_t>());
    for (StringId& id : l.strings) {
        id = intern(l.string());
    }
    m_gameplay_clock = l.in.read<int64_t>();
    m_real_clock = l.in.read<int64_t>();
    m_game_clock = l.in.read<int64_t>();
    m_next_stack = l.in.read<uint32_t>();

    const uint32_t object_count = l.in.read<uint32_t>();
    for (uint32_t i = 0; i < object_count; ++i) {
        auto o = std::make_unique<Object>();
        o->type = l.type();
        o->handle = l.in.read<uint64_t>();
        o->state = l.id();
        o->lock_owner = l.in.read<uint32_t>();
        o->properties_left = l.in.read<uint32_t>();
        o->attachment = l.in.read<uint32_t>();
        const uint8_t flags = l.in.read<uint8_t>();
        o->constructed = flags & 1;
        o->initialised = flags & 2;
        o->valid = flags & 4;
        o->reset_pending = flags & 8;
        if (!o->type) {
            o->valid = false; // a script that's gone: calls log the invalid-object error (6.1)
        }
        if (o->reset_pending) {
            m_reset_queue.push_back(o.get());
        }
        l.objects.push_back(o.get());
        if (o->handle != k_no_handle) {
            m_bound[o->handle].push_back(o.get());
            m_bound_forms.emplace(handle_form(o->handle), true);
        }
        m_objects.push_back(std::move(o));
    }
    for (const auto& o : m_objects) {
        o->variables = l.values();
    }

    const uint32_t stack_count = l.in.read<uint32_t>();
    std::vector<StackId> dropped;
    for (uint32_t i = 0; i < stack_count; ++i) {
        auto st = std::make_unique<Stack>();
        st->id = l.in.read<uint32_t>();
        st->type = static_cast<StackType>(l.in.read<uint8_t>());
        st->state = static_cast<StackState>(l.in.read<uint8_t>());
        const uint8_t flags = l.in.read<uint8_t>();
        st->resume = flags & 1;
        st->queued = flags & 2;
        st->in_latent = flags & 4;
        st->held = l.object();
        st->init_object = l.object();
        st->ret = l.value();
        st->pending.kind = static_cast<CallKind>(l.in.read<uint8_t>());
        st->pending.self = l.value();
        st->pending.search = l.type();
        st->pending.name = l.id();
        st->pending.args = l.values();
        const uint32_t frames = l.in.read<uint32_t>();
        bool lost = false;
        for (uint32_t k = 0; k < frames; ++k) {
            Frame f;
            f.function = l.function();
            f.self = l.value();
            f.object = l.object();
            f.ip = l.in.read<uint32_t>();
            f.slots = l.values();
            lost = lost || !f.function;
            f.bytes = f.function ? frame_bytes(*f.function) : 0;
            m_stack_memory += f.bytes;
            st->frames.push_back(std::move(f));
        }
        if (lost) {
            m_host.log(2, std::format("error: Stack {} refers to a function that no longer exists; dropping it.", st->id));
            dropped.push_back(st->id);
        }
        m_stacks.emplace(st->id, std::move(st));
    }

    auto ids = [&](auto& list) {
        const uint32_t n = l.in.read<uint32_t>();
        for (uint32_t i = 0; i < n; ++i) {
            list.push_back(l.in.read<uint32_t>());
        }
    };
    ids(m_ready);
    auto& q = *m_queues;
    auto messages = [&](auto& list) {
        const uint32_t n = l.in.read<uint32_t>();
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t stack = l.in.read<uint32_t>();
            const uint8_t flags = l.in.read<uint8_t>();
            list.push_back(Message{stack, (flags & 1) != 0, (flags & 2) != 0});
        }
    };
    messages(q.messages);
    messages(q.overflow);
    for (auto& side : q.sides) {
        ids(side.ring);
        ids(side.overflow);
    }
    q.write = l.in.read<int32_t>();
    for (int i = 0; i < 3; ++i) {
        const uint32_t n = l.in.read<uint32_t>();
        for (uint32_t k = 0; k < n; ++k) {
            const int64_t offset = l.in.read<int64_t>();
            q.waits[i].push_back({clock(static_cast<Clock>(i)) + offset, l.in.read<uint32_t>()});
        }
    }
    auto updates = [&](std::vector<UpdateRegistration>& list, int64_t now) {
        const uint32_t n = l.in.read<uint32_t>();
        for (uint32_t k = 0; k < n; ++k) {
            UpdateRegistration u{};
            u.handle = l.in.read<uint64_t>();
            u.next = now + l.in.read<int64_t>();
            u.interval = l.in.read<int64_t>();
            u.single = l.in.read<uint8_t>() != 0;
            list.push_back(u);
        }
    };
    updates(q.updates, m_gameplay_clock);
    updates(q.game_updates, m_game_clock);

    while (const uint32_t id = l.in.read<uint32_t>()) {
        auto& slot = l.arrays[id];
        if (!slot) {
            slot = std::make_shared<Array>();
        }
        slot->element_default = l.value();
        slot->values = l.values();
    }
    for (StackId id : dropped) {
        Stack& s = *m_stacks.at(id);
        unlock(s);
        for (const Frame& f : s.frames) {
            m_stack_memory -= f.bytes;
        }
        m_stacks.erase(id);
    }
}

} // namespace spud::script
