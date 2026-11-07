// Riding: the player's character becomes the mount's, the mount's actor follows it, and the rider's body sits on its
// saddle node.

#include "engine/app_internal.hpp"

#include "core/log.hpp"
#include "core/units.hpp"
#include "game/ai.hpp"
#include "game/crime.hpp"
#include "game/game.hpp"

namespace spud::engine {

namespace {

// A race's offset, x right and y ahead of the mount, into the world.
glm::vec3 offset_at(const glm::vec3& feet, float heading, const std::array<float, 3>& offset) {
    const float c = std::cos(heading), s = std::sin(heading);
    const float u = units::k_skyrim_unit_m;
    return feet + glm::vec3(offset[0] * c + offset[1] * s, -offset[0] * s + offset[1] * c, offset[2]) * u;
}

} // namespace

// the rider takes the mount's movement, and control comes back when the animation ends.
void App::mount(const game::Target& target) {
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    const game::Agent* agent = m_game->ai().agent(target.ref);
    const auto data = m_game->mount_data(target.base, tamed_dragon(target.ref));
    if (!actors || !agent || !data || !m_player || !m_rules || m_mount.ref) {
        return;
    }
    if (!data->horse) {
        mount_dragon(target, *data);
        return;
    }
    glm::vec3 feet(0.0f);
    bool found = false;
    for (const ActorSet::Entry& e : actors->list()) {
        if (e.ref == target.ref) {
            feet = e.position;
            found = true;
        }
    }
    if (!found) {
        return;
    }
    const auto type = [&](const char* edid) {
        const osr::MovementType* t = m_rules->movement_type(edid);
        return t ? *t : osr::MovementType{};
    };
    const auto setting = [&](const char* name, double fallback) {
        return float(m_rules->setting(name).value_or(fallback));
    };
    Riding r;
    r.walk_run = type("Horse_Default_MT");
    r.sprint = type("Horse_Sprint_MT");
    r.swim = type("Horse_Swim_MT");
    r.fall = type("Horse_Fall_MT");
    r.half_extents = glm::vec3(data->half[0], data->half[1], data->half[2]) * units::k_skyrim_unit_m;
    r.max_stamina = r.stamina = data->stamina;
    r.stamina_rate = data->stamina_regen;
    r.dampen_time = setting("fHorseControlsDampenTime", 1.0);
    r.max_upward_pitch = glm::radians(setting("fHorseMaxUpwardPitch", 25.0));
    r.max_angle_before_turn = glm::radians(setting("fHorseMaxAngleBeforeTurn", 90.0));
    r.camera_offset = glm::vec3(data->camera_offset[0], data->camera_offset[1], data->camera_offset[2]);
    m_mount = {target.ref, target.base, MountStage::entering, setting("fMountDismountTimeout", 8.0), target.steal, *data};
    m_game->ridden_horse = target.ref;
    m_game->ai().hold(target.ref, true);
    m_player->mount(r, feet, agent->heading);
    // The game lays the horse's third-person state over whatever view it was, and the exit
    // pops back to it (measured: state 10 after mounting from first person).
    m_mount_first_person = m_player->view() == View::first_person;
    m_player->set_view(View::third_person);
    m_mount.waiting_for_body = true; // the third-person body comes up first; update_mount sends it
    update_mount(0.0f);
}

// in the saddle. A horse the rider may not take is stolen now, and isn't their last.
void App::mounted() {
    m_mount.stage = MountStage::riding;
    if (m_mount.dragon) { // no theft and no last-ridden here; the leash starts at the rider
        game::Ai& ai = m_game->ai();
        ai.hold(m_mount.ref, false);
        const glm::vec3 p = m_player->feet() / units::k_skyrim_unit_m;
        m_mount.leash = p;
        ai.order_flight(m_mount.ref, game::AiMode::orbit, {p.x, p.y, p.z});
        return;
    }
    if (m_mount.steal) {
        m_game->crime().report_theft(m_mount.ref, m_mount.npc, 1, 0, m_game->owner_of(m_mount.ref), false);
    } else if (m_mount.data.horse) {
        m_game->set_last_horse(m_mount.ref);
    }
}

// refused in the air, in a jump, and with no room at the dismount spot.
void App::dismount() {
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    if (!actors || !m_player || m_mount.stage != MountStage::riding) {
        return;
    }
    if (m_mount.dragon) {
        const game::Agent* a = m_game->ai().agent(m_mount.ref);
        const bool flying = a && a->fly != game::FlyState::none && a->fly != game::FlyState::perching;
        if (flying) {
            // The landing request, and a second press takes it back.
            if (m_mount.landing_timer > 0.0f) {
                return;
            }
            const game::Settings& s = m_game->settings();
            const glm::vec3 p = m_mount.leash;
            m_mount.landing = !m_mount.landing;
            if (m_mount.landing) {
                m_ui->notification(s.text("sTESVDLC2FlyingMountLandingMessage"));
                m_game->ai().order_flight(m_mount.ref, game::AiMode::travel, {p.x, p.y, p.z});
            } else {
                m_mount.landing_timer = s.number("fFlyingMountLandingRequestTimer", 3.0f);
                m_ui->notification(s.text("sTESVDLC2FlyingMountCancelLandingMessage"));
                m_game->ai().order_flight(m_mount.ref, game::AiMode::orbit, {p.x, p.y, p.z});
            }
            return;
        }
        m_mount.stage = MountStage::leaving;
        m_mount.timeout = float(m_rules->setting("fMountDismountTimeout").value_or(8.0));
        m_game->ai().hold(m_mount.ref, true);
        start_mount_pair(*actors, "DragonMountExit");
        return;
    }
    if (m_player->state().state == physics::ControllerState::in_air) {
        return;
    }
    const glm::vec3 feet = m_player->feet();
    const glm::vec3 spot = offset_at(feet, m_player->mount_heading(), m_mount.data.dismount_offset);
    if (m_physics) {
        const glm::vec3 from = feet + glm::vec3(0, 0, 1.0f);
        const glm::vec3 to = spot + glm::vec3(0, 0, 0.1f);
        const float length = glm::length(to - from);
        if (m_physics->raycast_shared(from, (to - from) / length, length)) {
            log::info("mount: no room to dismount");
            return;
        }
    }
    m_mount.stage = MountStage::leaving;
    m_mount.timeout = float(m_rules->setting("fMountDismountTimeout").value_or(8.0));
    start_mount_pair(*actors, "HorseExit");
}

// or a forced exit: on foot at the spot, and the horse is the AI's again.
void App::dismounted() {
    game::Ai& ai = m_game->ai();
    if (m_mount.dragon) {
        const game::Agent* a = ai.agent(m_mount.ref);
        const float u = units::k_skyrim_unit_m;
        const glm::vec3 feet = a ? glm::vec3(a->position[0], a->position[1], a->position[2]) * u : m_player->feet();
        const glm::vec3 spot = offset_at(feet, a ? a->heading : 0.0f, m_mount.data.dismount_offset);
        m_player->set_paused(false);
        m_player->teleport(spot);
        m_player->set_view(View::third_person);
        ai.hold(m_mount.ref, false);
        ai.order_flight(m_mount.ref, game::AiMode::stay, {});
        m_game->ridden_horse = 0;
        m_mount = {};
        return;
    }
    const glm::vec3 feet = m_player->feet();
    const float heading = m_player->mount_heading();
    const glm::vec3 spot = offset_at(feet, heading, m_mount.data.dismount_offset);
    m_player->dismount(spot);
    m_body_stamina = m_player->status().stamina;
    if (game::Agent* a = ai.mutable_agent(m_mount.ref)) {
        const glm::vec3 p = feet / units::k_skyrim_unit_m;
        a->position = {p.x, p.y, p.z};
        a->heading = heading;
        a->spot = {};
        a->path.clear();
        a->next = 0;
    }
    ai.hold(m_mount.ref, false);
    m_game->ridden_horse = 0;
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        actors->set_variable(ActorSet::k_player_body, "bWantMountedWeaponAnims", 0.0f);
        actors->set_variable(ActorSet::k_player_body, "iWantMountedWeaponAnims", 0.0f);
    }
    // Leaving the horse's camera goes through the camera's transition state, iHorseTransitionMillis:Camera (500 in the
    // executable), to whichever view was under it.
    start_camera_tween(float(m_rules ? m_rules->setting("iHorseTransitionMillis").value_or(500.0) : 500.0) / 1000.0f);
    if (std::exchange(m_mount_first_person, false)) {
        m_player->set_view(View::first_person);
    }
    m_mount = {};
}

