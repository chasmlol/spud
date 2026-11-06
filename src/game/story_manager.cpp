#include "game/story_manager.hpp"

#include "core/log.hpp"
#include "script/natives.hpp"

#include <algorithm>
#include <memory>

namespace spud::game {

namespace {

constexpr uint32_t k_random = 0x1, k_do_all = 0x10000, k_shares_event = 0x20000, k_num_quests = 0x40000;

// 5.2. Quests started by event hear about it under this name.
struct EventName {
    uint32_t code;
    const char* name;
};
constexpr EventName k_events[] = {
    {event_code("KILL"), "OnStoryKillActor"}, {event_code("ASSU"), "OnStoryAssaultActor"}, {event_code("CLOC"), "OnStoryChangeLocation"},
    {event_code("SCPT"), "OnStoryScript"}, {event_code("ADIA"), "OnStoryDialogue"}, {event_code("AHEL"), "OnStoryHello"},
    {event_code("AFAV"), "OnStoryActivateActor"}, {event_code("AIPL"), "OnStoryAddToPlayer"}, {event_code("REMP"), "OnStoryRemoveFromPlayer"},
    {event_code("CRFT"), "OnStoryCraftItem"}, {event_code("LOCK"), "OnStoryPickLock"}, {event_code("INFC"), "OnStoryInfection"},
    {event_code("CURE"), "OnStoryCure"}, {event_code("TRES"), "OnStoryTrespass"}, {event_code("ARRT"), "OnStoryArrest"},
    {event_code("NVPE"), "OnStoryNewVoicePower"}, {event_code("DEAD"), "OnStoryDiscoverDeadBody"}, {event_code("SKIL"), "OnStoryIncreaseSkill"},
    {event_code("LEVL"), "OnStoryIncreaseLevel"}, {event_code("CHRR"), "OnStoryRelationshipChange"}, {event_code("INTM"), "OnStoryIntimidateNPC"},
    {event_code("BRIB"), "OnStoryBribeNPC"}, {event_code("FLAT"), "OnStoryFlatterNPC"}, {event_code("PRFV"), "OnStoryPlayerGetsFavor"},
    {event_code("PFIN"), "OnStoryPayFine"}, {event_code("JAIL"), "OnStoryJail"}, {event_code("STIJ"), "OnStoryServedTime"},
    {event_code("ESJA"), "OnStoryEscapeJail"}, {event_code("ADCR"), "OnStoryCrimeGold"}, {event_code("CAST"), "OnStoryCastMagic"},
};

bool priority_event(uint32_t type) {
    return type == event_code("QSTR") || type == event_code("AFAV");
}

} // namespace

StoryManager::StoryManager(QuestSystem& quests) : m_quests(quests) {
    load_tree();
}

void StoryManager::load_tree() {
    const forms::Database& db = m_quests.db();
    for (const char* sig : {"SMEN", "SMBN", "SMQN"}) {
        for (const forms::FormEntry* e : db.of_type(*forms::parse_sig(sig))) {
            Node n;
            n.form = e->form_id;
            n.kind = sig[2] == 'E' ? 'E' : sig[2] == 'B' ? 'B' : 'Q';
            uint32_t event = 0;
            const forms::Form form(db, *e);
            for (const forms::DecodedField& f : form.fields()) {
                const forms::Value& v = f.value;
                switch (f.sig) {
                case forms::sig("PNAM"): n.parent = v.form; break;
                case forms::sig("SNAM"): n.previous = v.form; break;
                case forms::sig("CTDA"): n.conditions.push_back(condition_item(v)); break;
                case forms::sig("DNAM"): n.flags = static_cast<uint32_t>(v.integer); break;
                case forms::sig("XNAM"): n.max_concurrent = static_cast<uint32_t>(v.integer); break;
                case forms::sig("MNAM"): n.quests_to_run = static_cast<uint32_t>(v.integer); break;
                case forms::sig("ENAM"): event = static_cast<uint32_t>(v.integer); break;
                case forms::sig("NNAM"): {
                    const bool duplicate = std::ranges::any_of(n.quests, [&v](const Node::Entry& q) { return q.quest == v.form; });
                    if (v.form && !duplicate) {
                        n.quests.push_back({v.form, 0, 0, 0});
                    }
                    break;
                }
                case forms::sig("FNAM"): break;
                case forms::sig("RNAM"):
                    // 2: the number, scaled by 1/24 twice, in game days.
                    if (!n.quests.empty()) {
                        n.quests.back().reset = static_cast<float>(v.real) / 576.0f;
                    }
                    break;
                default: break;
                }
            }
            if (n.kind == 'E') {
                m_event_nodes[event] = n.form;
            }
            m_nodes.emplace(n.form, std::move(n));
        }
    }
    // Children in SNAM order: first the one with no previous sibling, then each right after its previous sibling.
    for (auto& [form, n] : m_nodes) {
        if (auto parent = m_nodes.find(n.parent); parent != m_nodes.end() && n.previous == 0) {
            parent->second.children.insert(parent->second.children.begin(), form);
        }
    }
    bool moved = true;
    std::unordered_set<uint32_t> placed;
    while (moved) {
        moved = false;
        for (auto& [form, n] : m_nodes) {
            if (n.previous == 0 || placed.contains(form)) {
                continue;
            }
            auto parent = m_nodes.find(n.parent);
            if (parent == m_nodes.end()) {
                continue;
            }
            auto& children = parent->second.children;
            if (auto it = std::ranges::find(children, n.previous); it != children.end()) {
                children.insert(it + 1, form);
                placed.insert(form);
                moved = true;
            }
        }
    }
}

uint32_t StoryManager::queue(StoryEvent event) {
    event.id = m_next_id++;
    m_pending.push_back(event);
    return event.id;
}

void StoryManager::process_events() {
    while (!m_pending.empty()) {
        StoryEvent e = m_pending.front();
        m_pending.pop_front();
        (priority_event(e.type) ? m_active : m_deferred).push_back(e);
    }
    if (m_active.empty() && !m_deferred.empty()) {
        m_active.push_back(m_deferred.front());
        m_deferred.pop_front();
    }
    while (!m_active.empty()) {
        const StoryEvent e = m_active.front();
        m_active.pop_front();
        bool any_started = false;
        m_candidates.clear();
        if (e.type == event_code("QSTR")) {
            Quest* q = m_quests.quest(e.members[0].value);
            if (q && m_quests.fill_aliases(*q, nullptr) == QuestSystem::Fill::ok) {
                m_candidates.push_back(q);
            } else if (q) {
                m_quests.fill_failed(*q);
            }
        } else {
            process(e, &any_started);
        }
        for (Quest* q : m_candidates) {
            if (!m_quests.in_lists(*q)) {
                log::info("story: {} starts", q->editor_id);
                m_quests.add_starting(*q);
                any_started = true;
            }
        }
        if (e.type == event_code("ADIA") && m_quests.conversations) {
            m_quests.conversations->event_processed(e.id, any_started);
        }
        // SendStoryEventAndWait (section 8).
        if (auto it = m_waiting.find(e.id); it != m_waiting.end()) {
            const auto stacks = std::move(it->second);
            m_waiting.erase(it);
            for (script::StackId s : stacks) {
                m_quests.vm().return_from_latent(s, script::Value::of(any_started));
            }
        }
        if (m_active.empty() && !m_deferred.empty()) {
            m_active.push_back(m_deferred.front());
            m_deferred.pop_front();
        }
    }
}

// Section 4.
void StoryManager::process(const StoryEvent& e, bool* any_started) {
    m_quests.story_event = &e;
    const auto done = std::unique_ptr<QuestSystem, decltype([](QuestSystem* q) { q->story_event = nullptr; })>(&m_quests);
    Conditions& conditions = m_quests.conditions();
    auto root = m_nodes.find(m_root);
    conditions.set_event(&e);
    const bool root_passes = root == m_nodes.end() || conditions.evaluate(root->second.conditions, 0x14, 0x14);
    conditions.set_event(nullptr);
    if (!root_passes) {
        return;
    }
    auto node = m_event_nodes.find(e.type);
    if (node == m_event_nodes.end()) {
        return;
    }
    ++m_pass;
    m_shared = false;
    m_quests.conditions().set_event(&e);
    visit_branch(m_nodes.at(node->second), e);
    m_quests.conditions().set_event(nullptr);
    (void)any_started;
}

StoryManager::Visit StoryManager::visit_branch(Node& n, const StoryEvent& e) {
    if (n.kind == 'Q') {
        return visit_quest_node(n, e, false);
    }
    Conditions& conditions = m_quests.conditions();
    if (!conditions.evaluate(n.conditions, 0x14, 0x14)) {
        n.stamp = m_pass;
        return Visit::nothing;
    }
    Visit result = Visit::nothing;
    std::vector<uint32_t> children;
    for (uint32_t c : n.children) {
        if (m_nodes.at(c).stamp != m_pass) {
            children.push_back(c);
        }
    }
    if (n.flags & k_random) {
        while (!children.empty() && result == Visit::nothing) {
            const size_t i = std::uniform_int_distribution<size_t>(0, children.size() - 1)(m_random);
            const uint32_t child = children[i];
            children.erase(children.begin() + static_cast<ptrdiff_t>(i));
            result = visit_branch(m_nodes.at(child), e);
        }
    } else {
        for (uint32_t child : children) {
            result = visit_branch(m_nodes.at(child), e);
            if (result != Visit::nothing) {
                break;
            }
        }
    }
    n.stamp = m_pass;
    return result;
}

bool StoryManager::can_run(const Node& n, const Node::Entry& entry, bool random) const {
    const Quest* q = const_cast<QuestSystem&>(m_quests).quest(entry.quest);
    if (!q || entry.stamp == m_pass || m_quests.in_lists(*q)) {
        return false;
    }
    if ((n.flags & k_do_all) && entry.last_run != 0) {
        return false;
    }
    if (entry.reset > 0 && entry.last_run != 0 && m_quests.game_days() - entry.last_run < entry.reset) {
        return false;
    }
    if ((q->flags & quest_run_once) && entry.last_run > 0 && (!random || entry.reset <= 0)) {
        return false;
    }
    return true;
}

StoryManager::Visit StoryManager::visit_quest(Node& n, Node::Entry& entry, const StoryEvent& e) {
    Quest* q = m_quests.quest(entry.quest);
    Conditions& conditions = m_quests.conditions();
    const Conditions::QuestScope scope(conditions, entry.quest);
    entry.stamp = m_pass;
    if (!q || !conditions.evaluate(q->story_conditions, 0x14, 0x14) || m_quests.fill_aliases(*q, &e) != QuestSystem::Fill::ok) {
        return Visit::nothing;
    }
    m_candidates.push_back(q);
    entry.last_run = m_quests.game_days() > 0 ? m_quests.game_days() : 1e-6;
    (void)n;
    return Visit::started;
}

StoryManager::Visit StoryManager::visit_quest_node(Node& n, const StoryEvent& e, bool skip_conditions) {
    Conditions& conditions = m_quests.conditions();
    if (n.max_concurrent != 0) {
        uint32_t active = 0;
        for (const Node::Entry& entry : n.quests) {
            if (const Quest* q = m_quests.quest(entry.quest); q && m_quests.in_lists(*q)) {
                ++active;
            }
        }
        if (active >= n.max_concurrent) {
            n.stamp = m_pass;
            return Visit::nothing;
        }
    }
    if (!skip_conditions && !conditions.evaluate(n.conditions, 0x14, 0x14)) {
        n.stamp = m_pass;
        return Visit::nothing;
    }
    uint32_t count = 0;
    const bool random = (n.flags & k_random) != 0;
    bool stop = false;
    if (random) {
        std::vector<size_t> eligible;
        for (size_t i = 0; i < n.quests.size(); ++i) {
            if (can_run(n, n.quests[i], true)) {
                eligible.push_back(i);
            }
        }
        while (!eligible.empty() && !stop) {
            const size_t pick = std::uniform_int_distribution<size_t>(0, eligible.size() - 1)(m_random);
            Node::Entry& entry = n.quests[eligible[pick]];
            eligible.erase(eligible.begin() + static_cast<ptrdiff_t>(pick));
            if (visit_quest(n, entry, e) == Visit::started) {
                ++count;
                stop = !(n.flags & k_num_quests) || count >= n.quests_to_run;
            }
        }
    } else {
        for (Node::Entry& entry : n.quests) {
            if (!can_run(n, entry, false)) {
                continue;
            }
            if (visit_quest(n, entry, e) == Visit::started) {
                ++count;
                if (!(n.flags & k_num_quests) || count >= n.quests_to_run) {
                    stop = true;
                    break;
                }
            }
        }
    }
    // Step 5: the list ran out with Do All Before Repeating.
    if (!stop && (n.flags & k_do_all) && !skip_conditions) {
        bool any = false;
        for (Node::Entry& entry : n.quests) {
            if (entry.last_run > 0 && entry.reset <= 0) {
                entry.last_run = 0;
                entry.stamp = m_pass - 1;
                any = true;
            }
        }
        if (any) {
            return visit_quest_node(n, e, true);
        }
    }
    n.stamp = m_pass;
    if (n.flags & k_shares_event) {
        m_shared = true;
        return Visit::nothing;
    }
    return count > 0 ? Visit::started : Visit::nothing;
}

void StoryManager::send_quest_event(Quest& q, const StoryEvent& event) {
    for (const EventName& e : k_events) {
        if (e.code != event.type) {
            continue;
        }
        // Arguments in slot order; references and locations need their classes, which the handlers declare.
        std::vector<script::Value> args;
        script::VirtualMachine& vm = m_quests.vm();
        const script::Handle handle = script::form_handle(q.form);
        for (uint32_t i = 0; i < event.count; ++i) {
            const StoryEvent::Member& m = event.members[i];
            const char kind = static_cast<char>(m.code & 0xFF);
            if (kind == 'V') {
                args.push_back(script::Value::of(static_cast<int32_t>(m.value)));
            } else {
                const char* type = kind == 'L' ? "Location" : kind == 'K' ? "Keyword" : kind == 'F' ? "Form" : "ObjectReference";
                args.push_back(vm.form_value(m.value, type));
            }
        }
        vm.send_event(handle, e.name, std::move(args));
    }
}

void StoryManager::bind_natives() {
    using script::NativeCall;
    using script::Value;
    script::VirtualMachine& vm = m_quests.vm();
    auto form_of = [](const Value& v) -> uint32_t {
        return v.tag == script::Tag::object && v.object ? script::handle_form(v.object->handle) : 0;
    };
    auto make = [form_of](NativeCall& c) {
        StoryEvent e;
        e.type = event_code("SCPT");
        e.add("K1", script::handle_form(c.self->handle));
        e.add("L1", form_of(c.args[0]));
        e.add("R1", form_of(c.args[1]));
        e.add("R2", form_of(c.args[2]));
        e.add("V1", static_cast<uint32_t>(c.args[3].integer));
        e.add("V2", static_cast<uint32_t>(c.args[4].integer));
        return e;
    };
    vm.bind_native("Keyword", "SendStoryEvent", [this, make](NativeCall& c) {
        if (!m_quests.ready()) {
            c.vm.log_error(c.stack, "Cannot SendStoryEvent before start up quests have been initialized.", 1);
            return;
        }
        queue(make(c));
    }, script::native_flags("Keyword", "SendStoryEvent"));
    vm.bind_native("Keyword", "SendStoryEventAndWait", [this, make](NativeCall& c) {
        if (!m_quests.ready()) {
            c.vm.log_error(c.stack, "Cannot SendStoryEvent before start up quests have been initialized.", 1);
            c.vm.return_from_latent(c.stack, Value::of(false));
            return;
        }
        m_waiting[queue(make(c))].push_back(c.stack);
    }, script::native_flags("Keyword", "SendStoryEventAndWait"));
}

} // namespace spud::game
