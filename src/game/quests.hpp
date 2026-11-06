#pragma once

// Quests. The teller's lists and the frame
// live here; the story manager's tree is story_manager.hpp and scenes are scenes.hpp.
//
// What the world provides comes through QuestWorld, whose default answers from the records alone: that is all a
// world without actors and streaming references can say, and the systems take its methods over as they land.

#include "core/game_random.hpp"
#include "formats/forms_typed.hpp"
#include "formats/oss.hpp"
#include "game/conditions.hpp"
#include "game/conversations.hpp"
#include "script/vm.hpp"

#include <algorithm>
#include <future>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud::game {

// Quest flags (quest).
enum QuestFlag : uint16_t {
    quest_running = 0x1, quest_completed = 0x2, quest_idle_topics = 0x4, quest_repeat_stages = 0x8, quest_start_game = 0x10,
    quest_shown = 0x20, quest_failed = 0x40, quest_wait = 0x80, quest_run_once = 0x100, quest_active = 0x800,
    quest_keep_instance = 0x2000
};
enum StageFlag : uint8_t { stage_done = 0x1, stage_start_up = 0x2, stage_shut_down = 0x4, stage_keep_instance = 0x8 };
enum StageItemFlag : uint8_t { item_complete_quest = 0x1, item_fail_quest = 0x2 };
enum AliasFlag : uint32_t {
    alias_reserves = 0x1, alias_optional = 0x2, alias_quest_object = 0x4, alias_allow_reuse = 0x8, alias_allow_dead = 0x10,
    alias_in_loaded_area = 0x20, alias_essential = 0x40, alias_allow_disabled = 0x80, alias_stores_text = 0x100,
    alias_allow_reserved = 0x200, alias_protected = 0x400, alias_filled_by_other = 0x800, alias_allow_destroyed = 0x1000,
    alias_closest = 0x2000, alias_uses_stored_text = 0x4000, alias_initially_disabled = 0x8000, alias_allow_cleared = 0x10000
};
inline constexpr uint8_t k_quest_type_misc = 6;

// One story event.
constexpr uint32_t event_code(const char (&s)[5]) {
    return static_cast<uint32_t>(s[0]) | static_cast<uint32_t>(s[1]) << 8 | static_cast<uint32_t>(s[2]) << 16 |
           static_cast<uint32_t>(s[3]) << 24;
}

struct StoryEvent {
    uint32_t id = 0;
    uint32_t type = 0; // four-character code
    struct Member {
        uint16_t code = 0; // two characters, 'R1'
        uint32_t value = 0; // form, location, keyword or integer
    };
    std::array<Member, 6> members{};
    uint32_t count = 0;

    void add(const char (&code)[3], uint32_t value) {
        members[count++] = {static_cast<uint16_t>(code[0] | code[1] << 8), value};
    }
    const Member* member(uint16_t code) const {
        for (uint32_t i = 0; i < count; ++i) {
            if (members[i].code == code) {
                return &members[i];
            }
        }
        return nullptr;
    }
};

struct StageItem {
    uint8_t flags = 0;
    std::vector<ConditionItem> conditions;
    std::string log;        // CNAM
    uint32_t next_quest = 0; // NAM0
};

struct Stage {
    uint16_t index = 0;
    uint8_t flags = 0;
    std::vector<StageItem> items;
};

struct ObjectiveTarget {
    int32_t alias = -1;
    bool ignores_locks = false;
    std::vector<ConditionItem> conditions;
};

struct Objective {
    uint16_t index = 0;
    bool ored = false;
    std::string text;
    std::vector<ObjectiveTarget> targets;
    uint8_t state = 0; // 3.1: 0 dormant, 1 displayed, 2 completed hidden, 3 completed, 4 failed hidden, 5 failed
};

// The game (1.9): what the journal shows of a run once the quest has moved on.
struct InstanceText {
    uint32_t id = 0;
    std::vector<std::pair<uint32_t, uint32_t>> forms; // alias ID, the form its text comes from
    std::vector<std::pair<uint32_t, float>> globals;
    std::string log;
};

