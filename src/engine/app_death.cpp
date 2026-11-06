// How the player feels damage and how the player dies: the hurt voice and GetHit, the heartbeat and
// low health, drowning, and going down: the death camera, slow time and its sound, the death music and voice.

#include "core/units.hpp"
#include "engine/app_internal.hpp"
#include "game/game.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>

#include <cmath>
#include <random>

namespace spud::engine {

namespace {

constexpr int k_topic_hit = 29, k_topic_death = 33;
constexpr uint32_t k_get_hit_imad = 0x162;      // GetHit, hard-coded in the game
constexpr uint32_t k_low_health_imad = 0x10C445; // LowHealthImod, default object IMLH
constexpr const char* k_heartbeat_slow = "0004DAAD"; // UIHealthHeartbeatALPSD, HSSD
constexpr const char* k_heartbeat_fast = "0004DAAE"; // UIHealthHeartbeatBLPSD, HFSD
constexpr const char* k_drowning = "000B6344";       // NPCHumanDrowning
constexpr const char* k_down_loop = "00049502";      // MAGShoutSlowTimeActiveLP
constexpr const char* k_death_music = "00013686";    // MUSSpecialDeath, default object DTMS
constexpr float k_down_time_rate = 0.8f;
constexpr float k_down_distance = 200.0f;            // units, CreateCameraStates
constexpr float k_down_pitch = 30.0f;                // degrees, 0.5236 rad
// The game looks up a node by a name not yet resolved, and without one takes the
// root at the camera target height. The body's middle stands in, and follows the ragdoll.
constexpr const char* k_down_node = "npc spine2 [spn2]";

} // namespace

bool App::player_down() const {
    if (!m_walk || !m_player || !m_game) {
        return false;
    }
    const game::LifeState life = m_game->combat().fighter(game::k_player_ref).life;
    return life == game::LifeState::dead || life == game::LifeState::essential_down;
}

void App::player_pain(bool say, bool imod) {
    if (!m_game || m_game->combat().fighter(game::k_player_ref).life != game::LifeState::alive || m_pain_wait > 0.0f) {
        return;
    }
    m_pain_wait = m_rules ? float(m_rules->setting("fPainDelay").value_or(1.5f)) : 1.5f;
    if (imod && m_vfx) {
        m_vfx->modifier(k_get_hit_imad, 1.0f, true);
    }
    if (say) {
        m_game->combat().world().comment(game::k_player_ref, k_topic_hit, 0);
    }
}

void App::player_struck() {
    if (m_vfx) {
        m_vfx->modifier(k_get_hit_imad, 1.0f, true);
    }
}

void App::player_died() {
    if (m_ui) {
        m_ui->close_all(true); // all but the HUD: UI_CloseAllMenus
    }
    if (m_audio) {
        if (const auto music = m_audio->find_music_type(k_death_music)) {
            m_audio->music_add(*music);
        }
    }
    m_game->combat().world().comment(game::k_player_ref, k_topic_death, 0);
}

void App::end_down_view() {
    if (!m_down_view) {
        return;
    }
    m_down_view = false;
    m_time_rate = m_player_rate = 1.0f;
    if (m_player) {
        m_player->set_view(m_view_before_down);
    }
    if (m_audio) {
        if (const auto music = m_audio->find_music_type(k_death_music)) {
            m_audio->music_remove(*music);
        }
    }
    if (m_audio && m_down_loop) {
        m_audio->stop(m_down_loop);
    }
    m_down_loop = {};
    if (m_ui) {
        m_ui->hud_mode("VATSPlayback", false);
    }
}

void App::revive_player() {
    if (!m_game) {
        return;
    }
    game::Fighter& f = m_game->combat().fighter(game::k_player_ref);
    if (f.life == game::LifeState::alive) {
        return;
    }
    f.life = game::LifeState::alive;
    m_game->ai().hold(game::k_player_ref, false);
    std::erase_if(m_dying, [](const Dying& d) { return d.ref == game::k_player_ref; });
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        actors->new_player_body();
    }
    if (m_player) {
        m_player->set_down(false);
    }
}

