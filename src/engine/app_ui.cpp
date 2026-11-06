// Input handling and the F1 debug UI.

#include "engine/app_internal.hpp"

#include "core/log.hpp"
#include "core/units.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cmath>

namespace spud::engine {

void App::pump_events(bool& quit, bool interactive) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (interactive) {
            handle_event(e, quit);
        } else if (e.type == SDL_EVENT_QUIT) {
            quit = true;
        } else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            m_renderer->on_window_resized();
        }
    }
}

void App::handle_event(const SDL_Event& e, bool& quit) {
    if (m_imgui) {
        ImGui_ImplSDL3_ProcessEvent(&e);
    }
    const bool look = m_walk || m_rmb_look || m_tab_look || m_free_look;
    const bool ui_mouse = m_imgui && ImGui::GetIO().WantCaptureMouse && !look;
    const bool ui_keys = m_imgui && ImGui::GetIO().WantCaptureKeyboard;
    const bool is_key = e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_TEXT_INPUT;
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F1 && !ui_keys) {
        m_ui_visible = !m_ui_visible; // the debug overlay toggles over menus too
        return;
    }
    if (m_ui && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)) {
        m_ui->set_gamepad_active(false);
    }
    // The game UI (menus, console) sees input before the game does (input_focus.hpp).
    if (!(is_key ? ui_keys : ui_mouse) && m_focus.route(e)) {
        return;
    }
    switch (e.type) {
    case SDL_EVENT_QUIT:
        quit = true;
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        m_renderer->on_window_resized();
        break;
    case SDL_EVENT_KEY_DOWN:
        if (ui_keys) {
            break;
        }
        if (e.key.key == SDLK_N && !e.key.repeat && m_space && !m_sbs) {
            try {
                set_walk_mode(!m_walk);
            } catch (const std::exception& ex) {
                log::warn("walk: {}", ex.what());
            }
        } else if (m_walk) {
            handle_walk_key(e);
        } else if (e.key.scancode == m_binds[bind_look]) {
            m_tab_look = !m_tab_look;
        } else if (e.key.key == SDLK_ESCAPE) {
            m_tab_look = false;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button == SDL_BUTTON_RIGHT && !ui_mouse) {
            m_rmb_look = true;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_RIGHT) {
            m_rmb_look = false;
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (look) {
            m_mouse_dx += e.motion.xrel;
            m_mouse_dy += e.motion.yrel;
        }
        break;
    case SDL_EVENT_KEY_UP:
        if (m_walk && !ui_keys) {
            handle_walk_key(e);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!ui_mouse) {
            if (m_walk && m_player) {
                m_player->wheel(e.wheel.y); // the camera's zoom (Skyrim: wheel up zooms in)
            } else {
                m_wheel += e.wheel.y;
            }
        }
        break;
    default:
        break;
    }
}

CameraInput App::gather_input() {
    CameraInput in;
    if (!m_focus.game()) {
        // A menu or the console has the keyboard and mouse: no movement. With the game's cursor up the mouse moves
        // that cursor by its own motion, the OS pointer hidden (ui::Ui::shows_cursor); else the pointer is free.
        m_rmb_look = m_tab_look = false;
        SDL_SetWindowRelativeMouseMode(m_window, m_ui && (m_ui->shows_cursor() || m_ui->holds_mouse()));
        m_mouse_dx = m_mouse_dy = m_wheel = 0;
        return in;
    }
    const bool look = m_walk || m_rmb_look || m_tab_look || m_free_look;
    SDL_SetWindowRelativeMouseMode(m_window, look);
    if (!m_walk && (!(m_imgui && ImGui::GetIO().WantCaptureKeyboard) || look)) {
        const bool* k = key_state();
        auto down = [&](Bind b) { return k[m_binds[b]]; };
        // E/Q and the right-hand modifiers stay as fixed alternatives to the bound keys.
        in.forward = float(down(bind_forward)) - float(down(bind_back));
        in.right = float(down(bind_right)) - float(down(bind_left));
        in.up = float(down(bind_up) || k[SDL_SCANCODE_E]) -
                float(down(bind_down) || k[SDL_SCANCODE_RCTRL] || k[SDL_SCANCODE_Q]);
        in.fast = down(bind_fast) || k[SDL_SCANCODE_RSHIFT];
    }
    in.look_dx = m_mouse_dx;
    in.look_dy = m_invert_y ? -m_mouse_dy : m_mouse_dy;
    in.wheel = m_wheel;
    m_mouse_dx = m_mouse_dy = m_wheel = 0;
    return in;
}

