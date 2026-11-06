// The world simulation in the App: the player's whereabouts into WorldSim, the map menu's live data, fast travel.

#include "engine/app_internal.hpp"

#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"
#include "game/game.hpp"
#include "game/quests.hpp"
#include "game/story_manager.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace spud::engine {

namespace {

constexpr float k_map_transition = 0.75f;      // fMapTransitionSpeed:MapMenu x 1000 ms
constexpr float k_travel_fade = 0.5f;          // fFastTravelFadeSecs:General
constexpr float k_arrival_lift = 10.0f;        // MoveToWorldspacePosition

std::string fill_name(std::string format, const std::string& name) {
    if (const size_t at = format.find("%s"); at != std::string::npos) {
        format.replace(at, 2, name);
    }
    return format;
}

} // namespace

void App::ensure_world_sim() {
    if (m_world_sim || m_world_sim_failed) {
        return;
    }
    if (!m_rules) {
        load_rules();
    }
    try {
        m_world_sim = std::make_unique<WorldSim>(m_opt.cache, m_clock, m_rules ? &*m_rules : nullptr);
    } catch (const std::exception& e) {
        log::warn("world simulation off: {} (skytool ui world-tables writes it)", e.what());
        m_world_sim_failed = true;
        return;
    }
    m_world_sim->on_discovered = [this](const std::string& name, uint16_t type) {
        if (m_ui) {
            m_ui->hud_location_discovered(name, type);
        }
    };
    m_world_sim->on_location_name = [this](const std::string& name) {
        if (m_ui) {
            m_ui->hud_location_name(name);
        }
    };
    // The game's Change Location event: R1 the actor, L1 the old location, L2 the new.
    m_world_sim->on_location_change = [this](uint32_t from, uint32_t to) {
        if (m_game) {
            m_game->world().player_location = to;
        }
        if (m_papyrus) {
            m_papyrus->player_event("OnLocationChange", {m_papyrus->vm().form_value(from, "Location"), m_papyrus->vm().form_value(to, "Location")});
        }
        if (game::QuestSystem* quests = m_papyrus ? m_papyrus->quests() : nullptr) {
            game::StoryEvent e;
            e.type = game::event_code("CLOC");
            e.add("R1", script::k_player_ref);
            e.add("L1", from);
            e.add("L2", to);
            quests->story().queue(e);
        }
    };
    m_world_sim->on_cell_check = [this](const WorldSim::CellCheck& check) {
        if (m_game && check.reset_allowed) {
            m_game->reset_cell(check.cell);
        }
        reset_actors(check);
    };
    m_world_sim->on_zone_latched = [this](uint32_t zone, int32_t level) {
        if (m_actor_data) {
            m_actor_data->set_zone_level(zone, level);
        }
    };
}

void App::reset_actors(const WorldSim::CellCheck& check) {
    // The game's actors: each by their own timer, whatever the cell decided.
    if (!m_game || !m_ai_ready || check.first_visit) {
        return;
    }
    if (m_actors_by_cell.empty()) {
        m_game->ai().for_each([this](const game::Agent& a) {
            if (const forms::FormEntry* e = m_game->db().find(a.ref)) {
                m_actors_by_cell[e->parent].push_back(a.ref);
            }
        });
    }
    const auto here = m_actors_by_cell.find(check.cell);
    if (here == m_actors_by_cell.end()) {
        return;
    }
    constexpr uint32_t k_record_deleted = 0x20;
    constexpr int64_t k_acbs_respawn = 0x8;
    const forms::Database& db = m_game->db();
    for (const uint32_t ref : here->second) {
        game::Agent* a = m_game->ai().mutable_agent(ref);
        const forms::FormEntry* e = db.find(ref);
        const forms::FormEntry* base = a ? db.find(a->npc) : nullptr;
        if (!a || !e || !base) {
            continue;
        }
        const forms::Value* flags = forms::Form(db, *base).get("ACBS.flags");
        WorldSim::RefFacts f;
        f.actor = true;
        f.dead = m_world_sim->ref_dead(ref);
        f.respawns = flags && (flags->integer & k_acbs_respawn);
        f.plugin_deleted = (e->flags & k_record_deleted) != 0;
        f.zone_never_resets = m_world_sim->zone_never_resets(m_world_sim->cell_zone(check.cell, 0));
        f.detach_hours = a->detach_hours;
        f.location_cleared = m_world_sim->location_cleared(m_world_sim->cell_location(check.cell, 0, 0, 0));
        switch (m_world_sim->ref_reset(check, f)) {
        case WorldSim::RefReset::reset_actor:
            // Actor::Reset: with no stamp or a respawning base the pick is made again.
            if ((!a->detach_hours || f.respawns) && m_actor_data) {
                m_actor_data->reroll(ref);
            }
            m_game->reset_actor(ref);
            m_world_sim->set_ref_dead(ref, false);
            a->detach_hours = 0;
            ++m_actor_resets;
            break;
        case WorldSim::RefReset::clear_corpse:
            m_game->reset_container(ref);
            break;
        default:
            break;
        }
    }
}

