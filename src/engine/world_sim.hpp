#pragma once

// The world's own running: map markers and discovery, encounter zones, cell reset, cleared locations, and the way
// fast travel measures a journey. It holds the state a save will carry and nothing that
// draws; the App feeds it where the player is and acts on what it reports. Cheap every frame: a few hundred distance
// tests and hash lookups, no IO after construction.

#include "engine/game_clock.hpp"
#include "formats/osg.hpp"
#include "formats/osr.hpp"

#include <unordered_set>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud {

class WorldSim {
public:
    static constexpr uint8_t k_marker_visible = 0x1, k_marker_can_travel = 0x2;
    static constexpr uint32_t k_detach_forced = 0xFFFFFFFFu; // a detach time of -1: reset at the next check

    // Throws if the table can't be read.
    WorldSim(const std::filesystem::path& cache, const GameClock& clock, const osr::Rules* rules);

    // Where something is: a worldspace, or an interior cell (world 0).
    struct Place {
        uint32_t world = 0;
        uint32_t cell = 0;
        bool operator==(const Place&) const = default;
    };

    const osg::Worldspace* worldspace(uint32_t form_id) const;
    const osg::Worldspace* worldspace(std::string_view edid) const; // case-insensitive
    // The worldspace whose map data a worldspace shows: up its parents while they share map data (PNAM 0x4).
    const osg::Worldspace* map_world(const osg::Worldspace* world) const;
    const osg::World& table() const { return m_table; }

    // ---- the player ----------------------------------------------------------------------------------------
    // Every frame, in Skyrim units: refreshes the marker list, discovers markers, reports location changes.
    void update(const Place& place, int32_t cell_x, int32_t cell_y, const float position[3]);
    void set_player_level(int32_t level) { m_player_level = level; }
    // on a teleport-style move, before the destination's cells load.
    void player_moved(const Place& to, int32_t cell_x, int32_t cell_y);
    uint32_t player_location() const { return m_location; }
    // The save list's location: an interior's name; outside the map region's, else the cell's, else the world's.
    std::string place_name(const Place& place, const float position[3], int32_t cell_x, int32_t cell_y) const;
    // Its name in file names: the worldspace's EditorID, else the cell's.
    std::string place_edid(const Place& place) const;

    std::function<void(const std::string& name, uint16_t type)> on_discovered;
    std::function<void(const std::string& name)> on_location_name;
    // The volumes of a cell that has just loaded, for OnCellLoad.
    // The story manager's Change Location event (CLOC): old and new location, 0 for none.
    std::function<void(uint32_t from, uint32_t to)> on_location_change;
    uint32_t locations_discovered() const { return m_locations_discovered; }

    // ---- map markers -----------------------------------------------------------------------------------------
    size_t marker_count() const { return m_table.markers.size(); }
    const osg::MapMarker& marker(size_t i) const { return m_table.markers[i]; }
    uint8_t marker_flags(size_t i) const { return m_marker_flags[i]; }
    bool marker_disabled(size_t i) const { return m_marker_disabled[i] != 0; }
    std::string_view marker_name(size_t i) const { return m_table.string(m_table.markers[i].name); }
    std::optional<size_t> find_marker(uint32_t form_id) const;
    // The game's array: every marker of the map the player is on (rebuilt with the worldspace; an interior
    // keeps the last list).
    std::span<const uint32_t> player_markers() const { return m_player_markers; }
    // What the world map lists, in the game's order: the root world's markers, then its child worlds', in form order.
    std::vector<uint32_t> map_entries() const;
    // Up to 15 markers within fHUDCompassLocationMaxDist of the player, nearest first; none in an interior.
    std::vector<uint32_t> compass_markers(const float position[3]) const;
    // A position in `world` on its map's coordinates, and whether a world has no place on the map.
    glm::vec3 to_map(uint32_t world, const float position[3]) const;
    bool off_map(uint32_t world) const;
    void add_to_map(uint32_t form_id, bool allow_travel);
    void set_can_travel(uint32_t form_id, bool can);
    void set_marker_disabled(uint32_t form_id, bool disabled);