// An entry of the player's objective list (player), oldest first.
struct PlayerObjective {
    uint32_t quest = 0;
    uint16_t index = 0;
    uint32_t instance = 0;
    uint8_t state = 0;
};

struct Alias {
    uint32_t id = 0;
    bool location = false;
    std::string name;
    uint32_t flags = 0;
    int fill = -1; // 4.1; -1: none
    int32_t force_into = -1;       // ALFI
    uint32_t specific = 0;         // ALFR, ALFL
    uint32_t unique_actor = 0;     // ALUA
    int32_t other_alias = -1;      // ALFA
    uint32_t ref_type = 0;         // ALRT
    uint32_t keyword = 0;          // KNAM
    uint32_t external_quest = 0;   // ALEQ
    int32_t external_alias = -1;   // ALEA
    uint32_t create_object = 0;    // ALCO
    int32_t create_at = -1;        // ALCA
    bool create_in = false;
    uint32_t create_level = 4;     // ALCL
    int32_t near_alias = -1;       // ALNA
    uint32_t event = 0;            // ALFE
    uint16_t event_member = 0;     // ALFD
    std::vector<ConditionItem> conditions;
    std::vector<uint32_t> keywords, spells, factions, packages;
    uint32_t display_name = 0;     // ALDN
};

struct Quest {
    uint32_t form = 0;
    std::string editor_id, name;
    uint16_t flags = 0;
    uint8_t priority = 0, type = 0;
    uint32_t story_event = 0;     // ENAM
    std::vector<ConditionItem> story_conditions, dialogue_conditions;
    std::vector<Stage> stages;
    std::vector<Objective> objectives;
    std::vector<Alias> aliases;
    std::vector<uint32_t> text_globals;

    uint16_t current_stage = 0;
    bool has_run = false;     // quest
    bool promoting = false;   // the promote task exists
    uint32_t instance = 0;
    std::vector<InstanceText> instances;
    InstanceText* instance_text(uint32_t id) {
        const auto it = std::ranges::find(instances, id, &InstanceText::id);
        return it == instances.end() ? nullptr : &*it;
    }
    std::unordered_map<uint32_t, uint32_t> filled_refs;      // alias ID -> reference
    std::unordered_map<uint32_t, uint32_t> filled_locations; // alias ID -> location
    std::optional<StoryEvent> event;
    std::vector<std::pair<uint16_t, uint32_t>> outstanding;  // (stage, item) 2.5
    std::vector<uint32_t> scenes;
    std::string fill_failure; // the alias that stopped the last fill, for logs

    bool running_bit() const { return flags & quest_running; }
    bool wait_bit() const { return flags & quest_wait; }
    bool is_starting() const { return running_bit() && (wait_bit() || promoting); }
    bool is_running() const { return running_bit() && !wait_bit() && !promoting; }
    bool is_stopping() const { return !running_bit() && wait_bit(); }
    bool is_stopped() const { return !running_bit() && !wait_bit(); }
    Stage* stage(uint16_t index);
    Alias* alias(uint32_t id);
    Objective* objective(uint16_t index);
};

// A reference as the fill tests see it (4.3).
struct RefState {
    bool exists = false, deleted = false, disabled = false, dead = false, destroyed = false;
};

class QuestWorld {
public:
    explicit QuestWorld(const forms::Database& db);
    virtual ~QuestWorld() = default;