bool App::ref_place(uint32_t ref, WorldSim::Place& place, glm::vec3& position) const {
    if (!m_game) {
        return false;
    }
    if (ref == script::k_player_ref && m_space) {
        position = player_position_units();
        if (m_space->interior()) {
            place = {0, m_space->world().cells.empty() ? 0u : m_space->world().cells.front().form_id};
        } else {
            place = {m_place_world, 0};
        }
        return true;
    }
    if (const game::Agent* a = m_game->ai().agent(ref)) {
        place = m_world_sim->worldspace(a->space) ? WorldSim::Place{a->space, 0} : WorldSim::Place{0, a->space};
        position = glm::vec3(a->position[0], a->position[1], a->position[2]);
        return true;
    }
    const forms::Database& db = m_game->db();
    if (const std::array<float, 3>* moved = m_game->moved_position(ref); moved && m_game->placed_base(ref)) {
        place = m_place_of_created.contains(ref) ? m_place_of_created.at(ref) : WorldSim::Place{m_place_world, 0};
        position = glm::vec3((*moved)[0], (*moved)[1], (*moved)[2]);
        return true;
    }
    const forms::FormEntry* e = db.find(ref);
    const forms::FormEntry* cell = e ? db.find(e->parent) : nullptr;
    const auto placed = e ? forms::placement(db, *e) : std::nullopt;
    if (!cell || !placed) {
        return false;
    }
    place = {cell->parent, cell->parent ? 0 : cell->form_id};
    position = glm::vec3(placed->position[0], placed->position[1], placed->position[2]);
    return true;
}

void App::refresh_quest_marks() {
    // The game's targets, resolved into the player's space (the game
    //): the target itself, or the last door of the way there.
    m_quest_marks.clear();
    game::QuestSystem* quests = m_papyrus ? m_papyrus->quests() : nullptr;
    if (!quests || !m_world_sim || !m_space) {
        return;
    }
    constexpr uint8_t k_quest_type_misc = 6;
    int32_t cx = 0, cy = 0;
    const WorldSim::Place here = player_place(cx, cy);
    const glm::vec3 at = player_position_units();
    const float from[3] = {at.x, at.y, at.z};
    for (const game::QuestSystem::Target& t : quests->quest_targets()) {
        game::Quest* q = quests->quest(t.quest);
        const game::Objective* o = q ? q->objective(t.objective) : nullptr;
        WorldSim::Place place;
        glm::vec3 position;
        if (!o || q->type == k_quest_type_misc || !ref_place(t.ref, place, position)) {
            continue;
        }
        QuestMark m;
        m.ref = t.ref;
        m.text = o->text;
        m.position = m.walk = position;
        m.world = place.world;
        if (!m_world_sim->same_space(here, place)) {
            const float to[3] = {position.x, position.y, position.z};
            const osg::Door* door = m_world_sim->marker_door(here, from, place, to);
            if (!door) {
                continue;
            }
            m.position = m.walk = glm::vec3(door->position[0], door->position[1], door->position[2]);
            m.world = door->world;
            m.door = true;
            if (const osg::Door* out = m_world_sim->first_door(here, from, place, to)) {
                m.walk = glm::vec3(out->position[0], out->position[1], out->position[2]);
            }
        }
        m_quest_marks.push_back(std::move(m));
    }
}