// Each frame while mounted: the stages' ends, by the graphs' events or the timeout.
void App::update_mount(float dt) {
    if (!m_mount.ref) {
        return;
    }
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr;
        actors && m_mount.waiting_for_body && actors->player_body_ready(true)) {
        // One synchronised pair: the rider's event, and the same with pa_ for the mount's side of it.
        m_mount.waiting_for_body = false;
        const std::string event = m_mount.dragon ? "DragonMountEnter" : "HorseEnter";
        start_mount_pair(*actors, event);
        actors->set_variable(ActorSet::k_player_body, "bWantMountedWeaponAnims", 1.0f);
        actors->set_variable(ActorSet::k_player_body, "iWantMountedWeaponAnims", m_mount.dragon ? 2.0f : 1.0f);
    }
    if (!m_player || (!m_mount.dragon && !m_player->riding())) {
        m_mount = {}; // a load or a new player
        return;
    }
    if (m_mount.dragon) {
        update_dragon(dt);
        return;
    }
    if (game::Agent* a = m_game->ai().mutable_agent(m_mount.ref)) {
        const glm::vec3 p = m_player->feet() / units::k_skyrim_unit_m;
        a->position = {p.x, p.y, p.z};
        a->heading = m_player->mount_heading();
    }
    if (m_game->dismount_asked) {
        m_game->dismount_asked = false;
        dismount();
    }
    if (m_mount.stage == MountStage::riding) {
        // The horse's graph hears of the air from the character: a jump's lift-off, a fall that lasts, and the landing.
        const physics::CharacterState& state = m_player->state();
        const bool air = state.state == physics::ControllerState::in_air;
        m_mount.air_time = air ? m_mount.air_time + dt : 0.0f;
        const char* event = nullptr;
        if (!air && m_mount.in_air) {
            event = "landStart";
        } else if (air && !m_mount.in_air && state.velocity.z > 1.0f) {
            m_mount.in_air = true; // its own jump: the graph has been in it since the button
        } else if (air && !m_mount.in_air && m_mount.air_time > 0.25f) {
            event = "forwardFallStart";
        }
        if (event) {
            m_mount.in_air = air;
            if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
                actors->send_event(m_mount.ref, event);
                actors->send_event(ActorSet::k_player_body, event);
            }
        }
        return;
    }
    if ((m_mount.timeout -= dt) <= 0.0f) {
        end_mount_stage();
    }
}

