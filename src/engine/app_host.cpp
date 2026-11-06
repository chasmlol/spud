// ScriptHost: what scripts, bench scenarios and control sessions can do.

#include "engine/app_internal.hpp"
#include "engine/game_natives.hpp"

#include "core/file.hpp"
#include "core/game_random.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"

#include <SDL3/SDL.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <stb_image_write.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <cmath>
#include <functional>
#include <map>

namespace spud::engine {

namespace {

float parse_number(const std::string& name, const std::string& v) {
    float out = 0;
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    if (ec != std::errc() || p != v.data() + v.size() || !std::isfinite(out)) {
        throw Error(std::format("setting '{}' needs a number, got '{}'", name, v));
    }
    return out;
}

bool parse_bool(const std::string& name, const std::string& v) {
    const std::string s = path::to_lower(v);
    if (s == "1" || s == "true" || s == "on" || s == "yes") {
        return true;
    }
    if (s == "0" || s == "false" || s == "off" || s == "no") {
        return false;
    }
    throw Error(std::format("setting '{}' needs on/off, got '{}'", name, v));
}

std::string vec3_json(const glm::vec3& v) { return std::format("[{:.3f}, {:.3f}, {:.3f}]", v.x, v.y, v.z); }

} // namespace

std::optional<float> App::terrain_height(float x, float y) {
    return m_space ? m_space->terrain_height(x, y) : std::nullopt;
}

float App::cell_size() { return m_space ? m_space->cell_size() : units::k_cell_size_m; }

bool App::streaming_busy() {
    return m_load || m_renderer->busy() || (m_space && m_space->busy()) || player_busy() || (m_world_fx && m_world_fx->busy()) ||
           (m_tour && m_game && !m_ai_ready);
}

void App::load_atmosphere() {
    const auto file = m_opt.cache / "atmosphere" / "weather.osa";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        log::warn("no {}: exterior lighting uses the placeholder sun (re-import)", file.string());
        return;
    }
    try {
        m_atmosphere = osa::read(read_file(file));
    } catch (const std::exception& e) {
        log::warn("{}: {}", file.string(), e.what());
        return;
    }
    m_sky = std::make_unique<Sky>(*m_atmosphere, [this](std::string_view path) -> uint32_t {
        return m_renderer->textures().acquire(path, render::TextureKind::color, 1.0f);
    });
    m_world_fx = std::make_unique<WorldFx>(m_opt.cache, [this](std::string_view path) -> uint32_t {
        return m_renderer->textures().acquire(path, render::TextureKind::color, 1.0f);
    });
    m_portal_fx = std::make_unique<WorldFx>(m_opt.cache, [this](std::string_view path) -> uint32_t {
        return m_renderer->textures().acquire(path, render::TextureKind::color, 1.0f);
    });
    if (m_game) {
        wire_world_fx();
    }
    m_portal_water = std::make_unique<WaterSystem>(*m_atmosphere, [this](std::string_view path) -> uint32_t {
        return m_renderer->textures().acquire(path, render::TextureKind::color, 1.0f);
    });
    m_water_system = std::make_unique<WaterSystem>(*m_atmosphere, [this](std::string_view path) -> uint32_t {
        return m_renderer->textures().acquire(path, render::TextureKind::color, 1.0f);
    });
    m_clock.set_hour(m_sky_hour);
    m_clock.set_time_scale(m_sky_time_scale ? *m_sky_time_scale : m_atmosphere->settings.time_scale);
    m_sky->set_clock(m_clock);
    m_renderer->set_sky_mesh(m_sky->mesh_vertices(), m_sky->mesh_indices());
    m_sky_weather_pending = true;
}

void App::load_world(const std::string& edid) {
    if (!m_atmosphere) {
        load_atmosphere();
    }
    const std::string name = path::to_lower(edid);
    if (name == "none") {
        destroy_physics();
        m_space.reset();
        return;
    }
    if (m_space && path::to_lower(m_space->edid()) == name) {
        return;
    }
    const bool walking = m_walk;
    destroy_physics();
    m_doors.clear();
    m_others.clear();
    m_retired.clear();
    m_space.reset();
    m_space = std::make_unique<Space>(*m_renderer, m_opt.cache, name);
    if (m_atmosphere) {
        m_space->set_sky_static_bases(m_atmosphere->sky_statics);
        m_space->set_texture_swaps(texture_swaps());
    }
    ensure_world_sim();
    watch_cells(*m_space);
    if (!m_actor_data && std::filesystem::exists(m_opt.cache / "forms.osf")) {
        m_actor_data = std::make_unique<ActorData>(m_opt.cache);
        m_actor_data->set_texture_swaps(texture_swaps(), &m_addon_swaps);
    }
    if (m_actor_data) {
        m_space->attach_actors(*m_actor_data);
    }
    rebuild_doors();
    apply_destruction();
    preload_exteriors();
    m_door_last_camera = glm::vec3(std::numeric_limits<float>::quiet_NaN());
    if (walking) {
        set_walk_mode(true);
    }
}

void App::papyrus_activate(uint32_t ref, uint32_t base) {
    papyrus_call(""); // makes the VM when nothing has yet
    m_papyrus->activate(ref, base, game::k_player_ref);
    activate_children(ref);
}

// What goes off with a reference: at once with the parent as its activator, or after its delay.
void App::activate_children(uint32_t parent) {
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    if (!actors || !m_game) {
        return;
    }
    for (const ActivateChild& child : actors->activate_children(parent)) {
        if (child.delay > 0.0f) {
            m_delayed_activations.push_back({child.ref, parent, child.delay});
        } else {
            m_papyrus->activate(child.ref, m_game->inspect(child.ref).base, parent);
            activate_children(child.ref);
        }
    }
}

void App::update_delayed_activations(float dt) {
    for (size_t i = 0; i < m_delayed_activations.size();) {
        DelayedActivation& d = m_delayed_activations[i];
        d.seconds -= dt;
        if (d.seconds > 0.0f) {
            ++i;
            continue;
        }
        const DelayedActivation due = d;
        m_delayed_activations.erase(m_delayed_activations.begin() + static_cast<ptrdiff_t>(i));
        if (m_papyrus && m_game) {
            m_papyrus->activate(due.ref, m_game->inspect(due.ref).base, due.parent);
            activate_children(due.ref);
        }
    }
}