void App::watch_cells(Space& space) {
    if (!m_world_sim) {
        return;
    }
    const osg::Worldspace* world = m_world_sim->worldspace(space.edid());
    const uint32_t form = world ? world->form_id : 0;
    space.on_cell = [this, form](uint32_t cell, bool loaded) {
        if (m_loading_save) {
            return;
        }
        if (loaded) {
            m_world_sim->cell_loaded(cell, form);
        } else {
            m_world_sim->cell_detached(cell, true);
        }
    };
}

glm::vec3 App::player_position_units() const {
    return (m_walk && m_player ? m_player->feet() : m_camera.position) / units::k_skyrim_unit_m;
}

WorldSim::Place App::player_place(int32_t& cell_x, int32_t& cell_y) {
    const auto& w = m_space->world();
    const float cs = w.cell_size;
    const glm::vec3 feet = player_position_units() * units::k_skyrim_unit_m;
    cell_x = int32_t(std::floor(feet.x / cs));
    cell_y = int32_t(std::floor(feet.y / cs));
    if (m_space->interior()) {
        return {0, w.cells.empty() ? 0u : w.cells.front().form_id};
    }
    if (m_place_space != m_space.get() || m_place_cell[0] != cell_x || m_place_cell[1] != cell_y) {
        m_place_space = m_space.get();
        m_place_cell[0] = cell_x;
        m_place_cell[1] = cell_y;
        const osg::Worldspace* world = m_world_sim->worldspace(current_location());
        m_place_world = world ? world->form_id : 0;
    }
    return {m_place_world, 0};
}

void App::update_world_sim() {
    if (!m_world_sim || !m_space) {
        return;
    }
    m_loading_save = m_loading_save && m_loading;
    if (--m_quest_marks_in <= 0) {
        m_quest_marks_in = 30; // they change with objectives and doors, not by the frame
        refresh_quest_marks();
    }
    int32_t cx = 0, cy = 0;
    const WorldSim::Place place = player_place(cx, cy);
    update_attached_cells(place, cx, cy);
    const glm::vec3 u = player_position_units();
    const float position[3] = {u.x, u.y, u.z};
    if (m_game) {
        const int32_t level = m_game->character().level();
        m_world_sim->set_player_level(level);
        if (m_actor_data) {
            m_actor_data->set_player_level(level);
        }
    }
    m_world_sim->update(place, cx, cy, position);
    if (place.world) {
        m_last_exterior = u;
        m_last_exterior_world = place.world;
    }
    // The game's fade, then the menu's hide sets the pending move; the move runs on the next frame.
    if (m_travel_fade_marker) {
        m_travel_fade -= float(m_ui_dt);
        if (m_travel_fade <= 0.0f) {
            m_travel_pending = m_travel_fade_marker;
            m_travel_fade_marker = 0;
            if (m_ui) {
                m_ui->close("MapMenu");
            }
        }
    } else if (m_travel_pending && !(m_ui && m_ui->is_open("MapMenu"))) {
        const uint32_t marker = m_travel_pending;
        m_travel_pending = 0;
        do_fast_travel(marker);
    }
}

WorldSim::TravelChecks App::travel_checks() const {
    // Hostile actors, alarms, damaging effects and knock states need actors in combat. the game is
    // false while mounted.
    WorldSim::TravelChecks c;
    c.over_encumbered = m_game && !m_mount.ref && m_game->encumbrance() > m_game->carry_weight();
    return c;
}

float App::player_walk_speed() const {
    return movement::actor_speeds(m_tuning.walk_run, m_tuning.body, m_tuning.settings).forward[0];
}