    // ---- fast travel -----------------------------------------------------------------------------------------
    // What keeps the player from travelling, as the game asks in turn; systems that aren't
    // there yet leave their answer false.
    struct TravelChecks {
        bool hostile_near = false;
        bool alarm = false;
        bool over_encumbered = false;
        bool taking_damage = false;
        bool in_air = false;
        bool knocked_down = false;
    };
    // The game setting of the refusal's message; empty when travel is allowed.
    std::string_view travel_refusal(const TravelChecks& checks, const Place& place, int32_t cell_x, int32_t cell_y) const;
    void set_fast_travel_enabled(bool enabled) { m_fast_travel_enabled = enabled; }
    bool fast_travel_enabled() const { return m_fast_travel_enabled; }
    // straight in one space; between spaces through the load doors; nullopt without
    // a way.
    std::optional<float> path_length(const Place& from, const float from_position[3], const Place& to,
                                     const float to_position[3]) const;
    // The last door of that way still on the map `from` is on, for a marker to stand at; null in one space or without
    // a way.
    const osg::Door* marker_door(const Place& from, const float from_position[3], const Place& to,
                                 const float to_position[3]) const;
    const osg::Door* first_door(const Place& from, const float from_position[3], const Place& to,
                                const float to_position[3]) const;
    // Whether two places are the same space to walk in (a merged city and its parent are).
    bool same_space(const Place& a, const Place& b) const;

    // ---- encounter zones, cells, locations -------------------------------------------------------------------
    uint32_t cell_zone(uint32_t cell, uint32_t world) const; // The game; 0 for none
    uint32_t cell_location(uint32_t cell, uint32_t world, int32_t x, int32_t y) const;
    int32_t zone_calculated_level(uint32_t zone) const; // The game for the player's level
    // The level a leveled list or lock is worked out at in a zone (0: no zone): the latched level, the live one if
    // none is latched, the player's without a zone; at least 1.
    int32_t zone_level(uint32_t zone) const;
    void latch_zone_level(uint32_t zone); // on entering an interior or a worldspace
    bool zone_can_reset(uint32_t zone) const;

    // What the game decided for a cell that has just got its temporary references.
    struct CellCheck {
        uint32_t cell = 0;
        bool first_visit = false;   // no detach time: persistent references take their real level
        bool forced = false;        // a detach time of -1: Cell.Reset, the arrest path
        bool time_elapsed = false;  // the cell's own timer
        bool reset_allowed = false; // the zone agrees too: references that pass the rules reset
    };
    // Space streaming: a cell's references went or came. The check clears the detach time, whatever it decides.
    // keep_existing: streaming dropped it; a stamp from leaving it stands.
    void cell_detached(uint32_t cell, bool keep_existing = false);
    bool cell_attached(uint32_t cell) const { return m_attached.contains(cell); }
    CellCheck cell_loaded(uint32_t cell, uint32_t world);
    void reset_cell(uint32_t cell); // Papyrus Cell.Reset: the next load resets it
    uint32_t cell_detach_time(uint32_t cell) const;
    // Every check, for what holds per-reference state (items, actors): which applies the reference rules.
    std::function<void(const CellCheck&)> on_cell_check;
    // A zone's level is fixed: leveled picks go by it from now on.
    std::function<void(uint32_t zone, int32_t level)> on_zone_latched;

    // The game: what a cell's check does to one reference, for whatever holds its
    // state. The facts are the holder's; actors go by their own timer, whatever the cell's verdict.
    struct RefFacts {
        bool dynamic = false;      // made at run time (FormID 0xFF......)
        bool aliased = false;      // in a quest alias, or an aliased item in it
        bool actor = false;
        bool dead = false;
        bool starts_dead = false;
        bool respawns = false;     // CONT Respawns, ACBS Respawn, or no REFR flag No Respawn
        bool persistent = false;
        bool plugin_deleted = false; // the record is flagged deleted (in-game flag 0x80)
        bool zone_never_resets = false;
        bool zone_allows = true;   // the reference's own zone, when it differs from the cell's: its CanReset
        uint32_t detach_hours = 0; // an actor's stamp: the hours it left high process (0: never)
        bool location_cleared = false;
    };
    enum class RefReset {
        none,
        delete_ref,    // a forced reset's dynamic reference
        clear_corpse,
        reset_actor,   // Actor::Reset
        reset,         // the common reset, then the kind's own: CONT inventory, DOOR lock, TREE and FLOR harvest,
                       // an item back to its starting place when it isn't persistent
    };
    RefReset ref_reset(const CellCheck& check, const RefFacts& ref) const;

