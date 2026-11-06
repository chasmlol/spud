#pragma once

// IDLE records as trees under their actions: which graph event an action becomes for an actor

#include "game/conditions.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::game {

class Idles {
public:
    Idles(const forms::Database& db, Conditions& conditions) : m_db(&db), m_conditions(&conditions) {}

    // The event of the action named by its AACT EditorID, walking the roots of the behaviour under `folder` ("actors\character"
    // as DNAM spells it, any case); empty when no idle takes it.
    std::string pick(std::string_view action, std::string_view folder, uint32_t actor, uint32_t target,
                     bool kill_moves = true);

private:
    bool m_skip_kill_moves = false;
    struct Node {
        uint32_t form = 0, parent = 0, previous = 0;
        std::string event, behaviour; // ENAM; DNAM's folder, lower case
        uint8_t flags = 0;
        std::vector<ConditionItem> conditions;
        std::vector<Node*> children;
    };
    static void order(std::vector<Node*>& nodes);
    void build();
    const Node* walk(const Node& node, uint32_t actor, uint32_t target);

    const forms::Database* m_db;
    Conditions* m_conditions;
    bool m_built = false;
    std::unordered_map<uint32_t, std::unique_ptr<Node>> m_nodes;
    std::unordered_map<uint32_t, std::vector<Node*>> m_roots; // by AACT
    std::unordered_map<std::string, uint32_t> m_actions;      // by EditorID
};

} // namespace spud::game
