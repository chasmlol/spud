// The zoom key: hold C to look through a narrow field of view, the wheel to change how narrow. Each press starts
// from k_zoom_default. Nothing about it lowers the image: the view narrows, and what it looks at loads and draws in
// full, as it would from that much nearer (SpaceSettings::focus, lod_zoom).

#include "engine/app_internal.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace spud::engine {

namespace {

constexpr float k_ease_rate = 14.0f;        // 1/s: how fast the view eases to the asked magnification
constexpr float k_look_rate = 10.0f;        // 1/s: zoomed, the share of the mouse's motion the view takes each second
// The wheel's notch at the default magnification (x1.25), and how it shrinks further in: divided by
// 1 + k_wheel_falloff x ln(zoom / default), so x1.09 at 64x and x1.05 at 1000x, and the wheel keeps going.
const float k_wheel_step = std::log(1.25f);
constexpr float k_wheel_falloff = 0.6f;
constexpr float k_focus_from = 1.2f;        // below this magnification nothing loads for the focus
constexpr float k_focus_min_radius = 2.0f;  // cells
constexpr float k_focus_max_radius = 6.0f;  // cells: what the zoomed view covers at its edges, kept to a sane load
constexpr float k_focus_margin = 1.5f;      // cells past the view's edge

} // namespace

std::optional<float> App::zoom_hit_distance() const {
    if (!m_space || m_space->interior()) {
        return std::nullopt;
    }
    const glm::vec3 from = m_camera.position, dir = m_camera.forward();
    const float cs = m_space->world().cell_size;
    auto above = [&](float t) {
        const glm::vec3 p = from + dir * t;
        const std::optional<float> h = m_space->terrain_height(p.x, p.y);
        return !h || p.z > *h;
    };
    if (!above(0.0f)) {
        return std::nullopt;
    }
    // Steps of a hundredth of the distance so far find the land within a few thousand cells' reach in under a
    // thousand height samples; halving then pins it down.
    float lo = 0.0f, t = 1.0f;
    const float reach = cs * 200.0f;
    while (t < reach) {
        if (!above(t)) {
            for (int i = 0; i < 16; ++i) {
                const float mid = 0.5f * (lo + t);
                (above(mid) ? lo : t) = mid;
            }
            return t;
        }
        lo = t;
        t += std::max(1.0f, t * 0.01f);
    }
    return std::nullopt;
}

void App::zoom_wheel(float notches) {
    float z = std::log(m_zoom.target);
    const float step = k_wheel_step / (1.0f + k_wheel_falloff * std::max(z - std::log(k_zoom_default), 0.0f));
    z += notches * step;
    m_zoom.target = std::clamp(std::exp(z), k_zoom_min, k_zoom_max);
}

void App::smooth_look(CameraInput& in, float dt) {
    // Zoomed, the view eases towards where the mouse has asked it to be, as Zoomify's cinematic camera does; unzoomed
    // what is left goes at once, and the mouse is direct again.
    m_zoom.look_pending += glm::vec2(in.look_dx, in.look_dy);
    const float k = m_camera.zoom > 1.05f ? 1.0f - std::exp(-std::max(dt, 0.0f) * k_look_rate) : 1.0f;
    const glm::vec2 now = m_zoom.look_pending * k;
    m_zoom.look_pending -= now;
    if (glm::length(m_zoom.look_pending) < 1e-3f) {
        m_zoom.look_pending = glm::vec2(0.0f);
    }
    in.look_dx = now.x;
    in.look_dy = now.y;
}

void App::update_zoom(float dt) {
    if (m_zoom.held && (!m_focus.game() || m_map_open || !key_state()[SDL_SCANCODE_C])) {
        m_zoom.held = false; // a menu took the key-up, or the map took the view
    }
    float want = m_zoom.held ? m_zoom.target : 1.0f;
    if (m_zoom.forced > 0.0f) {
        m_zoom.shown = want = m_zoom.forced;
    } else {
        // Eased in the logarithm, so 2x to 4x takes as long as 8x to 16x.
        const float k = 1.0f - std::exp(-std::max(dt, 0.0f) * k_ease_rate);
        m_zoom.shown = std::exp(std::log(m_zoom.shown) + (std::log(want) - std::log(m_zoom.shown)) * k);
        if (std::abs(m_zoom.shown - want) < 0.002f * want) {
            m_zoom.shown = want;
        }
    }
    const float zoom = m_map_open ? 1.0f : m_zoom.shown;
    m_camera.zoom = zoom;
    m_space_settings.lod_zoom = zoom;
    m_space_settings.hide_player = zoom > 1.05f;
    m_space_settings.focus_radius = 0.0f;
    m_render_settings.terrain.focus_radius = 0.0f;
    if (zoom < k_focus_from) {
        m_zoom.has_focus = false;
        return;
    }
    const float cs = m_space ? m_space->world().cell_size : 1.0f;
    if (const std::optional<float> hit = zoom_hit_distance()) {
        const glm::vec3 at = m_camera.position + m_camera.forward() * *hit;
        m_zoom.focus = glm::vec2(at.x, at.y) / cs;
        m_zoom.has_focus = true;
    }
    if (!m_zoom.has_focus) {
        return; // the sky: what loaded for the last place the view met the land stays
    }
    // The zoomed view's half width where it meets the land, in cells, with a margin past its edge.
    const float distance = glm::length(m_zoom.focus - glm::vec2(m_camera.position) / cs);
    const float half_tan = std::tan(m_camera.fov_y * 0.5f) * aspect() / zoom;
    m_space_settings.focus = m_zoom.focus;
    m_space_settings.focus_radius =
        std::clamp(distance * half_tan + k_focus_margin, k_focus_min_radius, k_focus_max_radius);
    m_render_settings.terrain.focus = m_zoom.focus;
    m_render_settings.terrain.focus_radius = m_space_settings.focus_radius;
}

} // namespace spud::engine
