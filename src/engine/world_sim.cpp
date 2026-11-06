#include "engine/world_sim.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "formats/osl.hpp"
#include "formats/ost.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <queue>

namespace spud {

namespace {

constexpr uint32_t k_cell_interior = 0x1, k_cell_travel = 0x4; // CELL DATA; the second reads the other way in interiors
constexpr uint32_t k_world_no_travel = 0x2;                    // WRLD DATA
constexpr uint16_t k_parent_map_data = 0x4;                    // WRLD PNAM
constexpr uint8_t k_zone_never_resets = 0x1, k_zone_match_pc_below_minimum = 0x2;
constexpr uint32_t k_loc_ref_boss = 0x000130F7; // LCRT Boss (DOBJ 34)
constexpr float k_minimal_use_cost = 409600.0f;
constexpr size_t k_compass_nearest = 15;

float distance(const float a[3], const float b[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

uint64_t cell_key(uint32_t world, int32_t x, int32_t y) {
    return uint64_t{world} << 32 | uint64_t{uint16_t(int16_t(x))} << 16 | uint16_t(int16_t(y));
}

uint32_t space_of(const WorldSim::Place& p) { return p.world ? p.world : p.cell; }

} // namespace

WorldSim::WorldSim(const std::filesystem::path& cache, const GameClock& clock, const osr::Rules* rules)
    : m_clock(clock), m_rules(rules), m_table(osg::read(read_file(cache / "rules" / "world.osg"))) {
    m_reveal_distance = setting("iMapMarkerRevealDistance", m_reveal_distance);
    m_compass_distance = setting("fHUDCompassLocationMaxDist", m_compass_distance);
    m_hours_respawn = uint32_t(setting("iHoursToRespawnCell", float(m_hours_respawn)));
    m_hours_respawn_cleared = uint32_t(setting("iHoursToRespawnCellCleared", float(m_hours_respawn_cleared)));
    m_level_scaling = setting("fLevelScalingMult", m_level_scaling);

    m_marker_flags.resize(m_table.markers.size());
    m_marker_disabled.resize(m_table.markers.size());
    for (size_t i = 0; i < m_table.markers.size(); ++i) {
        const osg::MapMarker& m = m_table.markers[i];
        m_marker_index.emplace(m.form_id, uint32_t(i));
        // Can Travel without Visible is dropped.
        m_marker_flags[i] = (m.flags & k_marker_visible) ? m.flags : m.flags & ~k_marker_can_travel;
        m_marker_disabled[i] = m.start_disabled;
    }
    for (size_t i = 0; i < m_table.worldspaces.size(); ++i) {
        const osg::Worldspace& w = m_table.worldspaces[i];
        m_world_index.emplace(w.form_id, uint32_t(i));
        m_world_names.emplace(path::to_lower(m_table.string(w.edid)), w.form_id);
    }
    for (size_t i = 0; i < m_table.zones.size(); ++i) {
        m_zone_index.emplace(m_table.zones[i].form_id, uint32_t(i));
    }
    for (size_t i = 0; i < m_table.cells.size(); ++i) {
        m_cell_index.emplace(m_table.cells[i].form_id, uint32_t(i));
        if (const osg::Cell& c = m_table.cells[i]; c.world) {
            m_exterior_cells.emplace(cell_key(c.world, c.x, c.y), c.form_id);
        }
    }
    for (size_t i = 0; i < m_table.doors.size(); ++i) {
        const osg::Door& d = m_table.doors[i];
        m_door_index.emplace(d.form_id, uint32_t(i));
        m_space_doors[d.world ? d.world : d.cell].push_back(uint32_t(i));
    }
    for (size_t i = 0; i < m_table.locations.size(); ++i) {
        m_location_index.emplace(m_table.locations[i].form_id, uint32_t(i));
    }
    for (const osg::LocationRef& r : m_table.location_refs) {
        if (r.ref_type == k_loc_ref_boss) {
            m_location_bosses[r.location].push_back(r.ref);
        }
    }
    // Which location an exterior cell lies in: the locations' cell lists, as the Loading Menu reads them.
    const auto screens = cache / "interface" / "loadingscreens.osl";
    std::error_code ec;
    if (std::filesystem::exists(screens, ec)) {
        const osl::LoadingScreens l = osl::read(read_file(screens));
        for (const osl::WorldCell& c : l.world_cells) {
            m_world_cell_location[cell_key(c.worldspace, c.x, c.y)] = c.location;
        }
    }
    log::info("world: {} map markers, {} encounter zones, {} locations, {} load doors", m_table.markers.size(),
              m_table.zones.size(), m_table.locations.size(), m_table.doors.size());
}

float WorldSim::setting(std::string_view name, float fallback) const {
    const auto v = m_rules ? m_rules->setting(name) : std::nullopt;
    return v ? float(*v) : fallback;
}

const osg::Worldspace* WorldSim::worldspace(uint32_t form_id) const {
    const auto it = m_world_index.find(form_id);
    return it == m_world_index.end() ? nullptr : &m_table.worldspaces[it->second];
}

const osg::Worldspace* WorldSim::worldspace(std::string_view edid) const {
    const auto it = m_world_names.find(path::to_lower(edid));
    return it == m_world_names.end() ? nullptr : worldspace(it->second);
}

const osg::Worldspace* WorldSim::map_world(const osg::Worldspace* world) const {
    while (world && world->parent && (world->parent_flags & k_parent_map_data)) {
        const osg::Worldspace* parent = worldspace(world->parent);
        if (!parent) {
            break;
        }
        world = parent;
    }
    return world;
}

const osg::EncounterZone* WorldSim::zone(uint32_t form_id) const {
    const auto it = m_zone_index.find(form_id);
    return it == m_zone_index.end() ? nullptr : &m_table.zones[it->second];
}

void WorldSim::rebuild_player_markers(const Place& place) {
    // The game: an interior keeps the list; the game
    // takes the root world's persistent cell, then those of its children that share its map data.
    if (!place.world || (place.world == m_marker_world && !m_player_markers.empty())) {
        return;
    }
    m_marker_world = place.world;
    m_player_markers.clear();
    const osg::Worldspace* root = map_world(worldspace(place.world));
    if (!root) {
        return;
    }
    auto add_world = [&](uint32_t world) {
        for (size_t i = 0; i < m_table.markers.size(); ++i) {
            if (m_table.markers[i].world == world) {
                m_player_markers.push_back(uint32_t(i));
            }
        }
    };
    add_world(root->form_id);
    for (const osg::Worldspace& w : m_table.worldspaces) {
        if (w.parent == root->form_id && (w.parent_flags & k_parent_map_data)) {
            add_world(w.form_id);
        }
    }
}

void WorldSim::update(const Place& place, int32_t cell_x, int32_t cell_y, const float position[3]) {
    if (!m_placed || place != m_place) {
        player_moved(place, cell_x, cell_y);
    }
    m_cell_x = cell_x;
    m_cell_y = cell_y;
    rebuild_player_markers(place);

    const uint32_t location = cell_location(place.cell, place.world, cell_x, cell_y);
    if (location != m_location) {
        if (on_location_change) {
            on_location_change(m_location, location);
        }
        m_location = location;
    }
    if (!place.world) {
        return;
    }
    // The game: the game slices this over frames by a 0.05 ms budget, which
    // the list fits in. A marker counts in the player's own worldspace only, inside its radius (strictly).
    for (const uint32_t i : m_player_markers) {
        const osg::MapMarker& m = m_table.markers[i];
        if (m_marker_disabled[i] || m.world != place.world) {
            continue;
        }
        const float radius = std::abs(m.radius) < 0.001f ? m_reveal_distance : m.radius;
        if (distance(position, m.position) >= radius) {
            continue;
        }
        const std::string name(m_table.string(m.name));
        if ((m_marker_flags[i] & (k_marker_visible | k_marker_can_travel)) == (k_marker_visible | k_marker_can_travel)) {
            // HUD data type 29, deduplicated against the last two names.
            if (std::find(m_recent_names.begin(), m_recent_names.end(), name) == m_recent_names.end()) {
                if (m_recent_names.size() == 2) {
                    m_recent_names.erase(m_recent_names.begin());
                }
                m_recent_names.push_back(name);
                if (on_location_name) {
                    on_location_name(name);
                }
            }
            continue;
        }
        m_marker_flags[i] |= k_marker_visible | k_marker_can_travel;
        ++m_locations_discovered;
        log::info("world: discovered {} ({:08X})", name, m.form_id);
        if (on_discovered) {
            on_discovered(name, m.type);
        }
    }
}

std::string WorldSim::place_name(const Place& place, const float position[3], int32_t cell_x, int32_t cell_y) const {
    if (!place.world) {
        const auto it = m_cell_index.find(place.cell);
        return it == m_cell_index.end() || m_table.cell_names.empty() ? std::string()
                                                                     : std::string(m_table.string(m_table.cell_names[it->second]));
    }
    // Merged cities' cells carry the city's own regions under its parent's map.
    const osg::Worldspace* root = map_world(worldspace(place.world));
    for (const osg::MapRegion& r : m_table.map_regions) {
        if (r.world != place.world && !(root && r.world == root->form_id)) {
            continue;
        }
        bool inside = false;
        const float* p = m_table.region_points.data() + r.first_point;
        for (uint32_t i = 0, j = r.point_count - 1; i < r.point_count; j = i++) {
            const float xi = p[i * 2], yi = p[i * 2 + 1], xj = p[j * 2], yj = p[j * 2 + 1];
            if ((yi > position[1]) != (yj > position[1]) &&
                position[0] < (xj - xi) * (position[1] - yi) / (yj - yi) + xi) {
                inside = !inside;
            }
        }
        if (inside) {
            return std::string(m_table.string(r.name));
        }
    }
    for (size_t i = 0; i < m_table.cell_names.size(); ++i) {
        const osg::Cell& c = m_table.cells[i];
        if (c.world == place.world && c.x == cell_x && c.y == cell_y && m_table.cell_names[i] != osg::k_none) {
            return std::string(m_table.string(m_table.cell_names[i]));
        }
    }
    const auto w = m_world_index.find(place.world);
    return w == m_world_index.end() || m_table.world_names.empty() ? std::string()
                                                                  : std::string(m_table.string(m_table.world_names[w->second]));
}

std::string WorldSim::place_edid(const Place& place) const {
    if (const osg::Worldspace* w = worldspace(place.world)) {
        return std::string(m_table.string(w->edid));
    }
    const auto it = m_cell_index.find(place.cell);
    return it == m_cell_index.end() || m_table.cell_edids.empty() ? std::string()
                                                                 : std::string(m_table.string(m_table.cell_edids[it->second]));
}

void WorldSim::player_moved(const Place& to, int32_t cell_x, int32_t cell_y) {
    m_place = to;
    m_placed = true;
    m_cell_x = cell_x;
    m_cell_y = cell_y;
    const uint32_t next = cell_zone(to.cell, to.world);
    if (next != m_zone) {
        zone_changed(m_zone, next);
        m_zone = next;
    }
    // The game latches the cell's zone, the game the world's.
    if (to.world) {
        if (const osg::Worldspace* w = worldspace(to.world)) {
            latch_zone_level(w->encounter_zone);
        }
    } else {
        latch_zone_level(next);
    }
}

void WorldSim::zone_changed(uint32_t from, uint32_t to) {
    const uint32_t now = m_clock.hours_passed();
    if (from) {
        m_zones[from].detach = now; // OnPlayerLeave
    }
    if (to) {
        // OnPlayerEnter: the reset time is stamped once the zone has been left alone long enough.
        ZoneState& s = m_zones[to];
        s.attach = now;
        const osg::EncounterZone* z = zone(to);
        if (hours_elapsed(s.detach, z && location_cleared(z->location))) {
            s.reset = now;
        }
    }
}

std::optional<size_t> WorldSim::find_marker(uint32_t form_id) const {
    const auto it = m_marker_index.find(form_id);
    return it == m_marker_index.end() ? std::nullopt : std::optional<size_t>(it->second);
}

std::vector<uint32_t> WorldSim::map_entries() const {
    // alive, enabled, Visible; any worldspace of the list.
    std::vector<uint32_t> out;
    for (const uint32_t i : m_player_markers) {
        if (!m_marker_disabled[i] && (m_marker_flags[i] & k_marker_visible)) {
            out.push_back(i);
        }
    }
    return out;
}

std::vector<uint32_t> WorldSim::compass_markers(const float position[3]) const {
    // The game: Visible isn't tested, so undiscovered and hidden markers show; the nearby
    // list holds the player's worldspace within the radius, and the 15 nearest go on.
    std::vector<std::pair<float, uint32_t>> near;
    if (!m_place.world) {
        return {};
    }
    for (const uint32_t i : m_player_markers) {
        const osg::MapMarker& m = m_table.markers[i];
        if (m_marker_disabled[i] || m.world != m_place.world) {
            continue;
        }
        const float d = distance(position, m.position);
        if (d <= m_compass_distance) {
            near.emplace_back(d, i);
        }
    }
    std::sort(near.begin(), near.end());
    std::vector<uint32_t> out;
    for (size_t i = 0; i < near.size() && i < k_compass_nearest; ++i) {
        out.push_back(near[i].second);
    }
    return out;
}

glm::vec3 WorldSim::to_map(uint32_t world, const float position[3]) const {
    // The game: scaled about the middle of the world, then moved.
    glm::vec3 p(position[0], position[1], position[2]);
    const osg::Worldspace* w = worldspace(world);
    if (!w) {
        return p;
    }
    if (w->map_scale != 0.0f && w->map_scale != 1.0f) {
        const glm::vec2 centre = (glm::vec2(w->bounds_min[0], w->bounds_min[1]) + glm::vec2(w->bounds_max[0], w->bounds_max[1])) * 0.5f;
        p = glm::vec3((glm::vec2(p) - centre) * w->map_scale + centre, p.z * w->map_scale);
    }
    return p + glm::vec3(w->map_offset[0], w->map_offset[1], w->map_offset[2]);
}

bool WorldSim::off_map(uint32_t world) const {
    // The game: ONAM scale -1 with no offset.
    const osg::Worldspace* w = worldspace(world);
    return w && w->map_scale == -1.0f && !w->map_offset[0] && !w->map_offset[1] && !w->map_offset[2];
}

void WorldSim::add_to_map(uint32_t form_id, bool allow_travel) {
    if (const auto i = find_marker(form_id)) {
        m_marker_flags[*i] |= k_marker_visible | (allow_travel ? k_marker_can_travel : 0);
    }
}

void WorldSim::set_can_travel(uint32_t form_id, bool can) {
    if (const auto i = find_marker(form_id)) {
        m_marker_flags[*i] = can ? m_marker_flags[*i] | k_marker_can_travel : m_marker_flags[*i] & ~k_marker_can_travel;
    }
}

void WorldSim::set_marker_disabled(uint32_t form_id, bool disabled) {
    if (const auto i = find_marker(form_id)) {
        m_marker_disabled[*i] = disabled;
    }
}

std::string_view WorldSim::travel_refusal(const TravelChecks& c, const Place& place, int32_t cell_x, int32_t cell_y) const {
    // The game, in its order. sNoFastTravelCombat and sNoFastTravelUndiscovered
    // exist but nothing reads them.
    if (c.hostile_near) {
        return "sNoFastTravelHostileActorsNear";
    }
    if (c.alarm) {
        return "sNoFastTravelAlarm";
    }
    if (c.over_encumbered) {
        return "sNoFastTravelOverencumbered";
    }
    if (c.taking_damage) {
        return "sFastTravelNoTravelHealthDamage";
    }
    if (!m_fast_travel_enabled) {
        return "sNoFastTravelScriptBlock";
    }
    if (c.in_air) {
        return "sNoFastTravelInAir";
    }
    if (c.knocked_down) {
        return "sNoFastTravelDefault";
    }
    // The bit reads the other way round in an interior: set, it allows travel.
    if (!place.world) {
        const auto it = m_cell_index.find(place.cell);
        if (it == m_cell_index.end() || !(m_table.cells[it->second].flags & k_cell_travel)) {
            return "sNoFastTravelCell";
        }
        return {};
    }
    if (const osg::Worldspace* w = worldspace(place.world); w && (w->flags & k_world_no_travel)) {
        return "sNoFastTravelCell";
    }
    for (const osg::Cell& cell : m_table.cells) {
        if (cell.world == place.world && cell.x == cell_x && cell.y == cell_y && (cell.flags & k_cell_travel) &&
            !(cell.flags & k_cell_interior)) {
            return "sNoFastTravelCell";
        }
    }
    return {};
}

std::optional<float> WorldSim::path_length(const Place& from, const float from_position[3], const Place& to,
                                           const float to_position[3]) const {
    const auto r = route(from, from_position, to, to_position);
    return r ? std::optional<float>(r->length) : std::nullopt;
}

const osg::Door* WorldSim::marker_door(const Place& from, const float from_position[3], const Place& to,
                                       const float to_position[3]) const {
    const auto r = route(from, from_position, to, to_position);
    return r && r->marker_door >= 0 ? &m_table.doors[size_t(r->marker_door)] : nullptr;
}

const osg::Door* WorldSim::first_door(const Place& from, const float from_position[3], const Place& to,
                                      const float to_position[3]) const {
    const auto r = route(from, from_position, to, to_position);
    return r && r->first_door >= 0 ? &m_table.doors[size_t(r->first_door)] : nullptr;
}

bool WorldSim::same_space(const Place& a, const Place& b) const {
    if (space_of(a) == space_of(b)) {
        return true;
    }
    const osg::Worldspace *wa = worldspace(a.world), *wb = worldspace(b.world);
    return wa && wb && wa->space == wb->space;
}

std::optional<WorldSim::Route> WorldSim::route(const Place& from, const float from_position[3], const Place& to,
                                               const float to_position[3]) const {
    if (space_of(from) == space_of(to)) {
        return Route{distance(from_position, to_position), -1};
    }
    // TeleportDoorSearch: spaces joined by load doors, straight lines between them. A door that is minimal
    // use costs the search more but adds nothing to the length.
    struct Node {
        float cost;
        uint32_t door; // the door walked to; its exit is where the next leg starts
        bool operator>(const Node& o) const { return cost > o.cost; }
    };
    std::vector<float> best(m_table.doors.size(), std::numeric_limits<float>::max());
    std::vector<float> length(m_table.doors.size(), 0.0f);
    std::vector<int32_t> marker(m_table.doors.size(), -1), first(m_table.doors.size(), -1);
    const osg::Worldspace* map = map_world(worldspace(from.world));
    auto on_the_map = [&](const osg::Door& door) {
        return map ? door.world && map_world(worldspace(door.world)) == map : door.cell == from.cell;
    };
    std::priority_queue<Node, std::vector<Node>, std::greater<>> open;
    auto walk_to_doors = [&](uint32_t space, const float at[3], float cost, float walked, int32_t by, int32_t started) {
        const auto it = m_space_doors.find(space);
        if (it == m_space_doors.end()) {
            return;
        }
        for (const uint32_t d : it->second) {
            const osg::Door& door = m_table.doors[d];
            if (door.flags & osg::door_disabled) {
                continue;
            }
            const float leg = distance(at, door.position);
            const float c = cost + leg + ((door.flags & osg::door_minimal_use) ? k_minimal_use_cost : 0.0f);
            if (c < best[d]) {
                best[d] = c;
                length[d] = walked + leg;
                marker[d] = on_the_map(door) ? int32_t(d) : by;
                first[d] = started >= 0 ? started : int32_t(d);
                open.push({c, d});
            }
        }
    };
    walk_to_doors(space_of(from), from_position, 0.0f, 0.0f, -1, -1);
    float best_cost = std::numeric_limits<float>::max();
    std::optional<Route> result;
    while (!open.empty()) {
        const Node n = open.top();
        open.pop();
        if (n.cost > best[n.door] || n.cost >= best_cost) {
            continue;
        }
        const osg::Door& door = m_table.doors[n.door];
        const auto dest = m_door_index.find(door.destination);
        if (dest == m_door_index.end()) {
            continue;
        }
        const osg::Door& exit = m_table.doors[dest->second];
        const uint32_t space = exit.world ? exit.world : exit.cell;
        if (space == space_of(to)) {
            const float leg = distance(door.exit_position, to_position);
            if (n.cost + leg < best_cost) {
                best_cost = n.cost + leg;
                result = Route{length[n.door] + leg, marker[n.door], first[n.door]};
            }
            continue;
        }
        walk_to_doors(space, door.exit_position, n.cost, length[n.door], marker[n.door], first[n.door]);
    }
    return result;
}

uint32_t WorldSim::cell_zone(uint32_t cell, uint32_t world) const {
    const auto it = m_cell_index.find(cell);
    if (it != m_cell_index.end() && m_table.cells[it->second].encounter_zone) {
        return m_table.cells[it->second].encounter_zone;
    }
    const osg::Worldspace* w = worldspace(world);
    return w ? w->encounter_zone : 0;
}

uint32_t WorldSim::cell_location(uint32_t cell, uint32_t world, int32_t x, int32_t y) const {
    // the zone's, else the cell's own; outside, the world's map of cells, else the world's.
    const auto it = m_cell_index.find(cell);
    if (it != m_cell_index.end()) {
        const osg::Cell& c = m_table.cells[it->second];
        if (const osg::EncounterZone* z = zone(c.encounter_zone); z && z->location) {
            return z->location;
        }
        if (c.location) {
            return c.location;
        }
    }
    if (world) {
        if (const auto at = m_world_cell_location.find(cell_key(world, x, y)); at != m_world_cell_location.end()) {
            return at->second;
        }
        if (const osg::Worldspace* w = worldspace(world)) {
            const osg::EncounterZone* z = zone(w->encounter_zone);
            return w->location ? w->location : z ? z->location : 0;
        }
    }
    return 0;
}

int32_t WorldSim::zone_calculated_level(uint32_t form_id) const {
    // The game.
    const osg::EncounterZone* z = zone(form_id);
    if (!z) {
        return m_player_level;
    }
    int32_t level = z->min_level;
    if (m_player_level >= z->min_level) {
        level = int32_t(float(m_player_level - z->min_level) * m_level_scaling + float(z->min_level));
    } else if (z->flags & k_zone_match_pc_below_minimum) {
        level = m_player_level;
    }
    return z->max_level ? std::min<int32_t>(level, z->max_level) : level;
}

int32_t WorldSim::zone_level(uint32_t form_id) const {
    if (!form_id) {
        return std::max(m_player_level, 1);
    }
    const auto it = m_zones.find(form_id);
    const int32_t level = it != m_zones.end() && it->second.level ? it->second.level : zone_calculated_level(form_id);
    return std::max(level, 1);
}

void WorldSim::latch_zone_level(uint32_t form_id) {
    // only when no level is held; nothing ever levels a zone again.
    if (form_id && zone(form_id)) {
        ZoneState& s = m_zones[form_id];
        if (!s.level) {
            s.level = uint16_t(zone_calculated_level(form_id));
            if (on_zone_latched) {
                on_zone_latched(form_id, s.level);
            }
        }
    }
}

bool WorldSim::zone_can_reset(uint32_t form_id) const {
    // The game.
    const osg::EncounterZone* z = zone(form_id);
    if (!z) {
        return true;
    }
    if (z->flags & k_zone_never_resets) {
        return false;
    }
    const auto it = m_zones.find(form_id);
    if (it == m_zones.end() || !it->second.level) {
        return true;
    }
    return hours_elapsed(it->second.detach, location_cleared(z->location));
}

bool WorldSim::hours_elapsed(uint32_t stamp, bool cleared) const {
    if (!stamp) {
        return true;
    }
    const uint32_t now = m_clock.hours_passed();
    return now > stamp && now - stamp > (cleared ? m_hours_respawn_cleared : m_hours_respawn);
}

void WorldSim::cell_detached(uint32_t cell, bool keep_existing) {
    m_attached.erase(cell);
    if (cell && cell < 0xFF000000u && !(keep_existing && m_detach.count(cell))) {
        m_detach[cell] = m_clock.hours_passed();
    }
}

WorldSim::CellCheck WorldSim::cell_loaded(uint32_t cell, uint32_t world) {
    // The game.
    m_attached.insert(cell);
    CellCheck c;
    c.cell = cell;
    const auto it = m_detach.find(cell);
    if (it == m_detach.end() || !it->second) {
        c.first_visit = true;
    } else if (it->second == k_detach_forced) {
        c.forced = c.time_elapsed = c.reset_allowed = true;
    } else {
        const auto known = m_cell_index.find(cell);
        const int32_t x = known != m_cell_index.end() ? m_table.cells[known->second].x : 0;
        const int32_t y = known != m_cell_index.end() ? m_table.cells[known->second].y : 0;
        c.time_elapsed = hours_elapsed(it->second, location_cleared(cell_location(cell, world, x, y)));
        c.reset_allowed = c.time_elapsed;
        if (const uint32_t z = cell_zone(cell, world); z && zone(z)) {
            // With a zone its verdict stands and the cell's own hours don't count: a zone resets as a unit, each cell
            // left before the zone's reset stamp once as it loads (CellResetPending).
            const auto s = m_zones.find(z);
            const uint32_t stamp = s == m_zones.end() ? 0 : s->second.reset;
            c.reset_allowed = zone_can_reset(z) && stamp != 0 && it->second < stamp;
        }
    }
    if (it != m_detach.end()) {
        m_detach.erase(it);
    }
    m_first_visits += c.first_visit;
    m_elapsed_checks += c.time_elapsed;
    m_allowed_checks += c.reset_allowed;
    if (on_cell_check) {
        on_cell_check(c);
    }
    return c;
}

WorldSim::RefReset WorldSim::ref_reset(const CellCheck& check, const RefFacts& ref) const {
    if (check.first_visit) {
        return RefReset::none; // no reset, levelled entries roll their real level
    }
    if (check.forced && ref.dynamic) {
        return ref.aliased ? RefReset::none : RefReset::delete_ref;
    }
    if (ref.actor) {
        // The game: its own stamp against the hours, never the cell's verdict.
        const bool due = !ref.plugin_deleted && !ref.zone_never_resets && hours_elapsed(ref.detach_hours, ref.location_cleared);
        if (due || (check.forced && ref.detach_hours != m_clock.hours_passed())) {
            return RefReset::reset_actor;
        }
        const uint32_t hours_to_clear = uint32_t(setting("iHoursToClearCorpses", 24.0f));
        const uint32_t now = m_clock.hours_passed();
        if (ref.dead && !ref.starts_dead && ref.detach_hours && now > ref.detach_hours &&
            now - ref.detach_hours > hours_to_clear) {
            return RefReset::clear_corpse;
        }
        return RefReset::none;
    }
    // The zone's gate, then the game.
    if (!check.reset_allowed || !ref.zone_allows || !ref.respawns || ref.dynamic) {
        return RefReset::none;
    }
    return RefReset::reset;
}

void WorldSim::reset_cell(uint32_t cell) { m_detach[cell] = k_detach_forced; }

uint32_t WorldSim::cell_detach_time(uint32_t cell) const {
    const auto it = m_detach.find(cell);
    return it == m_detach.end() ? 0 : it->second;
}

bool WorldSim::location_cleared(uint32_t location) const {
    const auto it = m_cleared.find(location);
    return it != m_cleared.end() && it->second;
}

void WorldSim::set_location_cleared(uint32_t location, bool cleared) { m_cleared[location] = cleared; }

bool WorldSim::update_cleared_state(uint32_t location) {
    // The game: every Boss reference dead; with no boss the state stands.
    const auto it = m_location_bosses.find(location);
    if (it == m_location_bosses.end() || it->second.empty()) {
        return false;
    }
    bool all_dead = true;
    for (const uint32_t ref : it->second) {
        const auto dead = m_dead_refs.find(ref);
        all_dead = all_dead && dead != m_dead_refs.end() && dead->second;
    }
    const bool flipped = all_dead && !location_cleared(location);
    m_cleared[location] = all_dead;
    return flipped;
}

uint32_t WorldSim::exterior_cell(uint32_t world, int32_t x, int32_t y) const {
    const auto it = m_exterior_cells.find(cell_key(world, x, y));
    return it == m_exterior_cells.end() ? 0 : it->second;
}

uint32_t WorldSim::location_parent(uint32_t location) const {
    const auto it = m_location_index.find(location);
    return it == m_location_index.end() ? 0 : m_table.locations[it->second].parent;
}

void WorldSim::set_ref_dead(uint32_t ref, bool dead) { m_dead_refs[ref] = dead; }

bool WorldSim::ref_dead(uint32_t ref) const {
    const auto it = m_dead_refs.find(ref);
    return it != m_dead_refs.end() && it->second;
}

bool WorldSim::zone_never_resets(uint32_t form_id) const {
    const osg::EncounterZone* z = zone(form_id);
    return z && (z->flags & k_zone_never_resets);
}

void WorldSim::save(BinaryWriter& w) const {
    uint32_t changed = 0;
    BinaryWriter markers;
    for (size_t i = 0; i < m_table.markers.size(); ++i) {
        const osg::MapMarker& m = m_table.markers[i];
        const uint8_t start = (m.flags & k_marker_visible) ? m.flags : m.flags & ~k_marker_can_travel;
        if (m_marker_flags[i] != start || m_marker_disabled[i] != m.start_disabled) {
            markers.write<uint32_t>(m.form_id);
            markers.write<uint8_t>(m_marker_flags[i]);
            markers.write<uint8_t>(m_marker_disabled[i]);
            ++changed;
        }
    }
    w.write<uint32_t>(changed);
    w.write_bytes(markers.buffer());
    w.write<uint32_t>(m_locations_discovered);
    w.write<uint8_t>(m_fast_travel_enabled ? 1 : 0);
    const auto zones = ost::sorted(m_zones);
    w.write<uint32_t>(uint32_t(zones.size()));
    for (const auto& [form, s] : zones) {
        w.write<uint32_t>(form);
        w.write<uint32_t>(s.detach);
        w.write<uint32_t>(s.attach);
        w.write<uint32_t>(s.reset);
        w.write<uint16_t>(s.level);
    }
    const auto detach = ost::sorted(m_detach);
    w.write<uint32_t>(uint32_t(detach.size()));
    for (const auto& [cell, hours] : detach) {
        w.write<uint32_t>(cell);
        w.write<uint32_t>(hours);
    }
    const auto cleared = ost::sorted(m_cleared);
    w.write<uint32_t>(uint32_t(cleared.size()));
    for (const auto& [location, on] : cleared) {
        w.write<uint32_t>(location);
        w.write<uint8_t>(on ? 1 : 0);
    }
    const auto dead = ost::sorted(m_dead_refs);
    w.write<uint32_t>(uint32_t(dead.size()));
    for (const auto& [ref, on] : dead) {
        w.write<uint32_t>(ref);
        w.write<uint8_t>(on ? 1 : 0);
    }
}

void WorldSim::new_game() {
    for (size_t i = 0; i < m_table.markers.size(); ++i) {
        const osg::MapMarker& m = m_table.markers[i];
        m_marker_flags[i] = (m.flags & k_marker_visible) ? m.flags : m.flags & ~k_marker_can_travel;
        m_marker_disabled[i] = m.start_disabled;
    }
    m_locations_discovered = 0;
    m_fast_travel_enabled = true;
    m_zones.clear();
    m_detach.clear();
    m_cleared.clear();
    m_dead_refs.clear();
    m_placed = false;
    m_zone = 0;
    m_location = 0;
    m_player_markers.clear();
    m_marker_world = 0;
    m_recent_names.clear();
}

void WorldSim::load(BinaryReader& r) {
    new_game();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto form = r.read<uint32_t>();
        const auto flags = r.read<uint8_t>();
        const auto disabled = r.read<uint8_t>();
        if (const auto i = find_marker(form)) {
            m_marker_flags[*i] = flags;
            m_marker_disabled[*i] = disabled;
        }
    }
    m_locations_discovered = r.read<uint32_t>();
    m_fast_travel_enabled = r.read<uint8_t>() != 0;
    m_zones.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto form = r.read<uint32_t>();
        ZoneState& s = m_zones[form];
        s.detach = r.read<uint32_t>();
        s.attach = r.read<uint32_t>();
        s.reset = r.read<uint32_t>();
        s.level = r.read<uint16_t>();
    }
    m_detach.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto cell = r.read<uint32_t>();
        m_detach[cell] = r.read<uint32_t>();
    }
    m_cleared.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto location = r.read<uint32_t>();
        m_cleared[location] = r.read<uint8_t>() != 0;
    }
    m_dead_refs.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ref = r.read<uint32_t>();
        m_dead_refs[ref] = r.read<uint8_t>() != 0;
    }
    if (on_zone_latched) {
        for (const auto& [zone, s] : m_zones) {
            if (s.level) {
                on_zone_latched(zone, s.level);
            }
        }
    }
}

std::string WorldSim::state_json() const {
    size_t visible = 0, can_travel = 0;
    for (const uint8_t f : m_marker_flags) {
        visible += (f & k_marker_visible) != 0;
        can_travel += (f & k_marker_can_travel) != 0;
    }
    return std::format("{{\"markers\": {}, \"player_markers\": {}, \"visible\": {}, \"can_travel\": {}, "
                       "\"locations_discovered\": {}, \"location\": \"{:08X}\", \"zone\": \"{:08X}\", \"zone_level\": {}, "
                       "\"detached_cells\": {}, \"first_visits\": {}, \"elapsed_checks\": {}, \"allowed_checks\": {}}}",
                       m_table.markers.size(), m_player_markers.size(), visible, can_travel, m_locations_discovered,
                       m_location, m_zone, zone_level(m_zone), m_detach.size(), m_first_visits, m_elapsed_checks,
                       m_allowed_checks);
}

} // namespace spud
