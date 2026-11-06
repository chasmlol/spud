#pragma once

// The game state the Journal Menu's Quests and General Stats tabs show: the player's quests and their objectives,
// and the misc stats. The engine writes it into the movie as describes (RequestQuestsData,
// RequestObjectivesData, updateStats). A quest system implements JournalSource and hands it to the UI through
// UiHost::ui_journal(); until spud has one, the Journal answers from data captured in the user's own game, or
// with what a new game shows.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace spud::ui {

// One quest of the title list, as RequestQuestsData writes it.
struct JournalQuest {
    std::string text;       // the quest's name
    uint32_t form_id = 0;
    uint32_t instance = 0;
    bool active = false;    // tracked (its markers show)
    bool completed = false; // the quest is completed, or this is an earlier instance of it
    int type = 0;           // the quest type; misc quests (6) are not listed, their objectives go under MISCELLANEOUS
};

// One objective, as RequestObjectivesData writes it.
struct JournalObjective {
    std::string text;
    bool completed = false;
    bool failed = false;
    std::optional<double> target; // questTargetID: the form ShowTargetOnMap shows on the map
    // Under MISCELLANEOUS each objective also carries its quest and whether that quest is tracked.
    bool active = false;
    uint32_t form_id = 0;
    uint32_t instance = 0;
};

struct JournalObjectives {
    std::vector<JournalObjective> objectives;
    std::optional<std::string> description; // the quest's log text; none for MISCELLANEOUS
};

// A misc stat as updateStats sends it: its name (the movie shows the translation of "$" + name), its
// value (a number, or a name for the four "Favorite" stats), its category (0 General .. 5 Crime) and whether it
// shows while its value is 0.
struct MiscStat {
    std::string name;
    std::variant<double, std::string> value;
    int category = 0;
    bool always_shown = false;
};

// Every stat in the engine's table order, each at 0 or "None", those that show at 0.
std::vector<MiscStat> journal_stat_rows();

class JournalSource {
public:
    virtual ~JournalSource() = default;
    // The quests in the order the engine lists them (the player's objectives from the newest).
    virtual std::vector<JournalQuest> quests() = 0;
    // Some misc quest has a displayed objective (the list then ends with MISCELLANEOUS).
    virtual bool has_misc_objectives() = 0;
    // The objectives of a quest instance, or of MISCELLANEOUS for (0, 0); none when there is no such quest.
    virtual std::optional<JournalObjectives> objectives(uint32_t form_id, uint32_t instance) = 0;
    // Tracks or untracks a quest instance; its new state, or none when there is no such quest instance.
    virtual std::optional<bool> toggle_active(uint32_t form_id, uint32_t instance) = 0;
    // The quest form is completed (a remembered selection on it is dropped).
    virtual bool quest_completed(uint32_t form_id) = 0;
    // The misc stats in the engine's table order.
    virtual std::vector<MiscStat> misc_stats() = 0;
};

} // namespace spud::ui