void App::draw_ui() {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("spud (F1)");
    float avg = 0, worst = 0;
    for (float f : m_frame_ms) {
        avg += f;
        worst = std::max(worst, f);
    }
    avg = m_frame_ms.empty() ? 0 : avg / m_frame_ms.size();
    ImGui::Text("frame %.3f ms (%.0f fps), worst %.2f ms   gpu %.3f ms", avg, avg > 0 ? 1000.0f / avg : 0.0f, worst,
                m_last_gpu.total_ms);
    {
        float plot[240];
        int n = 0;
        for (float f : m_frame_ms) {
            plot[n++] = f;
        }
        ImGui::PlotLines("##ft", plot, n, 0, nullptr, 0.0f, std::max(4.0f, worst), ImVec2(-1, 40));
    }
    ImGui::Text("cpu: upd %.2f  stream %.2f  cull %.2f  rec %.2f  wait %.2f", m_last_zones[zone_update],
                m_last_zones[zone_streaming], m_last_zones[zone_cull], m_last_zones[zone_record],
                m_last_zones[zone_wait]);
    ImGui::Text("gpu: opq %.2f ter %.2f at %.2f sky %.2f wat %.2f bl %.2f post %.2f", m_last_gpu.passes_ms[0],
                m_last_gpu.passes_ms[1], m_last_gpu.passes_ms[2], m_last_gpu.passes_ms[3], m_last_gpu.passes_ms[4],
                m_last_gpu.passes_ms[5], m_last_gpu.passes_ms[6]);

    const glm::vec3 p = m_camera.position;
    const glm::vec3 u = p / units::k_skyrim_unit_m;
    const float cs = cell_size();
    ImGui::SeparatorText("camera");
    ImGui::Text("pos   %.2f %.2f %.2f m", p.x, p.y, p.z);
    ImGui::Text("units %.0f %.0f %.0f   cell %d, %d", u.x, u.y, u.z, int(std::floor(p.x / cs)),
                int(std::floor(p.y / cs)));
    ImGui::Text("heading %.1f  pitch %.1f (getangle z / x)", m_camera.skyrim_heading_deg(),
                m_camera.skyrim_pitch_deg());
    ImGui::SliderFloat("speed m/s", &m_camera.base_speed, 0.5f, 2000.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::InputInt2("cell", m_teleport);
    ImGui::SameLine();
    if (ImGui::Button("teleport") && m_space) {
        const glm::vec2 xy = glm::vec2(m_teleport[0] + 0.5f, m_teleport[1] + 0.5f) * cs;
        m_camera.position = glm::vec3(xy, terrain_height(xy.x, xy.y).value_or(0.0f) + 40.0f);
    }

    draw_player_ui();

    ImGui::SeparatorText("streaming");
    if (m_space) {
        const auto s = m_space->stats();
        ImGui::Text("cells %u loaded / %u total, %u queued", s.cells_loaded, s.cells_total, s.cells_wanted);
        ImGui::Text("refs %u ready / %u loaded, drawn %u", s.refs_ready, s.refs_loaded, s.drawn);
        ImGui::Text("models %u (%u pending), instance slots %u", s.models_loaded, s.models_pending, s.instance_slots);
        const auto t = m_space->terrain().stats();
        ImGui::Text("terrain %u cells, full-res %u (+%u), drawn %u near, %u far", t.land_cells, t.near_ready,
                    t.near_pending, t.drawn_near, t.drawn_far);
    }
    const auto ms = m_renderer->meshes().stats();
    ImGui::Text("meshes %u ready, %u loading, %u failed", ms.ready, ms.loading, ms.failed);
    ImGui::Text("  vtx %.0f/%.0f MiB, idx %.0f/%.0f MiB", ms.vertex_bytes / 1048576.0, ms.vertex_capacity / 1048576.0,
                ms.index_bytes / 1048576.0, ms.index_capacity / 1048576.0);
    const auto ts = m_renderer->textures().stats();
    ImGui::Text("textures %u ready, %u loading, %u missing, %.0f MiB", ts.ready, ts.loading, ts.missing,
                ts.gpu_bytes / 1048576.0);
    const auto us = m_renderer->uploader().stats();
    ImGui::Text("jobs %zu, uploads %zu queued %zu in flight, %.1f MiB last frame", m_renderer->jobs().pending(),
                us.queued, us.in_flight, us.bytes_last_poll / 1048576.0);
    uint64_t used = 0, budget = 0;
    m_renderer->device().memory_budget(used, budget);
    ImGui::Text("GPU memory %.0f / %.0f MiB", used / 1048576.0, budget / 1048576.0);
    const auto& fs = m_renderer->frame_stats();
    ImGui::Text("draw calls %u, mesh cmds %u, instances %u, blended %u, terrain %u", fs.draw_calls, fs.mesh_commands,
                fs.mesh_instances, fs.blended, fs.terrain_cells);
    ImGui::Text("mesh triangles %.2f M", fs.mesh_triangles / 1e6);
    ImGui::SliderFloat("min pixel radius", &m_space_settings.min_pixel_radius, 0.0f, 4.0f, "%.2f px");

    ImGui::SeparatorText("view");
    if (ImGui::SliderFloat("draw distance", &m_space_settings.draw_distance, 1.0f, 32.0f, "%.0f cells")) {
        m_render_settings.terrain.full_res_radius =
            std::min(m_space_settings.draw_distance, float(render::Terrain::k_max_full_res_radius));
    }
    ImGui::SliderFloat("full-res terrain", &m_render_settings.terrain.full_res_radius, 0.0f,
                       float(render::Terrain::k_max_full_res_radius), "%.0f cells");
    ImGui::SliderFloat("terrain tiling", &m_render_settings.terrain_tiling, 1.0f, 32.0f, "%.1f / cell");
    if (m_space) {
        auto& env = m_space->environment();
        ImGui::SliderFloat("fog start", &env.fog_start, 0.0f, 5000.0f, "%.0f m");
        ImGui::SliderFloat("fog end", &env.fog_end, 100.0f, 30000.0f, "%.0f m", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("fog max", &env.fog_max, 0.0f, 1.0f);
        ImGui::SliderFloat("fog power", &env.fog_power, 0.2f, 4.0f);
        ImGui::SliderFloat("sun azimuth", &m_sun_azimuth, 0.0f, 360.0f, "%.0f deg");
        ImGui::SliderFloat("sun elevation", &m_sun_elevation, -10.0f, 90.0f, "%.0f deg");
        ImGui::ColorEdit3("sun", &env.sun_color.x);
        ImGui::ColorEdit3("ambient sky", &env.ambient_sky.x);
        ImGui::ColorEdit3("ambient ground", &env.ambient_ground.x);
        ImGui::ColorEdit3("sky zenith", &env.sky_zenith.x);
        ImGui::ColorEdit3("sky horizon", &env.sky_horizon.x);
    }

    if (m_audio && m_audio->available()) {
        // The game's Audio menu sliders (docs/AUDIO.md "Settings"), saved to settings.ini as they change.
        ImGui::SeparatorText("audio");
        bool changed = false;
        float master = m_user.number("audio.master");
        if (ImGui::SliderFloat("master", &master, 0.0f, 100.0f, "%.0f %%")) {
            m_user.set("audio.master", std::format("{}", std::lround(master)));
            changed = true;
        }
        for (const auto& s : m_audio->menu_sliders()) {
            const std::string key = "audio." + s.key;
            float v = m_user.number(key);
            if (ImGui::SliderFloat(s.key.c_str(), &v, 0.0f, 100.0f, "%.0f %%")) {
                m_user.set(key, std::format("{}", std::lround(v)));
                changed = true;
            }
        }
        bool hrtf = m_user.flag("audio.hrtf");
        if (ImGui::Checkbox("headphone spatial audio (HRTF)", &hrtf)) {
            m_user.set("audio.hrtf", hrtf ? "on" : "off");
            changed = true;
        }
        if (changed) {
            apply_audio_settings();
            save_user_settings();
        }
    }

    ImGui::SeparatorText("debug");
    ImGui::Checkbox("wireframe", &m_render_settings.wireframe);
    ImGui::SameLine();
    ImGui::Checkbox("show disabled refs", &m_space_settings.show_disabled);
    ImGui::Checkbox("flip normal green", &m_render_settings.flip_green);
    ImGui::SameLine();
    ImGui::Checkbox("flip winding", &m_render_settings.flip_winding);
    ImGui::Checkbox("terrain", &m_render_settings.draw_terrain);
    ImGui::SameLine();
    ImGui::Checkbox("meshes", &m_render_settings.draw_meshes);
    ImGui::SameLine();
    ImGui::Checkbox("water", &m_render_settings.draw_water);
    ImGui::SameLine();
    ImGui::Checkbox("unlit", &m_render_settings.lighting_off);
    int view_mode = static_cast<int>(m_render_settings.debug_view);
    ImGui::Combo("view", &view_mode, "lit\0albedo\0normals\0lighting\0");
    m_render_settings.debug_view = static_cast<uint32_t>(view_mode);
    int rot = static_cast<int>(m_space_settings.rotation_mode);
    if (ImGui::Combo("rotation", &rot, k_rotation_mode_names.data(), static_cast<int>(k_rotation_mode_names.size()))) {
        m_space_settings.rotation_mode = static_cast<RotationMode>(rot);
    }
    if (ImGui::Checkbox("vsync", &m_vsync)) {
        m_renderer->set_present_mode(m_vsync ? render::PresentMode::vsync : render::PresentMode::no_vsync);
    }
    ImGui::SameLine();
    ImGui::Text("present: %s", m_renderer->present_mode_name().c_str());
    ImGui::End();
}

} // namespace spud::engine
