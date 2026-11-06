// The Journal Menu's Quests and General Stats tabs, from the running game.

#include "engine/journal_source.hpp"

#include "game/game.hpp"
#include "game/quests.hpp"

#include <algorithm>
#include <format>

namespace spud::engine {

namespace {

constexpr uint8_t k_displayed = 1, k_completed = 3, k_failed = 5;

bool shown(uint8_t state) { return state == k_displayed || state == k_completed || state == k_failed; }

} // namespace

// RequestQuestsData: the player's objectives from the newest back, one entry a quest and instance.
std::vector<ui::JournalQuest> GameJournal::quests() {
    std::vector<ui::JournalQuest> out;
    const auto& list = m_quests.player_objectives();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        game::Quest* q = m_quests.quest(it->quest);
        if (!q || !shown(it->state) || q->type == game::k_quest_type_misc) {
            continue;
        }
        if (std::ranges::any_of(out, [&](const ui::JournalQuest& e) { return e.form_id == it->quest && e.instance == it->instance; })) {
            continue;
        }
        ui::JournalQuest e;
        e.text = m_quests.quest_text(*q, it->instance, q->name);
        e.form_id = q->form;
        e.instance = it->instance;
        const bool current = it->instance == q->instance;
        e.active = current && (q->flags & game::quest_active);
        e.completed = !current || (q->flags & game::quest_completed);
        e.type = q->type;
        out.push_back(std::move(e));
    }
    return out;
}

bool GameJournal::has_misc_objectives() {
    return std::ranges::any_of(m_quests.player_objectives(), [&](const game::PlayerObjective& p) {
        const game::Quest* q = m_quests.quest(p.quest);
        return q && q->type == game::k_quest_type_misc && p.state == k_displayed;
    });
}

// RequestObjectivesData.
std::optional<ui::JournalObjectives> GameJournal::objectives(uint32_t form_id, uint32_t instance) {
    const bool misc = form_id == 0 && instance == 0;
    game::Quest* wanted = misc ? nullptr : m_quests.quest(form_id);
    if (!misc && !wanted) {
        return std::nullopt;
    }
    ui::JournalObjectives out;
    std::string joined; // an "or" objective's text waits for the next one
    const auto& list = m_quests.player_objectives();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        game::Quest* q = m_quests.quest(it->quest);
        if (!q) {
            continue;
        }
        const bool take = misc ? q->type == game::k_quest_type_misc && it->state == k_displayed
                               : q == wanted && it->instance == instance && shown(it->state);
        const game::Objective* o = take ? q->objective(it->index) : nullptr;
        if (!o) {
            continue;
        }
        const std::string text = m_quests.quest_text(*q, it->instance, o->text);
        if (o->ored) {
            joined += std::format("{} {} ", text, m_game.settings().text("sOr"));
            continue;
        }
        ui::JournalObjective e;
        e.text = joined + text;
        joined.clear();
        e.completed = it->state == 2 || it->state == k_completed;
        e.failed = it->state == 4 || it->state == k_failed;
        if (misc) {
            e.active = (q->flags & game::quest_active) != 0;
            e.form_id = q->form;
            e.instance = it->instance;
        }
        out.objectives.push_back(std::move(e));
    }
    if (!joined.empty()) {
        ui::JournalObjective e;
        e.text = joined.substr(0, joined.rfind(m_game.settings().text("sOr")));
        out.objectives.push_back(std::move(e));
    }
    if (wanted) {
        out.description = m_quests.quest_text(*wanted, instance, m_quests.log_text(*wanted, instance));
    }
    return out;
}

std::optional<bool> GameJournal::toggle_active(uint32_t form_id, uint32_t instance) {
    game::Quest* q = m_quests.quest(form_id);
    if (!q || q->instance != instance) {
        return std::nullopt;
    }
    q->flags ^= game::quest_active;
    return (q->flags & game::quest_active) != 0;
}

bool GameJournal::quest_completed(uint32_t form_id) {
    const game::Quest* q = m_quests.quest(form_id);
    return q && (q->flags & game::quest_completed);
}

std::vector<ui::MiscStat> GameJournal::misc_stats() {
    std::vector<ui::MiscStat> out = ui::journal_stat_rows();
    for (ui::MiscStat& s : out) {
        if (std::holds_alternative<double>(s.value)) {
            s.value = double(m_game.stat(s.name));
        }
    }
    return out;
}

} // namespace spud::engine