void App::do_fast_travel(uint32_t marker_form) {
    // The game and DoFastTravel.
    if (!m_world_sim || !m_space) {
        return;
    }
    if (travel_checks().over_encumbered) {
        return;
    }
    // A map marker's arrival, or for Game.FastTravel any reference: the carriages' are plain markers.
    const auto index = m_world_sim->find_marker(marker_form);
    WorldSim::Place to;
    float arrival[3], arrival_angle = 0.0f;
    if (index) {
        const osg::MapMarker& m = m_world_sim->marker(*index);
        to = {m.arrival_world, m.arrival_cell};
        std::copy_n(m.arrival_position, 3, arrival);
        arrival_angle = m.arrival_rotation[2];
    } else {
        glm::vec3 at;
        if (!ref_place(marker_form, to, at)) {
            return;
        }
        arrival[0] = at.x;
        arrival[1] = at.y;
        arrival[2] = at.z;
        if (const forms::FormEntry* e = m_game ? m_game->db().find(marker_form) : nullptr) {
            const auto placed = forms::placement(forms::Form(m_game->db(), *e));
            arrival_angle = placed ? placed->rotation[2] : 0.0f;
        }
    }
    // From the map, first thing, before moving; Game.FastTravel has no autosave.
    if (m_travel_from_map && m_user.number("bSaveOnTravel:SaveGame") > 0.5) {
        autosave();
    }
    m_travel_from_map = false;
    const osg::Worldspace* world = m_world_sim->worldspace(to.world);
    if (!world) {
        return; // MoveToWorldspacePosition: an interior reference has no world, and nothing happens
    }
    int32_t cx = 0, cy = 0;
    const WorldSim::Place from = player_place(cx, cy);
    const glm::vec3 at = player_position_units();
    const float from_position[3] = {at.x, at.y, at.z};
    if (m_mount.ref && from.world != to.world) {
        dismounted(); // DoFastTravel: the mount comes along inside its worldspace only
    }
    const float length = m_world_sim->path_length(from, from_position, to, arrival).value_or(0.0f);
    const float mult = m_rules ? float(m_rules->setting("fFastTravelSpeedMult").value_or(1.0)) : 1.0f;
    const float seconds = length / (mult * player_walk_speed());
    const float before = m_clock.days_passed() * 24.0f;
    m_clock.update(seconds);
    m_last_travel_hours = m_clock.days_passed() * 24.0f - before;

    const std::string from_world = m_space->edid();
    const std::string space(m_world_sim->table().string(world->space));
    const glm::vec3 feet = glm::vec3(arrival[0], arrival[1], arrival[2] + k_arrival_lift) *
                           units::k_skyrim_unit_m;
    const float heading = glm::degrees(arrival_angle);
    m_camera.position = feet;
    if (path::to_lower(from_world) != space) {
        load_world(space);
    }
    if (m_walk && m_player) {
        m_player->teleport(feet);
        m_player->set_facing(glm::radians(heading), 0.0f);
        m_camera.set_skyrim_angles(heading, 0.0f);
        m_camera.position = m_player->eye();
    } else {
        m_camera.set_skyrim_angles(heading, 0.0f);
        m_camera.position = feet + glm::vec3(0, 0, m_tuning.eye_height);
    }
    const int32_t acx = int32_t(std::floor(arrival[0] / 4096.0f));
    const int32_t acy = int32_t(std::floor(arrival[1] / 4096.0f));
    m_world_sim->player_moved(to, acx, acy);
    m_world_sim->set_fast_travel_enabled(true); // any completed move turns a script's block off again
    // The game: whoever follows the player comes to the destination.
    if (m_game) {
        for (const uint32_t ref : m_game->ai().followers(game::k_player_ref)) {
            m_game->ai().place_at(ref, to.world, {arrival[0], arrival[1], arrival[2]}, arrival_angle);
        }
    }
    log::info("fast travel to {:08X} {}: {:.0f} units, {:.1f} s, {:.2f} game hours", marker_form,
              index ? m_world_sim->marker_name(*index) : std::string_view(), length, seconds, m_last_travel_hours);

    if (m_ui) {
        m_loading_destination = {};
        m_loading_destination.worldspace = m_space->edid();
        m_loading_destination.cell_x = acx;
        m_loading_destination.cell_y = acy;
        m_loading_destination.from_worldspace = from_world;
        m_loading_destination_set = true;
        m_ui->open("loading");
        m_loading = true;
        m_loading_frames = 0;
        m_loading_start = Clock::now();
    }
    if (m_papyrus) {
        m_papyrus->fast_travel_ended(m_last_travel_hours);
    }
}

ui::MapSource* App::ui_map() {
    if (!m_world_sim || !m_space) {
        return nullptr;
    }
    int32_t cx = 0, cy = 0;
    const WorldSim::Place place = player_place(cx, cy);
    // Started straight into an interior, no exterior is behind it yet: the game's main world.
    constexpr uint32_t k_tamriel = 0x0000003C;
    const uint32_t world = place.world ? place.world : m_last_exterior_world ? m_last_exterior_world : k_tamriel;
    return m_world_sim->map_world(m_world_sim->worldspace(world)) ? this : nullptr;
}