// Getting on and off are synchronised pairs, the rider's event and the same with pa_ for the mount's side: the clip's
// marks place the rider's root, from the spot at the flank up to the saddle and back (measured in the game: the rider's
// root rises 130 units over 75 frames as it turns from facing the horse's side to its heading).
void App::start_mount_pair(ActorSet& actors, std::string_view event) {
    const std::string mount_event = "pa_" + std::string(event);
    if (!actors.start_pair(ActorSet::k_player_body, m_mount.ref, event, mount_event)) {
        actors.send_event(ActorSet::k_player_body, event);
        actors.send_event(m_mount.ref, mount_event);
    }
}

// The Keyword - Mount (default object 352) on a quest alias: Bend Will's DLC2TameDragon gives it to the dragon it holds.
bool App::tamed_dragon(uint32_t ref) const {
    const uint32_t keyword = m_game->keyword("DLC2DragonMountKeyword");
    return m_ride_any_dragon || (keyword && m_papyrus && m_papyrus->quests() && m_papyrus->quests()->alias_has_keyword(ref, keyword));
}

// The player doesn't steer a dragon: the character is carried, and the dragon's AI circles the leash.
void App::mount_dragon(const game::Target& target, const game::MountData& data) {
    const float timeout = float(m_rules->setting("fMountDismountTimeout").value_or(8.0));
    m_mount = {target.ref, target.base, MountStage::entering, timeout, false, data};
    m_mount.dragon = true;
    m_game->ridden_horse = target.ref;
    m_game->ai().hold(target.ref, true);
    m_player->set_paused(true);
    m_player->set_view(View::first_person); // mouse look turns the view; dragon_camera() places it
    m_mount.waiting_for_body = true;
    update_mount(0.0f);
}