void App::ensure_papyrus() {
    if (!m_papyrus) {
        m_papyrus = std::make_unique<Papyrus>(m_opt.cache, m_clock);
        if (m_world_sim) {
            auto& w = m_papyrus->world_calls();
            WorldSim* sim = m_world_sim.get();
            w.add_to_map = [sim](uint32_t form, bool travel) { sim->add_to_map(form, travel); };
            w.marker_flags = [sim](uint32_t form) {
                const auto i = sim->find_marker(form);
                return i ? sim->marker_flags(*i) : uint8_t(0);
            };
            w.set_marker_travel = [sim](uint32_t form, bool can) { sim->set_can_travel(form, can); };
            w.fast_travel = [this](uint32_t form) { m_travel_pending = form; };
            w.set_fast_travel_controls = [sim](bool on) { sim->set_fast_travel_enabled(on); };
            w.fast_travel_controls = [sim] { return sim->fast_travel_enabled(); };
            w.can_fast_travel = [this, sim] {
                int32_t cx = 0, cy = 0;
                const WorldSim::Place place = player_place(cx, cy);
                return sim->travel_refusal(travel_checks(), place, cx, cy).empty();
            };
            w.reset_cell = [sim](uint32_t cell) { sim->reset_cell(cell); };
            w.location_cleared = [sim](uint32_t location) { return sim->location_cleared(location); };
            w.set_location_cleared = [sim](uint32_t location, bool on) { sim->set_location_cleared(location, on); };
            w.set_in_chargen = [this](bool disable_saving) { m_saving_disabled = disable_saving; };
            w.request_autosave = [this] { autosave(); };
            w.request_save = [this] { // a force save: named and numbered like a manual save, refused silently
                if (can_save()) {
                    start_game();
                    SaveRecord& r = save_record();
                    begin_save(ost::Kind::manual, save_stem("Save", r.next_number), r.next_number);
                    ++r.next_number;
                }
            };
        }
        connect_game();
    }
}

bool App::papyrus_call(std::string_view target) {
    ensure_papyrus();
    start_game();
    if (target == "new_game") {
        start_quests();
        return true;
    }
    // quest:<EditorID>:<stage>: a quest started and set to a stage, for checks of what follows from it.
    if (target.starts_with("quest:")) {
        start_quests();
        const size_t colon = target.find(':', 6);
        game::QuestSystem* quests = m_papyrus->quests();
        game::Quest* q = quests->quest(target.substr(6, colon - 6));
        if (!q) {
            return false;
        }
        const bool started = q->is_running() || quests->start(*q, true);
        const bool staged = colon != target.npos && quests->set_stage(*q, uint16_t(std::stoi(std::string(target.substr(colon + 1)))));
        log::info("quest {}: started {} stage {} fill failure '{}'", q->editor_id, started, staged, q->fill_failure);
        return true;
    }
    // global:<hex>:<value>: a global as the game has it.
    if (target.starts_with("global:")) {
        const size_t colon = target.find(':', 7);
        m_papyrus->set_global(uint32_t(std::stoul(std::string(target.substr(7, colon - 7)), nullptr, 16)),
                              std::stof(std::string(target.substr(colon + 1))));
        return true;
    }
    // talk:<hex>: the conversation the player would start with this NPC_ or reference.
    if (target.starts_with("talk:")) {
        start_quests();
        const auto form = static_cast<uint32_t>(std::stoul(std::string(target.substr(5)), nullptr, 16));
        const uint32_t ref = m_papyrus->quests()->world().ref_of_unique_npc(form);
        m_papyrus->dialogue()->start_conversation(ref ? ref : form);
        return true;
    }
    return m_papyrus->call(target);
}

void App::set_resolution(uint32_t width, uint32_t height) {
    if (m_window) {
        SDL_SetWindowSize(m_window, static_cast<int>(width), static_cast<int>(height));
        m_renderer->on_window_resized();
    }
    m_renderer->set_render_size(width, height);
}

void App::apply_time_of_day(float hours) {
    m_sky_hour = std::fmod(std::fmod(hours, 24.0f) + 24.0f, 24.0f);
    m_clock.set_hour(m_sky_hour);
    if (m_sky) {
        m_sky->set_clock(m_clock);
    }
    m_portal_shadow_direction.valid = false;
    m_shadow_direction.valid = false; // a time set by hand shows at once, however few frames a script gives it
    // Placeholder until weather/climate: sunrise in the east at 6, south and highest at 12, west at 18.
    const float t = (hours - 6.0f) / 12.0f;
    m_sun_azimuth = 90.0f + t * 180.0f;
    m_sun_elevation = std::sin(t * 3.14159265f) * 65.0f;
}

void App::apply_sky_options() {
    if (m_opt.time) {
        apply_time_of_day(*m_opt.time);
    }
    if (m_opt.time_scale) {
        set_setting("time_scale", std::to_string(*m_opt.time_scale));
    }
    if (!m_opt.weather.empty()) {
        set_setting("weather", m_opt.weather);
    }
    if (m_opt.fov) {
        set_setting("fov_horizontal", std::to_string(*m_opt.fov));
    }
}

void App::spawn(const std::string& mesh, const glm::vec3& position_units, float heading_deg) {
    if (!m_space) {
        throw Error("spawn needs a world");
    }
    m_space->spawn(mesh, position_units * units::k_skyrim_unit_m, glm::radians(heading_deg));
}

void App::play_effect(const std::string& model, const glm::vec3& position_units, float heading_deg) {
    if (!m_world_fx) {
        throw Error("effect needs a world");
    }
    m_world_fx->spawn(model, position_units * units::k_skyrim_unit_m, glm::radians(heading_deg));
}