bool App::ui_compass_markers(std::vector<ui::CompassEntry>& out) {
    if (!m_world_sim || !m_space) {
        return false;
    }
    const glm::vec3 u = player_position_units();
    const float position[3] = {u.x, u.y, u.z};
    // Quest targets first, up to 16; interiors have them too.
    size_t quest_count = 0;
    for (const QuestMark& m : m_quest_marks) {
        if (quest_count++ < 16) {
            ui::CompassEntry e{{m.position.x, m.position.y, m.position.z}, 0.0, false};
            e.quest = true;
            e.quest_door = m.door;
            out.push_back(e);
        }
    }
    for (const uint32_t i : m_world_sim->compass_markers(position)) {
        const osg::MapMarker& m = m_world_sim->marker(i);
        out.push_back({{m.position[0], m.position[1], m.position[2]},
                       double(m.type),
                       (m_world_sim->marker_flags(i) & WorldSim::k_marker_can_travel) != 0});
    }
    if (m_custom_marker && !m_space->interior()) {
        // The player-set marker comes last (Compass::Update).
        out.push_back({{m_custom_marker->x, m_custom_marker->y, m_custom_marker->z}, 0.0, false, true});
    }
    return true;
}

std::vector<ui::MapEntry> App::map_open() {
    // The game: the player, the markers, the custom destination. Quest targets follow when
    // there are quests.
    int32_t cx = 0, cy = 0;
    const WorldSim::Place place = player_place(cx, cy);
    // Started straight into an interior, no exterior is behind it yet: the game's main world.
    constexpr uint32_t k_tamriel = 0x0000003C;
    const uint32_t world = place.world ? place.world : m_last_exterior_world ? m_last_exterior_world : k_tamriel;
    const osg::Worldspace* map = m_world_sim->map_world(m_world_sim->worldspace(world));

    // The game: the game camera outside, the last exterior position inside.
    const glm::vec3 eye = place.world ? m_camera.position / units::k_skyrim_unit_m : m_last_exterior;
    const float eye_units[3] = {eye.x, eye.y, eye.z};
    const glm::vec3 target = m_world_sim->to_map(world, eye_units);

    WorldMapCamera::Data data;
    data.min_height = map->camera_min_height;
    data.max_height = map->camera_max_height;
    data.initial_pitch = map->camera_initial_pitch;
    for (int i = 0; i < 2; ++i) {
        data.nw_cell[i] = map->nw_cell[i];
        data.se_cell[i] = map->se_cell[i];
        data.bounds_min[i] = map->bounds_min[i];
        data.bounds_max[i] = map->bounds_max[i];
    }
    const bool outside = place.world != 0;
    auto ground = [this, outside](float x, float y) {
        if (!outside) {
            return 0.0f;
        }
        const float m = units::k_skyrim_unit_m;
        return m_space->terrain_height(x * m, y * m).value_or(0.0f) / m;
    };
    m_map_camera.begin(data, target, aspect(), outside ? k_map_transition : 0.0f, ground);
    m_map_open = true;
    if (m_map_scene) {
        m_map_scene->open(path::to_lower(m_world_sim->table().string(map->edid)));
    }
    // the sky goes to the world map's own weather for as long as the map is up.
    if (m_sky && m_atmosphere) {
        if (const auto* weather = m_atmosphere->weather("WorldMapWeather")) {
            m_sky->hold_weather(weather->form_id);
        }
    }

    std::vector<ui::MapEntry> entries;
    m_map_rows.clear();
    entries.push_back({m_ui->game_setting("sCurrentLocation"), 66, false});
    m_map_rows.push_back({0, 0, {target.x, target.y, target.z}});
    for (const uint32_t i : m_world_sim->map_entries()) {
        const osg::MapMarker& m = m_world_sim->marker(i);
        entries.push_back({std::string(m_world_sim->marker_name(i)), double(m.type),
                           !(m_world_sim->marker_flags(i) & WorldSim::k_marker_can_travel)});
        const glm::vec3 p = m_world_sim->to_map(m.world, m.position);
        m_map_rows.push_back({m.form_id, m.world, {p.x, p.y, p.z}});
    }
    entries.push_back({m_ui->game_setting("sPlayerSetMarker"), 65, false});
    m_map_rows.push_back({0, 0, {0, 0, 0}, false, false, true});
    // Quest targets: the objective's text, icon 62, 63 for a door, 64 where
    // several share a target; the later ones never show.
    refresh_quest_marks();
    for (size_t i = 0; i < m_quest_marks.size(); ++i) {
        const QuestMark& m = m_quest_marks[i];
        const auto first = std::find_if(m_quest_marks.begin(), m_quest_marks.begin() + ptrdiff_t(i),
                                        [&](const QuestMark& o) { return o.ref == m.ref; });
        const bool repeat = first != m_quest_marks.begin() + ptrdiff_t(i);
        const bool shared = std::count_if(m_quest_marks.begin(), m_quest_marks.end(),
                                          [&](const QuestMark& o) { return o.ref == m.ref; }) > 1;
        const float at[3] = {m.position.x, m.position.y, m.position.z};
        const glm::vec3 p = m_world_sim->to_map(m.world, at);
        entries.push_back({m.text, shared ? 64.0 : m.door ? 63.0 : 62.0, false});
        m_map_rows.push_back({0, m.world, {p.x, p.y, p.z}, true, repeat, false});
    }
    return entries;
}

