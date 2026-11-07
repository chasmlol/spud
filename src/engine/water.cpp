#include "engine/water.hpp"

#include "core/units.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace spud {

namespace {

constexpr float k_byte_to_unit = 1.0f / 255.0f;

glm::vec3 colour(const osa::Water& w, uint32_t field) {
    uint8_t c[4];
    std::memcpy(c, &w.data[field], 4);
    return glm::vec3(c[0], c[1], c[2]) * k_byte_to_unit;
}

} // namespace

WaterSystem::WaterSystem(const osa::Atmosphere& atmosphere, TextureResolver textures)
    : m_atm(&atmosphere), m_textures(std::move(textures)) {
    std::fill(std::begin(m_flow_slots), std::end(m_flow_slots), -1);
}

uint32_t WaterSystem::material_of(uint32_t form) {
    auto [it, fresh] = m_material_of.try_emplace(form, uint32_t(m_forms.size()));
    if (fresh) {
        const osa::Water* w = m_atm->water(form);
        m_forms.push_back(w);
        glm::uvec4 noise(0);
        for (int i = 0; w && i < 3; ++i) {
            if (w->noise[i] != container::k_no_string) {
                noise[i] = m_textures(m_atm->string(w->noise[i]));
            }
        }
        m_noise.push_back(noise);
    }
    return it->second;
}

void WaterSystem::set_world(const osw::World& world) {
    m_world = &world;
    m_world_edid = world.edid;
    m_cells.clear();
    m_cell_at.clear();
    std::fill(std::begin(m_flow_slots), std::end(m_flow_slots), -1);
    if (!m_flow_normals) {
        m_flow_normals = m_textures("textures/water/riverflow.dds");
    }
    auto flow_of = [&](int32_t x, int32_t y) {
        auto it = std::lower_bound(world.flow_cells.begin(), world.flow_cells.end(), std::pair(y, x),
                                   [](const osw::FlowCell& c, const std::pair<int32_t, int32_t>& k) {
                                       return std::pair(c.y, c.x) < k;
                                   });
        return it != world.flow_cells.end() && it->x == x && it->y == y ? int32_t(it - world.flow_cells.begin()) : -1;
    };
    for (size_t i = 0; i < world.cells.size(); ++i) {
        const osw::Cell& c = world.cells[i];
        const uint32_t form = i < world.cell_waters.size() ? world.cell_waters[i] : 0;
        if ((c.flags & osw::cell_has_water) && form != 0) {
            m_cells.push_back({c.x, c.y, c.water_height, material_of(form), flow_of(c.x, c.y)});
        }
    }
    m_surface_material.clear();
    for (const osw::WaterSurface& s : world.water_surfaces) {
        m_surface_material.push_back(material_of(s.water));
    }
}

