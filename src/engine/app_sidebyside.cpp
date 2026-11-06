// Mirror mode: the game's input applied here as if from this machine's devices,
// the player kept beside the game's, and everything recorded on the shared timeline.

#include "engine/app_internal.hpp"

#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"
#include "engine/keys.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <fstream>

namespace spud::engine {

namespace {

// How far the two players may come apart before this one is put back beside the game's. Both run the same input, so
// they stay within a stride of each other until one of them meets something the other doesn't; each correction is
// logged, because it is a finding (units, seconds, degrees).
constexpr float k_drift_snap = 160.0f;
constexpr float k_drift_snap_at_once = 1000.0f;
constexpr double k_drift_hold = 0.75;
constexpr float k_rest_snap = 24.0f; // both standing: nothing to wait for, a gap this big won't close by itself
constexpr double k_rest_hold = 1.0;
constexpr float k_rest_speed = 2.0f;
constexpr float k_look_snap = 3.0f;
constexpr double k_look_hold = 0.25;
constexpr double k_world_period = 0.25;
constexpr float k_hour_drift = 1.0f / 60.0f;

float angle_between(float a, float b) {
    return std::abs(std::remainder(a - b, 360.0f));
}

std::string vec_json(const glm::vec3& v) {
    return std::format("[{:.2f},{:.2f},{:.2f}]", v.x, v.y, v.z);
}

} // namespace

void App::sbs_start() {
    m_sbs = std::make_unique<SideBySide>(*m_opt.mirror_port, m_opt.sbs_dir, m_opt.sbs_qpc0);
    m_ui_visible = false; // the picture is what gets compared: F1 still brings the overlay up
    SideBySide* sbs = m_sbs.get();
    log::set_listener([sbs](log::Level level, std::string_view message) { sbs->log_line(int(level), message); });
}

void App::sbs_stop() {
    if (m_sbs) {
        log::set_listener(nullptr);
        m_sbs->event("exit");
        m_sbs->flush();
    }
}

const bool* App::key_state() {
    const bool* real = SDL_GetKeyboardState(nullptr);
    if (!m_sbs) {
        return real;
    }
    for (size_t i = 0; i < m_sbs_keys.size(); ++i) {
        m_sbs_keys[i] = real[i] || m_sbs->keys[i];
    }
    return m_sbs_keys.data();
}

uint32_t App::mouse_state() {
    const uint32_t real = SDL_GetMouseState(nullptr, nullptr);
    return m_sbs ? real | m_sbs->mouse_buttons : real;
}

// The game's input as this window's own: the same events and the same held keys the devices would give.
void App::sbs_input(bool& quit) {
    const SDL_WindowID window = SDL_GetWindowID(m_window);
    auto key = [&](SDL_Scancode sc, bool down) {
        m_sbs->keys[sc] = down;
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.windowID = window;
        e.key.scancode = sc;
        e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
        e.key.down = down;
        handle_event(e, quit);
    };
    auto button = [&](uint8_t b, bool down) {
        const uint32_t mask = SDL_BUTTON_MASK(b);
        m_sbs->mouse_buttons = down ? m_sbs->mouse_buttons | mask : m_sbs->mouse_buttons & ~mask;
        SDL_Event e{};
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = window;
        e.button.button = b;
        e.button.down = down;
        e.button.clicks = 1;
        handle_event(e, quit);
    };
    for (const MirrorInput& in : m_sbs->take_input()) {
        using K = MirrorInput::Kind;
        switch (in.kind) {
        case K::key:
            if (const SDL_Scancode sc = scancode_from_dik(uint16_t(in.code)); sc != SDL_SCANCODE_UNKNOWN) {
                key(sc, in.down);
            }
            break;
        case K::mouse_button:
            button(in.code == 0 ? SDL_BUTTON_LEFT : in.code == 1 ? SDL_BUTTON_RIGHT : uint8_t(SDL_BUTTON_MIDDLE + in.code - 2), in.down);
            break;
        case K::mouse_move: {
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.windowID = window;
            e.motion.xrel = in.x;
            e.motion.yrel = in.y;
            handle_event(e, quit);
            break;
        }
        case K::wheel: {
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.windowID = window;
            e.wheel.y = in.x;
            e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
            handle_event(e, quit);
            break;
        }
        case K::text: {
            static char text[8];
            *SDL_UCS4ToUTF8(in.code, text) = '\0';
            SDL_Event e{};
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.windowID = window;
            e.text.text = text;
            handle_event(e, quit);
            break;
        }
        case K::release_all:
            for (int sc = 0; sc < SDL_SCANCODE_COUNT; ++sc) {
                if (m_sbs->keys[size_t(sc)]) {
                    key(SDL_Scancode(sc), false);
                }
            }
            for (uint8_t b = SDL_BUTTON_LEFT; b <= SDL_BUTTON_X2; ++b) {
                if (m_sbs->mouse_buttons & SDL_BUTTON_MASK(b)) {
                    button(b, false);
                }
            }
            break;
        case K::mark:
            sbs_mark(in.code);
            break;
        case K::resync:
            m_sbs_resync = true;
            break;
        }
    }
}

// A worldspace the importer folded into the one here (a city's walls in Tamriel) is the same ground.
bool App::sbs_same_space(const std::string& world) const {
    if (!m_space) {
        return false;
    }
    if (world == path::to_lower(m_space->edid())) {
        return true;
    }
    for (const auto& r : m_space->world().regions) {
        if (path::to_lower(r.edid) == world) {
            return true;
        }
    }
    return false;
}

// Keeps the player beside the game's. Input does the moving; this only steps in when the two have come apart, and
// says so in the timeline.
void App::sbs_follow() {
    const MirrorPose p = m_sbs->pose();
    const double now = m_sbs->now();
    m_sbs_drift = -1;
    m_sbs_look_drift = -1;
    if (!p.valid || !p.settled || m_loading || !m_space) {
        m_sbs_apart_since = m_sbs_look_since = -1;
        return;
    }
    if (!m_walk || !m_player) {
        place_player(p.position * units::k_skyrim_unit_m, p.heading, p.pitch);
    }
    Pose pose;
    pose.position_units = p.position;
    pose.heading_deg = p.heading;
    pose.pitch_deg = p.pitch;
    if (!sbs_same_space(p.world)) {
        const bool known = std::filesystem::exists(m_opt.cache / "worlds" / (p.world + ".osw")) ||
                           std::filesystem::exists(m_opt.cache / "interiors" / (p.world + ".osw"));
        if (p.world != m_sbs_missing_world) {
            m_sbs->event("follow_space", std::format("\"to\":\"{}\",\"from\":\"{}\",\"known\":{}", json_escape(p.world),
                                                     json_escape(m_space->edid()), known));
            m_sbs_missing_world = p.world;
        }
        if (known) {
            travel(p.world, pose);
            m_sbs_missing_world.clear();
        }
        return;
    }
    m_sbs_missing_world.clear();

    const glm::vec3 here = m_player->state().position / units::k_skyrim_unit_m;
    const glm::vec3 there = p.position + p.velocity * float(std::min(now - p.t, 0.1));
    const float drift = glm::length(here - there);
    const float heading = glm::degrees(m_player->yaw()), pitch = -glm::degrees(m_player->pitch());
    const float look = std::max(angle_between(heading, p.heading), std::abs(pitch - p.pitch));
    m_sbs_drift = drift;
    m_sbs_look_drift = look;
    m_sbs_apart_since = drift < k_drift_snap ? -1 : m_sbs_apart_since < 0 ? now : m_sbs_apart_since;
    m_sbs_look_since = look < k_look_snap ? -1 : m_sbs_look_since < 0 ? now : m_sbs_look_since;
    const bool resting = drift >= k_rest_snap && glm::length(p.velocity) < k_rest_speed &&
                         glm::length(m_player->state().velocity) / units::k_skyrim_unit_m < k_rest_speed;
    m_sbs_rest_since = !resting ? -1 : m_sbs_rest_since < 0 ? now : m_sbs_rest_since;
    const bool far = m_sbs_apart_since >= 0 && (now - m_sbs_apart_since >= k_drift_hold || drift >= k_drift_snap_at_once);
    const bool rested = m_sbs_rest_since >= 0 && now - m_sbs_rest_since >= k_rest_hold;
    if (far || rested || m_sbs_resync) {
        m_sbs->event("resync", std::format("\"why\":\"{}\",\"drift\":{:.1f},\"look\":{:.2f},\"from\":{},\"to\":{}",
                                           m_sbs_resync ? "key" : far ? "drift" : "rest", drift, look, vec_json(here), vec_json(p.position)));
        place_player(p.position * units::k_skyrim_unit_m, p.heading, p.pitch);
        m_sbs_apart_since = m_sbs_look_since = m_sbs_rest_since = -1;
    } else if (m_sbs_look_since >= 0 && now - m_sbs_look_since >= k_look_hold) {
        m_sbs->event("relook", std::format("\"look\":{:.2f},\"from\":[{:.2f},{:.2f}],\"to\":[{:.2f},{:.2f}]", look, heading,
                                           pitch, p.heading, p.pitch));
        m_player->set_facing(glm::radians(p.heading), -glm::radians(p.pitch));
        m_sbs_look_since = -1;
    }

    // First or third person: the toggle is mirrored, but the game can change it by itself (a zoom, furniture, a kill).
    const bool want_first = p.view == 0, want_third = p.view == 8 || p.view == 9;
    if ((want_first && m_player->view() != View::first_person) || (want_third && m_player->view() != View::third_person)) {
        if (m_sbs_view_since < 0) {
            m_sbs_view_since = now;
        } else if (now - m_sbs_view_since > 0.5) {
            m_sbs->event("view", std::format("\"to\":\"{}\"", want_first ? "first" : "third"));
            m_player->set_view(want_first ? View::first_person : View::third_person);
            m_sbs_view_since = -1;
        }
    } else {
        m_sbs_view_since = -1;
    }

    // The clock and the sky are the game's: the same hour and weather, or nothing on screen compares.
    if (std::abs(std::remainder(m_clock.hour() - p.hour, 24.0f)) > k_hour_drift || m_sbs_resync) {
        m_sbs->event("clock", std::format("\"from\":{:.3f},\"to\":{:.3f}", m_clock.hour(), p.hour));
        apply_time_of_day(p.hour);
    }
    if (p.weather && p.weather != m_sbs_weather && !p.interior) {
        m_sbs->event("weather", std::format("\"to\":\"{:08X}\"", p.weather));
        m_sbs_weather = p.weather;
        set_setting("weather", std::format("{:X}", p.weather));
    }
    m_sbs_resync = false;
}

void App::sbs_record(float dt) {
    if (!m_space) {
        return;
    }
    const double now = m_sbs->now();
    ++m_sbs_frame;
    // A row per world step's worth of time: the game writes one a frame, and at several hundred frames a second
    // here the lookups would drown in rows that say nothing new.
    if (now - m_sbs_last_row >= 1.0 / 120.0) {
        m_sbs_last_row = now;
        const float u = units::k_skyrim_unit_m;
        std::string j = std::format("\"f\":{},\"dt\":{:.5f},\"cpu_ms\":{:.3f},\"gpu_ms\":{:.3f}", m_sbs_frame, dt, m_last_cpu_ms,
                                    m_last_gpu.total_ms);
        if (m_player) {
            const auto& s = m_player->state();
            const auto& st = m_player->status();
            j += std::format(",\"pos\":{},\"yaw\":{:.3f},\"pitch\":{:.3f},\"cam\":{},\"camyaw\":{:.3f},\"campitch\":{:.3f},"
                             "\"view\":\"{}\",\"vel\":{},\"move\":{},\"ground\":{},\"sneak\":{},\"sprint\":{},\"swim\":{},"
                             "\"hp\":{:.2f},\"sp\":{:.2f}",
                             vec_json(s.position / u), glm::degrees(m_player->yaw()), -glm::degrees(m_player->pitch()),
                             vec_json(m_camera.position / u), m_camera.skyrim_heading_deg(), m_camera.skyrim_pitch_deg(),
                             m_player->view() == View::first_person ? "first" : "third", vec_json(s.velocity / u), int(s.state),
                             int(s.on_ground), int(st.sneaking), int(st.sprinting), int(st.swimming), st.health, st.stamina);
        }
        j += std::format(",\"world\":\"{}\",\"hour\":{:.4f},\"wx\":\"{:08X}\",\"drift\":{:.1f},\"lookdrift\":{:.2f}",
                         json_escape(m_space->edid()), m_clock.hour(), m_sky ? m_sky->weather() : 0u, m_sbs_drift, m_sbs_look_drift);
        m_sbs->frame(j);
    }
    if (now - m_sbs_last_world < k_world_period) {
        return;
    }
    m_sbs_last_world = now;
    std::string actors;
    if (ActorSet* set = m_space->actors()) {
        const glm::vec3 feet = m_player ? m_player->state().position : m_camera.position;
        const float u = units::k_skyrim_unit_m;
        for (const auto& a : set->list()) {
            if (glm::distance(a.position, feet) / u > 6000.0f) {
                continue; // the game's nearest-actors table doesn't reach further either
            }
            std::string clips;
            for (const auto& c : a.clips) {
                clips += std::format("{}\"{}\"", clips.empty() ? "" : ",", json_escape(c));
            }
            actors += std::format("{}{{\"ref\":\"{:08X}\",\"base\":\"{:08X}\",\"pos\":{},\"dist\":{:.0f},\"clips\":[{}]}}",
                                  actors.empty() ? "" : ",", a.ref, a.npc, vec_json(a.position / u), glm::distance(a.position, feet) / u, clips);
        }
    }
    std::string menus;
    for (const std::string& m : m_ui->open_menus()) {
        menus += (menus.empty() ? "" : ",") + std::format("\"{}\"", json_escape(m));
    }
    if (menus != m_sbs_menus) {
        m_sbs->event("menus", std::format("\"open\":[{}]", menus));
        m_sbs_menus = menus;
    }
    const std::string location = current_location();
    if (location != m_sbs_location) {
        m_sbs->event("location", std::format("\"name\":\"{}\",\"world\":\"{}\"", json_escape(location), json_escape(m_space->edid())));
        m_sbs_location = location;
    }
    if (m_crosshair_ref != m_sbs_crosshair) {
        m_sbs->event("crosshair", std::format("\"ref\":\"{:08X}\"", m_crosshair_ref));
        m_sbs_crosshair = m_crosshair_ref;
    }
    m_sbs->world(std::format("\"xhair\":\"{:08X}\",\"actors\":[{}],\"menus\":[{}],\"audio\":{}", m_crosshair_ref, actors, menus,
                             m_audio ? m_audio->state_json() : std::string("null")));
    m_sbs->flush();
}

// The mark key: everything this engine can say about this moment, in the session's marks folder.
void App::sbs_mark(uint32_t number) {
    const auto dir = m_sbs->dir() / "marks";
    std::filesystem::create_directories(dir);
    const std::string stem = std::format("{:03}-spud", number);
    std::ofstream(dir / (stem + ".json")) << state_json();
    m_sbs->event("mark", std::format("\"n\":{}", number));
    if (m_executor) {
        const std::string line = "screenshot_ui " + (dir / (stem + ".png")).generic_string();
        if (auto c = parse_command(line, "mark", 0)) {
            m_executor->push(std::move(*c), [](bool, const std::string&) {});
        }
    }
}

} // namespace spud::engine