    bool hours_elapsed(uint32_t stamp, bool cleared) const;
    bool location_cleared(uint32_t location) const;
    void set_location_cleared(uint32_t location, bool cleared);
    // after a boss's death: every counted Boss reference dead clears the location.
    // Returns true when it has just flipped to cleared.
    bool update_cleared_state(uint32_t location);
    uint32_t location_parent(uint32_t location) const;
    uint32_t exterior_cell(uint32_t world, int32_t x, int32_t y) const; // 0: none there
    void set_ref_dead(uint32_t ref, bool dead);
    bool ref_dead(uint32_t ref) const;
    bool zone_never_resets(uint32_t zone) const;

    std::string state_json() const;
    // A new game: everything back as the records have it.
    void new_game();

    // A save's change layer (docs/formats/osn.md): the flags that differ from the records and the timers.
    void save(BinaryWriter& w) const;
    void load(BinaryReader& r);

private:
    struct ZoneState {
        uint32_t detach = 0, attach = 0, reset = 0;
        uint16_t level = 0;
    };
    const osg::EncounterZone* zone(uint32_t form_id) const;
    void rebuild_player_markers(const Place& place);
    struct Route {
        float length = 0;
        int32_t marker_door = -1;
        int32_t first_door = -1; // the way out of the space it starts in
    };
    std::optional<Route> route(const Place& from, const float from_position[3], const Place& to,
                               const float to_position[3]) const;
    void zone_changed(uint32_t from, uint32_t to);
    float setting(std::string_view name, float fallback) const;

    const GameClock& m_clock;
    const osr::Rules* m_rules;
    osg::World m_table;
    std::unordered_map<uint32_t, uint32_t> m_marker_index;
    std::unordered_map<uint32_t, uint32_t> m_world_index, m_zone_index, m_cell_index, m_door_index;
    std::unordered_map<std::string, uint32_t> m_world_names;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_space_doors; // world or interior cell -> doors
    std::unordered_map<uint64_t, uint32_t> m_world_cell_location;      // (world, x, y) -> location
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_location_bosses;

    std::vector<uint8_t> m_marker_flags;
    std::vector<uint8_t> m_marker_disabled;
    std::vector<uint32_t> m_player_markers;
    uint32_t m_marker_world = 0; // the worldspace the list was made for
    std::vector<std::string> m_recent_names; // the last two location names the HUD was sent
    uint32_t m_locations_discovered = 0;
    uint32_t m_first_visits = 0, m_elapsed_checks = 0, m_allowed_checks = 0;

    Place m_place;
    int32_t m_cell_x = 0, m_cell_y = 0;
    uint32_t m_location = 0;
    uint32_t m_zone = 0;
    bool m_placed = false;
    int32_t m_player_level = 1;
    bool m_fast_travel_enabled = true;

    float m_reveal_distance = 1000.0f;   // iMapMarkerRevealDistance
    float m_compass_distance = 20000.0f; // fHUDCompassLocationMaxDist
    uint32_t m_hours_respawn = 240, m_hours_respawn_cleared = 720;
    float m_level_scaling = 1.0f; // fLevelScalingMult

    std::unordered_map<uint32_t, ZoneState> m_zones;
    std::unordered_map<uint32_t, uint32_t> m_detach;
    std::unordered_map<uint32_t, bool> m_cleared;
    std::unordered_map<uint32_t, bool> m_dead_refs;
    std::unordered_set<uint32_t> m_attached; // cells with their 3D now
    std::unordered_map<uint32_t, uint32_t> m_location_index;
    std::unordered_map<uint64_t, uint32_t> m_exterior_cells; // (world, x, y) -> cell
};

} // namespace spud