void WaterSystem::update(const osw::World& world, const render::View& view, const render::Environment& env,
                         const glm::vec3& water_multiplier, float sun_value, int grids, float seconds,
                         float game_seconds, float dt, const glm::vec3& player, bool wading) {
    // By name too: a new world can land at the old one's address, and its cells would then index the wrong flow maps.
    if (m_world != &world || m_world_edid != world.edid) {
        set_world(world);
    }
    m_materials.resize(m_forms.size());
    for (size_t i = 0; i < m_forms.size(); ++i) {
        render::GpuWaterMaterial& m = m_materials[i];
        m = {};
        const osa::Water* w = m_forms[i];
        if (!w) {
            continue;
        }
        const float* d = w->data;
        m.c[0] = glm::vec4(colour(*w, osa::water_shallow_color) * water_multiplier, d[osa::water_sparkle_power]);
        m.c[1] = glm::vec4(colour(*w, osa::water_deep_color) * water_multiplier, d[osa::water_sun_specular_magnitude]);
        m.c[2] = glm::vec4(colour(*w, osa::water_reflection_color) * water_multiplier, game_seconds);
        m.c[3] = glm::vec4(d[osa::water_fresnel], 0.4f, d[osa::water_specular_power], d[osa::water_specular_brightness]);
        // Each layer scrolls with its wind: compass bearing and speed, wrapped to one tile.
        float scroll[6];
        for (int l = 0; l < 3; ++l) {
            const float bearing = glm::radians(d[osa::water_noise_wind_direction + l]);
            const float distance = seconds * d[osa::water_noise_wind_speed + l];
            const float x = distance * std::sin(bearing), y = distance * std::cos(bearing);
            scroll[l * 2] = x - std::floor(x);
            scroll[l * 2 + 1] = y - std::floor(y);
        }
        m.uv_offset[0] = glm::vec4(scroll[0], scroll[1], scroll[2], scroll[3]);
        m.uv_offset[1] = glm::vec4(scroll[4], scroll[5], 0.0f, 0.0f);
        m.c[5] = glm::vec4(d[osa::water_sun_specular_power], d[osa::water_reflectivity], float(w->opacity) / 100.0f,
                           d[osa::water_refraction_magnitude]);
        m.c[6] = glm::vec4(d[osa::water_noise_amplitude], d[osa::water_noise_amplitude + 1],
                           d[osa::water_noise_amplitude + 2], d[osa::water_displacement + 4]);
        m.c[7] = glm::vec4(d[osa::water_noise_falloff], d[osa::water_reflection_magnitude], d[osa::water_sparkle_magnitude],
                           1.2f);
        m.c[8] = glm::vec4(glm::vec3(env.fog_near_color), std::max(d[osa::water_fog_above_amount] * 4.0f, 1.0f));
        m.c[9] = glm::vec4(glm::vec3(env.fog_far_color), env.fog_param.w);
        m.c[10] = glm::vec4(env.fog_param.x, env.fog_param.y, d[osa::water_fog_far],
                            d[osa::water_fog_far] - d[osa::water_fog_near]);
        m.c[11] = glm::vec4(d[osa::water_depth_control], d[osa::water_depth_control + 1], d[osa::water_depth_control + 2],
                            d[osa::water_depth_control + 3]);
        m.c[12] = glm::vec4(1.3f, 0.3f, 0.0f, 0.0f);
        m.c[13] = glm::vec4(0.05f, 0.0f, 0.0f, 0.0f);
        m.uv_scale = glm::vec4(d[osa::water_noise_uv_scale], d[osa::water_noise_uv_scale + 1],
                               d[osa::water_noise_uv_scale + 2], 0.0f);
        m.noise = m_noise[i];
    }

    const float cell = world.cell_size;
    const int32_t cam_x = int32_t(std::floor(view.position.x / cell)), cam_y = int32_t(std::floor(view.position.y / cell));
    const int32_t half = std::max(grids, 1) / 2;
    const render::Frustum frustum = render::Frustum::from_view_proj(view.view_proj);
    const float radius = cell * 0.70711f;
    m_near.clear();
    m_flow.clear();
    m_far.clear();
    m_flow_uploads.clear();
    constexpr float k_unit = units::k_skyrim_unit_m;
    constexpr float k_cell_units = 4096.0f;
    // The player, floored to 4 units, one texel of the ripples' window. The game holds it until the player has moved
    // 4 units; a plain floor is the same to the eye and keeps a pose's image independent of the way there.
    const glm::vec2 here(player.x / k_unit, player.y / k_unit);
    const glm::vec2 last_snap = m_snapped;
    m_snapped = glm::floor(here / 4.0f) * 4.0f;
    const glm::vec2 player_cells(m_snapped.x / k_cell_units, 1.0f - m_snapped.y / k_cell_units);
    for (const Cell& c : m_cells) {
        const glm::vec3 origin(float(c.x) * cell, float(c.y) * cell, c.height);
        if (!frustum.sphere(origin + glm::vec3(cell * 0.5f, cell * 0.5f, 0.0f) - view.position, radius)) {
            continue;
        }
        render::GpuWaterInstance s{};
        s.origin = glm::vec4(origin, cell);
        s.info = glm::uvec4(c.material, 0, 0, 0);
        const bool loaded = std::abs(c.x - cam_x) <= half && std::abs(c.y - cam_y) <= half;
        s.flow = glm::vec4(0.0f, float(c.x), float(-c.y), 0.0f);
        if (loaded && c.flow >= 0) {
            // The atlas wraps: a cell keeps its slot while it stays within 5 cells of the camera.
            const int32_t sx = ((c.x % 11) + 11) % 11, sy = ((-c.y % 11) + 11) % 11;
            int32_t& slot = m_flow_slots[sy * 11 + sx];
            if (slot != c.flow) {
                slot = c.flow;
                m_flow_uploads.push_back({uint32_t(sx), uint32_t(sy), world.flow_texels.data() + size_t(c.flow) * osw::k_flow_cell_bytes});
            }
            s.flow.x = 11.0f;
            s.flow_cell = glm::vec4(float(sx) / 11.0f, float(sy) / 11.0f, 0.0f, 0.0f);
            m_flow.push_back(s);
        } else {
            (loaded ? m_near : m_far).push_back(s);
        }
    }
    // The wading quad and its ripples: 2048 units round the player for as long as ripples live.
    m_frame.quad_rect = glm::vec4(0.0f);
    m_frame.ripple_steps = 0;
    m_frame.ripple_stamps = 0;
    m_frame.ripple_clear = false;
    m_since_ripple += dt;
    const Cell* water_here = nullptr;
    const int32_t player_x = int32_t(std::floor(player.x / cell)), player_y = int32_t(std::floor(player.y / cell));
    for (const Cell& c : m_cells) {
        if (c.x == player_x && c.y == player_y) {
            water_here = &c;
            break;
        }
    }
    const bool moved = wading && water_here && m_have_wader && glm::distance(here, m_last_wader) > 0.01f;
    if (moved || !m_stamps.empty()) {
        m_since_ripple = 0.0f;
    }
    const bool live = water_here && m_since_ripple < 10.0f;
    render::GpuWaterInstance quad{};
    if (live) {
        if (!m_ripples_running) {
            m_frame.ripple_clear = true;
            m_step_time = 0.0f;
        }
        m_step_time += dt;
        while (m_step_time >= 0.025f) { // 40 Hz
            m_step_time -= 0.025f;
            ++m_frame.ripple_steps;
        }
        m_frame.ripple_steps = std::min(m_frame.ripple_steps, 4u);
        m_frame.ripple_shift = glm::vec2(m_snapped.x - last_snap.x, last_snap.y - m_snapped.y) / 2048.0f;
        auto window = [&](const glm::vec2& p) {
            return glm::vec2(0.5f + (p.x - m_snapped.x) / 2048.0f, 0.5f - (p.y - m_snapped.y) / 2048.0f);
        };
        if (moved) {
            m_stamps.push_back(glm::vec4(m_last_wader, here));
        }
        for (const glm::vec4& s : m_stamps) {
            if (m_frame.ripple_stamps < render::k_water_ripple_segments) {
                m_frame.ripple_segments[m_frame.ripple_stamps++] =
                    glm::vec4(window(glm::vec2(s.x, s.y)), window(glm::vec2(s.z, s.w)));
            }
        }
        quad.origin = glm::vec4((m_snapped - 1024.0f) * k_unit, water_here->height, 2048.0f * k_unit);
        quad.flow = glm::vec4(0.0f, player_cells, 1.0f);
        quad.info = glm::uvec4(water_here->material, 0, 0, 0);
        if (water_here->flow >= 0) {
            const int32_t sx = ((water_here->x % 11) + 11) % 11, sy = ((-water_here->y % 11) + 11) % 11;
            const float fx = here.x / k_cell_units - float(water_here->x), fy = here.y / k_cell_units - float(water_here->y);
            quad.flow.x = 11.0f;
            quad.flow_cell = glm::vec4((float(sx) + fx) / 11.0f, (float(sy) + 1.0f - fy) / 11.0f, 0.0f, 0.0f);
        }
        m_frame.quad_rect = glm::vec4(m_snapped - 1024.0f, m_snapped + 1024.0f);
    }
    m_stamps.clear();
    m_ripples_running = live;
    m_last_wader = here;
    m_have_wader = wading;

    m_ripple_places.clear();
    for (const auto* list : {&m_near, &m_flow}) {
        for (const render::GpuWaterInstance& s : *list) {
            m_ripple_places.push_back(glm::vec3(s.origin) + glm::vec3(cell * 0.5f, cell * 0.5f, 5.0f * k_unit));
        }
    }
    m_surfaces.clear();
    m_surfaces.insert(m_surfaces.end(), m_near.begin(), m_near.end());
    m_surfaces.insert(m_surfaces.end(), m_flow.begin(), m_flow.end());
    m_surfaces.insert(m_surfaces.end(), m_far.begin(), m_far.end());
    m_frame.quad_first = uint32_t(m_surfaces.size());
    m_frame.quad_flow = live && quad.flow.x > 0.0f;
    if (live) {
        m_surfaces.push_back(quad);
    }
    // Placed water near enough to be loaded, in view.
    m_placed.clear();
    const float reach = cell * (float(half) + 0.5f);
    for (size_t i = 0; i < world.water_surfaces.size(); ++i) {
        const osw::WaterSurface& s = world.water_surfaces[i];
        const glm::vec3 c = glm::vec3(s.center[0], s.center[1], s.center[2]) - view.position;
        if (glm::length(glm::vec2(c)) > reach + s.radius || !frustum.sphere(c, s.radius)) {
            continue;
        }
        for (uint32_t v = 0; v < s.vertex_count; ++v) {
            const osw::WaterVertex& w = world.water_vertices[s.first_vertex + v];
            m_placed.push_back({glm::vec4(w.position[0], w.position[1], w.position[2], w.depth), glm::vec2(w.uv[0], w.uv[1]),
                                m_surface_material[i], s.vertex_uv,
                                glm::vec4(s.plane[0], s.plane[1], s.plane[2], s.plane[3])});
        }
    }
    // The cube is only ever read by water. Water within a few cells keeps it fresh for the turn that brings it into view.
    constexpr int32_t k_cube_cells = 3;
    m_frame.cube_needed = !m_surfaces.empty() || !m_placed.empty() || std::ranges::any_of(m_cells, [&](const Cell& c) {
        return std::abs(c.x - cam_x) <= k_cube_cells && std::abs(c.y - cam_y) <= k_cube_cells;
    });
    m_frame.placed = m_placed;
    m_frame.materials = m_materials;
    m_frame.surfaces = m_surfaces;
    m_frame.flow_first = uint32_t(m_near.size());
    m_frame.far_first = uint32_t(m_near.size() + m_flow.size());
    m_frame.flow_normals = m_flow_normals;
    m_frame.clock_running = dt > 0.0f;
    
    m_frame.flow_uploads = m_flow_uploads;
    m_frame.sun_dir = glm::normalize(env.sun_dir);
    m_frame.sun_color = glm::vec4(env.sun_color, std::clamp(5.0f * sun_value, 0.0f, 5.0f));
    // Under water: the water of the camera's cell, its fog and image space.
    m_frame.underwater = false;
    m_frame.fog_line = -1.0f;
    m_image_space = 0;
    // The near plane's corners against the water: under them all, the whole screen; between,
    // the waterline's row splits it. A rolled camera isn't followed.
    constexpr float k_near_m = 15.0f * units::k_skyrim_unit_m; // fNearDistance
    const glm::vec3 forward(-view.rotation[0][2], -view.rotation[1][2], -view.rotation[2][2]);
    const glm::vec3 up(view.rotation[0][1], view.rotation[1][1], view.rotation[2][1]);
    const float near_reach = k_near_m * std::tan(view.fov_y * 0.5f) * up.z;
    const float middle = view.position.z + k_near_m * forward.z;
    for (const Cell& c : m_cells) {
        const bool in_cell = c.x == cam_x && c.y == cam_y && m_forms[c.material];
        if (in_cell && std::abs(near_reach) > 1e-5f && std::abs(c.height - middle) < std::abs(near_reach) && view.position.z >= c.height) {
            const osa::Water& w = *m_forms[c.material];
            m_frame.fog = glm::vec4(colour(w, osa::water_shallow_color) * water_multiplier, 1.0f);
            m_frame.fog_far = w.data[osa::water_fog_under + 2];
            m_frame.fog_near = w.data[osa::water_fog_under + 1];
            m_frame.fog_line = 0.5f - 0.5f * (c.height - middle) / near_reach;
            break;
        }
        if (in_cell && view.position.z < c.height) {
            const osa::Water& w = *m_forms[c.material];
            m_frame.underwater = true;
            m_frame.fog_line = 0.0f;
            m_frame.fog = glm::vec4(colour(w, osa::water_shallow_color) * water_multiplier, 1.0f);
            m_frame.fog_far = w.data[osa::water_fog_under + 2];
            m_frame.fog_near = w.data[osa::water_fog_under + 1];
            m_image_space = w.image_space;
            break;
        }
    }
}