    // What the running engine knows; the records answer without it.
    struct Live {
        std::function<std::vector<uint32_t>()> loaded_refs;
        std::function<std::vector<uint32_t>()> loaded_actors;
        std::function<float(uint32_t ref)> distance_to_player;
        std::function<uint32_t(uint32_t ref)> ref_location;
        std::function<bool(uint32_t location)> location_cleared;
        std::function<uint32_t(uint32_t base, uint32_t at, bool inside, uint32_t level)> create_ref;
        std::function<void(uint32_t ref, RefState& state)> ref_state;
        std::function<std::vector<uint32_t>(uint32_t faction)> faction_changes; // refs scripts moved in or out of it
        std::function<std::string(std::string_view)> text_setting;
        std::function<float(std::string_view)> number_setting;
        std::function<float(uint32_t global)> global_value;
        std::function<std::array<float, 4>()> calendar; // year, month from 0, day from 1, days passed
        std::function<void(const Alias& alias, uint32_t ref, bool on)> alias_effects; // 4.8 steps 3-4, 4.9
        std::function<void(int hud_type, uint32_t quest, const std::string& text)> notice; // HUD data 14 to 19
    } live;
    std::string text_setting(std::string_view name);
    float number_setting(std::string_view name) { return live.number_setting ? live.number_setting(name) : 0.0f; }

    virtual RefState ref_state(uint32_t ref);
    // The loaded pass and the persistent pass of 4.4, and what 4.3 asks of references and locations.
    virtual std::vector<uint32_t> loaded_refs() { return live.loaded_refs ? live.loaded_refs() : std::vector<uint32_t>{}; }
    virtual std::vector<uint32_t> persistent_refs();
    std::vector<uint32_t> loaded_actors() { return live.loaded_actors ? live.loaded_actors() : loaded_refs(); }
    const std::vector<uint32_t>& persistent_actors();
    const std::vector<uint32_t>& refs_in_cell(uint32_t cell);
    // Indexes of the records, for a fill that names what it needs (quests.cpp, alias_candidates).
    bool is_persistent(uint32_t ref);
    const std::vector<uint32_t>& refs_of_type(uint32_t ref_type);
    const std::vector<uint32_t>& refs_of_base(uint32_t base);
    const std::vector<uint32_t>& bases_in_faction(uint32_t faction);
    virtual float distance_to_player(uint32_t ref) { return live.distance_to_player ? live.distance_to_player(ref) : 1e30f; }
    virtual uint32_t ref_location(uint32_t ref) { return live.ref_location ? live.ref_location(ref) : 0; }
    virtual uint32_t ref_of_unique_npc(uint32_t npc);
    virtual std::vector<uint32_t> linked_children(uint32_t ref); // what links to it with no keyword
    virtual std::vector<uint32_t> location_refs(uint32_t location, uint32_t ref_type);
    virtual uint32_t location_parent_with_keyword(uint32_t location, uint32_t keyword);
    virtual bool location_cleared(uint32_t location) { return live.location_cleared && live.location_cleared(location); }
    // Creates a reference (4.7); 0 if the world can't.
    virtual uint32_t create_ref(uint32_t base, uint32_t at, bool inside, uint32_t level) {
        return live.create_ref ? live.create_ref(base, at, inside, level) : 0;
    }
    // 4.8, 4.9: factions and spells on the reference are the owning systems' to apply.
    virtual void alias_applied(const Quest&, const Alias& a, uint32_t ref) {
        if (live.alias_effects) {
            live.alias_effects(a, ref, true);
        }
    }
    virtual void alias_removed(const Quest&, const Alias& a, uint32_t ref) {
        if (live.alias_effects) {
            live.alias_effects(a, ref, false);
        }
    }
    // The HUD and journal (3.2): started, completed, failed, objective lines.
    enum class Notice { quest_started, quest_completed, quest_failed, objective_shown, objective_completed, objective_failed };
    virtual void notify(Notice n, const Quest& q, std::string_view text) {
        if (live.notice && !text.empty()) {
            live.notice(14 + int(n), q.form, std::string(text));
        }
    }
    virtual void log_entry(const Quest&, const StageItem&) {}

protected:
    const forms::Database& m_db;

private:
    void index_refs();
    std::vector<uint32_t> m_persistent;
    std::vector<uint32_t> m_persistent_actors;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_by_cell;
    std::unordered_set<uint32_t> m_persistent_set;
    std::future<void> m_warming;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_by_ref_type, m_by_base, m_faction_bases;
    std::unordered_map<uint32_t, uint32_t> m_npc_refs;
    // (location, ref type) -> references, from each reference's XLRT and its cell's XLCN.
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_typed_refs;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_linked_children;
    bool m_indexed = false;
};

