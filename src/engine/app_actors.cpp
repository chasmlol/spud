// Animation events reaching the game: what an actor's graph raised this frame (
// "Events back to the game"; footsteps D.2, D.3).

#include "core/path.hpp"
#include "engine/app_internal.hpp"

#include "audio/system.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/units.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <sstream>

namespace spud::engine {

namespace {

// the tags of the FSTP records, compared case-insensitively.
bool is_footstep_tag(const osd::SoundDb& db, std::string_view tag) {
    return std::ranges::any_of(db.footsteps, [&](const osd::Footstep& f) {
        const std::string_view t = db.str(f.tag);
        return t.size() == tag.size() && std::equal(tag.begin(), tag.end(), t.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
               });
    });
}

} // namespace

// The ground's material at an actor's feet (D.3 step 3): what it stands on, else outdoors the land's dominant texture.
uint32_t App::footstep_material(const ActorEvent& e) {
    uint32_t havok = 0;
    if (e.player && m_player) {
        havok = m_player->state().ground_material;
    } else if (m_physics) {
        if (const auto hit = m_physics->raycast_shared(e.position + glm::vec3(0, 0, 0.5f), glm::vec3(0, 0, -1), 1.5f)) {
            havok = hit->material;
        }
    }
    if (const uint32_t material = havok ? m_audio->material_from_havok_id(havok) : 0) {
        return material;
    }
    if (m_space && !m_space->interior() && m_actor_data) {
        return m_actor_data->land_material(m_space->dominant_land_texture(e.position.x, e.position.y));
    }
    return 0; // an interior with no support material: silence
}

// `actor <hex reference|player> event <name> | idle <IDLE EditorID> | set <variable> <value>`.
// ObjectReference.ApplyHavokImpulse, as the native scales it:
// fGameplayImpulseScale 150 by the layer's multiplier, in Havok units. Clutter's multiplier is 1, a body's 0.04.
void App::havok_impulse(uint32_t ref, const std::array<float, 3>& direction, float magnitude) {
    constexpr float k_impulse_scale = 150.0f;
    if (m_physics) {
        const glm::vec3 d(direction[0], direction[1], direction[2]);
        m_physics->impulse_ref(ref, d * (magnitude * k_impulse_scale * units::k_skyrim_unit_m));
    }
}

void PlayerFace::face_changed(const game::FaceData& face) {
    if (m_app.m_actor_data) {
        m_app.m_actor_data->set_player_face(face);
    }
    if (ActorSet* actors = m_app.m_space ? m_app.m_space->actors() : nullptr) {
        actors->rebuild_player();
    }
}

bool PlayerFace::face_ready() const {
    const ActorSet* actors = m_app.m_space ? m_app.m_space->actors() : nullptr;
    return !actors || actors->player_ready();
}

void PlayerFace::zoom(bool face) {
    if (face != m_app.m_race_camera_face) {
        m_app.m_race_camera_face = face;
        m_app.m_race_camera_from = m_app.m_camera.position;
        m_app.m_race_camera_from_angles = {m_app.m_camera.yaw, m_app.m_camera.pitch};
        m_app.m_race_camera_time = 0.0f;
    }
}

// The game (): the world camera moves to a pose in front of the player, on the player's
// frame. The screen point sets the ray the pose lies along. Poses in game units.
void App::update_race_camera(float dt) {
    const bool open = m_ui && m_ui->is_open("RaceSex Menu") && m_player;
    if (!open) {
        m_race_camera_on = false;
        return;
    }
    if (!m_race_camera_on) {
        m_race_camera_on = true;
        m_race_camera_from = m_camera.position;
        m_race_camera_from_angles = {m_camera.yaw, m_camera.pitch};
        m_race_camera_time = 0.0f;
    }
    constexpr float k_look_at_percent = 0.955f; // fRSMCameraLookAtPercent
    constexpr float k_zoom_time = 1.0f;         // fPlayerZoomTime
    const float distance = m_race_camera_face ? 100.0f : 175.0f; // fPlayerFaceEditDistance, fPlayerBodyEditDistance
    const float screen_y = m_race_camera_face ? 0.4f : 0.5f;
    const float look_z = m_race_camera_face ? -4.0f : -20.0f;
    constexpr float k_yaw = -10.0f;
    const glm::vec3 feet = m_player->feet() / units::k_skyrim_unit_m;
    const glm::vec3 target = feet + glm::vec3(0, 0, 128.0f * k_look_at_percent);
    const float aspect = 16.0f / 9.0f;
    const float along = distance / aspect;
    const float heading = m_player->yaw();
    const glm::vec3 ahead(std::sin(heading), std::cos(heading), 0.0f);
    const glm::vec3 to = target + ahead * along + glm::vec3(0, 0, (0.5f - screen_y) * 2.0f * std::tan(m_camera.fov_y * 0.5f) * along);
    const glm::vec3 look = glm::normalize(target + glm::vec3(0, 0, look_z) - to);
    const float yaw = std::atan2(look.x, look.y) + glm::radians(k_yaw);
    const float pitch = std::asin(look.z);

    m_race_camera_time += dt;
    const float u = std::clamp(m_race_camera_time / k_zoom_time, 0.0f, 1.0f);
    const float t = (1.0f - std::sin(glm::pi<float>() * u + glm::half_pi<float>())) * 0.5f;
    m_camera.position = glm::mix(m_race_camera_from, to * units::k_skyrim_unit_m, t);
    float turn = std::remainder(yaw - m_race_camera_from_angles.x, glm::two_pi<float>());
    m_camera.yaw = m_race_camera_from_angles.x + turn * t;
    m_camera.pitch = glm::mix(m_race_camera_from_angles.y, pitch, t);
}