void App::set_setting(const std::string& name, const std::string& value) {
    auto& rs = m_render_settings;
    auto& ss = m_space_settings;
    using Setter = std::function<void(const std::string&)>;
    auto number = [&](float& target) {
        return Setter([p = &target, n = name](const std::string& v) { *p = parse_number(n, v); });
    };
    auto flag = [&](bool& target) {
        return Setter([p = &target, n = name](const std::string& v) { *p = parse_bool(n, v); });
    };
    render::Environment* env = m_space ? &m_space->environment() : nullptr;
    static render::Environment dummy_env;
    if (!env) {
        env = &dummy_env;
    }
    const std::map<std::string, Setter> setters = {
        {"draw_distance",
         [&](const std::string& v) {
             ss.draw_distance = std::clamp(parse_number(name, v), 0.0f, 64.0f);
             rs.terrain.full_res_radius = std::min(ss.draw_distance, float(render::Terrain::k_max_full_res_radius));
         }},
        {"full_res_terrain", number(rs.terrain.full_res_radius)},
        {"grids_to_load",
         [&](const std::string& v) { m_grids_to_load = std::clamp(int(parse_number(name, v)), 1, 31); }},
        {"min_pixel_radius", number(ss.min_pixel_radius)},
        {"lod_pixels", number(ss.lod_pixels)},
        {"tree_near", number(ss.tree_near)},
        {"door_effects", flag(rs.door_effects)},
        {"terrain_pixel_error", number(rs.terrain.pixel_error)},
        {"terrain_tiling", number(rs.terrain_tiling)},
        {"wireframe", flag(rs.wireframe)},
        {"show_disabled", flag(ss.show_disabled)},
        {"room_culling", flag(ss.room_culling)},
        {"occlusion", flag(ss.occlusion)},
        {"tight_pixel_radius", flag(ss.tight_pixel_radius)},
        {"door_auto_open", flag(m_doors_auto)},
        {"door_preload", flag(m_doors_preload)},
        {"flip_green", flag(rs.flip_green)},
        {"flip_winding", flag(rs.flip_winding)},
        {"unlit", flag(rs.lighting_off)},
        {"specular_aa", flag(rs.specular_aa)},
        {"point_lights", flag(rs.point_lights)},
        {"animate_lights", flag(ss.animate_lights)},
        {"light_fade_start", number(rs.light_fade_start)},
        // The game's SetTreeWindMagnitude: the wind magnitude, and the leaf
        // wave's time scale clamp(magnitude * 0.01, 0, 0.1); 0 stills the leaves.
        {"tree_wind",
         [&](const std::string& v) {
             const float mag = parse_number(name, v);
             rs.tree_wind_magnitude = mag;
             rs.tree_wave_scale = std::clamp(mag * 0.01f, 0.0f, 0.1f);
         }},
        {"draw_terrain", flag(rs.draw_terrain)},
        {"draw_grass", flag(rs.draw_grass)},
        {"draw_meshes", flag(rs.draw_meshes)},
        {"draw_water", flag(rs.draw_water)},
        {"draw_blended", flag(rs.draw_blended)},
        {"draw_decals", flag(rs.draw_decals)},
        {"composite", flag(rs.composite)},
        {"reflections", flag(rs.reflections)},
        {"hold_effect_state", flag(rs.hold_effect_state)},
        {"hold_actors", flag(m_hold_actors)},
        {"player_invulnerable",
         [&](const std::string& v) {
             if (!m_game) {
                 throw Error("player_invulnerable needs a game");
             }
             m_game->combat().fighter(game::k_player_ref).invulnerable = parse_number(name, v) != 0.0;
         }},
        {"auto_message", [&](const std::string& v) { m_ui->set_auto_message(int(parse_number(name, v))); }},
        {"lens_flare", flag(rs.lens_flare)},
        {"cloud_clock",
         [&](const std::string& v) {
             if (m_sky) {
                 m_sky->set_cloud_clock(parse_number(name, v));
             }
         }},
        {"inject_hdr",
         [&](const std::string& v) { m_renderer->inject_hdr(v == "none" ? std::vector<std::byte>{} : read_file(v)); }},
        {"volumetric", flag(rs.volumetric)},
        {"sun_shadow", flag(rs.sun_shadow)},
        {"shadows", flag(rs.shadows.enabled)}, // debug: A/B costs; off leaves shadow lights unlit, as the chosen none

        // The world animation clock of scripted runs (seconds), e.g. a game capture's g_CellAnimationTime.
        {"animation_time", number(m_script_time)},
        {"blend_background", number(rs.blend_background)},
        {"debug_view",
         [&](const std::string& v) {
             static const std::map<std::string, uint32_t> views = {
                 {"lit", 0},      {"albedo", 1}, {"normals", 2},   {"lighting", 3},
                 {"material", 4}, {"form", 5},   {"technique", 6}, {"uv", 7}, {"shadow", 8}};
             if (auto it = views.find(v); it != views.end()) {
                 rs.debug_view = it->second;
             } else {
                 rs.debug_view = static_cast<uint32_t>(std::clamp(parse_number(name, v), 0.0f, 8.0f));
             }
         }},
        {"rotation_mode",
         [&](const std::string& v) {
             ss.rotation_mode = static_cast<RotationMode>(std::clamp(int(parse_number(name, v)), 0, 4));
         }},
        {"fog_off",
         [&](const std::string& v) {
             if (m_sky) {
                 m_sky->set_fog_off(parse_number(name, v) != 0.0);
             }
         }},
        {"fog_start", number(env->fog_start)},
        {"fog_end", number(env->fog_end)},
        {"fog_max", number(env->fog_max)},
        {"fog_power", number(env->fog_power)},
        {"sun_azimuth", number(m_sun_azimuth)},
        {"sun_elevation", number(m_sun_elevation)},
        {"time_of_day", [&](const std::string& v) { apply_time_of_day(parse_number(name, v)); }},
        // Real seconds through the calendar at once (Calendar::Update), for checks of what time drives.
        {"clock_run", [&](const std::string& v) { m_clock.update(parse_number(name, v)); }},
        // Minutes taken off the time of the last save, for checks of the Tween Menu's autosave.
        {"autosave_age",
         [&](const std::string& v) {
             m_last_save_time -= std::chrono::duration_cast<Clock::duration>(std::chrono::minutes(int(parse_number(name, v))));
         }},
        // Fast travel to a map marker (hex FormID), as Game.FastTravel: no checks, the next frame.
        {"fast_travel", [&](const std::string& v) { m_travel_pending = uint32_t(std::stoul(v, nullptr, 16)); }},
        {"time_scale",
         [&](const std::string& v) {
             m_sky_time_scale = parse_number(name, v);
             m_clock.set_time_scale(*m_sky_time_scale);
         }},
        {"weather_transition",
         [&](const std::string& v) {
             // The console's sw: a transition from the current weather (EditorID or hex FormID).
             const auto* w = m_atmosphere ? m_atmosphere->weather(v) : nullptr;
             uint32_t id = w ? w->form_id : static_cast<uint32_t>(std::stoul(v, nullptr, 16));
             if (m_sky) {
                 m_sky->set_weather(id);
             }
         }},
        {"image_space_modifier",
         [&](const std::string& v) {
             // An IMAD by EditorID, at full strength from now; "none" removes every modifier.
             if (!m_sky) {
                 return;
             }
             if (v == "none") {
                 m_sky->clear_modifiers();
             } else if (!m_sky->apply_modifier(v)) {
                 throw Error("image_space_modifier: no IMAD '" + v + "'");
             }
         }},
        {"camera_orbit", [&](const std::string& v) { m_camera_orbit = glm::radians(std::stof(v)); }},
        {"weather",
         [&](const std::string& v) {
             // A FormID (hex, as the console's fw takes it) or a weather EditorID.
             uint32_t id = 0;
             const auto* w = m_atmosphere ? m_atmosphere->weather(v) : nullptr;
             if (w) {
                 id = w->form_id;
             } else {
                 const char* s = v.c_str();
                 const bool hex = v.size() > 2 && (v[1] == 'x' || v[1] == 'X');
                 auto [p, ec] = std::from_chars(s + (hex ? 2 : 0), s + v.size(), id, 16);
                 if (ec != std::errc() || p != s + v.size()) {
                     throw Error("weather: '" + v + "' is neither a FormID nor a known weather");
                 }
             }
             m_sky_weather = id;
             m_sky_weather_pending = true;
         }},
        {"fov", [&](const std::string& v) { m_camera.fov_y = glm::radians(std::clamp(parse_number(name, v), 10.0f, 150.0f)); }},
        {"fov_horizontal",
         [&](const std::string& v) {
             // Skyrim's FOV setting is horizontal; the vertical one follows from the aspect ratio.
             const float fx = glm::radians(std::clamp(parse_number(name, v), 10.0f, 170.0f));
             m_camera.fov_y = 2.0f * std::atan(std::tan(fx * 0.5f) / aspect());
         }},
        {"speed", number(m_camera.base_speed)},
        {"mouse_sensitivity", number(m_camera.sensitivity)},
        {"invert_y", flag(m_invert_y)},
        {"vsync",
         [&](const std::string& v) {
             m_vsync = parse_bool(name, v);
             m_renderer->set_present_mode(m_vsync ? render::PresentMode::vsync : render::PresentMode::no_vsync);
         }},
    };
    auto it = setters.find(name);
    if (it == setters.end() && set_player_setting(name, value)) {
        return;
    }
    if (it == setters.end()) {
        std::string known;
        for (const auto& [k, _] : setters) {
            known += (known.empty() ? "" : ", ") + k;
        }
        known += ", player_accel_time, player_decel_time, player_air_control, player_jump_height, player_gravity, "
                 "player_air_gravity_mult, player_eye_height";
        throw Error(std::format("unknown setting '{}' (known: {})", name, known));
    }
    it->second(value);
}

