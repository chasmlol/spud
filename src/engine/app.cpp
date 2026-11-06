#include "core/sampler.hpp"
#include "core/game_random.hpp"
#include "engine/app.hpp"

#include "engine/app_internal.hpp"
#include "engine/graphics_settings.hpp"
#include "engine/test_cache.hpp"

#include "core/log.hpp"
#include "core/platform.hpp"
#include "core/trace.hpp"
#include "core/units.hpp"

#include <bit>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace spud::engine {

namespace {

const char* k_usage = R"(usage: spud [options]
  --cache <dir>            import cache (default: %LOCALAPPDATA%\spud\cache\skyrim)
  --saves <dir>            saved games (default: Documents\My Games\spud\Saves)
  --world <edid>           worldspace (default: tamriel; "none" = no world)
  --cell <x> <y>           start above this cell
  --pos <x> <y> <z>        start position in Skyrim units (as getpos); z "+N" = N units above terrain
  --yaw <deg>              heading, clockwise from north (getangle z)
  --pitch <deg>            pitch, positive looks down (getangle x)
  --draw-distance <cells>  reference streaming radius (default 12)
  --width <w> --height <h> window / render size
  --headless               no window: render offscreen on the GPU (with --run, --start, --bench, --control)
  --run <script>           run a command script and exit (docs/AUTOMATION.md)
  --start <script>         run a command script, then keep the session going as an interactive one
  --control <port>         serve the command language on 127.0.0.1:<port> (newline-delimited JSON)
  --startup <script>       run a command script in the interactive session, which then carries on
  --screenshot <out.png>   wait until streaming is idle, render --frames N more (default 5), save, exit
  --msaa <n>               scene multisampling: 1, 2, 4 (default) or 8; 1 for parity captures
  --time <hour>            game hour (exterior lighting from the climate and weather)
  --weather <id>           force a weather: FormID in hex (as the console's fw) or EditorID
  --timescale <s>          game seconds per real second (default: the TimeScale global, 20; 0 freezes the hour)
  --fov <deg>              horizontal field of view, as the game's fov command
  --frames <n>
  --bench <file.bench> --bench-out <result.json>   run a perf scenario
  --validation             enable Vulkan validation layers
  --play-clock             headless: step the world on play's 60 Hz tick rather than every frame
  --no-vsync               present without vsync (immediate / mailbox)
  --rotation-mode <0-4>    debug: rebuild reference rotations from euler angles
  --show-disabled          draw initially-disabled references
  --walk                   start on foot, feet at --pos (default: noclip camera)
  --audio-cache <dir>      where audio/sound.osd and the sound files are (default: --cache, else
                           %LOCALAPPDATA%\spud\cache\audio when only that has them)
  --sound / --mute         play sound (default when windowed) / keep the speakers silent (default headless)
  --make-test-cache <dir>  write a small synthetic cache and exit
  --menu / --no-menu       start in the main menu or in the world (default: the menu when run interactively
                           without --world, --cell or --pos)
  --hud-recording / --no-hud-recording
                           the HUD replays the game's recorded HUD, or shows only live data (default: replay in
                           scripted runs, never for a player at the window)
  --windowed               a window this run, whatever the saved window mode
  --window-pos <x> <y>     put the window's client area at this desktop position (windowed)
  --mirror <port>          take input and pose from another process
                           --sbs-dir, --sbs-t0: its telemetry
  --settings <file>        read and write this settings file (default: %LOCALAPPDATA%\spud\settings.ini,
                           interactive sessions only)
controls: N switches between walking and the noclip camera, Esc pause menu, ~ console, F1 debug UI
  noclip: hold right mouse (or Tab) to look, WASD move, Space/E up, Ctrl/Q down, Shift fast, wheel speed
  walk:   Skyrim's keys (controlmap.txt): mouse look, WASD, Space jump, Alt sprint, Shift walk,
          Caps Lock always run, C auto-move
)";

bool parse_options(int argc, char** argv, Options& o) {
    auto num = [&](int& i, float& out) {
        if (i + 1 >= argc) {
            return false;
        }
        const char* s = argv[++i];
        if (*s == '+') {
            ++s;
        }
        auto [p, ec] = std::from_chars(s, s + std::strlen(s), out);
        return ec == std::errc() && *p == '\0';
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        float v[3];
        auto path_arg = [&](std::filesystem::path& out) {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };
        bool ok = true;
        if (a == "--cache") {
            ok = path_arg(o.cache);
        } else if (a == "--saves") {
            ok = path_arg(o.saves);
        } else if (a == "--world" && i + 1 < argc) {
            o.world = argv[++i];
            o.world_given = true;
        } else if (a == "--cell") {
            ok = num(i, v[0]) && num(i, v[1]);
            o.cell = std::array<int, 2>{static_cast<int>(v[0]), static_cast<int>(v[1])};
        } else if (a == "--pos") {
            o.pos_z_above_terrain = i + 3 < argc && argv[i + 3][0] == '+';
            ok = num(i, v[0]) && num(i, v[1]) && num(i, v[2]);
            o.pos_units = glm::vec3(v[0], v[1], v[2]);
        } else if (a == "--yaw") {
            ok = num(i, v[0]);
            o.yaw = v[0];
        } else if (a == "--pitch") {
            ok = num(i, v[0]);
            o.pitch = v[0];
        } else if (a == "--draw-distance") {
            ok = num(i, v[0]);
            o.draw_distance = v[0];
        } else if (a == "--time") {
            ok = num(i, v[0]);
            o.time = v[0];
        } else if (a == "--timescale") {
            ok = num(i, v[0]);
            o.time_scale = v[0];
        } else if (a == "--weather" && i + 1 < argc) {
            o.weather = argv[++i];
        } else if (a == "--fov") {
            ok = num(i, v[0]);
            o.fov = v[0];
        } else if (a == "--msaa") {
            ok = num(i, v[0]);
            o.msaa = static_cast<uint32_t>(v[0]);
            o.msaa_given = true;
        } else if (a == "--width") {
            ok = num(i, v[0]);
            o.width = static_cast<uint32_t>(v[0]);
        } else if (a == "--height") {
            ok = num(i, v[0]);
            o.height = static_cast<uint32_t>(v[0]);
        } else if (a == "--screenshot") {
            ok = path_arg(o.screenshot);
        } else if (a == "--frames") {
            ok = num(i, v[0]);
            o.frames = std::max(0, static_cast<int>(v[0]));
        } else if (a == "--play-clock") {
            o.play_clock = true; // a headless session on play's 60 Hz tick, as a window has
        } else if (a == "--validation") {
            o.validation = true;
        } else if (a == "--headless") {
            o.headless = true;
        } else if (a == "--no-vsync") {
            o.vsync = false;
            o.vsync_given = true;
        } else if (a == "--windowed") {
            o.windowed = true;
        } else if (a == "--menu" || a == "--no-menu") {
            o.menu = a == "--menu";
        } else if (a == "--hud-recording" || a == "--no-hud-recording") {
            o.hud_recording = a == "--hud-recording";
        } else if (a == "--settings") {
            ok = path_arg(o.settings);
        } else if (a == "--rotation-mode") {
            ok = num(i, v[0]);
            o.rotation_mode = std::clamp(static_cast<int>(v[0]), 0, 4);
        } else if (a == "--show-disabled") {
            o.show_disabled = true;
        } else if (a == "--walk") {
            o.walk = true;
        } else if (a == "--audio-cache") {
            ok = path_arg(o.audio_cache);
        } else if (a == "--sound") {
            o.sound = 1;
        } else if (a == "--mute") {
            o.sound = 0;
        } else if (a == "--make-test-cache") {
            ok = path_arg(o.make_test_cache);
        } else if (a == "--bench") {
            ok = path_arg(o.bench);
        } else if (a == "--bench-out") {
            ok = path_arg(o.bench_out);
        } else if (a == "--run") {
            ok = path_arg(o.run);
        } else if (a == "--start") {
            ok = path_arg(o.start);
        } else if (a == "--window-pos") {
            ok = num(i, v[0]) && num(i, v[1]);
            o.window_pos = std::array<int, 2>{static_cast<int>(v[0]), static_cast<int>(v[1])};
        } else if (a == "--startup") {
            ok = path_arg(o.startup);
        } else if (a == "--control") {
            ok = num(i, v[0]) && v[0] > 0 && v[0] < 65536;
            o.control_port = static_cast<uint16_t>(v[0]);
        } else if (a == "--mirror") {
            ok = num(i, v[0]) && v[0] > 0 && v[0] < 65536;
            o.mirror_port = static_cast<uint16_t>(v[0]);
        } else if (a == "--sbs-dir") {
            ok = path_arg(o.sbs_dir);
        } else if (a == "--sbs-t0" && i + 1 < argc) {
            o.sbs_qpc0 = std::strtoll(argv[++i], nullptr, 10);
        } else {
            ok = false;
        }
        if (!ok) {
            log::error("unknown or incomplete option '{}'", a);
            return false;
        }
    }
    if (o.cache.empty()) {
        o.cache = platform::default_cache_dir();
    }
    if (o.audio_cache.empty()) {
        // The import cache, when it holds the sounds; otherwise the audio-only cache the audio scenario launchers
        // import into (docs/AUDIO.md), if there is one.
        o.audio_cache = o.cache;
        const auto audio_only = platform::local_app_data() / "spud" / "cache" / "audio";
        if (!std::filesystem::exists(o.cache / "audio" / "sound.osd") &&
            std::filesystem::exists(audio_only / "audio" / "sound.osd")) {
            o.audio_cache = audio_only;
        }
    }
    return true;
}

