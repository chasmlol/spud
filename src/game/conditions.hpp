#pragma once

// Condition evaluation: CTDA lists as the game runs them, and the condition
// functions the records and the world's state can answer. Functions that read a system spud doesn't have yet
// report so (Conditions::available); they give 0, what the game's callbacks give when nothing is there.

#include "formats/forms_typed.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <span>
#include <unordered_map>
#include <vector>

namespace spud::game {

struct StoryEvent;

// One CTDA, as the game holds it.
enum ConditionFlag : uint8_t {
    condition_or = 0x01,
    condition_aliases = 0x02,
    condition_global = 0x04, // the comparison value is a global's
    condition_package_data = 0x08,
    condition_swap = 0x10,   // swap subject and target
};
enum class RunOn : uint8_t { subject, target, reference, combat_target, linked_reference, alias, package, event };

struct ConditionItem {
    uint16_t function = 0; // index into the game's function table (script opcode - 0x1000)
    uint8_t flags = 0;     // ConditionFlag | comparison << 5
    RunOn run_on = RunOn::subject;
    float value = 0.0f;
    uint32_t value_global = 0; // condition_global: the global's FormID
    uint32_t param1 = 0;       // a FormID or an integer, per function
    uint32_t param2 = 0;
    uint32_t reference = 0;    // RunOn::reference
    uint16_t member = 0;       // RunOn::event: the event member's two characters
    const std::string* text = nullptr; // CIS2: GetVMQuestVariable's variable, kept for the run
};

// A CTDA as the forms database decodes it.
ConditionItem condition_item(const forms::Value& ctda);
// CIS1, CIS2 after a CTDA: its string parameter.
void condition_text(ConditionItem& item, std::string_view text);
// The condition list of a record with one (the form's CTDA fields in order). Records with several lists (PERK, SPEL's
// effects) read theirs field by field with condition_item.
std::vector<ConditionItem> condition_list(const forms::Form& form);

// What only the running game knows, asked before the records. Character systems answers for the player; what no system
// answers falls through to the record's starting value.
class ConditionFacts {
public:
    virtual ~ConditionFacts() = default;
    // True when `out` holds the function's value for this subject.
    virtual bool condition_value(uint16_t function, uint32_t subject, uint32_t target, uint32_t param1, uint32_t param2,
                                 double& out) = 0;
    // A quest script's variable by name (GetVMQuestVariable): false when there's none.
    virtual bool quest_variable(uint32_t quest, const std::string& name, double& out) {
        (void)quest;
        (void)name;
        (void)out;
        return false;
    }
    // An animation graph's float by name: false with no graph or no such variable.
    virtual bool graph_real(uint32_t ref, const std::string& name, double& out) {
        (void)ref;
        (void)name;
        (void)out;
        return false;
    }
    // An animation graph's int by name: false with no graph or no such variable.
    virtual bool graph_value(uint32_t ref, const std::string& name, double& out) {
        (void)ref;
        (void)name;
        (void)out;
        return false;
    }
    // A global's value as the game has it now; false: the record's.
    virtual bool global_value(uint32_t global, float& out) {
        (void)global;
        (void)out;
        return false;
    }
};

// The world as conditions read it. Everything else comes from the records.
// The kill camera's first target entry, as the VATS conditions read it.
struct VatsEntry {
    bool active = false;       // mode 4, or a path being walked
    bool pairing = false;      // the walk happens before the pair starts: GetPairedAnimation answers for it
    bool first_person = false; // first person is on the camera's stack
    int action = 0;            // 0 hand to hand, 1 one-handed, 2 two-handed, 3 magic, 4 bow
    float distance = 0.0f;     // player to target, units
    int delivery = 0, casting = 0;
    uint32_t projectile_flags = 0;
    std::function<float(uint32_t subject, int side)> area_free;
    std::function<float(uint32_t subject, uint32_t target, int side)> target_visible;
};

struct WorldState {
    float game_hour = 12.0f;
    uint32_t weather = 0;          // current WTHR
    uint32_t outgoing_weather = 0;
    float weather_transition = 1.0f; // 0..1 from the outgoing to the current
    float wind_speed = 0.0f;       // the blended WTHR wind speed, 0..1
    uint32_t player_cell = 0;      // the CELL the player is in, 0: none
    uint32_t player_worldspace = 0;
    uint32_t player_location = 0;  // the LCTN the player is in, 0: none
    bool player_interior = false;
    std::array<float, 3> player_position{}; // Skyrim units
    float player_heading = 0.0f;            // radians clockwise from north
    bool player_first_person = false;
    uint32_t player_race = 0;  // what it is now; the record says otherwise for any made character
    bool player_female = false;
    bool player_moving = false, player_swimming = false, player_sneaking = false;
};

class Conditions {
public:
    static constexpr uint32_t k_player = 0x14;

