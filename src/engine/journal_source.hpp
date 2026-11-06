#pragma once

// The Journal's data from the quest system.

#include "ui/journal.hpp"

namespace spud::game {
class Game;
class QuestSystem;
} // namespace spud::game

namespace spud::engine {

class GameJournal final : public ui::JournalSource {
public:
    GameJournal(game::Game& game, game::QuestSystem& quests) : m_game(game), m_quests(quests) {}

    std::vector<ui::JournalQuest> quests() override;
    bool has_misc_objectives() override;
    std::optional<ui::JournalObjectives> objectives(uint32_t form_id, uint32_t instance) override;
    std::optional<bool> toggle_active(uint32_t form_id, uint32_t instance) override;
    bool quest_completed(uint32_t form_id) override;
    std::vector<ui::MiscStat> misc_stats() override;

private:
    game::Game& m_game;
    game::QuestSystem& m_quests;
};

} // namespace spud::engine
