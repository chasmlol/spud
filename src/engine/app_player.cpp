// The player on foot: rules, physics, walk-mode controls, script hooks and state.

#include "engine/app_internal.hpp"
#include "engine/keys.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/units.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>
#include <sstream>

namespace spud::engine {

namespace {

// fDefaultWorldFOV:Display and fDefault1stPersonFOV:Display are 80 (live values, read in the game). The game's FOV is
// horizontal at 16:9 and keeps its vertical angle at any aspect: 50.6 degrees.
const float k_walk_fov_y = 2.0f * std::atan(std::tan(glm::radians(40.0f)) * 0.5625f);

std::string vec3_json(const glm::vec3& v) { return std::format("[{:.4f}, {:.4f}, {:.4f}]", v.x, v.y, v.z); }

} // namespace

void App::load_rules() {
    const auto file = m_opt.cache / "rules" / "game.osr";
    try {
        m_rules = osr::read(read_file(file));
        load_control_map();
        m_tuning = MovementTuning::from_rules(*m_rules);
    } catch (const std::exception& e) {
        log::warn("player: cannot read {} ({}); built-in movement values and keys", file.string(), e.what());
        m_rules.reset();
        m_tuning = MovementTuning{};
    }
    const auto& t = m_tuning;
    const auto sp = movement::actor_speeds(t.walk_run, t.body, t.settings);
    log::info("player: walk {:.1f}, run {:.1f} units/s forward; jump {:.0f} units x {:.2f}; eye {:.1f} units; body "
              "{:.3f} x {:.3f} x {:.3f} m, step {:.3f} m",
              sp.forward[0], sp.forward[1], t.settings.jump_height_min, t.body.scale,
              t.eye_height / units::k_skyrim_unit_m, t.controller.half_extents.x, t.controller.half_extents.y,
              t.controller.half_extents.z, t.controller.step_up);
}

void App::create_physics() {
    if (!m_space) {
        throw Error("no world loaded");
    }
    if (!m_rules) {
        load_rules();
    }
    release_physics();
    m_physics = std::make_unique<physics::World>(m_opt.cache, m_space->world_ptr());
    for (const auto& d : m_doors) {
        if (d.open) {
            const osw::Door& door = m_space->world().doors[d.index];
            m_physics->set_ref_solid(door.form_id, false);
            if (d.same_space) {
                m_physics->set_ref_solid(door.destination, false);
            }
        }
    }
}

void App::destroy_physics() {
    if (m_walk) {
        m_camera.fov_y = m_fly_fov;
        m_camera.fov_offset = 0.0f;
        m_camera.sway = glm::mat3(1.0f);
    }
    m_walk = false;
    m_player.reset();
    release_physics();
    m_physics.reset();
}

// Actors hold the world their ragdolls are in, and leave it when they end: they let go of it before it goes.
void App::release_physics() {
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        actors->set_physics(nullptr);
    }
}

void App::set_walk_mode(bool walk) {
    if (walk == m_walk) {
        return;
    }
    if (!walk) {
        // Noclip: the player stays where it was; flying resumes from the camera.
        m_player->set_paused(true);
        m_camera.fov_y = m_fly_fov;
        m_camera.fov_offset = 0.0f;
        m_camera.sway = glm::mat3(1.0f);
        m_walk = false;
        return;
    }
    if (!m_space) {
        throw Error("no world to walk in");
    }
    if (!m_physics) {
        create_physics();
    }
    start_game();
    // Drop from the camera, like turning collision back on in the game; never from under the terrain.
    glm::vec3 feet = m_camera.position - glm::vec3(0, 0, m_tuning.eye_height);
    if (auto ground = terrain_height(feet.x, feet.y)) {
        feet.z = std::max(feet.z, *ground + 0.05f);
    }
    if (!m_player) {
        m_player = std::make_unique<Player>(*m_physics, m_space->world_ptr(), m_tuning, feet);
        m_player->set_ai_processing(m_ai_processing);
        m_player->watch(m_watch);
    } else {
        m_player->teleport(feet);
        m_player->set_paused(false);
    }
    m_fly_fov = m_camera.fov_y;
    m_camera.fov_y = k_walk_fov_y;
    m_walk = true;
    m_last_step = {};
}

void App::start_on_foot() {
    if (!m_opt.pos_units) {
        set_walk_mode(true);
        return;
    }
    glm::vec3 feet = *m_opt.pos_units * units::k_skyrim_unit_m;
    if (m_opt.pos_z_above_terrain) {
        feet.z += terrain_height(feet.x, feet.y).value_or(0.0f);
    }
    set_walk_mode(true);
    m_player->teleport(feet);
    m_camera.position = m_player->eye();
}

void App::place_player(const glm::vec3& feet, float heading_deg, float pitch_deg) {
    set_walk_mode(true);
    m_player->set_hold(true); // a script drives it from here (see run_session for when that ends)
    m_player->teleport(feet);
    m_player->set_facing(glm::radians(heading_deg), -glm::radians(pitch_deg));
    m_camera.set_skyrim_angles(heading_deg, pitch_deg);
    m_camera.position = m_player->eye();
}

