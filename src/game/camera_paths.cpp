#include "game/camera_paths.hpp"

#include "core/path.hpp"
#include "formats/forms_value.hpp"

#include <algorithm>

namespace spud::game {

namespace {

using forms::Form;
using forms::Value;

float real(const Form& f, std::string_view name, float fallback = 0.0f) {
    const Value* v = f.get(name);
    return v ? float(v->real) : fallback;
}

int whole(const Form& f, std::string_view name) {
    const Value* v = f.get(name);
    return v ? int(v->integer) : 0;
}

CameraShot read_shot(const forms::Database& db, uint32_t form) {
    CameraShot s;
    const forms::FormEntry* e = db.find(form);
    if (!e) {
        return s;
    }
    const Form f(db, *e);
    if (const Value* v = f.get("MODL"); v && !v->text.empty()) {
        s.model = path::to_lower(path::normalize(v->text));
    }
    s.action = whole(f, "DATA.action");
    s.location = whole(f, "DATA.location");
    s.look = whole(f, "DATA.target");
    s.flags = whole(f, "DATA.flags");
    s.player_rate = real(f, "DATA.player_time_multiplier", 1.0f);
    s.target_rate = real(f, "DATA.target_time_multiplier", 1.0f);
    s.global_rate = real(f, "DATA.global_time_multiplier", 1.0f);
    if (s.global_rate < 0.0001f) { // at load
        s.global_rate = 0.1f;
    }
    s.max_time = real(f, "DATA.max_time");
    s.min_time = real(f, "DATA.min_time");
    s.percent = real(f, "DATA.target_percent_between_actors");
    s.near_distance = real(f, "DATA.near_target_distance");
    if (const Value* v = f.get("MNAM")) {
        s.image_space = v->form;
    }
    return s;
}

// Siblings run along the `previous` chain from 0; a broken chain keeps the rest as they come.
template <class Node>
void order(std::vector<Node*>& nodes) {
    std::vector<Node*> out;
    uint32_t previous = 0;
    for (;;) {
        const auto it = std::ranges::find_if(nodes, [&](const Node* n) { return n && n->previous == previous; });
        if (it == nodes.end()) {
            break;
        }
        out.push_back(*it);
        previous = (*it)->form;
        *it = nullptr;
    }
    for (Node* n : nodes) {
        if (n) {
            out.push_back(n);
        }
    }
    nodes = std::move(out);
}

} // namespace

void CameraPaths::build() {
    m_built = true;
    const forms::Database& db = *m_db;
    for (const forms::FormEntry* e : db.of_type(forms::sig("CPTH"))) {
        const Form f(db, *e);
        if (f.deleted()) {
            continue;
        }
        auto node = std::make_unique<Node>();
        node->form = node->path.form = e->form_id;
        if (const Value* v = f.get("ANAM.parent")) {
            node->parent = v->form;
        }
        if (const Value* v = f.get("ANAM.previous")) {
            node->previous = v->form;
        }
        node->conditions = condition_list(f);
        for (const Value* v : f.all("SNAM")) {
            node->path.shots.push_back(read_shot(db, v->form));
        }
        m_nodes.emplace(e->form_id, std::move(node));
    }
    for (auto& [form, node] : m_nodes) {
        if (const auto it = m_nodes.find(node->parent); it != m_nodes.end()) {
            it->second->children.push_back(node.get());
        } else {
            m_roots.push_back(node.get());
        }
    }
    for (auto& [form, node] : m_nodes) {
        order(node->children);
    }
    order(m_roots);
}

// depth first, a child's answer wins and a parent's shots are the fallback.
const CameraPaths::Node* CameraPaths::resolve(const Node& node, uint32_t attacker, uint32_t victim) {
    if (!m_conditions->evaluate(node.conditions, attacker, victim)) {
        return nullptr;
    }
    for (const Node* child : node.children) {
        if (const Node* found = resolve(*child, attacker, victim)) {
            return found;
        }
    }
    return node.path.shots.empty() ? nullptr : &node;
}

const CameraPath* CameraPaths::pick(uint32_t attacker, uint32_t victim) {
    if (!m_built) {
        build();
    }
    for (const Node* root : m_roots) {
        if (const Node* found = resolve(*root, attacker, victim)) {
            const auto& shots = found->path.shots;
            const bool rejected = std::ranges::any_of(shots, [](const CameraShot& s) { return s.min_time == 0.0f && s.max_time == 0.0f; });
            return rejected ? nullptr : &found->path;
        }
    }
    return nullptr;
}

} // namespace spud::game