std::string driver_string(const VkPhysicalDeviceProperties& p) {
    const uint32_t v = p.driverVersion;
    if (p.vendorID == 0x10DE) { // NVIDIA
        return std::format("{}.{}", (v >> 22) & 0x3FF, (v >> 14) & 0xFF);
    }
    return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

} // namespace

App::App(Options options, Clock::time_point start) : m_opt(std::move(options)), m_process_start(start) {
    m_space_settings.draw_distance = m_opt.draw_distance.value_or(12.0f);
    m_space_settings.rotation_mode = static_cast<RotationMode>(m_opt.rotation_mode);
    m_space_settings.show_disabled = m_opt.show_disabled;
    m_render_settings.terrain.full_res_radius =
        std::min(m_space_settings.draw_distance, float(render::Terrain::k_max_full_res_radius));
}

App::~App() {
    m_audio.reset();
    m_others.clear(); // spaces behind doors: before the renderer, like the current one
    m_retired.clear();
    m_space.reset();
    save_user_settings();
    m_focus.attach(nullptr);
    m_map_scene.reset();
    m_ui_scenes.reset();
    m_backdrop.reset();
    m_ui.reset(); // before the renderer: it releases its textures and geometry through the device
    if (m_imgui) {
        ImGui_ImplSDL3_Shutdown();
    }
    m_renderer.reset();
    if (m_imgui) {
        ImGui::DestroyContext();
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
    }
    if (m_sdl) {
        SDL_Quit();
    }
}

void App::init(bool windowed, uint32_t width, uint32_t height, bool resizable, bool imgui,
               render::PresentMode present) {
    if (windowed) {
        if (m_opt.mirror_port) {
            // Beside the game, in the background: a pad reaches this window too, and showing it takes no focus.
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        }
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            throw Error(std::string("SDL_Init: ") + SDL_GetError());
        }
        m_sdl = true;
        SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
        if (resizable) {
            flags |= SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        } else if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
                   mode && uint32_t(mode->w) == width && uint32_t(mode->h) == height) {
            // At the display's native size, borderless fullscreen lets the swapchain flip
            // independently instead of going through desktop composition.
            flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS;
            log::info("window: borderless fullscreen {}x{} @ {:.0f} Hz", mode->w, mode->h, mode->refresh_rate);
        }
        if (m_opt.window_pos) {
            flags |= SDL_WINDOW_HIDDEN; // shown once it is in place, not first at the default spot
        }
        m_window = SDL_CreateWindow("spud", static_cast<int>(width), static_cast<int>(height), flags);
        if (!m_window) {
            throw Error(std::string("SDL_CreateWindow: ") + SDL_GetError());
        }
        if (m_opt.window_pos) {
            SDL_SetWindowPosition(m_window, (*m_opt.window_pos)[0], (*m_opt.window_pos)[1]);
            SDL_ShowWindow(m_window);
        }
        if (imgui) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::StyleColorsDark();
            ImGui_ImplSDL3_InitForVulkan(m_window);
            m_imgui = true;
        }
    } else {
        // Headless frames are presented exactly like windowed ones, to a window that is never shown (no
        // taskbar entry, no focus change). A queue that never presents makes the NVIDIA driver stall
        // ~1.5 ms in every 1024th submit, which play never sees (docs/AUTOMATION.md).
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw Error(std::string("SDL_Init: ") + SDL_GetError());
        }
        m_sdl = true;
        m_window = SDL_CreateWindow("spud (headless)", static_cast<int>(width), static_cast<int>(height),
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_UTILITY);
        if (!m_window) {
            throw Error(std::string("SDL_CreateWindow (hidden): ") + SDL_GetError());
        }
        // Nobody sees the frames, so nothing paces them: vsync would only slow scripts down.
        present = render::PresentMode::no_vsync;
    }
    render::RendererOptions ro;
    ro.validation = m_opt.validation;
    ro.window = m_window;
    ro.hidden_window = !windowed;
    ro.render_width = resizable ? 0 : width; // a resizable window renders at its own size
    ro.render_height = resizable ? 0 : height;
    ro.present_mode = present;
    ro.msaa = m_opt.msaa;
    m_vsync = present == render::PresentMode::vsync;
    ro.cache_root = m_opt.cache;
    const char* base = SDL_GetBasePath();
    ro.shader_dir = std::filesystem::path(base ? base : "") / "shaders";
    // One pipeline cache per build directory. Parallel worktrees build different shaders and, sharing one
    // file, kept overwriting each other's cache, forcing a full recompile (about a minute) on most launches.
    const auto build_key = std::hash<std::string>{}(ro.shader_dir.generic_string());
    ro.pipeline_cache =
        platform::local_app_data() / "spud" / "pipeline_cache" / std::format("{:016x}.bin", build_key);
    ro.imgui = m_imgui;
    m_renderer = std::make_unique<render::Renderer>(ro);
    create_ui(); // every mode, closed unless opened: no UI pass and no cost until then

    audio::AudioOptions ao;
    ao.cache = m_opt.audio_cache;
    ao.silent = m_opt.sound == 0 || (m_opt.sound < 0 && !windowed);
    m_audio = std::make_unique<audio::AudioSystem>(ao);
}

void App::place_camera() {
    const float cs = m_space ? m_space->cell_size() : units::k_cell_size_m;
    if (m_opt.pos_units) {
        m_camera.position = *m_opt.pos_units * units::k_skyrim_unit_m;
        if (m_opt.pos_z_above_terrain && m_space) {
            const auto h = m_space->terrain_height(m_camera.position.x, m_camera.position.y);
            m_camera.position.z += h.value_or(m_space->world().default_water_height);
        }
    } else {
        glm::vec2 xy = m_space ? m_space->centre() : glm::vec2(0);
        if (m_opt.cell) {
            xy = glm::vec2((*m_opt.cell)[0] + 0.5f, (*m_opt.cell)[1] + 0.5f) * cs;
        }
        float ground = 0;
        if (m_space) {
            ground = m_space->terrain_height(xy.x, xy.y)
                         .value_or(std::max(m_space->world().default_land_height,
                                            m_space->world().default_water_height));
        }
        m_camera.position = glm::vec3(xy, ground + 40.0f);
    }
    m_camera.set_skyrim_angles(m_opt.yaw.value_or(0.0f), m_opt.pitch.value_or(15.0f));
}