void App::queue_player(const PlayerMove& move, int ticks) {
    const bool idle = move.forward == 0 && move.right == 0 && !move.jump && !move.sneak && move.look == glm::vec2(0.0f) &&
                      move.gait != PlayerMove::Gait::sprint;
    if (idle && (!m_player || !m_walk)) {
        return; // 'wait' before there is a player: nothing to simulate
    }
    if (!m_player || !m_walk) {
        throw Error("not on foot: use 'player' or 'mode walk' first");
    }
    PlayerInput in;
    in.move = {move.right, move.forward};
    in.run = move.gait == PlayerMove::Gait::walk ? false : m_script_run || move.gait != PlayerMove::Gait::run;
    if (move.gait == PlayerMove::Gait::run) {
        in.run = true;
    }
    in.sprint_press = move.gait == PlayerMove::Gait::sprint; // on the first tick: each sprint move presses anew
    in.jump = move.jump;
    in.sneak = move.sneak;
    in.look = move.look;
    if (move.gamepad) {
        // The pad as the game's device delivers it: sticks through their deadzones, buttons by controlmap.txt's
        // gamepad column; a gamepad always runs (the stick picks the speed).
        const auto& s = m_tuning.settings;
        in.gamepad = true;
        in.run = true;
        in.move = movement::stick_deadzone(move.left_stick, s.l_thumb_deadzone, s.l_thumb_deadzone_max);
        in.right_stick = movement::stick_deadzone(move.right_stick, s.r_thumb_deadzone, s.r_thumb_deadzone_max);
        auto bound = [&](const char* event) {
            const osr::KeyBinding* b = m_rules ? m_rules->key(event) : nullptr;
            return b && b->gamepad != 0xFF && (move.buttons & b->gamepad);
        };
        in.jump = bound("Jump");
        in.sprint_press = bound("Sprint");
        in.sneak = bound("Sneak");
    }
    m_player->set_hold(true);
    m_player->queue_input(in, ticks);
}

int App::player_action(const std::string& what, const std::string& arg) {
    if (what == "ai") {
        if (arg != "on" && arg != "off") {
            throw Error("ai on|off");
        }
        m_ai_processing = arg == "on";
        if (m_player) {
            m_player->set_ai_processing(m_ai_processing);
        }
        return 0;
    }
    if (what == "watch") {
        m_watch.clear();
        std::istringstream words(arg);
        for (std::string w; words >> w;) {
            if (w == "off") {
                m_watch.clear();
                break;
            }
            m_watch.push_back(static_cast<uint32_t>(std::stoul(w, nullptr, 16)));
        }
        if (m_player) {
            m_player->watch(m_watch);
        }
        return 0;
    }
    if (what == "actor") {
        actor_command(arg);
        return 0;
    }
    if (what == "projectile") {
        projectile_command(arg);
        return 0;
    }
    if (!m_player || !m_walk) {
        throw Error("not on foot: use 'player' or 'mode walk' first");
    }
    if (what == "sneak") {
        if (arg != "on" && arg != "off") {
            throw Error("sneak on|off");
        }
        if (m_player->status().sneaking == (arg == "on")) {
            return 0;
        }
        PlayerMove press;
        press.sneak = true;
        queue_player(press, 3);
        return 3;
    }
    if (what == "run") {
        m_script_run = arg != "off";
        return 0;
    }
    if (what == "process_time") {
        m_player->set_process_time(std::stof(arg));
        return 0;
    }
    if (what == "breath") {
        m_player->set_breath(std::stof(arg));
        return 0;
    }
    if (what == "restore") {
        m_player->restore();
        queue_player({}, 1); // the frame the game's console command takes
        return 1;
    }
    if (what == "camera") {
        if (arg == "first" || arg == "third") {
            m_player->set_view(arg == "first" ? View::first_person : View::third_person);
            return 0;
        }
        if (arg == "player") {
            m_camera_free.reset();
            return 0;
        }
        std::array<float, 5> pose{};
        std::istringstream in(arg);
        std::string word;
        in >> word >> pose[0] >> pose[1] >> pose[2] >> pose[3] >> pose[4];
        if (word != "free" || !in) {
            throw Error(std::format("camera {}: first, third, player or free x y z heading pitch", arg));
        }
        m_camera_free = pose;
        return 0;
    }
    throw Error(std::format("unknown player action '{}'", what));
}

size_t App::player_scripted() { return m_player ? m_player->scripted() : 0; }

