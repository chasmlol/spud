#pragma once

// .osg — spud game world: what the world simulation needs of the load order, gathered at import so nothing scans
// the forms database at run time (rules/world.osg in the cache). Spec: docs/formats/osg.md
//
// Map markers, each worldspace's map data, encounter zones, locations, load doors, and the zone and location of every
// cell that names one. Positions and angles stay in Skyrim units and radians, as the world simulation's facts are stated.

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::osg {

inline constexpr container::FourCC k_kind = container::fourcc("GWLD");
inline constexpr uint32_t k_version = 1;
inline constexpr uint32_t k_none = 0xFFFFFFFFu;

// A MapMarker static's reference with marker data (XMRK, FNAM, FULL, TNAM).
struct MapMarker {
    uint32_t form_id;
    uint32_t world;  // WRLD form ID, 0 for a marker in an interior
    uint32_t cell;   // CELL form ID
    uint32_t name;   // string: FULL, k_none without one
    float position[3];
    float rotation_z;
    float radius;    // XRDS, 0 without
    uint8_t flags;   // FNAM as stored: 0x1 visible, 0x2 can travel to, 0x4 show all hidden
    uint8_t start_disabled; // 1: initially disabled, by its own flag or its enable parent's
    uint16_t type;   // TNAM: the map movie's IconTypes index
    uint32_t record_flags;  // REFR header flags (0x800 initially disabled)
    uint32_t enable_parent; // XESP's reference, 0 without
    uint32_t enable_flags;  // XESP's flags
    // Where fast travel lands: the marker's no-keyword linked reference (XLKR), else the marker.
    uint32_t arrival_world; // WRLD form ID, 0 for an interior
    uint32_t arrival_cell;
    float arrival_position[3];
    float arrival_rotation[3];
};
static_assert(sizeof(MapMarker) == 84);

struct Worldspace {
    uint32_t form_id;
    uint32_t edid;         // string
    uint32_t parent;       // WNAM form ID, 0 for none
    uint16_t parent_flags; // PNAM
    uint8_t flags;         // DATA
    uint8_t reserved0;
    int32_t usable_size[2]; // MNAM: the map's usable width and height
    int16_t nw_cell[2];     // MNAM
    int16_t se_cell[2];
    float camera_min_height; // MNAM
    float camera_max_height;
    float camera_initial_pitch;
    float map_scale;      // ONAM, 1 when absent
    float map_offset[3];  // ONAM: cell x, y, z offset
    float bounds_min[2];  // NAM0
    float bounds_max[2];  // NAM9
    uint32_t encounter_zone; // XEZN, 0 for none
    uint32_t location;       // XLCN, 0 for none
    uint32_t space;          // string: the lower-case name of the .osw that holds it (a city: the world it merged into)
};
static_assert(sizeof(Worldspace) == 88);

struct EncounterZone {
    uint32_t form_id;
    uint32_t owner;    // NPC_ or FACT form ID, 0 for none
    uint32_t location; // LCTN form ID, 0 for none
    int8_t rank;
    int8_t min_level;
    uint8_t flags;     // DATA as stored
    int8_t max_level;
};
static_assert(sizeof(EncounterZone) == 16);

struct Location {
    uint32_t form_id;
    uint32_t name;    // string: FULL, k_none without one
    uint32_t parent;  // PNAM form ID, 0 for none
    uint32_t marker;  // MNAM: the world location marker, 0 for none
    float radius;     // RNAM
    uint32_t first_keyword; // into keywords
    uint32_t keyword_count;
    uint32_t reserved;
};
static_assert(sizeof(Location) == 32);

// A cell that names an encounter zone or a location, or has flags the simulation reads.
struct Cell {
    uint32_t form_id;
    uint32_t world;  // WRLD form ID, 0 for an interior
    int16_t x, y;    // exterior grid
    uint16_t flags;  // DATA as stored
    uint16_t reserved0;
    uint32_t encounter_zone; // XEZN, 0 for none
    uint32_t location;       // XLCN, 0 for none
};
static_assert(sizeof(Cell) == 24);

// One area of a region with a map name (REGN RDMP): the save list's location outside.
struct MapRegion {
    uint32_t world;
    uint32_t name;        // string
    uint32_t first_point; // into region_points: x, y pairs
    uint32_t point_count;
};
static_assert(sizeof(MapRegion) == 16);

// A load door: the graph fast travel measures its way through between spaces.
struct Door {
    uint32_t form_id;
    uint32_t world; // WRLD form ID, 0 for a door in an interior
    uint32_t cell;  // CELL form ID
    uint32_t destination; // XTEL: the door it leads to
    float position[3];
    float exit_position[3]; // XTEL: where the player comes out
    uint32_t flags;         // DoorFlag
};
static_assert(sizeof(Door) == 44);

enum DoorFlag : uint32_t {
    door_minimal_use = 0x1, // DOOR FNAM 0x8: the search adds 409600 to it
    door_disabled = 0x2,    // initially disabled
};

// A location's special reference (LCSR, ACSR, less RCSR).
struct LocationRef {
    uint32_t location;
    uint32_t ref_type; // LCRT form ID
    uint32_t ref;
};
static_assert(sizeof(LocationRef) == 12);

class World {
public:
    std::vector<MapMarker> markers;
    std::vector<Worldspace> worldspaces;
    std::vector<EncounterZone> zones;
    std::vector<Location> locations;
    std::vector<Cell> cells;
    std::vector<uint32_t> keywords;
    std::vector<Door> doors;
    std::vector<LocationRef> location_refs;
    // Added chunks: a reader that doesn't know them skips them. One string per worldspace; a name and an EditorID per
    // cell, in the order of `worldspaces` and `cells`.
    std::vector<uint32_t> world_names;
    std::vector<uint32_t> cell_names, cell_edids;
    std::vector<MapRegion> map_regions;
    std::vector<float> region_points;

    uint32_t add_string(std::string_view s);
    std::string_view string(uint32_t offset) const;
    const std::vector<char>& string_data() const { return m_strings; }
    void set_string_data(std::vector<char> data);

private:
    std::vector<char> m_strings;
    std::unordered_map<std::string, uint32_t> m_string_index;
};

std::vector<std::byte> write(const World& w);
World read(std::span<const std::byte> file); // throws ParseError

} // namespace spud::osg