    Conditions(const forms::Database& db, const WorldState& world) : m_db(db), m_world(world) {}

    // The quest a list belongs to, for the functions that read its aliases (the evaluating quest, TLS).
    uint32_t run_quest() const { return m_run_quest; }
    class QuestScope {
    public:
        QuestScope(Conditions& c, uint32_t quest) : m_conditions(c), m_previous(c.m_run_quest) { c.m_run_quest = quest; }
        ~QuestScope() { m_conditions.m_run_quest = m_previous; }
        QuestScope(const QuestScope&) = delete;
        QuestScope& operator=(const QuestScope&) = delete;

    private:
        Conditions& m_conditions;
        uint32_t m_previous;
    };

    // the list with these references as subject and target (0: none).
    bool evaluate(std::span<const ConditionItem> list, uint32_t subject, uint32_t target);
    // Whether a group of the list asks nothing of its subject and fails: then the list fails for every subject.
    bool fails_for_all(std::span<const ConditionItem> list);
    // Whether only an actor can meet this condition.
    static bool needs_actor(const ConditionItem& c);
    bool evaluate(const ConditionItem& item, uint32_t subject, uint32_t target);
    // A function's value for this subject. `known` false: the function's system isn't there, the value is 0.
    double value(uint16_t function, uint32_t subject, uint32_t target, uint32_t param1, uint32_t param2, bool* known = nullptr);

    static bool available(uint16_t function);
    uint32_t linked_ref(uint32_t reference, uint32_t keyword);
    uint32_t location_of(uint32_t reference);
    uint32_t editor_location_of(uint32_t reference);
    bool has_keyword(uint32_t form_id, uint32_t keyword);
    uint32_t race_of(uint32_t npc);
    uint32_t npc_trait(uint32_t npc, uint16_t template_flag); // the NPC_ giving this kind of data, through templates

    void set_facts(ConditionFacts* facts) { m_facts = facts; }
    // The story event being processed, which GetEventData reads; null when none is.
    void set_event(const StoryEvent* event) { m_event = event; }
    // The aliases of the quest being filled, which alias parameters read; null out of a fill.
    void set_aliases(const std::unordered_map<uint32_t, uint32_t>* refs,
                     const std::unordered_map<uint32_t, uint32_t>* locations) {
        m_alias_refs = refs;
        m_alias_locations = locations;
    }
    // A reference's location as the running world has it (wilderness comes from the location's cell lists); 0: the
    // records say.
    std::function<uint32_t(uint32_t reference)> ref_location;
    // The base of a reference a script made, which no record holds.
    std::function<uint32_t(uint32_t reference)> created_base;

private:
    double event_data(uint32_t param1, uint32_t param2);
    uint32_t m_run_quest = 0;
    const forms::Form* form(uint32_t id);
    uint32_t base_of(uint32_t reference);    // the placed reference's NAME
    uint32_t npc_of(uint32_t reference);     // ... when it's an NPC_
    uint32_t cell_of(uint32_t reference);
    uint32_t worldspace_of(uint32_t cell);
    int32_t faction_rank(uint32_t reference, uint32_t faction); // -2: not in it
    double actor_value(uint32_t reference, uint32_t av);
    bool location_within(uint32_t location, uint32_t ancestor);
    bool weather_flag(uint32_t weather, uint32_t flag);
    uint32_t relationship(uint32_t a, uint32_t b, uint32_t* association);
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> m_relationships; // by the pair: the RELA, its association

    const forms::Database& m_db;
    const WorldState& m_world;
    ConditionFacts* m_facts = nullptr;
    const StoryEvent* m_event = nullptr;
    const std::unordered_map<uint32_t, uint32_t>* m_alias_refs = nullptr;
    const std::unordered_map<uint32_t, uint32_t>* m_alias_locations = nullptr;
    // Exterior cells by world and grid: a persistent reference's own cell is the world's persistent one.
    std::unordered_map<uint64_t, uint32_t> m_exterior_cells;
    bool m_read_exteriors = false;
    std::unordered_map<uint32_t, std::unique_ptr<forms::Form>> m_forms;
};

} // namespace spud::game