void App::place_down_camera() {
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    glm::mat3 rotation(1.0f);
    glm::vec3 target = m_player->feet() + glm::vec3(0.0f, 0.0f, m_player->eye_height() * 0.5f);
    if (actors) {
        actors->node_world(ActorSet::k_player_body, k_down_node, rotation, target);
    }
    // Where the view is blocked it tries four more angles, 90 apart in a radian argument, as the game's constant has it.
    const float distance = k_down_distance * units::k_skyrim_unit_m;
    for (int i = 0; i < 5; ++i) {
        m_camera.set_skyrim_angles(glm::degrees(m_down_yaw + float(i) * 90.0f), k_down_pitch);
        m_camera.position = target - m_camera.forward() * distance;
        if (!m_physics || !m_physics->sphere_cast(target, m_camera.position, 0.1f, physics::default_character_layers())) {
            break;
        }
    }
}

void App::update_player_condition(float dt) {
    if (!m_game || !m_player || !m_walk) {
        end_down_view();
        return;
    }
    if (m_load) {
        return; // the loading screen hides all this, and the save ends it
    }
    m_pain_wait = std::max(m_pain_wait - dt, 0.0f);
    const bool down = player_down();
    if (down && !m_down_view) { // at once, no tween, first person too
        static std::mt19937 random{std::random_device{}()};
        m_down_view = true;
        m_down_yaw = std::uniform_real_distribution<float>(0.0f, glm::two_pi<float>())(random);
        m_time_rate = m_player_rate = k_down_time_rate;
        m_view_before_down = m_player->view();
        m_player->set_view(View::third_person);
        if (m_ui) {
            m_ui->hud_mode("VATSPlayback", true);
        }
        if (m_audio) {
            if (const auto loop = m_audio->find_descriptor(k_down_loop)) {
                m_down_loop = m_audio->play(*loop);
            }
        }
    } else if (!down && m_down_view) {
        end_down_view();
    }

    const game::ActorValues& values = m_game->character().values();
    const float full = std::max(values.maximum(game::av_health), 1.0f);
    const float fraction = values.current(game::av_health) / full;
    const auto setting = [&](std::string_view name, float fallback) {
        return m_rules ? float(m_rules->setting(name).value_or(fallback)) : fallback;
    };
    // changing band stops one and starts the other, nothing at 0 or above the slow mark.
    const float slow = setting("fPlayerHealthHeartbeatSlow", 0.2f), fast = setting("fPlayerHealthHeartbeatFast", 0.1f);
    const int band = fraction <= 0.0f || fraction > slow ? 0 : fraction > fast ? 1 : 2;
    if (band != m_heartbeat_band && m_audio) {
        m_heartbeat_band = band;
        if (m_heartbeat) {
            m_audio->stop(m_heartbeat);
            m_heartbeat = {};
        }
        if (const auto beat = band ? m_audio->find_descriptor(band == 1 ? k_heartbeat_slow : k_heartbeat_fast) : std::nullopt) {
            m_heartbeat = m_audio->play(*beat);
        }
    }
    // at once, then every fLowHealthIModInterval, stronger as health falls from 0.2 to 0.1. It ends
    // with life: 2.5 s after dying the game's image is sharp.
    if (fraction <= 0.2f && !down) {
        m_low_health_wait -= dt;
        if (m_low_health_wait <= 0.0f && m_vfx) {
            m_low_health_wait = 2.0f;
            const float t = std::clamp((0.2f - fraction) / 0.1f, 0.0f, 1.0f);
            m_vfx->modifier(k_low_health_imad, 0.8f + (1.5f - 0.8f) * t, false);
        }
    } else {
        m_low_health_wait = 0.0f;
    }
    // Drowning: its sound, and GetHit without a line.
    const bool drowning = m_player->status().breath <= 0.0f && !down;
    if (drowning) {
        player_pain(false, true);
    }
    if (drowning != (m_drowning != 0) && m_audio) {
        if (m_drowning) {
            m_audio->stop(m_drowning);
            m_drowning = {};
        } else if (const auto sound = m_audio->find_descriptor(k_drowning)) {
            m_drowning = m_audio->play(*sound);
        }
    }
}

} // namespace spud::engine