void App::trace_player(const std::string& file) {
    if (!m_player) {
        throw Error("not on foot: use 'player' or 'mode walk' first");
    }
    if (file != "off") {
        m_trace_file = file;
        m_player->start_trace();
        return;
    }
    if (m_trace_file.empty()) {
        throw Error("no trace is running");
    }
    // The trace format: one row per 60 Hz tick,
    // feet and camera in Skyrim units, angles like getangle (degrees, pitch positive down).
    std::string csv = "t,x,y,z,heading,pitch,cam_x,cam_y,cam_z,on_ground,swimming,sneaking,sprinting,stamina,health,"
                      "frame,vx,vy,vz,ctrl_state,support,normal_x,normal_y,normal_z,fall_start,fall_time,state1,"
                      "cam_state,sprint_request,world_delta,ground_layer,process_time,stamina_delay,breath\n";
    const auto rows = m_player->stop_trace();
    const uint64_t first = rows.empty() ? 0 : rows.front().tick;
    const float u = units::k_skyrim_unit_m;
    for (const auto& r : rows) {
        const auto& s = r.state;
        const auto& st = r.status;
        const glm::vec3 p = s.position / u;
        const glm::vec3 cam = r.camera / u;
        const glm::vec3 v = s.velocity / u;
        const uint32_t flags = (st.sprinting ? 0x100u : 0u) | (st.sneaking ? 0x200u : 0u) | (st.swimming ? 0x400u : 0u);
        csv += std::format("{:.6f},{:.4f},{:.4f},{:.4f},{:.5f},{:.5f},{:.4f},{:.4f},{:.4f},{},{},{},{},{:.4f},{:.4f},"
                           "{},{:.4f},{:.4f},{:.4f},{},{},{:.5f},{:.5f},{:.5f},{:.4f},{:.5f},{:08X},{},{},{:.7f},{},{:.6f},"
                           "{:.6f},{:.4f}\n",
                           double(r.tick - first) * Player::k_tick, p.x, p.y, p.z, glm::degrees(r.yaw),
                           -glm::degrees(r.pitch), cam.x, cam.y, cam.z,
                           int(s.state == physics::ControllerState::on_ground), int(st.swimming),
                           int(st.sneaking && !st.swimming), int(st.sprinting), st.stamina, st.health, r.tick, v.x, v.y,
                           v.z, int(s.state), s.on_ground || s.on_steep ? 2 : 0, s.surface_normal.x, s.surface_normal.y,
                           s.surface_normal.z, s.fall_start / u, s.air_time, flags, r.third_person ? 9 : 0,
                           int(st.sprint_request),
                           Player::k_tick, s.ground_layer, st.process_time, st.stamina_regen_delay, st.breath);
    }
    if (m_trace_file.has_parent_path()) {
        std::filesystem::create_directories(m_trace_file.parent_path());
    }
    write_file(m_trace_file, std::as_bytes(std::span(csv.data(), csv.size())));
    // The proxy's manifold per row (<name>.contacts.csv).
    std::string contacts = "row,i,x,y,z,nx,ny,nz,distance,tag,layer\n";
    for (size_t row = 0; row < rows.size(); ++row) {
        const auto& m = rows[row].state.manifold;
        for (size_t i = 0; i < m.size(); ++i) {
            const glm::vec3 q = m[i].position / u;
            contacts += std::format("{},{},{:.4f},{:.4f},{:.4f},{:.5f},{:.5f},{:.5f},{:.6f},{},{}\n", row, i, q.x,
                                    q.y, q.z, m[i].normal.x, m[i].normal.y, m[i].normal.z, m[i].distance, m[i].tag,
                                    int(m[i].layer));
        }
    }
    auto contacts_file = m_trace_file;
    contacts_file.replace_extension(".contacts.csv");
    write_file(contacts_file, std::as_bytes(std::span(contacts.data(), contacts.size())));
    // Watched references (<name>.refs.csv).
    std::string refs = "row,form,x,y,z\n";
    bool any_refs = false;
    for (size_t row = 0; row < rows.size(); ++row) {
        for (const auto& w : rows[row].refs) {
            const glm::vec3 q = w.position / u;
            refs += std::format("{},{:08X},{:.4f},{:.4f},{:.4f}\n", row, w.form_id, q.x, q.y, q.z);
            any_refs = true;
        }
    }
    if (any_refs) {
        auto refs_file = m_trace_file;
        refs_file.replace_extension(".refs.csv");
        write_file(refs_file, std::as_bytes(std::span(refs.data(), refs.size())));
    }
    log::info("trace: {} ticks to {}", rows.size(), m_trace_file.string());
    m_trace_file.clear();
}

bool App::player_busy() const { return m_walk && m_player && (m_player->waiting() || m_physics->busy()); }

void App::sit_player(uint32_t ref) {
    if (!m_player || !m_space) {
        return;
    }
    const glm::vec3 feet = m_player->feet() / units::k_skyrim_unit_m;
    const game::AiFurniture* f = m_game->ai().free_seat(player_space(), ref, {feet.x, feet.y, feet.z});
    game::AiFurniture marker; // an Is Marker furniture isn't in the world's refs: a mining marker. It's where its reference is.
    if (!f) {
        const auto at = m_game->ref_position(ref);
        if (!at) {
            log::warn("furniture {:08X}: no free marker", ref);
            return;
        }
        marker.ref = ref;
        marker.position = {(*at)[0], (*at)[1], (*at)[2]};
        f = &marker;
    }
    m_player->teleport(glm::vec3(f->position[0], f->position[1], f->position[2]) * units::k_skyrim_unit_m);
    ActorSet* actors = m_space->actors();
    // The idle tree under the activate action: its Chair branch answers by the furniture's keywords, for one who is getting in
    // (GetSitting 2, states 1 and 2).
    m_game->set_player_sit_state(2);
    m_game->set_player_furniture(ref);
    m_seat = {};
    m_seat.ref = ref;
    const bool third = m_game->furniture_has_keyword(m_game->base_of(ref), "FurnitureForce3rdPerson");
    if (third && m_player->view() == View::first_person) {
        m_player->set_view(View::third_person);
        m_seat.view_back = true;
    }
    // The tree is the third-person graph's, whichever body shows.
    std::string event = m_game->idles().pick("ActionActivate", "actors\\character", game::k_player_ref, ref);
    if (event.empty()) {
        const float to_player = std::atan2(feet.x - f->position[0], feet.y - f->position[1]);
        const float turn = std::remainder(to_player - f->heading, 2.0f * std::numbers::pi_v<float>);
        const char* side = std::abs(turn) < 0.7854f ? "Front" : std::abs(turn) > 2.3562f ? "Behind" : turn < 0.0f ? "Left" : "Right";
        event = std::format("IdleChair{}Enter", side);
    }
    log::info("furniture {:08X}: {}", ref, event);
    m_seat.enter = std::move(event); // sent as soon as the body it's for is there: changing view takes a moment
    m_game->set_player_sit_state(3);
    m_game->script_event(game::k_player_ref, "OnSit", {{ref}});
}