void App::request_screenshot() { m_renderer->request_capture(); }
void App::request_ui_screenshot() { m_renderer->request_overlay_capture(); }

void App::save_png(const std::filesystem::path& file, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    if (!stbi_write_png(file.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba.data(),
                        static_cast<int>(w * 4))) {
        throw Error("cannot write " + file.string());
    }
}

void App::save_screenshot(const std::filesystem::path& file) {
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!m_renderer->take_capture(rgba, w, h)) {
        throw Error("no frame was captured (is a world loaded?)");
    }
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    if (!stbi_write_png(file.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba.data(),
                        static_cast<int>(w * 4))) {
        throw Error("cannot write " + file.string());
    }
    const auto& fs = m_renderer->frame_stats();
    log::info("screenshot: {} ({}x{}); draws {}, instances {}, terrain cells {}", file.string(), w, h, fs.draw_calls,
              fs.mesh_instances, fs.terrain_cells);
}

void App::request_hdr_screenshot() { m_renderer->request_hdr_capture(); }

void App::save_hdr_screenshot(const std::filesystem::path& file) {
    std::vector<uint16_t> half;
    uint32_t w = 0, h = 0;
    if (!m_renderer->take_hdr_capture(half, w, h)) {
        throw Error("no HDR frame was captured (is a world loaded?)");
    }
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    // NumPy .npy v1.0: magic, header length, a dict padded with spaces to a 64-byte boundary, then the data.
    std::string dict = std::format("{{'descr': '<f2', 'fortran_order': False, 'shape': ({}, {}, 4), }}", h, w);
    const size_t unpadded = 10 + dict.size() + 1;
    dict.append((64 - unpadded % 64) % 64, ' ');
    dict.push_back('\n');
    std::string head = std::string("\x93NUMPY\x01\x00", 8);
    head.push_back(static_cast<char>(dict.size() & 0xFF));
    head.push_back(static_cast<char>(dict.size() >> 8));
    head += dict;
    std::vector<std::byte> out(head.size() + half.size() * sizeof(uint16_t));
    std::memcpy(out.data(), head.data(), head.size());
    std::memcpy(out.data() + head.size(), half.data(), half.size() * sizeof(uint16_t));
    write_file(file, out);
    log::info("screenshot_hdr: {} ({}x{}, {}x MSAA)", file.string(), w, h, m_renderer->msaa());
}

void App::request_shadow_screenshot() { m_renderer->request_shadow_capture(); }

void App::save_shadow_screenshot(const std::filesystem::path& file) {
    std::vector<uint16_t> texels;
    uint32_t layers = 0, size = 0;
    if (!m_renderer->take_shadow_capture(texels, layers, size)) {
        throw Error("no shadow maps were captured (sun shadows on, a world loaded?)");
    }
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    std::string dict = std::format("{{'descr': '<u2', 'fortran_order': False, 'shape': ({}, {}, {}), }}", layers, size,
                                   size);
    const size_t unpadded = 10 + dict.size() + 1;
    dict.append((64 - unpadded % 64) % 64, ' ');
    dict.push_back('\n');
    std::string head = std::string("\x93NUMPY\x01\x00", 8);
    head.push_back(static_cast<char>(dict.size() & 0xFF));
    head.push_back(static_cast<char>(dict.size() >> 8));
    head += dict;
    std::vector<std::byte> out(head.size() + texels.size() * sizeof(uint16_t));
    std::memcpy(out.data(), head.data(), head.size());
    std::memcpy(out.data() + head.size(), texels.data(), texels.size() * sizeof(uint16_t));
    write_file(file, out);
    log::info("screenshot_shadow: {} ({} x {}x{})", file.string(), layers, size, size);
}

