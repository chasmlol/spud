#include "ui/player/object.hpp"

#include "ui/player/vm.hpp"

#include <algorithm>

namespace spud::ui::player {

String make_string(std::string_view s) { return std::make_shared<const std::string>(s); }

const String& empty_string() {
    static const String e = make_string("");
    return e;
}

// ---- properties ---------------------------------------------------------------------------------

Property* PropertyMap::find(std::string_view key) {
    auto it = m_index.find(key);
    return it == m_index.end() ? nullptr : &m_entries[it->second].prop;
}

const Property* PropertyMap::find(std::string_view key) const {
    auto it = m_index.find(key);
    return it == m_index.end() ? nullptr : &m_entries[it->second].prop;
}

Property& PropertyMap::insert(std::string_view key) {
    if (auto it = m_index.find(key); it != m_index.end()) {
        return m_entries[it->second].prop;
    }
    m_index.emplace(std::string(key), m_entries.size());
    m_entries.push_back({std::string(key), {}, true});
    return m_entries.back().prop;
}

bool PropertyMap::erase(std::string_view key) {
    auto it = m_index.find(key);
    if (it == m_index.end()) {
        return false;
    }
    m_entries[it->second].alive = false;
    m_entries[it->second].prop = {};
    m_index.erase(it);
    if (++m_dead > 16 && m_dead > m_index.size()) {
        compact();
    }
    return true;
}

void PropertyMap::compact() {
    std::vector<Entry> kept;
    kept.reserve(m_index.size());
    for (auto& e : m_entries) {
        if (e.alive) {
            kept.push_back(std::move(e));
        }
    }
    m_entries = std::move(kept);
    m_index.clear();
    for (size_t i = 0; i < m_entries.size(); ++i) {
        m_index.emplace(m_entries[i].key, i);
    }
    m_dead = 0;
}

// ---- heap -----------------------------------------------------------------------------------------

Object* Heap::make(ObjKind kind, Object* proto) {
    m_objects.push_back(std::make_unique<Object>(kind));
    Object* o = m_objects.back().get();
    o->proto = proto;
    ++m_since;
    return o;
}

void Heap::collect(const std::vector<Object*>& roots) {
    std::vector<Object*> stack(roots.begin(), roots.end());
    auto push_value = [&](const Value& v) {
        if (v.type == Type::object && v.o && !v.o->marked) {
            stack.push_back(v.o);
        }
    };
    while (!stack.empty()) {
        Object* o = stack.back();
        stack.pop_back();
        if (!o || o->marked) {
            continue;
        }
        o->marked = true;
        if (o->proto) {
            stack.push_back(o->proto);
        }
        o->props.for_each([&](const std::string&, const Property& p) {
            push_value(p.value);
            if (p.getter) {
                stack.push_back(p.getter);
            }
            if (p.setter) {
                stack.push_back(p.setter);
            }
        });
        for (const auto& v : o->elements) {
            push_value(v);
        }
        push_value(o->primitive);
        push_value(o->super_this);
        if (o->super_home) {
            stack.push_back(o->super_home);
        }
        if (o->super_proto) {
            stack.push_back(o->super_proto);
        }
        if (o->script && o->script->scope) {
            for (const Scope* s = o->script->scope.get(); s; s = s->parent.get()) {
                stack.push_back(s->object);
            }
        }
        if (o->trace_native) {
            o->trace_native(stack);
        }
    }
    auto it = std::remove_if(m_objects.begin(), m_objects.end(), [](const std::unique_ptr<Object>& o) {
        if (o->marked) {
            o->marked = false;
            return false;
        }
        return true;
    });
    m_objects.erase(it, m_objects.end());
    m_since = 0;
}

} // namespace spud::ui::player