void App::stand_player() {
    if (!m_seat.ref || m_seat.leaving >= 0.0f) {
        return;
    }
    m_seat.leaving = 2.0f; // the NPCs' figure: Ai::leave_use
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        if (!actors->send_event(ActorSet::k_player_body, "IdleChairExitStart")) {
            actors->send_event(ActorSet::k_player_body, "IdleForceDefaultState"); // the first-person graph has no exit from here
            m_seat.leaving = 0.0f;
        }
    }
    m_game->set_player_sit_state(4);
    m_game->script_event(game::k_player_ref, "OnGetUp", {{m_seat.ref}});
}

void App::update_seat(float dt, bool moving) {
    if (!m_seat.enter.empty()) {
        ActorSet* actors = m_space ? m_space->actors() : nullptr;
        if ((actors && actors->send_event(ActorSet::k_player_body, m_seat.enter)) || (m_seat.enter_left -= dt) <= 0.0f) {
            m_seat.enter.clear();
        }
        return;
    }
    if (m_seat.leaving < 0.0f) {
        if (moving) {
            stand_player();
        }
    } else if ((m_seat.leaving -= dt) <= 0.0f) {
        seat_left();
    }
}

void App::seat_left() {
    if (m_seat.view_back && m_player) {
        m_player->set_view(View::first_person);
    }
    m_seat = {};
    m_game->set_player_sit_state(0);
    m_game->set_player_furniture(0);
}

void App::step_player() {
    if (!m_opt.headless) { // scripts drive the pad through their own commands
        poll_gamepad();
    }
    const auto now = Clock::now();
    const double dt = m_player_rate * (m_tour ? m_player_step
                                       : m_last_step == Clock::time_point{} ? 0.0
                                                                            : std::min(0.25, ms_between(m_last_step, now) / 1000.0));
    m_last_step = now;
    if (!m_walk || !m_player) {
        m_camera.shake = glm::vec3(0.0f);
        return;
    }
    // Mouse look turns the player the way the game does (fMouseHeading* settings, the movement type's turn rate).
    // No controls for the animation, or under the kill camera.
    const bool held = (m_mount.ref && m_mount.stage != MountStage::riding) || m_kill_camera.active;
    m_player->look_counts(held ? glm::vec2(0.0f) : m_walk_look);
    m_walk_look = glm::vec2(0.0f);
    // The game returns early while the life state is non-zero.
    const bool down = player_down();
    m_player->set_down(down);
    PlayerInput input = held || m_player_ai_driven || down ? PlayerInput{} : walk_input();
    if (m_seat.ref) { // a step gets them up; nothing moves them until they're out of it
        update_seat(float(dt), input.move != glm::vec2(0.0f));
        input.move = glm::vec2(0.0f);
        input.jump = input.sneak = input.sprint_press = false;
    }
    if (m_mount.ref && input.jump && m_player->status().speed < 130.0f) {
        // HorseJumpRoot: at a walk or standing the horse rears up rather than jumps (ForwardJump's GetMovementSpeed >= 130).
        input.jump = false;
        if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
            actors->send_event(m_mount.ref, "standingRearUp");
        }
    }
    if (m_game) {
        m_player->set_weapon_drawn(m_game->combat().fighter(game::k_player_ref).weapon_drawn);
    }
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        glm::vec3 feet;
        float heading = 0.0f;
        // The game runs both graphs all along; here the new body comes up with its weapon away.
        const bool third = m_player->view() == View::third_person;
        if (third != m_body_third_person && actors->player_body_ready(third)) {
            m_body_third_person = third;
            if (m_game && m_game->combat().fighter(game::k_player_ref).weapon_drawn) {
                m_combat_world->perform_action(game::k_player_ref, "ActionForceEquip");
            }
        }
        if (m_kill_move.target) {
            constexpr float k_body_wait_s = 2.0f;
            input = PlayerInput{};
            const std::string_view event = m_kill_move.event;
            const uint32_t lead = m_kill_move.lead, target = m_kill_move.target;
            if (third == m_body_third_person &&
                (lead != game::k_player_ref || actors->weapon_drawn(game::k_player_ref)) &&
                actors->start_pair(lead, target, event, event.substr(3))) {
                m_kill_move.target = 0;
            } else if ((m_kill_move.waited += float(dt)) > k_body_wait_s) {
                log::warn("kill move: no body to play {} on", event);
                m_game->combat().cancel_kill_move(lead);
                m_game->combat().cancel_kill_move(target);
                m_kill_move.target = 0;
            }
        } else if (m_kill_move_view && !m_kill_camera.active && !actors->paired(game::k_player_ref, feet, heading)) {
            m_kill_move_view = false;
            m_player->set_view(View::first_person);
        }
        std::erase_if(m_paired, [&](uint32_t ref) {
            if (actors->paired(ref, feet, heading) || (m_kill_move.target && (ref == m_kill_move.lead || ref == m_kill_move.target))) {
                return false;
            }
            game::Ai& ai = m_game->ai();
            ai.hold(ref, false);
            if (const game::Agent* agent = ai.agent(ref); agent && actors->feet(ref, feet, heading)) {
                const glm::vec3 u = feet / units::k_skyrim_unit_m;
                ai.place_at(ref, agent->location.space, {u.x, u.y, u.z}, heading);
            }
            return true;
        });
        if (!m_vehicles.contains(game::k_player_ref) && actors->paired(game::k_player_ref, feet, heading)) {
            // The marks have it: no steering, and the capsule goes where the body has been put.
            input = PlayerInput{};
            if (glm::distance(feet, m_player->feet()) > 0.01f) {
                m_player->teleport(feet);
            }
            m_player->set_facing(heading, m_player->pitch());
        }
    }
    if (m_vehicles.contains(game::k_player_ref)) {
        input = PlayerInput{};
        vehicle_look();
    }
    m_player->set_lockstep(m_tour, m_shown_fraction);
    m_player->frame(dt, input);
    sprint_breath(float(dt));
    player_air_actions();
    m_camera.position = m_player->camera();
    if (const ActorSet* actors = m_space ? m_space->actors() : nullptr; actors && m_player->view() == View::third_person) {
        m_camera.position.z += actors->foot_drop(ActorSet::k_player_body);
    }
    m_camera.fov_offset = 0.0f;
    if (const ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        m_camera.fov_offset = actors->camera_fov_offset();
    }
    m_camera.yaw = m_player->camera_yaw();
    m_camera.set_pitch(m_player->camera_pitch());
    vehicle_view();
    if (m_camera_orbit != 0.0f) { // a shot of the body from another side (docs/AUTOMATION.md)
        const glm::vec3 pivot = m_player->feet() + glm::vec3(0.0f, 0.0f, 1.0f), d = m_camera.position - pivot;
        const float c = std::cos(m_camera_orbit), s = std::sin(m_camera_orbit);
        m_camera.position = pivot + glm::vec3(d.x * c + d.y * s, d.y * c - d.x * s, d.z);
        m_camera.yaw += m_camera_orbit;
    }
    if (m_mount.dragon) {
        dragon_camera();
    }
    if (m_down_view) {
        place_down_camera();
    }
    if (m_camera_free) { // the game's tfc: the body stays as it was, seen from elsewhere
        const auto& f = *m_camera_free;
        m_camera.position = glm::vec3(f[0], f[1], f[2]) * units::k_skyrim_unit_m;
        m_camera.set_skyrim_angles(f[3], f[4]);
    }
    m_camera.sway = glm::mat3(1.0f);
    if (!m_camera_free && !m_mount.dragon && m_camera_orbit == 0.0f) {
        follow_camera_node(float(dt));
    }
    update_kill_camera(float(dt) / std::max(m_player_rate, 0.0001f));
    step_camera_shake(float(dt));
    update_player_condition(float(dt));
    m_help.update(float(dt));
    update_los_events();
}