std::string App::state_json() {
    const glm::vec3 p = m_camera.position;
    const float cs = cell_size();
    const auto e = m_renderer->render_extent();
    std::string j = "{";
    j += std::format("\"world\": \"{}\", ", m_space ? json_escape(m_space->edid()) : "none");
    if (m_space) {
        // Spaces and doors: the logical location (a merged city's own world), the
        // room the camera is in, open doors and the view through one.
        size_t open = 0;
        for (const auto& d : m_doors) {
            open += d.open ? 1 : 0;
        }
        j += std::format("\"space\": {{\"interior\": {}, \"location\": \"{}\", \"camera_room\": {}, "
                         "\"rooms_visible\": {}, \"rooms_outside\": {}, \"doors\": {}, \"doors_open\": {}, "
                         "\"portal_door\": \"{}\", \"crossings\": {}, \"other_spaces\": {}}}, ",
                         m_space->interior() ? "true" : "false", json_escape(current_location()),
                         m_space->camera_room(), m_space->rooms_visible(), m_space->rooms_outside() ? "true" : "false",
                         m_doors.size(), open,
                         m_portal_door >= 0
                             ? std::format("{:08X}", m_space->world().doors[m_doors[size_t(m_portal_door)].index].form_id)
                             : std::string(),
                         m_crossings, m_others.size());
    }
    j += std::format("\"camera\": {{\"position_m\": {}, \"position_units\": {}, \"heading_deg\": {:.2f}, "
                     "\"pitch_deg\": {:.2f}, \"fov_deg\": {:.1f}, \"fov_offset_deg\": {:.2f}}}, ",
                     vec3_json(p), vec3_json(p / units::k_skyrim_unit_m), m_camera.skyrim_heading_deg(),
                     m_camera.skyrim_pitch_deg(), glm::degrees(m_camera.fov_y), m_camera.fov_offset);
    j += std::format("\"kill_camera\": {{\"active\": {}, \"shot\": {}, \"phase\": {}, \"rate\": {:.3f}, \"seconds\": {:.2f}}}, ",
                     m_kill_camera.active, m_kill_camera.shot, m_kill_camera.phase, m_kill_camera.rate, m_kill_camera.elapsed);
    j += std::format("\"cell\": [{}, {}], ", int(std::floor(p.x / cs)), int(std::floor(p.y / cs)));
    j += player_json();
    const auto ms = m_renderer->meshes().stats();
    const auto ts = m_renderer->textures().stats();
    const auto us = m_renderer->uploader().stats();
    SpaceStats ss{};
    render::TerrainStats tstats{};
    if (m_space) {
        ss = m_space->stats();
        tstats = m_space->terrain().stats();
    }
    j += std::format("\"loaded\": {{\"cells\": {}, \"refs\": {}, \"refs_ready\": {}, \"models\": {}, \"meshes\": {}, "
                     "\"textures\": {}, \"terrain_cells\": {}, \"terrain_full_res\": {}, \"terrain_drawn\": [{}, {}, {}]}}, ",
                     ss.cells_loaded, ss.refs_loaded, ss.refs_ready, ss.models_loaded, ms.ready, ts.ready,
                     tstats.land_cells, tstats.near_ready, tstats.drawn_near, tstats.drawn_far, 0u);
    j += std::format("\"pending\": {{\"cells\": {}, \"models\": {}, \"meshes\": {}, \"textures\": {}, \"jobs\": {}, "
                     "\"uploads\": {}, \"terrain_full_res\": {}}}, ",
                     ss.cells_wanted, ss.models_pending, ms.loading, ts.loading, m_renderer->jobs().pending(),
                     us.queued + us.in_flight + us.completed, tstats.near_pending);
    j += std::format("\"failed\": {{\"meshes\": {}, \"textures_missing\": {}}}, ", ms.failed, ts.missing);
    j += std::format("\"streaming_busy\": {}, ", streaming_busy() ? "true" : "false");
    if (m_world_sim) {
        j += std::format("\"world_sim\": {}, \"last_travel_hours\": {:.4f}, \"actor_resets\": {}, ", m_world_sim->state_json(),
                         m_last_travel_hours, m_actor_resets);
        j += "\"quest_marks\": [";
        for (const QuestMark& m : m_quest_marks) {
            j += std::format("{{\"ref\": \"{:08X}\", \"door\": {}, \"position_units\": [{:.1f}, {:.1f}, {:.1f}], \"text\": \"{}\"}}, ", m.ref,
                             m.door ? "true" : "false", m.position.x, m.position.y, m.position.z, json_escape(m.text));
        }
        j += "], ";
        if (m_map_open) {
            const glm::vec3 p = m_map_camera.position(), t = m_map_camera.target();
            j += std::format("\"map_camera\": {{\"position\": [{:.3f}, {:.3f}, {:.3f}], \"target\": [{:.3f}, {:.3f}, {:.3f}], "
                             "\"yaw\": {:.6f}, \"pitch\": {:.6f}, \"zoom\": {:.5f}, \"settled\": {}}}, ",
                             p.x, p.y, p.z, t.x, t.y, t.z, m_map_camera.yaw(), m_map_camera.pitch(), m_map_camera.zoom(),
                             m_map_camera.settled() ? "true" : "false");
        }
    }
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        j += std::format("\"actors_animate_ms\": {:.3f}, \"actors\": [", actors->animate_ms());
        const auto list = actors->list();
        for (size_t i = 0; i < list.size(); ++i) {
            const auto& a = list[i];
            std::string clips;
            for (const auto& c : a.clips) {
                clips += std::format("{}\"{}\"", clips.empty() ? "" : ", ", c);
            }
            const auto looking = m_gazes.find(a.ref);
            const uint32_t gaze = looking != m_gazes.end() ? looking->second.target : 0;
            std::string parts;
            for (const auto& p : a.parts) {
                parts += std::format("{}\"{}\"", parts.empty() ? "" : ", ", p);
            }
            j += std::format("{}{{\"ref\": \"{:08X}\", \"npc\": \"{:08X}\", \"position_units\": [{:.1f}, {:.1f}, {:.1f}], "
                             "\"gaze\": \"{:08X}\", \"clips\": [{}], \"parts\": [{}]}}",
                             i ? ", " : "", a.ref, a.npc, a.position.x / units::k_skyrim_unit_m,
                             a.position.y / units::k_skyrim_unit_m, a.position.z / units::k_skyrim_unit_m, gaze, clips, parts);
        }
        j += "], ";
    }
    j += std::format("\"clock\": {{\"year\": {}, \"month\": {}, \"day\": {}, \"hour\": {:.6f}, \"days_passed\": {:.6f}, "
                     "\"time_scale\": {}, \"hours_passed\": {}}}, ",
                     m_clock.year(), m_clock.month(), m_clock.day(), m_clock.hour(), m_clock.days_passed(),
                     m_clock.time_scale(), m_clock.hours_passed());
    if (m_space) {
        // The lighting shaders' exterior inputs, world space.
        const auto& env = m_space->environment();
        auto v3 = [](const glm::vec3& v) { return std::format("[{:.7g}, {:.7g}, {:.7g}]", v.x, v.y, v.z); };
        auto v4 = [](const glm::vec4& v) { return std::format("[{:.7g}, {:.7g}, {:.7g}, {:.7g}]", v.x, v.y, v.z, v.w); };
        j += std::format("\"sky\": {{\"hour\": {:.4f}, \"weather\": \"{:08X}\", \"skyrim_inputs\": {}, "
                         "\"dir_light_direction\": {}, \"dir_light_color\": {}, \"directional_ambient\": [{}, {}, {}], "
                         "\"amb_spec\": {}, \"fog_param\": {}, \"fog_near_color\": {}, \"fog_far_color\": {}}}, ",
                         m_sky ? m_sky->hour() : m_sky_hour, m_sky ? m_sky->weather() : 0u, env.skyrim_inputs ? "true" : "false", v3(env.sun_dir),
                         v3(env.dir_light_color), v4(env.dir_ambient[0]), v4(env.dir_ambient[1]), v4(env.dir_ambient[2]),
                         v4(env.amb_spec), v4(env.fog_param), v4(env.fog_near_color), v4(env.fog_far_color));
        // The sun's cascades as the mask reads them: rows per metre, near and far in units.
        std::string cascades;
        for (uint32_t c = 0; c < m_sun_shadows.count; ++c) {
            const render::ShadowCascade& sc = m_sun_shadows.cascades[c];
            cascades += std::format("{}{{\"rows\": [{}, {}, {}], \"near\": {:.7g}, \"far\": {:.7g}}}", c ? ", " : "",
                                    v4(sc.rows[0]), v4(sc.rows[1]), v4(sc.rows[2]), sc.lo.z, sc.hi.z);
        }
        j += std::format("\"sun_cascades\": [{}], ", cascades);
        // Placed point lights that are on and within the fade distance.
        const auto& lights = m_space->world().lights;
        const auto& states = m_space->point_lights();
        const float reach = units::to_metres(m_render_settings.light_fade_start + m_render_settings.light_fade_range);
        std::string list;
        for (size_t i = 0; i < states.size() && i < lights.size(); ++i) {
            const auto& s = states[i];
            const glm::vec3 d = s.position - p;
            if (!(s.flags & render::point_light_on) || glm::dot(d, d) - s.radius * s.radius > reach * reach) {
                continue;
            }
            list += std::format("{}{{\"index\": {}, \"ref\": \"{:08X}\", \"base\": \"{:08X}\", \"position_units\": {}, "
                                "\"radius_units\": {:.3f}, \"color\": {}, \"dimmer\": {:.6g}, \"shadow\": {}}}",
                                list.empty() ? "" : ", ", i, lights[i].form_id, lights[i].base_form_id,
                                vec3_json(s.position / units::k_skyrim_unit_m), s.radius / units::k_skyrim_unit_m,
                                v3(s.color), lights[i].dimmer,
                                (s.flags & render::point_light_shadow) ? static_cast<int>(s.shadow) : -2);
        }
        j += std::format("\"point_lights\": [{}], ", list);
    }
    const auto& mem = m_renderer->device().memory_events();
    uint64_t used = 0, budget = 0;
    m_renderer->device().memory_budget(used, budget);
    j += std::format("\"memory\": {{\"gpu_used_mb\": {}, \"gpu_budget_mb\": {}, \"vk_allocations\": {}, "
                     "\"vk_frees\": {}, \"vk_allocated_mb\": {}, \"textures_mb\": {}, \"mesh_vertices_mb\": {}, "
                     "\"mesh_indices_mb\": {}}}, ",
                     used >> 20, budget >> 20, mem.allocations.load(), mem.frees.load(),
                     mem.allocated_bytes.load() >> 20, ts.gpu_bytes >> 20, ms.vertex_bytes >> 20, ms.index_bytes >> 20);
    const auto& fs = m_renderer->frame_stats();
    std::string passes;
    for (uint32_t i = 0; i < render::k_gpu_pass_count; ++i) {
        passes += std::format("{}\"{}\": {:.4f}", i ? ", " : "", render::gpu_pass_name(i), m_last_gpu.passes_ms[i]);
    }
    std::string zones;
    for (uint32_t i = 0; i < k_zone_count; ++i) {
        zones += std::format("{}\"{}\": {:.4f}", i ? ", " : "", cpu_zone_name(i), m_last_zones[i]);
    }
    j += std::format("\"frame\": {{\"number\": {}, \"cpu_ms\": {:.4f}, \"gpu_ms\": {:.4f}, \"cpu_zones_ms\": {{{}}}, "
                     "\"gpu_passes_ms\": {{{}}}, \"draw_calls\": {}, \"mesh_instances\": {}, \"drawn_refs\": {}, "
                     "\"mesh_triangles\": {}, \"terrain_cells\": {}, \"shadow_draw_calls\": {}, "
                     "\"shadow_triangles\": {}}}, ",
                     m_renderer->frame_number(), m_last_cpu_ms, m_last_gpu.total_ms, zones, passes, fs.draw_calls,
                     fs.mesh_instances, ss.drawn, fs.mesh_triangles, fs.terrain_cells, fs.shadow_draw_calls,
                     fs.shadow_triangles);
    // Medians over the recent window: single-frame GPU timings are too noisy for A/B comparisons.
    {
        std::vector<float> v;
        auto median = [&](auto&& get) {
            v.clear();
            for (const auto& t : m_gpu_timings) {
                v.push_back(get(t));
            }
            if (v.empty()) {
                return 0.0f;
            }
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            return v[v.size() / 2];
        };
        std::string med;
        for (uint32_t i = 0; i < render::k_gpu_pass_count; ++i) {
            med += std::format(", \"{}\": {:.4f}", render::gpu_pass_name(i),
                               median([i](const render::GpuTiming& t) { return t.passes_ms[i]; }));
        }
        j += std::format("\"gpu_median_ms\": {{\"frames\": {}, \"total\": {:.4f}{}}}, ", m_gpu_timings.size(),
                         median([](const render::GpuTiming& t) { return t.total_ms; }), med);
    }
    if (m_sky) {
        j += std::format("\"sky\": {{\"weather\": \"{:08X}\", \"outgoing\": \"{:08X}\", \"region\": \"{:08X}\"}}, ",
                         m_sky->weather(), m_sky->outgoing_weather(), m_sky->region());
    }
    j += "\"audio\": " + (m_audio ? m_audio->state_json() : std::string("{\"available\": false}")) + ", ";
    j += std::format("\"render\": {{\"width\": {}, \"height\": {}, \"present_mode\": \"{}\", \"gpu\": \"{}\"}}, ", e.width,
                     e.height, m_renderer->present_mode_name(), json_escape(m_renderer->device().name()));
    if (m_ui) {
        const auto ui = m_ui->stats();
        j += std::format("\"ui\": {{\"visible\": {}, \"captures_input\": {}, \"cpu_ms\": {:.4f}, \"gpu_ms\": {:.4f}}}",
                         m_ui->visible() ? "true" : "false", m_ui->captures_input() ? "true" : "false", ui.cpu_ms,
                         ui.gpu_ms);
    } else {
        j += "\"ui\": null";
    }
    j += "}";
    return j;
}