void App::actor_speak(uint32_t ref, const std::string& line, int emotion, int strength) {
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        if (line.empty()) {
            actors->stop_speaking(ref);
        } else {
            actors->speak(ref, line, emotion, strength);
        }
    }
}

std::optional<glm::vec3> App::speaker_position(uint32_t ref) const {
    // The game follows the process's head node; a talking activator has only its reference.
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        if (const std::optional<glm::vec3> head = actors->head_position(ref)) {
            return head;
        }
    }
    const auto at = m_game ? m_game->ref_position(ref) : std::nullopt;
    return at ? std::optional(glm::vec3((*at)[0], (*at)[1], (*at)[2]) * units::k_skyrim_unit_m) : std::nullopt;
}

void App::actor_command(const std::string& line) {
    std::istringstream in(line);
    std::string who, what, name;
    float value = 0.0f;
    float z = 0.0f;
    in >> who >> what >> name >> value >> z;
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    if (actors && what == "graph") {
        std::string list;
        for (const std::string& node : actors->graph_nodes(who == "player" ? ActorSet::k_player_body : static_cast<uint32_t>(std::stoul(who, nullptr, 16)))) {
            list += node + " ";
        }
        log::info("actor {} graph: {}", who, list);
        return;
    }
    if (!actors || (name.empty() && what != "kill" && what != "activate")) {
        throw Error("actor <reference|player> event <name> | idle <editor id> | set <variable> <value> | kill | drop <item>");
    }
    if (what == "package" && m_game) {
        m_game->ai().force_package(static_cast<uint32_t>(std::stoul(who, nullptr, 16)), static_cast<uint32_t>(std::stoul(name, nullptr, 16)));
        return;
    }
    if (what == "action" && m_game && m_combat_world) {
        const uint32_t ref = who == "player" ? game::k_player_ref : static_cast<uint32_t>(std::stoul(who, nullptr, 16));
        log::info("actor {} action {}: {}", who, name, m_combat_world->perform_action(ref, name) ? "taken" : "not taken");
        return;
    }
    if (what == "killmove" && m_game) { // <victim>:<the killer's pa_ event>, the pair as the action tree starts it
        const uint32_t killer = who == "player" ? game::k_player_ref : static_cast<uint32_t>(std::stoul(who, nullptr, 16));
        const size_t colon = name.find(':');
        const uint32_t victim = static_cast<uint32_t>(std::stoul(name.substr(0, colon), nullptr, 16));
        const std::string event = name.substr(colon + 1);
        m_game->combat().fighter(killer).combat_target = victim;
        const bool ok = actors->start_pair(who == "player" ? ActorSet::k_player_body : killer, victim, event, std::string_view(event).substr(3));
        if (ok) {
            m_game->ai().hold(victim, true);
            m_game->combat().attack_started(killer, event);
        }
        log::info("actor {} killmove {}: {}", who, name, ok ? "taken" : "not taken");
        return;
    }
    if ((what == "hit" || what == "fight") && m_game) {
        // The checks of: a swing's worth of hit, or a fight started.
        const auto parse = [](const std::string& s) { return s == "player" ? game::k_player_ref : uint32_t(std::stoul(s, nullptr, 16)); };
        const uint32_t attacker = parse(who), victim = parse(name);
        game::Combat& combat = m_game->combat();
        if (what == "fight") {
            combat.start_combat(attacker, victim);
            return;
        }
        const float health = m_game->actor_values(victim).current(game::av_health);
        combat.hit_target(attacker, victim, 0, false);
        log::info("hit {:08X}: health {:.4f} -> {:.4f}, armor {:.4f}", victim, health, m_game->actor_values(victim).current(game::av_health), m_game->actor_values(victim).current(game::av_damage_resist));
        return;
    }
    const uint32_t ref = who == "player" ? ActorSet::k_player_body : static_cast<uint32_t>(std::stoul(who, nullptr, 16));
    const bool ok = what == "event" ? actors->send_event(ref, name)
                    : what == "idle" ? actors->play_idle(ref, name)
                    : what == "set"  ? actors->set_variable(ref, name, value)
                    : what == "kill" ? actors->kill(ref)
                    : what == "activate" ? (papyrus_activate(ref, m_game->inspect(ref).base), true)
                    : what == "speak" ? (actors->speak(ref, name, 0, 0), true)
                    : what == "speak_as" ? (actors->speak(ref, name, static_cast<int>(value), static_cast<int>(z)), true)
                    : what == "damage" ? (damage_ref(ref, std::stof(name), false), true)
                    : what == "drop" ? (drop_item({.form = static_cast<uint32_t>(std::stoul(name, nullptr, 16)), .count = 1}), true)
                    : what == "impulse" ? (havok_impulse(ref, {std::stof(name), value, z}, 1.0f), true)
                    : what == "blood" ? (actors->add_weapon_blood(ref, {name == "set" ? 1.0f : name == "add" ? 0.25f : -1.0f}), true)
                                     : throw Error("actor: event, idle, set, kill, impulse or blood");
    log::info("actor {} {} {}: {}", who, what, name, ok ? "taken" : "not taken");
}