bool App::map_frame(const ui::MapFrameInput& input, std::vector<ui::MapPlace>& places, double& heading) {
    WorldMapCamera::Input in;
    in.pan[0] = input.pan[0];
    in.pan[1] = input.pan[1];
    in.zoom = input.zoom;
    // The game: the motion through the game, x
    // fMapLookMouseSpeed:MapMenu (3), x the frame time in World::Update. ProcessLookInput divides x by the frame time, so
    // a count turns the same at any frame rate; y it doesn't, so a count tilts by the frame time: the game's 1/60 here.
    const movement::Settings& look = m_tuning.settings;
    const float mouse = look.mouse_heading_sensitivity * float(m_user.number("controls.mouse_sensitivity")) * 3.0f;
    in.look[0] = input.look[0] * mouse * look.mouse_heading_x_scale;
    in.look[1] = input.look[1] * mouse * look.mouse_heading_y_scale * (m_invert_y ? -1.0f : 1.0f) / 60.0f;
    m_map_camera.update(float(input.dt), in);
    const bool settled = m_map_camera.settled();
    places.resize(m_map_rows.size());
    for (size_t i = 0; i < m_map_rows.size(); ++i) {
        const MapRow& row = m_map_rows[i];
        ui::MapPlace& p = places[i];
        // The custom destination with no marker placed is a dead reference: false, -1, -1.
        if (row.custom && !m_custom_marker) {
            p = {};
            continue;
        }
        const glm::vec3 at = row.custom ? *m_custom_marker : glm::vec3(row.position[0], row.position[1], row.position[2]);
        const auto screen = m_map_camera.project(at);
        p.x = screen.x;
        p.y = screen.y;
        p.rotation = 0;
        const bool on_screen = screen.x >= 0.0f && screen.x <= 1.0f && screen.y >= 0.0f && screen.y <= 1.0f && screen.depth >= 0.0f;
        p.shown = settled && !row.hidden && (row.quest || on_screen) && !(row.world && m_world_sim->off_map(row.world));
        if (row.quest && !on_screen) {
            // Held at the edge, inset 20 pixels, with an arrow towards it.
            const double ix = 20.0 / 1280.0, iy = 20.0 / 720.0;
            const double raw_x = p.x, raw_y = 1.0 - p.y;
            p.x = std::clamp(p.x, ix, 1.0 - ix);
            p.y = std::clamp(p.y, iy, 1.0 - iy);
            p.rotation = glm::degrees(std::atan2((1.0 - p.y) - raw_y, raw_x - p.x)) + 270.0;
        }
    }
    // The player marker points along the player's heading as it falls on the map.
    const glm::vec3 target = m_map_camera.target();
    const glm::vec3 ahead = target + glm::vec3(std::sin(m_camera.yaw), std::cos(m_camera.yaw), 0.0f) * 1000.0f;
    const auto a = m_map_camera.project(target), b = m_map_camera.project(ahead);
    heading = glm::degrees(std::atan2(double(b.x - a.x) * aspect(), double(a.y - b.y)));
    return settled;
}