class Scenes;
class StoryManager;

class QuestSystem {
public:
    QuestSystem(const forms::Database& db, const oss::Scripts& scripts, script::VirtualMachine& vm, Conditions& conditions,
                QuestWorld& world);
    ~QuestSystem();

    // Binds quest scripts and starts every start-game quest (1.2). Sets the teller's ready flag.
    void new_game();
    // The teller's frame (1.4, 1.6): events, then quests and their scenes.
    void update(float dt_seconds, double game_days, bool menu_paused);

    // The packages of the running quests' aliases this reference fills, alias order.
    std::vector<uint32_t> alias_packages(uint32_t ref) const;
    mutable std::unordered_map<uint32_t, std::vector<uint32_t>> m_alias_packages; // by ref, as of the last update()
    mutable bool m_alias_packages_stale = true;
    bool alias_has_keyword(uint32_t ref, uint32_t keyword) const;
    Quest* quest(uint32_t form);
    Quest* quest(std::string_view editor_id);
    const std::vector<std::unique_ptr<Quest>>& all_quests() const { return m_quests; }
    const std::vector<Quest*>& running() const { return m_running; }
    bool ready() const { return m_ready; }

    // 1.3: immediate fills now; queued posts a QSTR. `pending` as the game's out flag.
    bool start(Quest& q, bool immediate, bool* pending = nullptr);
    void stop(Quest& q);
    void reset(Quest& q); // 1.7, Quest.Reset
    // 2.3. False if the stage can't be set.
    bool set_stage(Quest& q, uint16_t stage);

    // 3.1.
    void set_objective_displayed(Quest& q, uint16_t index, bool display, bool force);
    void set_objective_completed(Quest& q, uint16_t index, bool complete);
    void set_objective_failed(Quest& q, uint16_t index, bool fail);
    void all_objectives(Quest& q, bool complete);
    void set_completed(Quest& q, bool completed);
    void set_failed(Quest& q, bool failed);

    // 4.9, 4.10.
    void force_ref(Quest& q, Alias& a, uint32_t ref);
    void clear_ref(Quest& q, Alias& a);
    void force_location(Quest& q, Alias& a, uint32_t location);
    void clear_location(Quest& q, Alias& a);

    enum class Fill { ok, failed };
    // What a Find Matching fill's conditions can only be true of, when they say: false when they don't narrow it down.
    bool alias_candidates(const Alias& a, std::vector<uint32_t>& out);
    static bool only_actors(const Alias& a);
    // 4.2. Clears what it filled on failure.
    Fill fill_aliases(Quest& q, const StoryEvent* event);
    // A QSTR that couldn't fill: waiting calls return false (1.5).
    void fill_failed(Quest& q) { finish_waiters(q, true, true); }
    void add_starting(Quest& q);
    bool in_lists(const Quest& q) const;
    bool reserved(uint32_t form) const;

    // The player's objectives (3.2), and text with its tags resolved through the instance record.
    const std::vector<PlayerObjective>& player_objectives() const { return m_player_objectives; }
    std::string quest_text(const Quest& q, uint32_t instance, std::string text);
    const std::string& log_text(const Quest& q, uint32_t instance) const;

    // Quest targets for the HUD (5.1): references of displayed objectives of running quests.
    struct Target {
        uint32_t quest, ref;
        uint16_t objective;
    };
    std::vector<Target> quest_targets();

    // The script interface.
    void bind_natives();
    // The quest's or alias's script objects (4.11).
    void bind_alias_scripts(Quest& q);
    script::Handle alias_handle(const Quest& q, const Alias& a) const;

