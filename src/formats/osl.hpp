#pragma once

// .osl — spud loading screens. Spec: docs/formats/osl.md
//
// The load order's loading screens (Skyrim LSCR records) with their conditions, and the starting state of
// everything those conditions read, converted by the importer so the Loading Menu can choose a screen the way
// the game does: which location a destination lies in (locations and their
// parents, the worldspaces' cell-to-location maps, interior cells), and what a new game starts with (quests,
// globals, Papyrus quest variables, the player's keywords, references that start dead, form lists).

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::osl {

inline constexpr container::FourCC k_kind = container::fourcc("LSCR");
inline constexpr uint32_t k_version = 1;
inline constexpr uint32_t k_none = 0xFFFFFFFFu;

// One the game, in the order the game's form array holds them (the order forms are first created while the
// plugins load: load order, then file order; overrides keep the first position and take the last data).
struct LoadScreen {
    uint32_t form_id;
    uint32_t edid;         // string
    uint32_t text;         // string: DESC through the string tables
    uint32_t model;        // string: cache path of the NNAM static's model (.osm), k_none without NNAM
    uint32_t camera_path;  // string: cache path of MOD2 (.ose), k_none without one
    uint32_t record_flags; // 0x400 "displays in main menu"
    float scale;           // SNAM, 1 when absent
    int16_t rotation[3];   // RNAM, degrees about X, Y, Z
    int16_t rotation_min;  // ONAM, degrees; -180 / 180 when absent
    int16_t rotation_max;
    uint16_t reserved0;
    float offset[3];       // XNAM, Skyrim units
    uint32_t first_condition; // into conditions
    uint32_t condition_count;
    uint32_t static_form;  // NNAM's form ID, 0 without
    uint32_t has_inv_marker; // the model's root carries the BSInvMarker "INV": its rotation replaces the half turn
    float inv_rotation[3];   // the marker's X, Y, Z (uint16 x 0.001), radians
};
static_assert(sizeof(LoadScreen) == 80);

// One CTDA, as the game holds it.
struct Condition {
    uint16_t function;     // condition function index (script opcode - 0x1000)
    uint8_t flags;         // compare op << 5 | 0x10 swap subject/target | 0x08 package data | 0x04 global | 0x02 aliases | 0x01 OR
    uint8_t run_on;        // 0 subject, 1 target, 2 reference, 3 combat target, 4 linked reference, 5 alias, 6 package, 7 event
    float value;           // the comparison value (flags 0x04: the global's)
    uint32_t value_global; // flags 0x04: the global's form ID
    uint32_t param1;       // form ID (resolved into the load order) or integer, per function
    uint32_t param2;
    uint32_t reference;    // run_on 2: the reference's form ID
    int32_t param3;
    uint32_t string_param; // string: CIS1/CIS2 (GetVMQuestVariable's variable), k_none
};
static_assert(sizeof(Condition) == 32);

struct Location {
    uint32_t form_id;
    uint32_t edid;   // string
    uint32_t parent; // PNAM form ID, 0 for none
    uint32_t reserved;
};
static_assert(sizeof(Location) == 16);

struct Worldspace {
    uint32_t form_id;
    uint32_t edid;     // string
    uint32_t location; // The game's own location: XLCN, else its encounter zone's location; 0 for none
    uint32_t reserved;
};
static_assert(sizeof(Worldspace) == 16);

// A worldspace's cell-to-location map entry (from the locations' LCEC lists, every version in load order, later
// ones replacing earlier ones).
struct WorldCell {
    uint32_t worldspace; // form ID
    int16_t x, y;        // cell coordinates
    uint32_t location;   // form ID
    uint32_t reserved;
};
static_assert(sizeof(WorldCell) == 16);

// An interior cell and the location the game gives it (encounter zone's, else XLCN).
struct InteriorCell {
    uint32_t form_id;
    uint32_t edid;     // string
    uint32_t location; // form ID, 0 for none
    uint32_t reserved;
};
static_assert(sizeof(InteriorCell) == 16);

enum QuestFlag : uint16_t {
    quest_start_game_enabled = 0x0001, // DNAM flags as stored
};

struct Quest {
    uint32_t form_id;
    uint32_t edid;          // string
    uint16_t flags;         // QUST DNAM flags
    uint16_t reserved0;
    int32_t startup_stage;  // the first stage flagged "start up stage" (INDX 0x02), -1 for none
    uint32_t first_stage;   // into stages
    uint32_t stage_count;
    uint32_t reserved[2];
};
static_assert(sizeof(Quest) == 32);

struct QuestStage {
    uint16_t index; // INDX stage index
    uint8_t flags;  // INDX flags (0x02 start up, 0x04 shut down)
    uint8_t reserved;
};
static_assert(sizeof(QuestStage) == 4);

struct Global {
    uint32_t form_id;
    uint32_t edid; // string
    float value;   // FLTV: the value a new game starts with
    uint32_t reserved;
};
static_assert(sizeof(Global) == 16);

// A Papyrus variable of a quest's script as a new game starts it: the VMAD property value, else the compiled
// script's initial value of the variable.
struct QuestVariable {
    uint32_t quest; // form ID
    uint32_t name;  // string, as the condition names it ("::PlayerHasBeastBlood_var")
    float value;
    uint32_t source; // 1 VMAD property, 2 the script's initial value, 0 not found (0)
};
static_assert(sizeof(QuestVariable) == 16);

struct FormListEntry {
    uint32_t list;  // FLST form ID
    uint32_t form;  // a member's form ID
};
static_assert(sizeof(FormListEntry) == 8);

struct LoadingScreens {
    std::vector<LoadScreen> screens;
    std::vector<Condition> conditions;
    std::vector<Location> locations;
    std::vector<Worldspace> worldspaces;
    std::vector<WorldCell> world_cells;
    std::vector<InteriorCell> interior_cells;
    std::vector<Quest> quests;
    std::vector<QuestStage> stages;
    std::vector<Global> globals;
    std::vector<QuestVariable> quest_variables;
    std::vector<uint32_t> player_keywords; // the player's race and NPC keywords as a new game starts
    std::vector<uint32_t> dead_references; // references conditions name that start dead (record flag 0x200)
    std::vector<FormListEntry> form_lists;

    uint32_t add_string(std::string_view s);
    std::string_view string(uint32_t offset) const;
    const std::vector<char>& string_data() const { return m_strings; }
    void set_string_data(std::vector<char> data);

private:
    std::vector<char> m_strings;
    std::unordered_map<std::string, uint32_t> m_string_index;
};

std::vector<std::byte> write(const LoadingScreens& table);
// Throws ParseError on a malformed file.
LoadingScreens read(std::span<const std::byte> file);

} // namespace spud::osl
