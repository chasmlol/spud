#pragma once

// The story manager. Events are queued
// from anywhere, flushed once a frame and walked through the SMEN, SMBN, SMQN tree to start quests.

#include "core/game_random.hpp"
#include "game/quests.hpp"

#include <deque>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud::game {

class StoryManager {
public:
    explicit StoryManager(QuestSystem& quests);

    // Returns the event's id.
    uint32_t queue(StoryEvent event);
    // Section 3: flush, then the time slice. Walks finish in one go here (see quests.cpp, fill_aliases).
    void process_events();
    void bind_natives();
    // 5.2: OnStory... to the quest's scripts.
    void send_quest_event(Quest& q, const StoryEvent& event);

private:
    enum class Visit { started, nothing };
    struct Node {
        uint32_t form = 0;
        uint32_t parent = 0, previous = 0;
        char kind = 'B'; // 'E' event, 'B' branch, 'Q' quest node
        uint32_t flags = 0;
        uint32_t max_concurrent = 0; // XNAM
        uint32_t quests_to_run = 1;  // MNAM
        std::vector<ConditionItem> conditions;
        std::vector<uint32_t> children;
        struct Entry {
            uint32_t quest = 0;
            float reset = 0;   // in days, 2 of the design
            double last_run = 0;
            uint32_t stamp = 0;
        };
        std::vector<Entry> quests;
        uint32_t stamp = 0;
    };

    void load_tree();
    void process(const StoryEvent& event, bool* any_started);
    Visit visit_branch(Node& n, const StoryEvent& e);
    Visit visit_quest_node(Node& n, const StoryEvent& e, bool skip_conditions);
    Visit visit_quest(Node& n, Node::Entry& entry, const StoryEvent& e);
    bool can_run(const Node& n, const Node::Entry& entry, bool random) const;

    QuestSystem& m_quests;
    std::unordered_map<uint32_t, Node> m_nodes;
    std::unordered_map<uint32_t, uint32_t> m_event_nodes; // event code -> SMEN
    uint32_t m_root = 0x5B;
    std::deque<StoryEvent> m_pending, m_active, m_deferred;
    std::unordered_map<uint32_t, std::vector<script::StackId>> m_waiting; // event id -> parked stacks
    uint32_t m_next_id = 1;
    uint32_t m_pass = 0;
    bool m_shared = false;
    std::vector<Quest*> m_candidates;
    std::mt19937 m_random{session_seed()};
};

} // namespace spud::game
