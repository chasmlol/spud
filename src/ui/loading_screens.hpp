#pragma once

// The loading screens: which one the Loading Menu shows and the texts it cycles through, chosen as
// The game does: the game evaluates every LSCR's
// conditions with the player's current location set to the destination, keeps those with conditions ahead of
// those without, and picks one at random; RequestLoadingText answers with its text and then with
// random others. The data is the importer's interface/loadingscreens.osl (docs/formats/osl.md).
//
// What the conditions read of the game goes through ConditionState. spud has no quests, globals, Papyrus or
// actors yet, so StartingState answers with what a new game starts with, from the load order.

#include "formats/osl.hpp"
#include "fx/random.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud::ui {

// What the conditions read of the game besides the destination.
class ConditionState {
public:
    virtual ~ConditionState() = default;
    virtual uint16_t quest_stage(uint32_t quest) const = 0;
    virtual bool quest_stage_done(uint32_t quest, uint16_t stage) const = 0;
    virtual bool quest_running(uint32_t quest) const = 0;              // GetQuestRunning's test
    virtual bool quest_completed(uint32_t quest) const = 0;            // The game bit 1
    virtual std::optional<float> global_value(uint32_t global) const = 0;
    virtual std::optional<float> quest_variable(uint32_t quest, std::string_view name) const = 0;
    virtual bool player_has_keyword(uint32_t keyword) const = 0;
    virtual bool reference_dead(uint32_t reference) const = 0;
};

// The load order's starting values, as a new game's first load sees them (measured): quests at stage 0 with their
// DNAM flags (the start-game-enabled ones running), the new game's char-gen quest started; globals at their FLTV,
// Papyrus quest variables at their VMAD property or script initial value, the player's keywords those of the player
// NPC_ and its race, references dead when they start dead.
class StartingState final : public ConditionState {
public:
    explicit StartingState(const osl::LoadingScreens& data);
    // Start a quest as a new game starts the char-gen quest: running, at its start-up stage (that stage done), or
    // at `stage` when its scripts take it further before the state is read (spud runs no Papyrus).
    void start_quest(uint32_t quest, std::optional<uint16_t> stage = std::nullopt);
    // The game as measured where it differs from "running" (0x01): GetQuestRunning also wants bit 0x80 clear.
    void set_quest_flags(uint32_t quest, uint8_t flags);
    // The current stage as measured; the stages done stay as they are.
    void set_quest_stage(uint32_t quest, uint16_t stage);
    // A global's value instead of its FLTV (a state measured in the game).
    void set_global(uint32_t global, float value);

    uint16_t quest_stage(uint32_t quest) const override;
    bool quest_stage_done(uint32_t quest, uint16_t stage) const override;
    bool quest_running(uint32_t quest) const override;
    bool quest_completed(uint32_t quest) const override;
    std::optional<float> global_value(uint32_t global) const override;
    std::optional<float> quest_variable(uint32_t quest, std::string_view name) const override;
    bool player_has_keyword(uint32_t keyword) const override;
    bool reference_dead(uint32_t reference) const override;

private:
    struct QuestState {
        bool running = false;
        uint8_t flags = 0; // 0x01 running, 0x02 completed, 0x80 (GetQuestRunning wants it clear)
        uint16_t stage = 0;
        std::vector<uint16_t> done;
    };
    const osl::LoadingScreens& m_data;
    std::unordered_map<uint32_t, QuestState> m_quests;
    std::unordered_map<uint32_t, float> m_globals;
};

// The moment a loading screen is chosen: the destination's location (put on the player as
// its current location while the conditions run), the worldspace the player is in (GetInWorldspace reads the
// player, who has not moved yet), and Main (a session reset, when only "displays in main menu" screens
// qualify).
struct ConditionContext {
    uint32_t location = 0;          // The game form ID, 0 for none
    uint32_t player_worldspace = 0; // The game form ID, 0 for none
    bool session_reset = false;
    const ConditionState* state = nullptr;
};

class LoadingScreens {
public:
    // Null if the cache has no loadingscreens.osl (an import from before it existed).
    static std::unique_ptr<LoadingScreens> load(const std::filesystem::path& file);
    explicit LoadingScreens(osl::LoadingScreens data);

    const osl::LoadingScreens& data() const { return m_data; }
    const osl::LoadScreen* find(uint32_t form_id) const;
    std::string_view edid(const osl::LoadScreen& s) const { return m_data.string(s.edid); }
    std::string_view text(const osl::LoadScreen& s) const { return m_data.string(s.text); }

    // The location a destination lies in, as the loading code computes the game location: a move to a
    // worldspace position takes the worldspace's cell map, then its own location; an interior cell its
    // encounter zone's location, then XLCN. 0 for none.
    uint32_t location_at(std::string_view worldspace_edid, int cell_x, int cell_y) const;
    uint32_t interior_location(std::string_view cell_edid) const;
    uint32_t worldspace(std::string_view edid) const; // its form ID, 0 if unknown
    std::string_view location_edid(uint32_t location) const;

    // One condition item / one record's condition list, as the game evaluates them with the player as
    // subject and target (). GetRandomPercent draws from `rng`.
    bool conditions_true(const osl::LoadScreen& s, const ConditionContext& ctx, fx::BsRandom& rng) const;

    // the eligible list (conditioned first, in the game's sort), how many of them have
    // conditions, the candidates with a model, and the pick (null when no candidate has a model).
    struct Choice {
        std::vector<const osl::LoadScreen*> eligible; // what RequestLoadingText draws from; the chosen is removed
        uint32_t conditioned = 0;                     // LoadingMenu
        const osl::LoadScreen* chosen = nullptr;      // LoadingMenu
    };
    Choice choose(const ConditionContext& ctx, fx::BsRandom& rng) const;

    // the text to answer with (nullopt: no value), updating the choice.
    std::optional<std::string> next_text(Choice& c, bool no_screen, bool use_all_non_default_first,
                                         fx::BsRandom& rng) const;

private:
    bool item_true(const osl::Condition& c, const ConditionContext& ctx, fx::BsRandom& rng) const;
    bool location_within(uint32_t location, uint32_t ancestor) const;

    osl::LoadingScreens m_data;
    std::unordered_map<uint32_t, size_t> m_screen_of;
    std::unordered_map<uint32_t, uint32_t> m_parent;   // location -> parent
    std::unordered_map<uint32_t, uint32_t> m_location_name; // location -> edid string
    std::unordered_map<uint64_t, uint32_t> m_world_cell; // (worldspace << 32 | x << 16 | y) -> location
    std::unordered_map<std::string, uint32_t> m_world_by_edid;  // lower-case edid -> worldspace form ID
    std::unordered_map<uint32_t, uint32_t> m_world_location;   // worldspace -> its own location
    std::unordered_map<std::string, uint32_t> m_interior;      // lower-case edid -> location
    std::unordered_set<uint32_t> m_worldspaces;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_form_lists;
};

// The game's quicksort of the loading-screen lists (): screens with conditions before screens
// without, in the order this exact partitioning leaves them.
void sort_conditioned_first(std::vector<const osl::LoadScreen*>& list);

} // namespace spud::ui