// the leash goes after the rider at the base speed, in x and y, and z is the rider's.
void App::update_dragon(float dt) {
    const game::Agent* a = m_game->ai().agent(m_mount.ref);
    if (!a) {
        return;
    }
    const float u = units::k_skyrim_unit_m;
    m_player->carried_to(glm::vec3(a->position[0], a->position[1], a->position[2]) * u);
    m_mount.landing_timer = std::max(m_mount.landing_timer - dt, 0.0f);
    if (m_mount.stage != MountStage::riding) {
        if ((m_mount.timeout -= dt) <= 0.0f) {
            end_mount_stage();
        }
        return;
    }
    const glm::vec3 rider(a->position[0], a->position[1], a->position[2]);
    const glm::vec2 to = glm::vec2(rider) - glm::vec2(m_mount.leash);
    const float most = m_game->settings().number("fPlayerFlyingMountBaseTargetSpeed", 700.0f) * dt;
    const float length = glm::length(to);
    if (length > 0.0f) {
        const glm::vec2 step = to * (std::min(length, most) / length);
        m_mount.leash += glm::vec3(step, 0.0f);
    }
    m_mount.leash.z = rider.z;
    if (game::Agent* m = m_game->ai().mutable_agent(m_mount.ref); m && !m_mount.landing) {
        m->flight.centre = {m_mount.leash.x, m_mount.leash.y, m_mount.leash.z};
    }
    if (m_mount.landing && a->fly == game::FlyState::none) {
        m_mount.landing = false;
        dismount(); // the queue's landing reached: off
    }
}

// the player's root and Cam3 height, the race-independent offset, and the flying zoom, on the free look.
void App::dragon_camera() {
    const game::Settings& s = m_game->settings();
    const game::Agent* a = m_game->ai().agent(m_mount.ref);
    const float u = units::k_skyrim_unit_m;
    const bool flying = a && a->fly != game::FlyState::none && a->fly != game::FlyState::perching;
    const float back = 600.0f + (flying ? s.number("fVanityModeMinDistFly", 1200.0f) : s.number("fVanityModeMinDist", 155.0f));
    const float yaw = m_player->yaw();
    const float up = glm::radians(s.number("fDragonMaxUpwardPitch", 45.0f));
    const float pitch = std::clamp(m_player->pitch(), -glm::half_pi<float>(), up);
    const glm::vec3 view(std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch));
    const glm::vec3 pivot = m_player->feet() + glm::vec3(0, 0, m_player->tuning().camera3_height);
    m_camera.position = pivot - view * (back * u);
    m_camera.yaw = yaw;
    m_camera.set_pitch(pitch);
}

void App::end_mount_stage() {
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        actors->send_event(m_mount.ref, "PairEnd");
        if (m_mount.stage == MountStage::leaving) {
            actors->send_event(m_mount.ref, "SprintStop"); // it went into the pair sprinting
        }
    }
    if (m_mount.stage == MountStage::entering) {
        mounted();
    } else if (m_mount.stage == MountStage::leaving) {
        dismounted();
    }
}

} // namespace spud::engine
