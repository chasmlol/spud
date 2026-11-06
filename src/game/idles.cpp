#include "game/idles.hpp"

#include "formats/forms_typed.hpp"

#include <algorithm>

namespace spud::game {

namespace {

using forms::Form;
using forms::Value;

constexpr uint8_t k_flag_sequence = 0x02, k_flag_candidate = 0x0A;

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
    return out;
}

} // namespace

// The siblings of one parent by their ANAM chain: each names the one before it.
void Idles::order(std::vector<Node*>& nodes) {
    std::vector<Node*> out;
    const auto next_of = [&](uint32_t previous) {
        return std::ranges::find_if(nodes, [&](const Node* n) { return n && n->previous == previous; });
    };
    for (auto it = next_of(0); it != nodes.end(); it = next_of(out.back()->form)) {
        out.push_back(*it);
        *it = nullptr;
    }
    for (Node* n : nodes) { // a broken chain keeps the rest in load order
        if (n) {
            out.push_back(n);
        }
    }
    nodes = std::move(out);
}

void Idles::build() {
    m_built = true;
    const forms::Database& db = *m_db;
    for (const forms::FormEntry* e : db.of_type(forms::sig("AACT"))) {
        m_actions.emplace(std::string(Form(db, *e).editor_id()), e->form_id);
    }
    for (const forms::FormEntry* e : db.of_type(forms::sig("IDLE"))) {
        const Form f(db, *e);
        if (f.deleted()) {
            continue;
        }
        auto node = std::make_unique<Node>();
        node->form = e->form_id;
        if (const Value* v = f.get("ANAM.parent")) {
            node->parent = v->form;
        }
        if (const Value* v = f.get("ANAM.previous")) {
            node->previous = v->form;
        }
        if (const Value* v = f.get("ENAM")) {
            node->event = v->text;
        }
        if (const Value* v = f.get("DATA.flags")) {
            node->flags = uint8_t(v->integer);
        }
        if (const Value* v = f.get("DNAM")) {
            std::string path = lower(v->text);
            const size_t cut = path.find("\\behaviors");
            node->behaviour = cut == std::string::npos ? std::string() : path.substr(0, cut);
        }
        node->conditions = condition_list(f);
        m_nodes.emplace(e->form_id, std::move(node));
    }
    for (auto& [form, node] : m_nodes) {
        if (const auto it = m_nodes.find(node->parent); it != m_nodes.end()) {
            it->second->children.push_back(node.get());
        } else {
            m_roots[node->parent].push_back(node.get());
        }
    }
    for (auto& [form, node] : m_nodes) {
        if (!node->children.empty()) {
            order(node->children);
        }
    }
    for (auto& [action, roots] : m_roots) {
        order(roots);
    }
}

const Idles::Node* Idles::walk(const Node& node, uint32_t actor, uint32_t target) {
    if ((m_skip_kill_moves && lower(node.event).starts_with("pa_kill")) ||
        !m_conditions->evaluate(node.conditions, actor, target)) {
        return nullptr;
    }
    const Node* candidate = !node.event.empty() || (node.flags & k_flag_candidate) ? &node : nullptr;
    if (node.flags & k_flag_sequence) {
        return candidate;
    }
    for (const Node* child : node.children) {
        if (const Node* found = walk(*child, actor, target)) {
            return found;
        }
    }
    return candidate;
}

std::string Idles::pick(std::string_view action, std::string_view folder, uint32_t actor, uint32_t target,
                        bool kill_moves) {
    if (!m_built) {
        build();
    }
    m_skip_kill_moves = !kill_moves;
    const auto it = m_actions.find(std::string(action));
    if (it == m_actions.end()) {
        return {};
    }
    const auto roots = m_roots.find(it->second);
    if (roots == m_roots.end()) {
        return {};
    }
    const std::string wanted = lower(folder);
    for (const Node* root : roots->second) {
        if (!root->behaviour.empty() && root->behaviour != wanted) {
            continue;
        }
        if (const Node* found = walk(*root, actor, target)) {
            return found->event;
        }
    }
    return {};
}

} // namespace spud::game