void App::process_actor_events() {
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    if (!actors) {
        return;
    }
    if (m_game) {
        for (const ActorEvent& e : actors->events()) {
            const uint32_t ref = e.player ? game::k_player_ref : e.ref;
            m_game->combat().animation_event(ref, e.tag, e.payload);
            m_game->casters().animation_event(ref, e.tag);
        }
    }
    for (const ActorEvent& e : actors->events()) {
        if (e.player && !e.first_person && m_player && m_player->view() == View::third_person) {
            if (e.tag == "StartAnimatedCameraDelta") { // a new blend starts from nothing
                m_camera_delta.node = e.payload;
                m_camera_delta.rate = 1.0f;
                m_camera_delta.weight = 0.0f;
            } else if (e.tag == "EndAnimatedCamera") {
                m_camera_delta.node.clear();
                m_camera_delta.rate = -1.0f;
            }
        }
        if (m_papyrus && !m_animation_waits.empty()) {
            const uint32_t sender = e.player ? game::k_player_ref : e.ref;
            const std::string tag = path::to_lower(e.tag);
            std::erase_if(m_animation_waits, [&](const AnimationWait& w) {
                if (w.ref != sender || w.event != tag) {
                    return false;
                }
                m_papyrus->vm().return_from_latent(w.stack, script::Value::of(true));
                return true;
            });
        }
        if (m_papyrus && !m_animation_listeners.empty()) {
            const uint32_t sender = e.player ? game::k_player_ref : e.ref;
            const std::string tag = path::to_lower(e.tag);
            for (const AnimationListener& l : m_animation_listeners) {
                if (l.sender == sender && l.event == tag) {
                    script::VirtualMachine& vm = m_papyrus->vm();
                    vm.send_event(l.handle, "OnAnimationEvent", {vm.form_value(sender, "ObjectReference"),
                                                                 script::Value::of_string(vm.intern(e.tag))});
                }
            }
        }
        if (path::to_lower(e.tag) == "exitcartend") { // no vehicle
            set_vehicle(e.player ? game::k_player_ref : e.ref, 0);
        }
        if (e.player && m_seat.ref && e.tag == "IdleFurnitureExit") {
            if (m_seat.leaving < 0.0f) {
                m_game->script_event(game::k_player_ref, "OnGetUp", {{m_seat.ref}});
            }
            seat_left();
        }
        if (e.tag == "CameraShake") { // the payload is the strength
            shake_camera(e.position / units::k_skyrim_unit_m, float(std::atof(e.payload.c_str())), 0.0f);
        }
        if (e.tag.starts_with("Flight") && m_game) {
            m_game->ai().flight_event(e.ref, e.tag);
        }
        if (m_mount.ref && m_mount.stage != MountStage::riding && (e.ref == m_mount.ref || e.player)) {
            if (e.tag == "PairEnd" || e.tag == "MountEnd" || e.tag == "HorseExitOut" || e.tag == "DragonMountExitOut") {
                end_mount_stage();
            }
        }
        if (!m_audio) {
            continue;
        }
        const std::string annotation = e.payload.empty() ? e.tag : std::format("{}.{}", e.tag, e.payload);
        if (m_audio->animation_event(e.ref, annotation, e.position)) {
            continue;
        }
        if (!is_footstep_tag(m_audio->db(), e.tag)) {
            continue;
        }
        const uint32_t material = footstep_material(e);
        if (!material) {
            continue;
        }
        osd::FootstepGait gait = osd::gait_walk;
        if (e.player && m_player) {
            const auto& status = m_player->status();
            gait = status.swimming        ? osd::gait_swim
                   : status.sneaking      ? osd::gait_sneak
                   : status.sprinting     ? osd::gait_sprint
                   : e.run_blend >= 0.5f  ? osd::gait_run
                                          : osd::gait_walk;
        }
        const bool first_view = e.player && m_player && m_player->view() == View::first_person;
        if (e.footstep_sets.empty()) {
            m_audio->footstep(osd::k_none, gait, e.tag, material, e.position, e.player, first_view);
        }
        for (uint32_t form : e.footstep_sets) { // each distinct set plays its own sound
            const auto set = m_audio->find_footstep_set(std::format("{:08X}", form));
            m_audio->footstep(set ? *set : osd::k_none, gait, e.tag, material, e.position, e.player, first_view);
        }
    }
    actors->clear_events();
}

} // namespace spud::engine
