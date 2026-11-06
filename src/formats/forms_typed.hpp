#pragma once

// Typed access to the forms database for the systems that need it (items, magic, leveled lists, references,
// ...). Built on the schema: fields and members are found by their schema names (src/importer/schema/*.txt), so
// these accessors read any version of a record the schema describes. Anything not covered here is reachable
// generically: Form::get("DATA.value"), Form::all("KWDA"), or decode_form().

#include "formats/forms.hpp"
#include "formats/forms_value.hpp"

#include <memory>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spud::forms {

// One form with its fields decoded once.
class Form {
public:
    Form(const Database& db, const FormEntry& entry);

    const FormEntry& entry() const { return *m_entry; }
    const Database& database() const { return *m_db; }
    uint32_t id() const { return m_entry->form_id; }
    Sig type() const { return m_entry->type; }
    bool deleted() const { return (m_entry->flags & form_deleted) != 0; }
    std::string_view plugin() const;

    // The first field named or signed `field` ("DATA", or its schema name "game_data"), or a member path in it:
    // "DATA.value", "game_data.weight". For a layered form (FormEntry::layers > 1), the last one: the record the
    // game loaded last sets it.
    const Value* get(std::string_view path) const;
    // Every field with that signature or name, in order (for a layered form, each record's in load order: lists
    // the game appends to, like magic effects, hold them all).
    std::vector<const Value*> all(std::string_view field) const;
    const std::vector<DecodedField>& fields() const { return *m_fields; }

    std::string_view editor_id() const;
    std::string_view name() const;        // FULL
    std::string_view description() const; // DESC
    std::vector<uint32_t> keywords() const;

private:
    const Database* m_db;
    const FormEntry* m_entry;
    std::shared_ptr<const std::vector<DecodedField>> m_fields; // shared: see the cache in forms_typed.cpp
};

// Value and weight of an inventory item (ALCH, AMMO, APPA, ARMO, BOOK, INGR, KEYM, LIGH, MISC, SCRL, SLGM, WEAP),
// from wherever the record type keeps them.
std::optional<int64_t> item_value(const Form& f);
std::optional<double> item_weight(const Form& f);

struct ItemCount {
    uint32_t item;
    int64_t count;
};
// CONT, NPC_ inventory (CNTO).
std::vector<ItemCount> container_items(const Form& f);

struct LeveledEntry {
    uint32_t form;
    uint16_t level;
    uint16_t count;
};
struct LeveledList {
    uint8_t chance_none = 0;
    uint8_t flags = 0;
    uint32_t chance_none_global = 0; // LVLG
    std::vector<LeveledEntry> entries;
};
// LVLI, LVLN, LVSP.
std::optional<LeveledList> leveled_list(const Form& f);

struct EffectEntry {
    uint32_t effect; // MGEF
    float magnitude;
    uint32_t area;
    uint32_t duration;
};
// SPEL, ENCH, ALCH, INGR, SCRL (EFID + EFIT pairs).
std::vector<EffectEntry> effects(const Form& f);

// FLST entries (LNAM).
std::vector<uint32_t> form_list(const Form& f);

// GLOB value.
std::optional<float> global_value(const Form& f);

// A game setting's value (GMST DATA, by the EditorID's first letter): integer, float or text.
std::optional<Value> setting_value(const Form& f);

struct Placement {
    uint32_t base;
    std::array<float, 3> position; // Skyrim units
    std::array<float, 3> rotation; // radians
    float scale = 1.0f;
};
// REFR, ACHR and placed projectiles/hazards (NAME, DATA, XSCL).
std::optional<Placement> placement(const Form& f);
// The same from the record's bytes, without decoding it: what scans over thousands of references use.
std::optional<Placement> placement(const Database& db, const FormEntry& entry);
// A reference's NAME from the record's bytes; 0 when it has none.
uint32_t placed_base(const Database& db, const FormEntry& entry);

struct Response {
    uint32_t emotion_type = 0;
    uint32_t emotion_value = 0;
    uint8_t number = 0;
    uint32_t sound = 0; // SNDR
    std::string text;   // resolved .ILSTRINGS text
};
// INFO: its responses (TRDT + NAM1 pairs), in order. A shared info (DNAM) is not followed; see shared_info().
std::vector<Response> responses(const Form& f);
uint32_t shared_info(const Form& f);

struct QuestStage {
    uint16_t index = 0;
    uint8_t flags = 0;
    std::vector<std::string> log_entries; // CNAM, resolved .DLSTRINGS text
};
// QUST stages (INDX sections) in order.
std::vector<QuestStage> quest_stages(const Form& f);

struct FactionRank {
    uint32_t rank = 0;
    std::string male_title;
    std::string female_title;
};
std::vector<FactionRank> faction_ranks(const Form& f); // FACT RNAM/MNAM/FNAM

struct FactionMembership {
    uint32_t faction;
    int8_t rank;
};
std::vector<FactionMembership> npc_factions(const Form& f); // NPC_ SNAM

struct NavmeshTriangle {
    std::array<uint16_t, 3> vertices{};
    std::array<int16_t, 3> edges{}; // the triangle across each edge, -1 if none
    uint16_t flags = 0;
    uint16_t cover_flags = 0;
};
struct NavmeshEdgeLink {
    uint32_t type = 0;
    uint32_t navmesh = 0; // NAVM on the other side
    int16_t triangle = 0; // its triangle
};
struct NavmeshDoorLink {
    int16_t triangle = 0;
    uint32_t door = 0; // REFR
};
struct NavmeshGeometry {
    uint32_t world = 0; // WRLD, or 0 for an interior
    uint32_t cell = 0;  // CELL of an interior navmesh
    int16_t grid_x = 0, grid_y = 0; // exterior cell
    std::vector<std::array<float, 3>> vertices;
    std::vector<NavmeshTriangle> triangles;
    std::vector<NavmeshEdgeLink> edge_links;
    std::vector<NavmeshDoorLink> doors;
    std::vector<int16_t> cover_triangles;
    // Search grid: divisor x divisor cells over the bounds, each the triangles it overlaps.
    uint32_t grid_divisor = 0;
    std::array<float, 2> grid_cell_size{};
    std::array<float, 3> grid_min{}, grid_max{};
    std::vector<std::vector<int16_t>> grid_cells;
};
// NAVM NVNM.
std::optional<NavmeshGeometry> navmesh(const Form& f);

} // namespace spud::forms