constexpr float k_unit_m = units::k_skyrim_unit_m;

void WaterSystem::add_ripple(const glm::vec2& at) {
    // The start at the point and the end half the length (0.01 of the window's half, 1024 units) on both axes.
    m_stamps.push_back(glm::vec4(at, at + glm::vec2(5.12f)));
}

void WaterSystem::wade(uint32_t id, const glm::vec3& feet) {
    const int32_t x = int32_t(std::floor(feet.x / 4096.0f)), y = int32_t(std::floor(feet.y / 4096.0f));
    const glm::vec2 here(feet);
    if (m_cell_at.size() != m_cells.size()) { // a town's actors ask every tick, of a world's water
        m_cell_at.clear();
        for (const Cell& c : m_cells) {
            m_cell_at.try_emplace(cell_key(c.x, c.y), c.height);
        }
    }
    const auto cell = m_cell_at.find(cell_key(x, y));
    if (cell == m_cell_at.end() || feet.z * k_unit_m >= cell->second) {
        m_waders.erase(id);
        return;
    }
    const auto [it, fresh] = m_waders.try_emplace(id, here);
    if (!fresh && glm::distance(it->second, here) > 0.01f) {
        m_stamps.push_back(glm::vec4(it->second, here));
    }
    it->second = here;
}

void WaterSystem::set_lights(std::span<const render::PointLight> lights, const glm::vec3& camera) {
    std::vector<std::pair<float, const render::PointLight*>> near;
    for (const render::PointLight& p : lights) {
        const float d = glm::distance(p.position, camera);
        if ((p.flags & render::point_light_on) && d < p.radius + 40.0f) {
            near.push_back({d, &p});
        }
    }
    std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    m_frame.light_count = uint32_t(std::min<size_t>(near.size(), render::k_water_lights));
    for (uint32_t i = 0; i < m_frame.light_count; ++i) {
        const render::PointLight& p = *near[i].second;
        m_frame.light_positions[i] = glm::vec4((p.position - camera) / k_unit_m, p.radius / k_unit_m);
        m_frame.light_colors[i] = glm::vec4(p.color, 0.0f);
    }
}

} // namespace spud