// The view rides what the graph plays, so every animation moves it: the first-person eye sits on the Cam1 node, and
// the third-person camera blends in the node a StartAnimatedCameraDelta names.
void App::follow_camera_node(float dt) {
    // The node's frame is x right, y ahead, z up; a view's is x right, y up, z behind.
    const glm::mat3 to_view(glm::vec3(1, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    const ActorSet* actors = m_space ? m_space->actors() : nullptr;
    m_camera_delta.weight = std::clamp(m_camera_delta.weight + 2.0f * m_camera_delta.rate * dt, 0.0f, 1.0f);
    if (m_player->view() == View::first_person) {
        glm::vec3 offset;
        glm::mat3 turn;
        if (actors && !m_player->status().riding && actors->camera_node("camera1st [cam1]", offset, turn)) {
            m_camera.position += m_player->feet() + offset - m_player->eye();
            m_camera.sway = to_view * glm::transpose(turn) * glm::transpose(to_view);
        }
        m_camera.position += m_player->first_person_lag();
        return;
    }
    if (m_camera_delta.weight <= 0.0f) {
        return;
    }
    if (actors && !m_camera_delta.node.empty()) { // once it ends, the last pose fades out
        actors->camera_delta(m_camera_delta.node, m_camera_delta.translation, m_camera_delta.rotation);
    }
    const glm::vec3 ahead = m_camera.forward();
    const glm::vec3 right = glm::normalize(glm::cross(ahead, glm::vec3(0, 0, 1)));
    const glm::mat3 camera(right, ahead, glm::cross(right, ahead));
    const float w = m_camera_delta.weight;
    const glm::mat3 turn = glm::mat3_cast(glm::slerp(glm::quat(1, 0, 0, 0), m_camera_delta.rotation, w));
    const glm::vec3 pivot = m_player->eye();
    m_camera.position = pivot + camera * (w * m_camera_delta.translation * units::k_skyrim_unit_m) +
                        camera * turn * glm::transpose(camera) * (m_camera.position - pivot);
    m_camera.sway = to_view * glm::transpose(turn) * glm::transpose(to_view);
}

void App::shake_camera(const glm::vec3& source, float strength, float duration) {
    if (strength <= 0.0f || !m_rules || m_rules->camera_shake.empty()) {
        return;
    }
    const auto setting = [&](std::string_view name, float fallback) { return float(m_rules->setting(name).value_or(fallback)); };
    const float distance = glm::distance(m_camera.position / units::k_skyrim_unit_m, source);
    const float delta = setting("fCameraShakeDistFadeDelta", 2500.0f) * strength;
    const float inner = std::max(setting("fCameraShakeDistFadeStart", 2000.0f) * strength, setting("fCameraShakeDistMin", 300.0f));
    if (distance >= inner + delta) {
        return;
    }
    const float faded = distance > inner ? strength * (1.0f - (distance - inner) / delta) : strength;
    m_shake_strength = faded > 1.0f ? 1.0f : std::max(setting("fCameraShakeMultMin", 0.0f), faded);
    m_shake_left = duration > 0.0f ? duration : setting("fCameraShakeTime", 1.25f);
}

void App::step_camera_shake(float dt) {
    m_camera.shake = glm::vec3(0.0f);
    m_shake_clock += dt;
    if (m_shake_left <= 0.0f || m_shake_strength <= 0.0f || !m_rules || m_rules->camera_shake.empty()) {
        return;
    }
    const auto& track = m_rules->camera_shake;
    const float at = std::fmod(m_shake_clock / osr::k_camera_shake_step, float(track.size()));
    const auto& a = track[size_t(at) % track.size()];
    const auto& b = track[(size_t(at) + 1) % track.size()];
    const float t = at - std::floor(at);
    m_shake_left -= dt;
    if (m_shake_left < 0.0f) {
        m_shake_left = m_shake_strength = 0.0f;
        return;
    }
    const float fade = float(m_rules->setting("fCameraShakeFadeTime").value_or(0.25f));
    const float scale = m_shake_left >= fade ? m_shake_strength : m_shake_left * m_shake_strength / fade;
    m_camera.shake = glm::mix(glm::vec3(a[0], a[1], a[2]), glm::vec3(b[0], b[1], b[2]), t) * scale;
}

// Skyrim's bindings (controlmap.txt) for the walk-mode events, with its defaults if the rules are missing.
PlayerInput App::walk_input() {
    PlayerInput in;
    in.run = m_always_run;
    in.jump = m_jump_pressed;
    in.sneak = m_sneak_pressed;
    m_jump_pressed = m_sneak_pressed = false;
    // A menu or the console has the keyboard, mouse and pad (input_focus.hpp); else walk mode reads them even
    // with the debug UI up.
    if (!m_window || !m_focus.game()) {
        return in;
    }
    const bool* k = key_state();
    auto held = [&](const char* event, SDL_Scancode fallback) {
        if (const osr::KeyBinding* b = m_rules ? m_rules->key(event) : nullptr) {
            for (uint16_t dik : b->keys) {
                const SDL_Scancode sc = scancode_from_dik(dik);
                if (sc != SDL_SCANCODE_UNKNOWN && k[sc]) {
                    return true;
                }
            }
            return false;
        }
        return k[fallback];
    };
    const float forward = float(held("Forward", SDL_SCANCODE_W)) - float(held("Back", SDL_SCANCODE_S));
    if (m_auto_move && forward != 0.0f) {
        m_auto_move = false; // forward or back takes over again
    }
    in.move = {float(held("Strafe Right", SDL_SCANCODE_D)) - float(held("Strafe Left", SDL_SCANCODE_A)),
               m_auto_move ? 1.0f : forward};
    if (held("Run", SDL_SCANCODE_LSHIFT)) {
        in.run = !in.run; // held: the opposite of always-run
    }
    const bool sprint = held("Sprint", SDL_SCANCODE_LALT);
    in.sprint_press = sprint && !m_sprint_held;
    m_sprint_held = sprint;
    gamepad_input(in);
    return in;
}

// A gamepad, as the game reads one through XInput: the sticks as axis / 32767 through their deadzones, the
// buttons by controlmap.txt's gamepad column. Any deflection or button makes the tick a gamepad tick.
void App::gamepad_input(PlayerInput& in) {
    if (!m_gamepad) {
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            if (count > 0) {
                m_gamepad = SDL_OpenGamepad(ids[0]);
            }
            SDL_free(ids);
        }
        if (!m_gamepad) {
            return;
        }
    }
    if (!SDL_GamepadConnected(m_gamepad)) {
        SDL_CloseGamepad(m_gamepad);
        m_gamepad = nullptr;
        return;
    }
    auto axis = [&](SDL_GamepadAxis a) { return float(SDL_GetGamepadAxis(m_gamepad, a)) / 32767.0f; };
    // SDL's stick y grows downwards; XInput's (and the game's) upwards.
    const glm::vec2 left(axis(SDL_GAMEPAD_AXIS_LEFTX), -axis(SDL_GAMEPAD_AXIS_LEFTY));
    const glm::vec2 right(axis(SDL_GAMEPAD_AXIS_RIGHTX), -axis(SDL_GAMEPAD_AXIS_RIGHTY));
    static const std::pair<SDL_GamepadButton, uint16_t> k_buttons[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
        {SDL_GAMEPAD_BUTTON_START, 0x0010},       {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040},  {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000},       {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000},        {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
    };
    uint16_t buttons = 0;
    for (const auto& [b, bit] : k_buttons) {
        if (SDL_GetGamepadButton(m_gamepad, b)) {
            buttons |= bit;
        }
    }
    const auto& s = m_tuning.settings;
    const glm::vec2 move = movement::stick_deadzone(left, s.l_thumb_deadzone, s.l_thumb_deadzone_max);
    const glm::vec2 look = movement::stick_deadzone(right, s.r_thumb_deadzone, s.r_thumb_deadzone_max);
    const uint16_t pressed = buttons & ~m_pad_buttons;
    m_pad_buttons = buttons;
    if (move == glm::vec2(0.0f) && look == glm::vec2(0.0f) && buttons == 0) {
        return;
    }
    auto bound = [&](const char* event, uint16_t mask) {
        const osr::KeyBinding* b = m_rules ? m_rules->key(event) : nullptr;
        return b && b->gamepad != 0xFF && (mask & b->gamepad);
    };
    in.gamepad = true;
    in.run = true;
    in.move = move;
    in.right_stick = look;
    in.jump = in.jump || bound("Jump", pressed);
    in.sneak = in.sneak || bound("Sneak", pressed);
    in.sprint_press = in.sprint_press || bound("Sprint", pressed);
    if (m_player && bound("Toggle POV", pressed)) {
        m_player->set_view(m_player->view() == View::first_person ? View::third_person : View::first_person);
    }
}

// What a pressed event does in the world, on the keyboard's and the pad's way alike.
void App::walk_event(std::string_view event) {
    if (event == "Activate") {
        activate();
    } else if (event == "Ready Weapon" && m_game && m_combat_world) {
        game::Fighter& f = m_game->combat().fighter(game::k_player_ref);
        f.weapon_drawn = !f.weapon_drawn;
        m_combat_world->perform_action(game::k_player_ref, f.weapon_drawn ? "ActionDraw" : "ActionSheath");
    } else if (event == "Auto-Move") {
        m_auto_move = !m_auto_move;
    }
}

bool App::pad_held(const char* event) const {
    const osr::KeyBinding* b = m_rules ? m_rules->key(event) : nullptr;
    if (!b || b->gamepad == 0xFF) {
        return false;
    }
    if (b->gamepad == 0x0009 || b->gamepad == 0x000a) {
        return m_pad_trigger[b->gamepad - 0x0009];
    }
    return (m_pad_seen & b->gamepad) != 0;
}

void App::poll_gamepad() {
    if (!m_gamepad) {
        if (++m_pad_search < 60) { // a plugged-in pad is looked for now and then
            return;
        }
        m_pad_search = 0;
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            if (count > 0) {
                m_gamepad = SDL_OpenGamepad(ids[0]);
            }
            SDL_free(ids);
        }
        if (!m_gamepad) {
            return;
        }
    }
    if (!SDL_GamepadConnected(m_gamepad)) {
        SDL_CloseGamepad(m_gamepad);
        m_gamepad = nullptr;
        return;
    }
    static const std::pair<SDL_GamepadButton, uint16_t> k_buttons[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
        {SDL_GAMEPAD_BUTTON_START, 0x0010},       {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040},  {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000},       {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_WEST, 0x4000},        {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
    };
    uint16_t buttons = 0;
    for (const auto& [b, bit] : k_buttons) {
        if (SDL_GetGamepadButton(m_gamepad, b)) {
            buttons |= bit;
        }
    }
    const auto press = [&](uint32_t code, bool down) {
        if (m_ui) {
            m_ui->set_gamepad_active(true);
            const bool in_world = !m_ui->captures_input();
            m_ui->gamepad_button(code, down);
            if (down && in_world && m_walk && m_rules) {
                for (const osr::KeyBinding& k : m_rules->keys) {
                    if (k.context == 0 && k.gamepad == code) {
                        walk_event(k.event);
                    }
                }
            }
        }
    };
    const uint16_t changed = buttons ^ m_pad_seen;
    m_pad_seen = buttons;
    for (const auto& [b, bit] : k_buttons) {
        if (changed & bit) {
            press(bit, (buttons & bit) != 0);
        }
    }
    // XInput's trigger threshold, 30 of 255.
    const bool triggers[2] = {SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 30 * 32767 / 255,
                              SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 30 * 32767 / 255};
    for (uint32_t i = 0; i < 2; ++i) {
        if (triggers[i] != m_pad_trigger[i]) {
            m_pad_trigger[i] = triggers[i];
            press(0x0009 + i, triggers[i]);
        }
    }
    if (m_ui) {
        const auto axis = [&](SDL_GamepadAxis a) { return float(SDL_GetGamepadAxis(m_gamepad, a)) / 32767.0f; };
        m_ui->set_thumbstick(0, axis(SDL_GAMEPAD_AXIS_LEFTX), -axis(SDL_GAMEPAD_AXIS_LEFTY));
        m_ui->set_thumbstick(1, axis(SDL_GAMEPAD_AXIS_RIGHTX), -axis(SDL_GAMEPAD_AXIS_RIGHTY));
    }
}

void App::handle_walk_key(const SDL_Event& e) {
    auto is = [&](const char* event, SDL_Scancode fallback) {
        if (const osr::KeyBinding* b = m_rules ? m_rules->key(event) : nullptr) {
            return std::ranges::any_of(b->keys, [&](uint16_t dik) { return scancode_from_dik(dik) == e.key.scancode; });
        }
        return e.key.scancode == fallback;
    };
    if (e.type == SDL_EVENT_KEY_UP) {
        // Toggle POV switches the view on release.
        if (m_player && !player_down() && is("Toggle POV", SDL_SCANCODE_F)) {
            m_player->set_view(m_player->view() == View::first_person ? View::third_person : View::first_person);
        }
        return;
    }
    if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat || player_down()) { // The game stands aside
        return;
    }
    if (m_rules) {
        for (const osr::KeyBinding& b : m_rules->keys) {
            if (b.context == 0 && (scancode_from_dik(b.keys[0]) == e.key.scancode || scancode_from_dik(b.keys[1]) == e.key.scancode)) {
                m_help.input(b.event);
            }
        }
    }
    if (is("Jump", SDL_SCANCODE_SPACE)) {
        m_jump_pressed = true;
    } else if (is("Sneak", SDL_SCANCODE_LCTRL)) {
        m_sneak_pressed = true;
    } else if (is("Toggle Always Run", SDL_SCANCODE_CAPSLOCK)) {
        m_always_run = !m_always_run;
    } else if (is("Auto-Move", SDL_SCANCODE_C)) {
        walk_event("Auto-Move");
    } else if (is("Activate", SDL_SCANCODE_E)) {
        walk_event("Activate");
    } else if (is("Ready Weapon", SDL_SCANCODE_R)) {
        walk_event("Ready Weapon");
    }
}

bool App::set_player_setting(const std::string& name, const std::string& value) {
    // Test conditions only: the movement rules themselves come from the game data.
    float* target = nullptr;
    auto& t = m_tuning;
    if (name == "player_equipped_weight") {
        target = &t.body.equipped_weight;
    } else if (name == "player_inventory_weight") {
        target = &t.body.inventory_weight;
    } else if (name == "player_speed_mult") {
        target = &t.body.speed_mult;
    } else {
        return false;
    }
    float v = 0;
    auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
    if (ec != std::errc() || p != value.data() + value.size() || !std::isfinite(v)) {
        throw Error(std::format("setting '{}' needs a number, got '{}'", name, value));
    }
    *target = v;
    if (m_player) {
        m_player->tuning() = m_tuning; // the next tick picks it up
    }
    return true;
}

std::string App::player_json() {
    std::string j = std::format("\"mode\": \"{}\", ", m_walk ? "walk" : "fly");
    const float u = units::k_skyrim_unit_m;
    if (m_player) {
        const auto& s = m_player->state();
        const auto& st = m_player->status();
        const glm::vec2 h(s.velocity);
        j += std::format("\"player\": {{\"position_m\": {}, \"position_units\": {}, \"velocity_mps\": {}, "
                         "\"horizontal_speed_mps\": {:.4f}, \"state\": {}, \"on_ground\": {}, \"on_steep\": {}, "
                         "\"waiting\": {}, \"ground_layer\": {}, \"ground_material\": \"{:08X}\", \"ground_normal\": {}, "
                         "\"heading_deg\": {:.4f}, \"pitch_deg\": {:.4f}, \"stamina\": {:.4f}, \"health\": {:.4f}, "
                         "\"sprinting\": {}, \"sneaking\": {}, \"swimming\": {}, \"eye_height_units\": {:.3f}, "
                         "\"ticks\": {}, \"scripted_ticks\": {}}}, ",
                         vec3_json(s.position), vec3_json(s.position / u), vec3_json(s.velocity), glm::length(h),
                         int(s.state), s.on_ground, s.on_steep, m_player->waiting(), s.ground_layer, s.ground_material,
                         vec3_json(s.ground_normal), glm::degrees(m_player->yaw()), -glm::degrees(m_player->pitch()),
                         st.stamina, st.health, st.sprinting, st.sneaking, st.swimming, m_player->eye_height() / u,
                         m_player->ticks(), m_player->scripted());
    } else {
        j += "\"player\": null, ";
    }
    // The values in use, from rules/game.osr, for tests and tools.
    const auto& t = m_player ? m_player->tuning() : m_tuning;
    const auto sp = movement::actor_speeds(t.walk_run, t.body, t.settings);
    j += std::format("\"movement\": {{\"walk_units\": {:.3f}, \"run_units\": {:.3f}, \"scale\": {:.4f}, "
                     "\"equipped_weight\": {:.2f}, \"jump_height_min\": {:.2f}, \"half_extents_m\": {}, "
                     "\"step_up_m\": {:.4f}, \"max_slope_deg\": {:.2f}}}, ",
                     sp.forward[0], sp.forward[1], t.body.scale, t.body.equipped_weight, t.settings.jump_height_min,
                     vec3_json(t.controller.half_extents), t.controller.step_up, t.controller.max_slope_deg);
    if (m_physics) {
        const auto ps = m_physics->stats();
        j += std::format("\"physics\": {{\"cells_loaded\": {}, \"cells_loading\": {}, \"bodies\": {}, \"shapes\": {}, "
                         "\"shape_mb\": {:.1f}}}, ",
                         ps.cells_loaded, ps.cells_loading, ps.bodies, ps.shapes_cached, ps.shape_bytes / 1048576.0);
    }
    return j;
}

void App::draw_player_ui() {
    ImGui::SeparatorText("player (N: walk / fly)");
    ImGui::Text("mode %s", m_walk ? "walk" : "fly");
    if (m_player) {
        const auto& s = m_player->state();
        const auto& st = m_player->status();
        const glm::vec3 u = s.position / units::k_skyrim_unit_m;
        ImGui::Text("feet %.0f %.0f %.0f units, speed %.1f units/s%s%s%s", u.x, u.y, u.z,
                    glm::length(glm::vec2(s.velocity)) / units::k_skyrim_unit_m, s.on_ground ? ", ground" : ", air",
                    s.on_steep ? ", steep" : "", m_player->waiting() ? ", waiting for collision" : "");
        ImGui::Text("stamina %.1f, health %.1f%s%s%s%s", st.stamina, st.health, st.sprinting ? ", sprinting" : "",
                    st.sneaking ? ", sneaking" : "", m_always_run ? ", always run" : ", walking",
                    m_auto_move ? ", auto-move" : "");
        ImGui::Text("ground layer %u material %08X", s.ground_layer, s.ground_material);
    }
    if (m_physics) {
        const auto ps = m_physics->stats();
        ImGui::Text("collision: %u cells (+%u), %u bodies, %u shapes %.1f MiB", ps.cells_loaded, ps.cells_loading,
                    ps.bodies, ps.shapes_cached, ps.shape_bytes / 1048576.0);
    }
}

} // namespace spud::engine