void App::update_audio() {
    if (!m_audio || !m_audio->available()) {
        return;
    }
    audio::Listener l;
    l.position = m_camera.position;
    l.forward = m_camera.forward();
    const glm::vec3 right = glm::normalize(glm::cross(l.forward, glm::vec3(0, 0, 1)));
    l.up = glm::normalize(glm::cross(right, l.forward));
    m_audio->set_listener(l);
    audio::Environment env = m_audio_env;
    env.world = m_space ? m_space->world().form_id : 0;
    if (m_space && !m_space->interior()) {
        // A merged city is still its own world to the audio rules (location music, ambience).
        const std::string location = current_location();
        for (const auto& r : m_space->world().regions) {
            if (r.edid == location) {
                env.world = r.form_id;
            }
        }
    }
    if (m_audio_interior || (m_space && m_space->interior())) {
        env.interior = m_audio_interior ? *m_audio_interior : m_space->world().form_id;
        env.world = 0;
    } else {
        sample_water(env);
    }
    m_audio->set_environment(env);
}

// WaterAudio::Update: sample one grid point per frame
// over
// [-R, +R]^2 around the listener (R = the water sound's max distance); a point counts when its cell has water
// and the land there (the one vertex the game's triangle lookup picks) is at or below the water. Keep at most
// uWaterAudioSamplePointsToUse points within R; the emitter is their mean, smoothed 50/50 with the last one.
void App::sample_water(audio::Environment& env) {
    if (!m_space) {
        return;
    }
    const glm::vec3 l = m_camera.position;
    const auto here = m_audio->water_at(env.world, l.x, l.y);
    env.underwater = here.has_water && l.z < here.height.value_or(m_space->world().default_water_height);
    if (here.water != m_water.water) {
        m_water = {};
        m_water.water = here.water;
    }
    env.water = here.water;
    if (!here.water || here.radius <= 0.0f) {
        env.water_samples = 0;
        return;
    }
    const uint32_t n = 8; // uWaterAudioAxisSamplePoints
    const uint32_t keep = 8; // uWaterAudioSamplePointsToUse
    const float r = here.radius;
    const float spacing = 2.0f * r / float(n - 1);
    const float x = l.x - r + float(m_water.i) * spacing, y = l.y - r + float(m_water.j) * spacing;
    if (++m_water.i == n) {
        m_water.i = 0;
        m_water.j = (m_water.j + 1) % n;
    }
    const auto point = m_audio->water_at(env.world, x, y);
    if (point.has_water) {
        if (const auto h = m_space->land_vertex_height(x, y)) {
            if (*h <= point.height.value_or(m_space->world().default_water_height)) {
                m_water.samples.push_back({x, y, *h});
            }
        }
    }
    if (!m_water.samples.empty()) {
        glm::vec3 mean{0};
        for (const auto& s : m_water.samples) {
            mean += s;
        }
        mean /= float(m_water.samples.size());
        std::erase_if(m_water.samples, [&](const glm::vec3& s) { return glm::length(s - l) > r; });
        if (m_water.samples.size() > keep) {
            auto far = std::max_element(m_water.samples.begin(), m_water.samples.end(), [&](auto& a, auto& b) {
                return glm::length(a - l) < glm::length(b - l);
            });
            m_water.samples.erase(far);
        }
        m_water.emitter = m_water.have_emitter ? (m_water.emitter + mean) * 0.5f : mean;
        m_water.have_emitter = true;
    }
    env.water_samples = static_cast<uint32_t>(m_water.samples.size());
    env.water_emitter = m_water.emitter;
}