    Conversations* conversations = nullptr; // set by the app: a load replaces this system, not the actors
    StoryManager& story() { return *m_story; }
    Scenes& scenes() { return *m_scenes; }
    const StoryEvent* story_event = nullptr; // the one the Story Manager is on, for GetEventData
    Conditions& conditions() { return m_conditions; }
    QuestWorld& world() { return m_world; }
    script::VirtualMachine& vm() { return m_vm; }
    const forms::Database& db() const { return m_db; }
    const oss::Scripts& scripts() const { return m_scripts; }
    double game_days() const { return m_days; }

    // Save and load of everything here that isn't the records.
    std::vector<std::byte> save() const;
    void load(std::span<const std::byte> bytes);

private:
    void load_records();
    void parse(Quest& q, const forms::Form& form);

    bool run_stage(Quest& q, Stage& stage);
    void run_item(Quest& q, Stage& stage, uint32_t item);
    void item_done(uint32_t quest, uint16_t stage, uint32_t item);
    void reset_progress(Quest& q);
    void update_quests(bool menu_paused, float dt);
    void finish_promotion(Quest& q);
    void init_quest(Quest& q);
    void begin_stop(Quest& q);
    void finish_stop(Quest& q);
    void release_aliases(Quest& q);
    void clear_filled(Quest& q);
    void update_reservations(Quest& q, int delta);
    void set_objective_state(Quest& q, Objective& o, uint8_t state);
    void update_player_objective(const Quest& q, const Objective& o);
    std::string objective_notice(const Quest& q, const Objective& o);
    uint32_t stored_form(const Alias& a, uint32_t ref) const;
    void create_instance_text(Quest& q);
    void store_alias_text(const Quest& q, InstanceText& record, const Alias& a);
    void update_alias_text(Quest& q, const Alias& a);
    void release_instance_text(Quest& q);
    std::string form_text(uint32_t form, std::string_view modifier);
    uint32_t alias_form(const Quest& q, const InstanceText* record, std::string_view name, const Alias** out) const;
    std::optional<std::string> alias_tag(const Quest& q, const InstanceText* record, std::string_view value, const std::string* modifier);
    std::optional<std::string> relationship_tag(const Quest& q, const InstanceText* record, std::string_view first, std::string_view second);
    std::optional<std::string> global_tag(const InstanceText* record, std::string_view editor_id, const std::string* modifier);
    std::string replace_tags(const Quest& q, const InstanceText* record, std::string text);
    void finish_waiters(Quest& q, bool started, bool failed);
    void stage_waiters(uint32_t quest, uint16_t stage);

    uint32_t fill_ref(Quest& q, Alias& a, const StoryEvent* event);
    uint32_t fill_location(Quest& q, Alias& a, const StoryEvent* event);
    bool usable(const Alias& a, uint32_t ref);
    bool in_aliases(const Quest& q, uint32_t ref) const;
    uint32_t pick(Quest& q, const Alias& a, std::vector<uint32_t> candidates);

    const forms::Database& m_db;
    const oss::Scripts& m_scripts;
    script::VirtualMachine& m_vm;
    Conditions& m_conditions;
    QuestWorld& m_world;

    std::vector<std::unique_ptr<Quest>> m_quests;
    std::unordered_map<uint32_t, Quest*> m_by_form;
    std::unordered_map<std::string, Quest*> m_by_name;
    std::vector<Quest*> m_starting, m_running, m_stopping;
    std::vector<PlayerObjective> m_player_objectives;
    std::unordered_map<uint32_t, uint32_t> m_reservations; // 6.3 of
    bool m_ready = false;
    double m_days = 0;
    std::mt19937 m_random{session_seed()};

    struct Waiter {
        script::StackId stack;
        uint32_t quest;
        int stage; // -1: waiting on the start or stop
        uint32_t items_left;
        bool start_only; // Quest.Start, not SetStage
    };
    std::vector<Waiter> m_waiters;

    std::unique_ptr<StoryManager> m_story;
    std::unique_ptr<Scenes> m_scenes;
};


} // namespace spud::game