void App::map_click(size_t entry) {
    // The game.
    if (entry >= m_map_rows.size() || !m_map_rows[entry].marker) {
        return;
    }
    const uint32_t form = m_map_rows[entry].marker;
    const auto index = m_world_sim->find_marker(form);
    if (!index) {
        return;
    }
    const glm::vec3 place(m_map_rows[entry].position[0], m_map_rows[entry].position[1], m_map_rows[entry].position[2]);
    if (!(m_world_sim->marker_flags(*index) & WorldSim::k_marker_can_travel)) {
        m_ui->message(m_ui->game_setting("sPlaceMarkerUndiscovered"),
                      {m_ui->game_setting("sYes"), m_ui->game_setting("sNo")}, [this, place](int button) {
                          if (button == 0) {
                              m_custom_marker = place;
                          }
                      });
        return;
    }
    int32_t cx = 0, cy = 0;
    const WorldSim::Place at = player_place(cx, cy);
    const std::string_view refusal = m_world_sim->travel_refusal(travel_checks(), at, cx, cy);
    if (!refusal.empty()) {
        m_ui->hud_corner_message(m_ui->game_setting(std::string(refusal)));
        return;
    }
    const std::string name(m_world_sim->marker_name(*index));
    m_ui->message(fill_name(m_ui->game_setting("sFastTravelConfirm"), name),
                  {m_ui->game_setting("sYes"), m_ui->game_setting("sNo"), m_ui->game_setting("sPlaceMarker")},
                  [this, form, place](int button) {
                      if (button == 0) {
                          begin_fast_travel(form);
                      } else if (button == 2) {
                          m_custom_marker = place;
                      }
                  });
}

void App::map_click_ground(double x, double y) {
    // PlacePlayerMarker: the ray through the cursor onto the land, asking first when a marker stands.
    auto place = [this, x, y] {
        const glm::vec3 from = m_map_camera.position(), dir = m_map_camera.ray(float(x), float(y));
        const float m = units::k_skyrim_unit_m;
        for (float t = 0.0f; t < 400000.0f; t += 256.0f) {
            const glm::vec3 at = from + dir * t;
            if (at.z <= m_space->terrain_height(at.x * m, at.y * m).value_or(-1e9f) / m) {
                m_custom_marker = at;
                return;
            }
        }
    };
    if (!m_custom_marker) {
        place();
        return;
    }
    m_ui->message(m_ui->game_setting("sMoveMarkerQuestion"),
                  {m_ui->game_setting("sMoveMarker"), m_ui->game_setting("sLeaveMarker"), m_ui->game_setting("sRemoveMarker")},
                  [this, place](int button) {
                      if (button == 0) {
                          place();
                      } else if (button == 2) {
                          m_custom_marker.reset();
                      }
                  });
}

void App::begin_fast_travel(uint32_t marker) {
    // FastTravelConfirmCallback::Run: the fade of fFastTravelFadeSecs, then the map hides.
    m_ui->request_fade({false, true, k_travel_fade, 0, true});
    m_travel_fade_marker = marker;
    m_travel_fade = k_travel_fade;
    m_travel_from_map = true;
}

void App::map_centre() {
    const glm::vec3 eye = m_camera.position / units::k_skyrim_unit_m;
    const float eye_units[3] = {eye.x, eye.y, eye.z};
    int32_t cx = 0, cy = 0;
    const WorldSim::Place place = player_place(cx, cy);
    if (place.world) {
        m_map_camera.set_target(m_world_sim->to_map(place.world, eye_units));
    }
}

void App::map_close() {
    m_map_open = false;
    if (m_map_scene) {
        m_map_scene->close();
    }
    if (m_sky) {
        m_sky->release_weather();
    }
}

std::optional<render::View> App::map_view() const {
    if (!m_map_open || !m_space) {
        return std::nullopt;
    }
    const float m = units::k_skyrim_unit_m;
    return render::View::make(m_map_camera.position() * m, m_map_camera.yaw(), -m_map_camera.pitch(), m_map_camera.fov_y(),
                              aspect(), m_map_camera.near_plane() * m);
}

} // namespace spud::engine