bool App::audio_busy() {
    return m_audio && m_audio->capture_busy();
}

// `audio <sub> ...`: docs/AUDIO.md lists the subcommands. Positions are Skyrim units, like every other command.
bool App::audio_command(const std::vector<std::string>& a, std::string& result) {
    if (!m_audio || !m_audio->available()) {
        throw Error("audio is not available (no audio/sound.osd in the audio cache, or no audio device)");
    }
    auto& au = *m_audio;
    const std::string& sub = a[0];
    auto arg = [&](size_t i) -> const std::string& {
        if (i >= a.size()) {
            throw Error(std::format("audio {}: missing argument {}", sub, i));
        }
        return a[i];
    };
    auto number = [&](size_t i) { return parse_number("audio " + sub, arg(i)); };
    auto position = [&](size_t i) -> std::optional<glm::vec3> {
        if (i >= a.size()) {
            return std::nullopt;
        }
        if (a[i] == "here") {
            return m_camera.position;
        }
        return glm::vec3(number(i), number(i + 1), number(i + 2)) * units::k_skyrim_unit_m;
    };
    auto need = [&](std::optional<uint32_t> v, const std::string& what, const std::string& name) {
        if (!v) {
            throw Error(std::format("audio {}: unknown {} '{}'", sub, what, name));
        }
        return *v;
    };
    if (sub == "play") {
        const auto d = need(au.find_descriptor(arg(1)), "sound descriptor", arg(1));
        result = std::to_string(au.play(d, position(2)));
        return true;
    }
    if (sub == "playfile") {
        // `audio playfile <cache path> <category|none> <output model|none> [x y z | here]`: a file without a
        // descriptor, as dialogue plays its lines.
        const auto category = arg(2) == "none" ? osd::k_none : need(au.find_category(arg(2)), "sound category", arg(2));
        const auto model =
            arg(3) == "none" ? osd::k_none : need(au.find_output_model(arg(3)), "output model", arg(3));
        result = std::to_string(au.play_file(arg(1), category, model, position(4)));
        return true;
    }
    if (sub == "speak") {
        // `audio speak <cache path> [x y z | here]`: non-dialogue speech (speech highlight by distance).
        result = std::to_string(au.speak(arg(1), position(2).value_or(m_camera.position)));
        return true;
    }
    if (sub == "stop") {
        au.stop(static_cast<uint32_t>(number(1)));
        return true;
    }
    if (sub == "music") {
        if (arg(1) == "stop") {
            au.music_stop_all();
            return true;
        }
        const auto t = need(au.find_music_type(arg(2)), "music type", arg(2));
        if (arg(1) == "add") {
            au.music_add(t);
        } else if (arg(1) == "remove") {
            au.music_remove(t);
        } else {
            throw Error("audio music add|remove <type> | stop");
        }
        return true;
    }
    if (sub == "capture") {
        // `audio capture <file.wav> <seconds> [async]`: without async the script waits until it is written.
        if (!au.capture(arg(1), number(2))) {
            throw Error("audio capture: a capture is already running");
        }
        return a.size() > 3 && a[3] == "async";
    }
    if (sub == "capture_wait") {
        return false;
    }
    if (sub == "animevent") {
        // `audio animevent <reference> <annotation> [x y z | here] [node x y z]`: a reference's animation
        // sound annotation ("SoundPlay.OBJSawMillLoad03Drop"), as its animation would send it (Skyrim units).
        uint32_t ref = 0;
        const std::string& form = arg(1);
        const size_t skip = form.starts_with("0x") || form.starts_with("0X") ? 2 : 0;
        if (auto [p, ec] = std::from_chars(form.data() + skip, form.data() + form.size(), ref, 16);
            ec != std::errc() || p != form.data() + form.size() || form.size() == skip) {
            throw Error(std::format("audio animevent: '{}' is not a FormID", form));
        }
        const glm::vec3 at = position(3).value_or(m_camera.position);
        std::optional<glm::vec3> node;
        if (a.size() > 7 && a[6] == "node") {
            node = position(7);
        }
        if (!au.animation_event(ref, arg(2), at, node)) {
            throw Error(std::format("audio animevent: '{}' is not a sound annotation", arg(2)));
        }
        return true;
    }
    if (sub == "playrel") {
        // `audio playrel <sound> <dx> <dy> <dz>`: Skyrim units from the listener, world axes.
        const auto d = need(au.find_descriptor(arg(1)), "sound descriptor", arg(1));
        result = std::to_string(au.play(d, m_camera.position + glm::vec3(number(2), number(3), number(4)) *
                                                                   units::k_skyrim_unit_m));
        return true;
    }
    if (sub == "state") {
        const std::string json = au.state_json();
        if (arg(1) == "-") {
            result = json;
        } else {
            write_file(arg(1), std::as_bytes(std::span(json.data(), json.size())));
        }
        return true;
    }
    if (sub == "volume") {
        // Menu levels go through the user settings (`audio save` writes them); other categories straight to the mix.
        const float v = number(2) / 100.0f;
        if (arg(1) == "master") {
            m_user.set("audio.master", std::format("{}", std::lround(v * 100.0f)));
            au.set_master_volume(v);
        } else {
            const uint32_t category = need(au.find_category(arg(1)), "sound category", arg(1));
            for (const auto& s : au.menu_sliders()) {
                if (s.category == category) {
                    m_user.set("audio." + s.key, std::format("{}", std::lround(v * 100.0f)));
                }
            }
            au.set_category_volume(category, v);
        }
        return true;
    }
    if (sub == "hrtf") {
        // `audio hrtf on|off`: the mix only, so a script can compare the two without touching the saved setting.
        au.set_hrtf(parse_bool("audio hrtf", arg(1)));
        return true;
    }
    if (sub == "env") {
        if (arg(1) == "hour") {
            apply_time_of_day(number(2));
        } else if (arg(1) == "weather") {
            m_audio_env.weather =
                arg(2) == "none" ? 0 : au.db().weathers[need(au.find_weather(arg(2)), "weather", arg(2))].form_id;
        } else if (arg(1) == "interior") {
            m_audio_interior.reset();
            if (arg(2) != "none") {
                m_audio_interior = au.db().cells[need(au.find_cell(arg(2)), "cell", arg(2))].form_id;
            }
        } else if (arg(1) == "underwater") {
            m_audio_env.underwater = parse_bool("audio env underwater", arg(2));
        } else {
            throw Error("audio env hour|weather|interior|underwater <value>");
        }
        return true;
    }
    if (sub == "footstep") {
        const auto set = arg(1) == "default" ? osd::k_none : need(au.find_footstep_set(arg(1)), "footstep set", arg(1));
        static const std::pair<const char*, osd::FootstepGait> k_gaits[] = {
            {"walk", osd::gait_walk}, {"run", osd::gait_run}, {"sprint", osd::gait_sprint},
            {"sneak", osd::gait_sneak}, {"swim", osd::gait_swim}};
        auto gait = std::find_if(std::begin(k_gaits), std::end(k_gaits), [&](auto& g) { return arg(2) == g.first; });
        if (gait == std::end(k_gaits)) {
            throw Error("audio footstep: gait is walk|run|sprint|sneak|swim");
        }
        const auto material = need(au.find_material(arg(4)), "material", arg(4));
        // `audio footstep <set|default> <gait> <tag> <material> [x y z | here] [player | player1st]`
        std::vector<std::string> rest(a.begin() + 5, a.end());
        std::string who;
        if (!rest.empty() && (rest.back() == "player" || rest.back() == "player1st")) {
            who = rest.back();
            rest.pop_back();
        }
        glm::vec3 where = m_camera.position;
        if (rest.size() == 3) {
            where = glm::vec3(parse_number("audio footstep", rest[0]), parse_number("audio footstep", rest[1]),
                              parse_number("audio footstep", rest[2])) *
                    units::k_skyrim_unit_m;
        } else if (!rest.empty() && rest[0] != "here") {
            throw Error("audio footstep <set|default> <gait> <tag> <material> [x y z | here] [player | player1st]");
        }
        au.footstep(set, gait->second, arg(3), au.db().materials[material].form_id, where, !who.empty(),
                    who == "player1st");
        return true;
    }
    if (sub == "impact") {
        const auto set = need(au.find_impact_set(arg(1)), "impact data set", arg(1));
        const auto material = need(au.find_material(arg(2)), "material", arg(2));
        au.impact(set, au.db().materials[material].form_id, position(3).value_or(m_camera.position));
        return true;
    }
    if (sub == "collision") {
        // `audio collision <material a> <material b> <speed m/s> [x y z | here]`
        const auto ma = need(au.find_material(arg(1)), "material", arg(1));
        const auto mb = need(au.find_material(arg(2)), "material", arg(2));
        au.collision(au.db().materials[ma].form_id, au.db().materials[mb].form_id,
                     position(4).value_or(m_camera.position), number(3));
        return true;
    }
    if (sub == "seed") {
        // `audio seed <n>`: seed the game's shared generator, which is clock-seeded like the game's, for a
        // repeatable run.
        GameRandom::shared().seed(static_cast<uint32_t>(std::stoul(arg(1))));
        return true;
    }
    if (sub == "save") {
        // `audio save`: write the Audio menu values to the settings file (only with one: interactive or --settings).
        save_user_settings();
        return true;
    }
    if (sub == "menu" || sub == "loading" || sub == "dialogue") {
        const bool on = parse_bool("audio " + sub, arg(1));
        sub == "menu" ? au.set_menu_mode(on) : sub == "loading" ? au.set_loading_menu(on) : au.set_dialogue(on);
        return true;
    }
    throw Error(std::format("unknown audio subcommand '{}' (play, playfile, speak, stop, music, capture, state, "
                            "volume, hrtf, env, footstep, impact, collision, menu, loading, dialogue, seed, save)",
                            sub));
}

} // namespace spud::engine