std::array<float, k_zone_count> App::run_frame(float real_time, ImDrawData* ui, std::optional<float> previous_time) {
    std::array<float, k_zone_count> zones{};
    // Main::Update's frame timers: the world's time stands still while a menu pauses the game. The menus
    // keep their own (m_ui_dt), and the Mist Menu's scene is no world.
    const float real_dt = std::max(real_time - (previous_time ? *previous_time : m_last_real_time), 0.0f);
    if (m_ui && m_ui->pausing_menus() > 0 && !m_ui->backdrop()) {
        m_paused_time += std::max(real_time - m_last_real_time, 0.0f);
    }
    m_last_real_time = real_time;
    const float time = real_time - m_paused_time;
    if (previous_time) {
        *previous_time -= m_paused_time;
    }
    if (std::exchange(m_load_go, false) && m_load) {
        finish_load(); // between frames too, and it draws some of its own
    }
    if (!m_load_request.empty()) {
        load_game(std::exchange(m_load_request, {})); // between frames: it can replace the space
    }
    trace::begin_frame(m_renderer->frame_number() + 1);
    // The world steps first, while the GPU finishes the last frame: nothing here needs this frame's slot. Waiting for it
    // first would put the two end to end, and every tick frame would pay for both.
    const auto t1 = Clock::now();
    if (m_world_still && m_loading) {
        update_ai(0.0f); // who is where: the actors the loading screen waits for
    }
    // Two clocks. What is cheap steps on every frame's time, so it needs no smoothing: doors, projectiles, the sky, lights
    // and effects. What is heavy steps on the world's 60 Hz tick, and frames between show it on its way.
    const float frame_dt = m_time_rate * std::max(time - (previous_time ? *previous_time : m_last_time), 0.0f);
    const bool paused = m_ui && m_ui->pausing_menus() > 0 && !m_ui->backdrop();
    const float tick_dt = m_world_still || (paused && m_fixed_step > 0.0) ? 0.0f : m_fixed_step > 0.0 ? m_time_rate * float(m_fixed_step) : frame_dt;
    if (ActorSet* actors = m_space && !m_world_still ? m_space->actors() : nullptr) {
        actors->set_ahead(m_fixed_step > 0.0);
        actors->join(); // what steps next reads and writes them
    }
    {
        SPUD_TRACE("doors");
        update_doors(time); // may carry the camera into another space
    }
    if (!m_world_still) {
        {
            SPUD_TRACE("crosshair");
            update_crosshair();
        }
        update_encumbrance(tick_dt);
        {
            SPUD_TRACE("ai");
            update_ai(tick_dt);
            update_carts(tick_dt);
        }
        SPUD_TRACE("combat");
        update_combat(tick_dt);
    }
    if (m_world_still && m_fixed_step > 0.0 && !m_ai_stepped && m_shown_fraction >= 0.5f && !paused && !m_loading) {
        SPUD_TRACE("ai");
        if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
            actors->join();
        }
        update_ai(float(m_fixed_step), true);
    }
    {
        SPUD_TRACE("projectiles");
        update_projectiles(frame_dt);
    }
    if (!m_world_still) {
        update_mount(tick_dt);
    }
    if (m_space) {
        SPUD_TRACE("space update");
        const bool interior = m_space->interior();
        // Main::Update: the calendar runs on the frame's time unless a menu pauses the game. A jump of the clock (a
        // scripted `set animation_time`, a stall) is not elapsed play.
        if (const float dt = std::max(time - m_last_time, 0.0f); dt <= 1.0f) {
            if (!(m_ui && m_ui->pausing_menus() > 0)) {
                m_clock.update(dt);
            }
            m_play_seconds += dt; // real time, menus included
        }
        m_audio_env.game_hour = m_clock.hour();
        if (m_sky) {
            // Interiors keep the climate last in force; a Show Sky interior takes the weather of its own region.
            if (!interior) {
                m_sky_world = m_space->edid();
            }
            m_sky->set_world(m_sky_world);
            if (m_sky_weather_pending && m_sky->has_climate()) {
                m_sky->force_weather(m_sky_weather);
                m_sky_weather_pending = false;
            }
            const uint32_t flags = m_space->world().flags;
            const int mode = !interior || (flags & osw::world_use_sky_lighting) ? 3 : flags & osw::world_show_sky ? 2 : 1;
            m_sky->set_interior(interior, mode, interior ? interior_sky_weather(*m_space) : 0);
            m_sky->set_ui_blur(m_ui && m_ui->blur_count() > 0);
            m_sky->set_world_map(m_map_open && !interior);
            const glm::vec3 cam_units = m_camera.position / units::k_skyrim_unit_m;
            const float last = previous_time ? *previous_time : m_last_time;
            m_sky->update(m_clock, std::max(time - m_last_time, 0.0f), std::max(time - last, 0.0f), cam_units,
                          m_camera.yaw, m_camera.pitch);
        }
        {
            SPUD_TRACE("environment");
            evaluate_environment(*m_space, std::max(time - m_last_time, 0.0f));
        }
        update_race_camera(float(m_ui_dt)); // the menu pauses the world, not this move (its clock is real milliseconds)
        if (m_physics) {
            SPUD_TRACE("moved refs");
            update_moved_refs(frame_dt);
        }
        if (!m_world_still) {
            {
                SPUD_TRACE("world sim");
                update_world_sim();
            }
            {
                SPUD_TRACE("destruction");
                update_destruction(tick_dt);
            }
            {
                SPUD_TRACE("activations");
                update_delayed_activations(tick_dt);
            }
            {
                SPUD_TRACE("traps");
                update_traps(tick_dt);
            }
            if (ActorSet* actors = m_space->actors()) {
                if (m_game) {
                    actors->set_disabled_test([this](uint32_t ref) { return m_game->ref_disabled(ref); });
                    std::array<std::array<float, 4>, 5> wander;
                    constexpr std::array k_emotions = {"Angry", "Fear", "Happy", "Sad", "Neutral"};
                    constexpr float k_image[5][4] = {{-0.05f, 0.15f, -0.1f, 0.1f},
                                                     {0.0f, 0.0f, -0.5f, 0.5f},
                                                     {-0.05f, 0.15f, -0.2f, 0.2f},
                                                     {-0.15f, -0.05f, -0.4f, 0.4f},
                                                     {-0.05f, 0.15f, -0.2f, 0.2f}};
                    for (size_t e = 0; e < wander.size(); ++e) {
                        size_t i = 0;
                        for (const char* angle : {"Pitch", "Heading"}) {
                            for (const char* end : {"Min", "Max"}) {
                                const std::string name = std::format("fEye{}{}OffsetEmotion{}", angle, end, k_emotions[e]);
                                wander[e][i] = m_game->settings().number(name.c_str(), k_image[e][i]);
                                ++i;
                            }
                        }
                    }
                    actors->set_eye_wander(wander);
                }
                // Animated objects stand in for their references: the placed model and its fixed body step aside.
                const auto changes = actors->take_object_changes();
                if (!changes.empty()) {
                    std::unordered_map<uint32_t, bool> hidden;
                    for (const ActorSet::ObjectChange& c : changes) {
                        hidden[c.ref] = c.live;
                    }
                    m_space->set_refs_hidden(hidden);
                }
                for (const ActorSet::ObjectChange& c : changes) {
                    if (m_physics) {
                        m_physics->set_ref_solid(c.ref, !c.live);
                        c.live ? m_physics->add_animated(c.ref, c.collision, c.scale) : m_physics->remove_animated(c.ref);
                    }
                }
                for (ActorSet::ObjectMove& m : actors->take_object_moves()) {
                    if (m_physics) {
                        m_physics->move_animated(m.ref, std::move(m.nodes));
                    }
                }
                update_actor_bodies(*actors);
                const bool walking = m_walk && m_player != nullptr;
                actors->set_time(tick_dt);
                actors->set_grid_radius(m_grids_to_load / 2);
                actors->set_hold(m_hold_actors);
                actors->set_physics(m_physics.get());
                ActorSet::PlayerBody body;
                if (walking) {
                    // The Speed and Direction channels: units per second, and turns clockwise from ahead.
                    const glm::vec3 v = m_player->state().velocity / units::k_skyrim_unit_m;
                    const game::Agent* dragon = m_mount.dragon ? m_game->ai().agent(m_mount.ref) : nullptr;
                    const CartTether* cart = cart_of(game::k_player_ref);
                    const float yaw = cart ? cart->heading : dragon ? dragon->heading : m_mount.ref ? m_player->mount_heading() : m_player->yaw();
                    const float ahead = v.x * std::sin(yaw) + v.y * std::cos(yaw);
                    const float right = v.x * std::cos(yaw) - v.y * std::sin(yaw);
                    body.dead = player_dead();
                    body.third_person = m_player->view() == View::third_person || m_race_camera_on || body.dead;
                    body.first_person = !body.third_person && m_first_person_geometry;
                    if (cart) { // a seat is the third-person graph's, and the view its head
                        body.third_person = true;
                        body.first_person = false;
                    }
                    // The game draws the arms from a camera on Cam1, without the lag (the game
                    //); here they share the view, so the body takes the lag with it.
                    body.feet = m_player->feet() + m_player->first_person_lag();
                    body.heading = yaw;
                    body.pitch = -glm::degrees(m_player->pitch());
                    body.speed = std::sqrt(v.x * v.x + v.y * v.y);
                    // The game's walk/run blend, on the speeds of the forward sector.
                    const auto& tuning = m_player->tuning();
                    const auto speeds = movement::actor_speeds(tuning.walk_run, tuning.body, tuning.settings);
                    const float walk = speeds.forward[0], run = speeds.forward[1];
                    body.run_blend = walk >= run         ? 0.5f
                                     : body.speed < walk ? 0.0f
                                     : body.speed < run  ? (body.speed - walk) / (run - walk)
                                                         : body.speed / run;
                    body.sneaking = m_player->status().sneaking;
                    body.sprinting = m_player->status().graph_sprint;
                    // The turn of this frame, as the actor's rotation request: negated like the game's channel.
                    const float turn = std::remainder(yaw - m_last_player_yaw, 6.2831855f);
                    body.turn_delta = -glm::degrees(turn);
                    m_last_player_yaw = yaw;
                    // The graph hears what the player asks for, not the body's velocity: a reversal in the air snaps
                    // while the velocity is still swinging round (measured: Speed 342 at 117 u/s), and a stop keeps
                    // the last direction.
                    const glm::vec2 asked = m_player->status().asked;
                    if (!m_mount.ref) {
                        body.speed = glm::length(asked);
                    }
                    const bool steering = m_mount.ref ? right != 0.0f || ahead != 0.0f : asked != glm::vec2(0.0f);
                    if (steering) {
                        const float turns = (m_mount.ref ? std::atan2(right, ahead) : std::atan2(asked.x, asked.y)) / 6.2831855f;
                        m_body_direction = turns < 0.0f ? turns + 1.0f : turns;
                    }
                    body.direction = m_body_direction;
                    if (m_mount.ref) {
                        // The mount goes where the character is, and the rider sits on its saddle wherever that is now.
                        ActorSet::PlayerBody horse = body;
                        horse.third_person = horse.first_person = false;
                        if (!m_mount.dragon) {
                            actors->drive(m_mount.ref, horse);
                        }
                        // Riding, the rider's root is on the saddle; the paired mount and dismount clips start from the
                        // spot at the flank, and bring the rider from it to the saddle and back themselves.
                        const float c = std::cos(yaw), s = std::sin(yaw);
                        if (m_mount.stage == MountStage::riding) {
                            const glm::vec3 saddle = actors->node_offset(m_mount.ref, m_mount.dragon ? "npc saddlebone" : "saddlebone");
                            body.feet += glm::vec3(saddle.x * c + saddle.y * s, -saddle.x * s + saddle.y * c, saddle.z);
                            const float head = actors->node_offset(ActorSet::k_player_body, "npc head [head]").z;
                            m_player->tuning().eye_height = saddle.z + head; // first person sees from the rider's head
                        } else {
                            const auto& o = m_mount.data.mount_offset;
                            body.feet += glm::vec3(o[0] * c + o[1] * s, -o[0] * s + o[1] * c, o[2]) * units::k_skyrim_unit_m;
                        }
                        body.third_person = true;
                        body.first_person = false;
                        actors->set_player_head_hidden(m_player->view() == View::first_person);
                    }
                }
                if (!m_mount.ref) {
                    actors->set_player_head_hidden(false);
                }
                {
                    SPUD_TRACE("player body");
                    actors->set_player(body);
                    if (walking) {
                        // Each actor update: the active graph's mode, and the motion of its clips.
                        const uint32_t graph = ActorSet::k_player_body;
                        const bool driven = actors->get_bool(graph, "bAllowRotation") || actors->get_bool(graph, "bAnimationDriven");
                        const glm::vec3 clip = actors->take_motion(graph);
                        int32_t state = 0;
                        actors->get_int(graph, "iState", state);
                        m_player->set_graph(driven, tick_dt > 0.0f ? glm::vec2(clip) / tick_dt : glm::vec2(0.0f), state);
                    }
                }
            }
            for (auto& o : m_others) {
                if (ActorSet* actors = o->space ? o->space->actors() : nullptr) {
                    actors->set_player({}); // the body stays with the space the player is in
                }
            }
        }
        if (ActorSet* actors = m_space->actors()) {
            if (m_world_still && m_loading) {
                actors->set_time(0.0f); // the world stands, and actors come in behind the loading screen
            }
            actors->set_still(m_world_still && !m_loading);
            if (m_fixed_step > 0.0 && !m_world_still && m_actor_data) {
                m_actor_data->clear_skin_jobs(); // this tick poses everyone anew
            }
        }
        m_space->set_actor_blend(m_fixed_step > 0.0 ? (m_world_still ? m_shown_fraction : m_tick_blend) : 1.0f);
        if (ActorSet* actors = m_walk && m_player ? m_space->actors() : nullptr) {
            // Both on the view's clock, not the world tick's: on two clocks the body shakes against the camera. A rider
            // sits where the mount puts them.
            actors->hold_player(m_player->feet() + m_player->first_person_lag(), m_player->feet(), m_player->yaw(),
                                m_player->view() == View::third_person && !m_mount.ref);
        }
        {
            SPUD_TRACE("space step");
            m_space->update(m_camera.position, m_camera.forward(), m_space_settings, m_render_settings.terrain);
        }
        if (ActorSet* actors = m_space->actors()) {
            actors->set_still(false); // only for that call: a door can swap this space out before the next
        }
        if (!m_world_still) {
            SPUD_TRACE("actor events");
            process_actor_events();
        }
        if (m_ref_effects) {
            SPUD_TRACE("ref effects");
            m_ref_effects->update(frame_dt, m_space.get(), m_world_fx.get(), m_vfx.get());
        }
        if (m_sky) {
            // Region emittance: the region weather's Effect Lighting colour.
            for (uint32_t w : m_space->emittance_weathers()) {
                if (auto c = m_sky->weather_colour(w, osa::colour_effect_lighting)) {
                    m_space->set_emittance(w, *c);
                }
            }
        }
        {
            SPUD_TRACE("lights");
            m_space->update_lights(m_camera.position, time, m_space_settings);
        }
    }
    update_audio();
    // Scripts, quests and saves follow the rest of the tick by a frame on play's clock: in the same order, a tick's work is
    // the same, and no one frame carries all of it.
    if (m_fixed_step > 0.0) {
        if (m_tick_tail) {
            m_tick_tail = false;
            run_tick_tail(m_tick_tail_dt, real_dt);
        }
        if (!m_world_still) {
            m_tick_tail = true;
            m_tick_tail_dt = tick_dt;
        }
    } else if (!m_world_still) {
        run_tick_tail(tick_dt, real_dt);
    }
    const auto stepped = Clock::now();
    zones[zone_streaming] = static_cast<float>(ms_between(t1, stepped));
    {
        SPUD_TRACE("wait_frame");
        m_renderer->wait_frame();
    }
    const auto t2 = Clock::now();
    zones[zone_wait] = static_cast<float>(ms_between(stepped, t2));
    {
        SPUD_TRACE("begin_frame");
        m_renderer->begin_frame();
    }

    const render::View view = map_view().value_or(m_camera.view(aspect()));
    m_space_settings.pixels_per_radian =
        float(m_renderer->render_extent().height) / (2.0f * std::tan(view.fov_y * 0.5f));
    m_render_settings.terrain.pixels_per_radian = m_space_settings.pixels_per_radian;
    m_space_views.clear();
    // While the Mist Menu is open (main menu, loading screens) its backdrop is the only scene.
    if (m_space && !(m_ui && m_ui->backdrop())) {
        SPUD_TRACE("build_draws");
        m_space->build_draws(view, m_space_settings);
        pick_portal(view);
        if (!m_precip_occlusion) {
            m_precip_occlusion = std::make_unique<PrecipOcclusion>(*m_renderer);
        }
        m_prepass = {};
        if (m_space->environment().sky.exterior) {
            m_prepass = m_precip_occlusion->frame(*m_space, m_render_settings, view, m_space->environment().sky);
        }
        m_space_views.push_back(m_space->space_view());
        if (m_water_system) {
            if (ActorSet* actors = m_world_still ? nullptr : m_space->actors()) {
                for (const ActorSet::Visual* v : actors->visuals()) {
                    if (v->ref != game::k_player_ref) {
                        m_water_system->wade(v->ref, v->position / units::k_skyrim_unit_m);
                    }
                }
            }
            m_water_system->set_lights(m_space->point_lights(), view.position);
            const glm::vec3 multiplier = m_sky ? m_sky->water_multiplier() : glm::vec3(1.0f);
            // The flow atlas is 11 cells, the user's uGridsToLoad in the captures; water is detailed that far.
            m_water_system->update(m_space->world(), view, m_space->environment(), multiplier,
                                   m_sky ? m_sky->sun_glare() : 0.0f, 11, time, m_sky ? m_sky->hour() * 3600.0f : 0.0f,
                                   std::max(time - m_water_time, 0.0f), m_walk && m_player ? m_player->feet() : m_camera.position,
                                   m_walk && m_player && m_player->status().submersion > 0.0f);
            m_space->set_rain_ripples(m_water_system->ripple_places(), m_sky ? m_sky->rain_ripples() : 0.0f);
            if (m_world_fx) {
                // A splash where the player enters the water.
                const bool swimming = m_walk && m_player && m_player->status().swimming;
                if (swimming && !m_was_swimming) {
                    m_world_fx->spawn("meshes/effects/watersplash.osm", m_player->feet(), 0.0f);
                    if (m_water_system) {
                        m_water_system->add_ripple(glm::vec2(m_player->feet()) / units::k_skyrim_unit_m);
                    }
                }
                m_was_swimming = swimming;
                m_world_fx->update(m_space->world(), view, std::max(time - m_water_time, 0.0f));
                update_object_sequences();
                m_space_views.back().particles = m_world_fx->frame();
                if (m_vfx) {
                    m_vfx->update(view, std::max(time - m_water_time, 0.0f));
                    m_space_views.back().decals = m_vfx->decals();
                }
                m_fx_shapes.clear();
                for (const WorldFx::Shape& s : m_world_fx->shapes()) {
                    m_fx_shapes.push_back({s.model, s.submesh, s.rotation, s.position, s.base_color, s.uv, s.animated});
                }
                m_space->set_fx_shapes(m_fx_shapes);
            }
            m_water_time = time;
            m_space_views.back().water = m_water_system->frame();
            if (m_sky) {
                m_sky->set_water_image_space(m_water_system->image_space());
            }
        }
        // The sun's (or moon's) shadow cascades for this view and their casters.
        const render::Environment& env = m_space->environment();
        const glm::vec3 shadow_dir =
            m_shadow_direction.update(env.sun_dir, view.position, std::max(time - m_shadow_time, 0.0f),
                                      m_render_settings.shadows.static_update_scale);
        m_shadow_time = time;
        const bool indoors = m_space->interior();
        m_sun_shadows = indoors || (m_space->world().flags & osw::world_no_sky)
                            ? render::SunShadows{}
                            : render::compute_sun_shadows(view, shadow_dir, env.near_units, env.far_units,
                                                          m_render_settings.shadows, false);
        hold_far_cascade(m_sun_shadows, view, shadow_dir, m_space.get());
        m_space->build_shadow_draws(view, m_sun_shadows, m_render_settings.shadows, m_space_settings);
        const float fov_h = 2.0f * std::atan(std::tan(view.fov_y * 0.5f) * view.aspect);
        m_space->build_shadow_lights(view, m_sun_shadows, m_render_settings.shadows, glm::degrees(fov_h) / 80.0f, indoors,
                                     m_space_settings);
        render::SpaceView& sv = m_space_views.back();
        sv.sun_shadows = &m_sun_shadows;
        for (uint32_t c = 0; c < render::k_max_cascades; ++c) {
            sv.shadow_draws[c] = &m_space->shadow_draws(c);
        }
        sv.shadow_lights = m_space->shadow_lights();
        for (uint32_t i = 0; i < render::k_max_shadow_lights; ++i) {
            sv.shadow_light_draws[i] = &m_space->shadow_light_draws(i);
        }
        if (m_map_open && m_map_scene && m_map_scene->active() && !m_space->interior()) {
            m_map_scene->apply(sv, view);
        }
        SPUD_TRACE("portal");
        prepare_portal(view, time);
    } else {
        m_portal_door = -1;
        m_prepass = {}; // it points into a space a load may have just replaced
    }
    const auto t3 = Clock::now();
    zones[zone_cull] = static_cast<float>(ms_between(t2, t3));

    render::FrameInput input;
    input.view = &view;
    input.spaces = m_space_views;
    input.prepass = m_prepass;
    input.portal = m_portal_door >= 0 ? &m_portal_input : nullptr;
    if (m_vfx) {
        input.screen_blood = m_vfx->splats();
        std::ranges::copy(m_vfx->splat_textures(), input.screen_blood_textures);
    }
    if (m_actor_data) {
        input.skin = m_actor_data->skin_frame(!m_world_still, m_world_still ? m_shown_fraction : m_tick_blend);
    }
    input.settings = m_render_settings;
    // The cloud bank's controllers run on the map's own timer, through the pause.
    input.menu_paused = m_ui && m_ui->pausing_menus() > 0 && !m_ui->backdrop();
    input.time = m_map_open ? real_time : time;
    input.previous_time = m_map_open ? real_time - real_dt : previous_time ? *previous_time : m_last_time;
    m_last_time = time;
    input.ui = ui;
    if (m_ui) {
        update_menus();
        m_ui->update(m_ui_dt);
        input.overlay = m_ui->overlay();
        if (const auto scene = m_ui->backdrop_scene()) {
            // The Mist Menu's scene replaces the world (which it hides) with its own camera.
            // The game gives this scene LoadScreenIS, at the main menu too (captures mist-menu, loadscreen).
            if (!m_atmosphere) {
                load_atmosphere();
            }
            constexpr uint32_t k_load_screen_is = 0x000FF9DB;
            if (const osa::ImageSpace* is = m_atmosphere ? m_atmosphere->image_space(k_load_screen_is) : nullptr) {
                m_backdrop->set_image_space(is->hdr);
            }
            m_backdrop->update(*scene, aspect(), float(m_ui_dt));
            m_space_views.clear();
            m_space_views.push_back(m_backdrop->space_view());
            input.view = &m_backdrop->view();
            input.spaces = m_space_views;
            input.portal = nullptr;
        } else {
            m_backdrop->release();
            // The Map Menu's local map, a second view into the target its movie shows (engine/local_map.hpp).
            int32_t place_x = 0, place_y = 0;
            const WorldSim::Place place = m_space && m_world_sim ? player_place(place_x, place_y) : WorldSim::Place{};
            input.offscreen = m_local_map->frame(*m_ui, m_space.get(), m_render_settings, m_camera.position,
                                                 player_position_units(), place.world ? place.world : place.cell,
                                                 m_grids_to_load, m_water_system ? m_water_system->frame() : nullptr);
        }
        // The menus' UI 3D scenes, which the overlay draws at their places among the menus.
        if (m_space) { // none at the Main Menu after a game
            const auto& env = m_space->environment();
            m_ui_scenes->set_world_ambient(env.ambient_sky, env.ambient_ground);
        }
        m_ui_scenes->update(m_ui->ui_scenes(), *m_ui);
        input.ui_scenes = m_ui_scenes->inputs();
    }
    {
        SPUD_TRACE("render");
        m_renderer->render(input);
        // A tick's jobs stand until the next: the frames between pose what's in view again from the same rows.
        if (m_actor_data && !(m_fixed_step > 0.0)) {
            m_actor_data->clear_skin_jobs();
        }
    }
    zones[zone_record] = static_cast<float>(ms_between(t3, Clock::now()));

    m_renderer->take_gpu_timings(m_taken_timings);
    for (const auto& t : m_taken_timings) {
        m_last_gpu = t;
        m_gpu_timings.push_back(t);
    }
    if (m_gpu_timings.size() > 64) {
        m_gpu_timings.erase(m_gpu_timings.begin(), m_gpu_timings.end() - 64);
    }
    m_last_zones = zones;
    if (m_papyrus) {
        m_papyrus->after_render();
    }
    trace::end_frame();
    return zones;
}

void App::serve_control(ControlServer& server, ScriptExecutor& executor) {
    for (auto& r : server.poll()) {
        if (!r.error.empty()) {
            server.respond(r.client, ControlServer::error(r.id, r.error));
            continue;
        }
        try {
            auto cmd = parse_command(r.line, "control", static_cast<int>(++m_control_seq));
            if (!cmd) {
                server.respond(r.client, ControlServer::ok(r.id));
                continue;
            }
            executor.push(std::move(*cmd), [&server, client = r.client, id = r.id](bool ok, const std::string& res) {
                server.respond(client, ok ? ControlServer::ok(id, res) : ControlServer::error(id, res));
            });
        } catch (const std::exception& e) {
            server.respond(r.client, ControlServer::error(r.id, e.what()));
        }
    }
}

int App::run_session() {
    const bool windowed = !m_opt.headless;
    // A player at the window (not an agent on --control): the main menu and the saved settings.
    const bool interactive = windowed && !m_opt.control_port;
    const bool menu = m_opt.menu.value_or(interactive && !m_opt.goes_to_world());
    // --start: read before anything opens, so a bad line is reported without a window coming up.
    std::vector<Command> start_script = m_opt.start.empty() ? std::vector<Command>{} : parse_script(m_opt.start);
    load_user_settings(interactive);
    uint32_t w = m_opt.width ? m_opt.width : 1920;
    uint32_t h = m_opt.height ? m_opt.height : 1080;
    bool vsync = m_opt.vsync;
    if (!m_user_file.empty()) {
        if (!m_opt.width && !m_opt.height) {
            std::sscanf(m_user.get("graphics.resolution").c_str(), "%ux%u", &w, &h);
        }
        vsync = m_opt.vsync_given ? m_opt.vsync : m_user.flag("graphics.vsync");
        if (!m_opt.msaa_given) {
            const auto step = size_t(std::clamp(m_user.number("graphics.antialiasing"), 0.0f, 3.0f));
            m_opt.msaa = k_msaa_steps[step];
        }
    }
    init(windowed, w, h, windowed, windowed, vsync ? render::PresentMode::vsync : render::PresentMode::no_vsync);
    apply_startup_settings();
    if (menu) {
        m_ui_visible = false; // the debug overlay stays out of the menus until F1
        m_ui->open("main_menu");
    } else {
        if (m_opt.world != "none") {
            load_world(m_opt.world);
        }
        m_ui->set_in_world(true);
        m_ui->open("HUD Menu"); // open during play
    }
    place_camera();
    if (m_opt.walk && m_space) {
        start_on_foot();
    }
    apply_sky_options();
    if (interactive && !menu && m_space) {
        start_quests(); // seconds of work: here, as New Game does behind its loading screen, not on the first conversation
    }

    std::unique_ptr<ControlServer> server;
    if (m_opt.control_port) {
        server = std::make_unique<ControlServer>(*m_opt.control_port);
    }
    if (m_opt.mirror_port) {
        sbs_start();
    }
    ScriptExecutor executor(*this);
    m_executor = &executor;

    // The start script runs as --run would run it, from the first frame on; then the session carries on as it
    // began (the player has the window, a control client the socket). A failing line drops the rest of the
    // script; headless, where nobody could take over, it ends the session as --run's failure does.
    bool start_failed = false;
    size_t start_left = start_script.size();
    for (auto& c : start_script) {
        executor.push(std::move(c), [this, &executor, &start_failed, &start_left](bool ok, const std::string& res) {
            --start_left;
            if (!ok) {
                log::error("start: {}; the rest of {} is skipped", res, m_opt.start.string());
                executor.clear();
                start_failed = true;
                start_left = 0;
                return;
            }
            if (!res.empty()) {
                std::fputs(res.c_str(), stdout);
                std::fputc('\n', stdout);
                std::fflush(stdout);
            }
            if (start_left == 0) {
                log::info("start: {} done", m_opt.start.string());
            }
        });
    }
    if (!m_opt.startup.empty()) {
        // Scenario launchers set the scene up this way; the user keeps control, and a
        // failing line is reported without ending the session.
        for (auto& c : parse_script(m_opt.startup)) {
            executor.push(std::move(c), [](bool ok, const std::string& result) {
                if (!ok) {
                    log::error("startup: {}", result);
                } else if (!result.empty()) {
                    log::info("startup: {}", result);
                }
            });
        }
    }

    bool quit = false;
    auto last = Clock::now();
    const auto start = last;
    while (!quit && !m_quit_requested && !(start_failed && !windowed)) {
        const auto frame_start = Clock::now();
        const double frame_ms = ms_between(last, frame_start);
        // While the start script runs, a frame is the fixed 1/60 s step --run uses: its waits are frames whatever
        // the display's refresh, and the menus' animations advance with them. Then the session runs on real time.
        const float dt = start_left > 0 ? 1.0f / 60.0f : static_cast<float>(std::min(0.1, frame_ms / 1000.0));
        m_last_cpu_ms = static_cast<float>(frame_ms);
        m_frame_ms.push_back(static_cast<float>(frame_ms));
        if (m_frame_ms.size() > 240) {
            m_frame_ms.pop_front();
        }
        last = frame_start;

        if (windowed) {
            pump_events(quit, true);
            if (m_sbs) {
                sbs_input(quit);
                sbs_follow();
            }
            if (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }
        } else if (m_window) {
            pump_events(quit, false);
        }
        // Commands are applied here, at one fixed point of the frame.
        if (server) {
            serve_control(*server, executor);
        }
        executor.before_frame(dt);
        if (executor.quit_requested()) {
            break;
        }
        ImDrawData* ui = nullptr;
        if (windowed) {
            CameraInput ci = gather_input();
            if (m_walk) {
                // On foot the mouse turns the player (step_player), not the free camera.
                m_walk_look += glm::vec2(ci.look_dx, ci.look_dy);
                ci.look_dx = ci.look_dy = 0;
            }
            m_camera.update(ci, dt);
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            if (m_ui_visible) {
                draw_ui();
            }
            ImGui::Render();
            ui = ImGui::GetDrawData();
        }
        // Play: the world's heavy half steps at 60 Hz whatever the screen does. Scripts keep a step a frame: their frames
        // mean what they say.
        const bool ticking = (windowed || m_opt.play_clock) && start_left == 0 && executor.idle();
        m_fixed_step = ticking ? k_world_tick : 0.0;
        m_space_settings.fade_seconds = ticking ? float(dt) : -1.0f;
        m_render_settings.spread_water_cube = ticking;
        if (ticking) {
            m_tick_accum += double(dt);
            const bool tick_due = m_tick_accum >= k_world_tick && (!m_loading || m_load_settle == LoadSettle::tick);
            if (tick_due) {
                m_tick_accum = std::min(m_tick_accum - k_world_tick, 3.0 * k_world_tick); // a long stall isn't made up
            }
            m_world_still = !tick_due;
            m_shown_fraction = m_tick_blend = float(std::clamp(m_tick_accum / k_world_tick, 0.0, 1.0));
        } else {
            m_world_still = false;
            m_tick_blend = 1.0f;
        }
        step_player();
        const float update_ms = static_cast<float>(ms_between(frame_start, Clock::now()));
        m_ui_dt = dt;
        run_frame(static_cast<float>(ms_between(start, frame_start) / 1000.0), ui);
        m_last_zones[zone_update] = update_ms;
        if (m_sbs) {
            sbs_record(dt);
        }
        executor.after_frame();
        if (executor.quit_requested()) {
            break;
        }
        if (m_player && windowed && executor.idle()) {
            // The script is done: live controls again. Headless sessions keep the player on scripted ticks
            // only, so a control client sees the same results however fast frames come.
            m_player->set_hold(false);
        }
        if (!windowed && executor.idle() && !streaming_busy()) {
            // An idle headless session doesn't need to spin the GPU.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    m_executor = nullptr;
    sbs_stop();
    if (server) {
        // Let the network thread deliver the last responses (e.g. to "quit").
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return start_failed ? 1 : 0;
}

int App::run_batch(std::vector<Command> commands) {
    const bool windowed = !m_opt.headless;
    const uint32_t w = m_opt.width ? m_opt.width : 1920;
    const uint32_t h = m_opt.height ? m_opt.height : 1080;
    load_user_settings(false); // defaults unless --settings: scripts must not depend on the player's file
    init(windowed, w, h, false, false, m_opt.vsync ? render::PresentMode::vsync : render::PresentMode::no_vsync);
    apply_startup_settings();
    if (m_opt.menu.value_or(false)) {
        m_ui->open("main_menu"); // as an interactive start would; the script drives it from there
    } else {
        // A script that starts by choosing its world shouldn't load the default one first.
        const bool picks_world = !commands.empty() && commands.front().kind == Command::Kind::world;
        if (!picks_world && m_opt.world != "none") {
            load_world(m_opt.world);
        }
        m_ui->set_in_world(true);
        m_ui->open("HUD Menu"); // open during play
    }
    place_camera();
    if (m_opt.walk && m_space) {
        start_on_foot();
    }
    apply_sky_options();

    ScriptExecutor executor(*this);
    m_executor = &executor;
    std::string error;
    for (auto& c : commands) {
        // Inline results ("state -") go to stdout; the first error stops the script.
        executor.push(std::move(c), [&error](bool ok, const std::string& result) {
            if (!ok) {
                error = result;
            } else if (!result.empty()) {
                std::fputs(result.c_str(), stdout);
                std::fputc('\n', stdout);
                std::fflush(stdout);
            }
        });
    }
    auto last = Clock::now();
    try {
        for (;;) {
            const auto frame_start = Clock::now();
            const double dt = std::min(0.1, ms_between(last, frame_start) / 1000.0);
            m_last_cpu_ms = static_cast<float>(ms_between(last, frame_start));
            last = frame_start;
            bool quit = false;
            if (m_window) {
                pump_events(quit, false);
                if (quit) {
                    log::error("run: window closed before the script finished");
                    return 4;
                }
            }
            const bool more = executor.before_frame(dt);
            if (!error.empty()) {
                throw Error(error);
            }
            if (!more || executor.quit_requested() || m_quit_requested) {
                break;
            }
            step_player();
            // Fixed time: screenshots are reproducible (water animation, UI animations). `set animation_time` sets
            // it, e.g. to a game capture's clock; one frame back is a frame of 1/60 s earlier.
            m_ui_dt = 1.0 / 60.0;
            run_frame(m_script_time, nullptr, m_script_time - 1.0f / 60.0f);
            executor.after_frame();
            if (!error.empty()) {
                throw Error(error);
            }
            if (executor.quit_requested()) {
                break;
            }
        }
    } catch (const std::exception& e) {
        log::error("run: {}", e.what());
        m_executor = nullptr;
        return 1;
    }
    m_executor = nullptr;
    return 0;
}

void App::run_tick_tail(float tick_dt, float real_dt) {
    SPUD_TRACE("saves");
    update_saves(tick_dt);
    if (m_papyrus) {
        SPUD_TRACE("papyrus");
        if (m_load_settle == LoadSettle::tick) {
            behind_screen([this] { m_papyrus->quests()->update(0.0f, m_clock.days_passed(), false); });
            m_load_settle = LoadSettle::done;
        }
        m_papyrus->update(m_fixed_step > 0.0 ? float(m_fixed_step) : real_dt, m_ui && m_ui->pausing_menus() > 0);
    }
}

// The far cascade costs most of the sun's pass, and what casts into it moves on the world's tick: frames between keep its
// map, which stands in the world where it was drawn, and read it from where the camera is now. A sharp turn or a jump
// draws it anyway, so what the frame looks at is never outside the map.
void App::hold_far_cascade(render::SunShadows& sun, const render::View& view, const glm::vec3& shadow_dir, const Space* space) {
    if (sun.count < 2) {
        return;
    }
    // The maps are one set: only the view that drew them can keep them. Through a door from indoors that's the door's.
    const bool door_too = m_portal_door >= 0 && m_portal_sun.count > 0 && m_sun_shadows.count > 0;
    render::ShadowCascade& far = sun.cascades[sun.count - 1];
    const glm::vec3 forward(view.rotation[0][2], view.rotation[1][2], view.rotation[2][2]);
    constexpr float k_most_moved_m = 4.0f, k_least_turn_cos = 0.9986f; // 3 degrees
    constexpr float k_least_sun_cos = 0.999999f;
    const bool hold = m_render_settings.shadows.hold_far_cascade && m_fixed_step > 0.0 && m_world_still &&
                      m_far_cascade_valid && !door_too && m_far_cascade_space == space &&
                      glm::distance(view.position, m_far_cascade_camera) < k_most_moved_m &&
                      glm::dot(forward, m_far_cascade_forward) > k_least_turn_cos &&
                      glm::dot(shadow_dir, m_far_cascade_sun) > k_least_sun_cos;
    if (!hold) {
        m_far_cascade = far;
        m_far_cascade_camera = view.position;
        m_far_cascade_forward = forward;
        m_far_cascade_sun = shadow_dir;
        m_far_cascade_space = space;
        m_far_cascade_valid = !door_too;
        return;
    }
    const float end_z = far.end_z;
    far = m_far_cascade;
    far.end_z = end_z;
    const glm::vec3 moved = view.position - m_far_cascade_camera; // its rows take points relative to the camera
    for (glm::vec4& row : far.rows) {
        row.w += glm::dot(glm::vec3(row), moved);
    }
    sun.hold_last = true;
}

// Objects physics moves step at its 60 Hz. On play's clock frames between show them on their way between its last two
// steps, one step behind as the player is; a jump shows no way there.
void App::update_moved_refs(float frame_dt) {
    std::vector<physics::MovedRef> moved = m_physics->take_moved_refs();
    if (m_fixed_step <= 0.0) {
        for (const physics::MovedRef& m : moved) {
            m_space->move_ref(m.form_id, m.position, m.rotation);
        }
        return;
    }
    if (m_smooth_space != m_space.get()) {
        m_smooth_space = m_space.get();
        m_smooth_refs.clear();
    }
    constexpr float k_snap_m = 2.0f;
    for (const physics::MovedRef& m : moved) {
        const auto [it, fresh] = m_smooth_refs.try_emplace(m.form_id);
        SmoothRef& s = it->second;
        const bool snap = fresh || glm::distance(s.after_position, m.position) > k_snap_m;
        s.before_position = snap ? m.position : s.after_position;
        s.before_rotation = snap ? m.rotation : s.after_rotation;
        s.after_position = m.position;
        s.after_rotation = m.rotation;
        s.idle = 0.0f;
    }
    const float a = m_player ? m_player->shown_fraction() : 1.0f;
    for (auto it = m_smooth_refs.begin(); it != m_smooth_refs.end();) {
        SmoothRef& s = it->second;
        s.idle += frame_dt;
        // Two steps with no news: it has come to rest where the last one left it.
        const bool resting = s.idle > 2.0f * float(k_world_tick);
        const float t = resting ? 1.0f : a;
        m_space->move_ref(it->first, glm::mix(s.before_position, s.after_position, t), glm::slerp(s.before_rotation, s.after_rotation, t));
        it = resting ? m_smooth_refs.erase(it) : std::next(it);
    }
}

// A locked test level: every setting a score depends on, written out here so that neither the player's INI files nor a
// change of spud's defaults moves the numbers. "user-2026-11": the user's SkyrimPrefs.ini and Skyrim.ini as they were
// when the 300 fps tour was agreed; far view at the shipped thresholds.
void App::apply_preset(const std::string& name) {
    if (name != "user-2026-11") {
        throw Error("bench: unknown preset " + name);
    }
    m_opt.msaa = 4;                                   // the agreed test level (bUseTAA=1 in the INI is banned here)
    m_grids_to_load = 11;                             // uGridsToLoad
    m_render_settings.shadows.enabled = true;
    m_render_settings.shadows.resolution = 4096;      // iShadowMapResolution
    m_render_settings.shadows.distance = 10000.0f;    // fShadowDistance
    m_render_settings.shadows.interior_distance = 3000.0f; // fInteriorShadowDistance
    m_render_settings.volumetric = true;              // bVolumetricLightingEnable
    m_render_settings.reflections = true;             // bScreenSpaceReflectionEnabled
    m_render_settings.composite = true;               // bSAOEnable: ambient occlusion in the deferred composite
    m_render_settings.light_fade_start = 3500.0f;     // fLightLODStartFade
    m_render_settings.grass_fade_start = 7000.0f;     // fGrassStartFadeDistance
}

// The state a replay must reproduce.
uint64_t App::world_digest() {
    uint64_t h = m_game ? m_game->ai().digest() : 0;
    if (const char* dump = std::getenv("SPUD_DIGEST_DUMP"); dump && m_game) {
        // Who is where, a file per section: what to diff when two runs differ.
        static int section = 0;
        m_game->ai().dump(std::format("{}.{}", dump, section++));
    }
    const auto mix = [&h](uint32_t v) { h = (h ^ v) * 0x100000001B3ull; };
    if (m_player) {
        const glm::vec3 feet = m_player->feet();
        mix(std::bit_cast<uint32_t>(feet.x));
        mix(std::bit_cast<uint32_t>(feet.y));
        mix(std::bit_cast<uint32_t>(feet.z));
    }
    mix(std::bit_cast<uint32_t>(m_clock.hour()));
    return h;
}

int App::run_bench() {
    BenchScenario scenario;
    try {
        scenario = parse_bench(m_opt.bench);
    } catch (const std::exception& e) {
        log::error("bench: {}", e.what());
        return 2;
    }
    if (m_opt.bench_out.empty()) {
        log::error("bench: --bench-out <result.json> is required");
        return 2;
    }
    if (scenario.draw_distance) {
        m_space_settings.draw_distance = *scenario.draw_distance;
        m_render_settings.terrain.full_res_radius =
            std::min(*scenario.draw_distance, float(render::Terrain::k_max_full_res_radius));
    }
    m_opt.validation = false;
    if (scenario.seed) {
        set_session_seed(*scenario.seed); // before anything that draws is made
    }
    if (!scenario.preset.empty()) {
        apply_preset(scenario.preset);
    }
    m_fixed_step = scenario.fixed_hz > 0.0f ? 1.0 / double(scenario.fixed_hz) : 0.0;
    m_tour = m_fixed_step > 0.0;
    m_render_settings.spread_water_cube = m_tour;
    init(!m_opt.headless, scenario.width, scenario.height, false, false, render::PresentMode::no_vsync);
    if (scenario.world != "none") {
        try {
            load_world(scenario.world);
        } catch (const std::exception& e) {
            log::error("bench: {}", e.what());
            return 3;
        }
    }
    place_camera();
    ScriptExecutor executor(*this);
    m_executor = &executor; // a crossing carries the rest of a fly path (app_doors.cpp)
    const std::unique_ptr<ScriptExecutor*, void (*)(ScriptExecutor**)> release_executor(
        &m_executor, [](ScriptExecutor** e) { *e = nullptr; });
    if (scenario.start) {
        Command start;
        start.kind = Command::Kind::teleport;
        start.pose = *scenario.start;
        start.source = scenario.file.filename().string();
        executor.push(start);
        executor.before_frame(0);
    }
    if (scenario.player) {
        // On foot from the start: the player's collision settles with the rest of the streaming.
        glm::vec3 feet = scenario.player->position_units * units::k_skyrim_unit_m;
        if (scenario.player->z_above_terrain) {
            feet.z = terrain_height(feet.x, feet.y).value_or(0.0f) + feet.z;
        }
        place_player(feet, scenario.player->heading_deg, scenario.player->pitch_deg);
    }

    BenchResult result;
    result.scenario = scenario.name;
    result.gpu = m_renderer->device().name();
    result.driver = driver_string(m_renderer->device().properties());
    result.width = scenario.width;
    result.height = scenario.height;
    result.draw_distance = m_space_settings.draw_distance;
    result.present_mode = m_renderer->present_mode_name();
    result.headless = m_opt.headless;
    result.fixed_hz = scenario.fixed_hz;
    result.seed = scenario.seed.value_or(0);
    result.target_ms = scenario.target_ms;
    result.wall_limit_s = scenario.wall_limit_s;
    result.preset = scenario.preset;
    for (uint32_t p = 0; p < render::k_gpu_pass_count; ++p) {
        result.pass_names.push_back(render::gpu_pass_name(p));
    }
    log::info("bench: '{}' at {}x{}, draw distance {}, present mode {}", scenario.name, scenario.width,
              scenario.height, result.draw_distance, result.present_mode);

    bool quit = false;
    // A tour's world: fixed ticks, so everything they drive replays, paced by real time,
    // so the tour lasts as long however fast the frames are. Frames between ticks draw the same world; a frame late for
    // its tick takes it at once, and frames catch up after a stall. Behind a loading screen the clock stands still.
    const double tick = m_fixed_step;
    uint64_t world_ticks = 0;
    float tour_time = 0.0f; // what frames show: the ticks, and how far into the next
    auto tour_frame = [&](bool step, float fraction) {
        m_world_still = !step;
        world_ticks += step ? 1 : 0;
        m_shown_fraction = m_tick_blend = fraction;
        const float now = m_loading ? tour_time : std::max(float((double(world_ticks) + double(fraction)) * tick), tour_time);
        const float before = std::exchange(tour_time, now);
        m_ui_dt = m_loading ? tick : double(now - before);
        return run_frame(now, nullptr, before);
    };
    // Warm-up at the start pose until streaming has settled (not measured). A tour's setup runs in it.
    const auto warm_start = Clock::now();
    result.startup_ms = ms_between(m_process_start, warm_start);
    for (auto& c : scenario.setup) {
        executor.push(std::move(c));
    }
    int settled = 0, warm_frames = 0;
    while (!quit) {
        if (m_window) {
            pump_events(quit, false);
        }
        const bool scripted = !executor.idle() && executor.before_frame(m_loading ? 0.0 : tick);
        m_player_step = tick;
        step_player();
        if (tick > 0.0) {
            tour_frame(!m_loading, 0.0f);
        } else {
            run_frame(0.0f, nullptr);
        }
        if (scripted) {
            executor.after_frame();
        }
        if (!executor.idle()) {
            continue;
        }
        if (tick > 0.0) {
            break; // its setup ended on the world it starts from: more ticks here would differ from run to run
        }
        ++warm_frames;
        settled = streaming_busy() ? 0 : settled + 1;
        if (settled >= 120 && warm_frames >= 240) {
            break;
        }
        if (ms_between(warm_start, Clock::now()) > 180000.0) {
            log::warn("bench: streaming did not settle at the start pose within 180 s");
            break;
        }
    }
    result.warmup_ms = ms_between(warm_start, Clock::now());
    m_renderer->flush_gpu_timings();
    m_renderer->take_gpu_timings(m_taken_timings);
    m_gpu_timings.clear();

    for (auto& c : scenario.commands) {
        executor.push(std::move(c));
    }
    uint64_t first_frame = 0;
    auto match_timings = [&](const std::vector<render::GpuTiming>& timings) {
        for (const auto& t : timings) {
            // Frames are in order, with gaps where a loading screen was.
            const auto it = std::lower_bound(result.frames.begin(), result.frames.end(), t.frame,
                                             [](const BenchFrame& f, uint64_t n) { return f.frame_number < n; });
            if (it == result.frames.end() || it->frame_number != t.frame) {
                continue;
            }
            it->gpu_ms = t.total_ms;
            std::copy(t.passes_ms, t.passes_ms + render::k_gpu_pass_count, it->passes.begin());
        }
    };

    // Recording: the camera advances on real frame time. Room for every frame up front: growing the
    // vector mid-run would copy all frames so far inside a measured frame.
    static_assert(render::k_gpu_pass_count <= k_max_bench_passes);
    result.frames.reserve(1u << 18);
    std::array<uint64_t, 4> alloc_history{};
    alloc_history.fill(m_renderer->device().memory_events().allocations.load());
    const auto rec_start = Clock::now();
    auto prev_start = rec_start;
    int digested = -1; // the last segment whose digest is in
    const uint64_t first_tick = world_ticks; // the setup's ticks went by unpaced
    double loading_s = 0;     // real time behind loading screens: the tour's clock doesn't count it
    bool counted_last = true; // the last frame is in result.frames
    // The camera between two ticks: the free camera moves on ticks, and frames between them show it on its way, as the
    // player's interpolates. Across a door or a jump it doesn't.
    FreeCamera cam_from = m_camera, cam_to = m_camera;
    const Space* cam_space = m_space.get();
    try {
        while (!quit) {
            const auto frame_start = Clock::now();
            const double dt = ms_between(prev_start, frame_start) / 1000.0;
            if (m_loading && tick > 0.0) {
                loading_s += dt;
            }
            if (!result.frames.empty() && counted_last) {
                result.frames.back().cpu_ms = static_cast<float>(ms_between(prev_start, frame_start));
            }
            prev_start = frame_start;
            if (scenario.wall_limit_s > 0.0f && ms_between(m_process_start, frame_start) / 1000.0 > scenario.wall_limit_s) {
                log::warn("bench: stopped at the {} s limit, in section {}", scenario.wall_limit_s, executor.current_segment());
                result.finished = false;
                break;
            }
            if (m_window) {
                pump_events(quit, false);
            }
            // Where the clock is, in ticks: a whole tick due steps the world in this frame.
            const double clock = tick > 0.0 ? (ms_between(rec_start, frame_start) / 1000.0 - loading_s) / tick : 0.0;
            const double ticked = double(world_ticks - first_tick);
            const bool step = tick > 0.0 && !m_loading && ticked + 1.0 <= clock;
            if (tick > 0.0 && !step && !m_loading) {
                // Between ticks: nothing steps, the view moves on.
                const float a = float(std::clamp(clock - ticked, 0.0, 1.0));
                m_player_step = 0.0;
                m_shown_fraction = a;
                step_player();
                if (!m_walk) {
                    m_camera.position = glm::mix(cam_from.position, cam_to.position, a);
                    m_camera.yaw = cam_from.yaw + std::remainder(cam_to.yaw - cam_from.yaw, 6.2831855f) * a;
                    m_camera.set_pitch(cam_from.pitch + (cam_to.pitch - cam_from.pitch) * a);
                }
                sampler::set_tag(uint32_t(executor.current_segment() + 1));
                const auto still_zones = tour_frame(false, a);
                BenchFrame f;
                f.frame_number = m_renderer->frame_number();
                f.t = ms_between(rec_start, frame_start) / 1000.0;
                f.segment = executor.current_segment();
                f.still = true;
                f.zones = still_zones;
                f.passes.fill(-1.0f);
                result.frames.push_back(std::move(f));
                counted_last = true;
                match_timings(m_gpu_timings);
                m_gpu_timings.clear();
                if (!m_walk) {
                    m_camera = cam_to; // what scripts and the next tick start from
                }
                continue;
            }
            const double script_dt = tick > 0.0 ? (m_loading ? 0.0 : tick) : result.frames.empty() ? 0.0 : dt;
            if (!executor.before_frame(script_dt) || executor.quit_requested()) {
                break;
            }
            const int segment = executor.current_segment();
            sampler::set_tag(uint32_t(segment + 1) + (step ? 1000u : 0u)); // tick frames apart
            if (tick > 0.0 && segment != digested) {
                if (digested >= 0) {
                    result.digests.push_back(world_digest());
                }
                digested = segment;
            }
            if (tick > 0.0) {
                m_player_step = tick; // loading too: the player's first steps tell when its collision is in
                m_shown_fraction = 0.0f;
                const bool jumped = m_space.get() != cam_space || glm::distance(m_camera.position, cam_to.position) > 10.0f;
                cam_from = jumped ? m_camera : cam_to;
                cam_to = m_camera;
                cam_space = m_space.get();
            }
            step_player();
            const float update_ms = static_cast<float>(ms_between(frame_start, Clock::now()));
            const bool loading_frame = m_loading;
            auto zones = tick > 0.0 ? tour_frame(step, float(std::clamp(clock - ticked - 1.0, 0.0, 1.0)))
                                    : run_frame(static_cast<float>(ms_between(rec_start, frame_start) / 1000.0), nullptr);
            zones[zone_update] = update_ms;
            executor.after_frame();

            // Diagnostics for long frames: device-memory allocations around them.
            const uint64_t allocs = m_renderer->device().memory_events().allocations.load();
            alloc_history[result.frames.size() % 4] = allocs;
            if (zones[zone_wait] > 20.0f || zones[zone_record] > 20.0f) {
                log::warn("bench: long frame {} at t={:.2f}s: wait {:.1f} ms, record {:.1f} ms; "
                          "vkAllocateMemory +{} over the last 4 frames",
                          result.frames.size(), ms_between(rec_start, frame_start) / 1000.0, zones[zone_wait],
                          zones[zone_record], allocs - alloc_history[(result.frames.size() + 1) % 4]);
            }

            counted_last = !(tick > 0.0 && (loading_frame || m_loading));
            if (!counted_last) {
                continue; // a real loading screen: not part of the score, and thousands of frames long
            }
            BenchFrame f;
            f.frame_number = m_renderer->frame_number();
            if (first_frame == 0) {
                first_frame = f.frame_number;
            }
            f.t = ms_between(rec_start, frame_start) / 1000.0;
            f.segment = segment;
            f.tick_late_ms = float(std::max(clock - (ticked + 1.0), 0.0) * tick * 1000.0);
            f.zones = zones;
            f.passes.fill(-1.0f);
            result.frames.push_back(std::move(f));
            // GPU timings arrive a couple of frames later; match them by frame number.
            match_timings(m_gpu_timings);
            m_gpu_timings.clear();

            result.max_pending_jobs = std::max<uint64_t>(result.max_pending_jobs, m_renderer->jobs().pending());
            result.max_upload_bytes_per_frame =
                std::max<uint64_t>(result.max_upload_bytes_per_frame, m_renderer->uploader().stats().bytes_last_poll);
            if (m_space) {
                result.cells_loaded = std::max<uint64_t>(result.cells_loaded, m_space->stats().cells_loaded);
            }
            result.meshes_loaded = std::max<uint64_t>(result.meshes_loaded, m_renderer->meshes().stats().ready);
            if ((result.frames.size() & 63) == 0) {
                uint64_t used = 0, budget = 0;
                m_renderer->device().memory_budget(used, budget);
                result.peak_gpu_bytes = std::max(result.peak_gpu_bytes, used);
                result.textures_loaded =
                    std::max<uint64_t>(result.textures_loaded, m_renderer->textures().stats().ready);
            }
        }
    } catch (const std::exception& e) {
        log::error("bench: {}", e.what());
        return 2;
    }
    if (!result.frames.empty()) {
        result.frames.back().cpu_ms = static_cast<float>(ms_between(prev_start, Clock::now()));
    }
    if (quit) {
        log::error("bench: window closed before the scenario finished");
        return 4;
    }
    if (m_fixed_step > 0.0) {
        result.digests.push_back(world_digest());
    }
    result.wall_s = ms_between(m_process_start, Clock::now()) / 1000.0;
    result.loading_s = loading_s;
    m_renderer->flush_gpu_timings();
    m_renderer->take_gpu_timings(m_taken_timings);
    match_timings(m_taken_timings);
    match_timings(m_gpu_timings);
    {
        uint64_t used = 0, budget = 0;
        m_renderer->device().memory_budget(used, budget);
        result.peak_gpu_bytes = std::max(result.peak_gpu_bytes, used);
        result.textures_loaded = std::max<uint64_t>(result.textures_loaded, m_renderer->textures().stats().ready);
    }
    result.segments = executor.segments();
    try {
        write_bench_json(m_opt.bench_out, result);
    } catch (const std::exception& e) {
        log::error("bench: cannot write {}: {}", m_opt.bench_out.string(), e.what());
        return 5;
    }
    log::info("bench: {}", bench_summary(result));
    log::info("bench: wrote {}", m_opt.bench_out.string());
    return 0;
}

int run(int argc, char** argv, std::chrono::steady_clock::time_point process_start) {
    // The main thread builds every frame: when it wakes from a fence wait it must get a core at once,
    // even while the streaming workers keep every core busy.
    platform::set_thread(nullptr, "spud main", platform::ThreadPriority::highest);
    sampler::start();
    sampler::catch_crashes();
    Options opt;
    if (!parse_options(argc, argv, opt)) {
        std::fputs(k_usage, stderr);
        return 1;
    }
    try {
        if (!opt.make_test_cache.empty()) {
            make_test_cache(opt.make_test_cache);
            return 0;
        }
        if (!opt.start.empty() && (!opt.run.empty() || !opt.screenshot.empty() || !opt.bench.empty())) {
            log::error("--start keeps a session going; it doesn't combine with --run, --screenshot or --bench");
            return 1;
        }
        if (!opt.bench.empty()) {
            App app(opt, process_start);
            return app.run_bench();
        }
        if (!opt.run.empty() || !opt.screenshot.empty()) {
            std::vector<Command> commands;
            if (!opt.run.empty()) {
                commands = parse_script(opt.run);
            } else {
                // --screenshot is sugar for a short script, always headless.
                opt.headless = true;
                for (const std::string& line : {std::string("wait_loaded 300"),
                                                std::format("wait_frames {}", opt.frames),
                                                "screenshot " + opt.screenshot.string()}) {
                    commands.push_back(*parse_command(line, "--screenshot", 1));
                }
            }
            App app(opt, process_start);
            return app.run_batch(std::move(commands));
        }
        if (opt.headless && !opt.control_port && opt.start.empty()) {
            log::error("--headless needs --run, --start, --control, --bench or --screenshot");
            return 1;
        }
        App app(opt, process_start);
        return app.run_session();
    } catch (const std::exception& e) {
        log::error("fatal: {}", e.what());
        return 1;
    }
}

} // namespace spud::engine
