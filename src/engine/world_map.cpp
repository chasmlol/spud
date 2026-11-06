#include "engine/world_map.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spud {

namespace {

constexpr float k_cell_units = 4096.0f;
constexpr float k_start_zoom = 0.25f;       // World::SetTarget
constexpr float k_fov_degrees = 80.0f;      // fDefaultFOV:Display, measured 79.99998
constexpr float k_near = 128.0f;            // fMapMenuNearClipPlane:MapMenu
constexpr float k_reference_aspect = 0.5625f; // a fixed vertical FOV, 16:9 reference
constexpr float k_min_pan_speed = 60000.0f; // fMapWorldMinPanSpeed:MapMenu
constexpr float k_max_pan_speed = 75000.0f; // fMapWorldMaxPanSpeed:MapMenu
constexpr float k_zoom_speed = 2.0f;        // fMapWorldZoomSpeed:MapMenu
constexpr float k_height_force = 4.0f;      // fMapWorldHeightAdjustmentForce:MapMenu
constexpr float k_max_dt = 0.033f;          // World::Update's cap
constexpr float k_yaw_range = 80.0f;        // fMapWorldYawRange:MapMenu
constexpr float k_min_pitch = 15.0f;        // fMapWorldMinPitch:MapMenu
constexpr float k_max_pitch = 75.0f;        // fMapWorldMaxPitch:MapMenu

} // namespace

float WorldMapCamera::fov_y() const {
    return 2.0f * std::atan(std::tan(glm::radians(k_fov_degrees) * 0.5f) * k_reference_aspect);
}

float WorldMapCamera::near_plane() const { return k_near; }

void WorldMapCamera::correct_bounds() {
    // GetBoundsCorrection: the point of the map's corner-cell rectangle nearest to where the camera would
    // stand. A world with no corner cells keeps to its bounds instead.
    const Data& d = m_data;
    const bool cells = d.nw_cell[0] || d.nw_cell[1] || d.se_cell[0] || d.se_cell[1];
    const float x0 = cells ? float(d.nw_cell[0]) * k_cell_units : d.bounds_min[0];
    const float x1 = cells ? float(d.se_cell[0]) * k_cell_units : d.bounds_max[0];
    const float y0 = cells ? float(d.se_cell[1]) * k_cell_units : d.bounds_min[1];
    const float y1 = cells ? float(d.nw_cell[1]) * k_cell_units : d.bounds_max[1];
    const glm::vec2 at = glm::vec2(m_target) + m_translation;
    m_translation = glm::vec2(std::clamp(at.x, std::min(x0, x1), std::max(x0, x1)),
                              std::clamp(at.y, std::min(y0, y1), std::max(y0, y1))) -
                    glm::vec2(m_target);
}

void WorldMapCamera::place() {
    const float range = m_data.max_height - m_data.min_height;
    m_end = glm::vec3(m_target.x + m_translation.x, m_target.y + m_translation.y,
                      m_data.min_height + m_zoom * range + m_land);
}

void WorldMapCamera::begin(const Data& data, const glm::vec3& target, float aspect, float transition_seconds,
                           Ground ground) {
    m_data = data;
    m_ground = std::move(ground);
    m_aspect = aspect;
    m_transition = transition_seconds;
    m_elapsed = 0;
    m_from = target;
    m_from_pitch = glm::radians(data.initial_pitch);
    set_target(target);
    m_position = transition_seconds > 0 ? m_from : m_end;
    m_yaw = transition_seconds > 0 ? 0.0f : m_end_yaw;
    m_pitch = transition_seconds > 0 ? m_from_pitch : m_end_pitch;
}

void WorldMapCamera::set_target(const glm::vec3& target) {
    // World::SetTarget: translation back along the pitch, then the rest is a look-at from the camera to the
    // target, so the target falls on the middle of the screen; World::Begin takes yaw and pitch from that.
    m_target = target;
    m_zoom = k_start_zoom;
    const float range = m_data.max_height - m_data.min_height;
    const float back = std::tan(glm::radians(90.0f - m_data.initial_pitch)) * (m_data.min_height + m_zoom * range);
    m_translation = glm::vec2(0.0f, -back);
    correct_bounds();
    const glm::vec2 at = glm::vec2(m_target) + m_translation;
    m_land = m_ground ? m_ground(at.x, at.y) : 0.0f;
    place();
    const glm::vec3 look = m_target - m_end;
    m_end_yaw = std::atan2(look.x, look.y);
    m_end_pitch = std::atan2(-look.z, glm::length(glm::vec2(look)));
    m_position = m_end;
    m_yaw = m_end_yaw;
    m_pitch = m_end_pitch;
}

void WorldMapCamera::update(float dt, const Input& input) {
    if (!settled()) {
        // The transition: a straight line and a slerp over the time; yaw and
        // pitch here turn about one axis unless the bounds moved the camera sideways.
        m_elapsed += dt;
        const float t = m_transition > 0 ? std::min(m_elapsed / m_transition, 1.0f) : 1.0f;
        m_position = glm::mix(m_from, m_end, t);
        m_yaw = m_end_yaw * t;
        m_pitch = m_from_pitch + (m_end_pitch - m_from_pitch) * t;
        return;
    }
    // MapCameraStates::World::Update.
    dt = std::min(dt, k_max_dt);
    // Look slows to nothing towards the ends of its range, on the way out only: half fMapWorldYawRange either side of
    // north, half the span of the pitch settings either side of the world's own pitch.
    const float tilted = m_pitch - glm::radians(m_data.initial_pitch);
    float turn = input.look[0], tilt = input.look[1];
    if (m_yaw * turn > 0.0f) {
        turn *= 1.0f - std::abs(m_yaw) / (glm::radians(k_yaw_range) * 0.5f);
    }
    if (tilted * tilt > 0.0f) {
        tilt *= 1.0f - std::abs(tilted) * 2.0f / glm::radians(k_max_pitch - k_min_pitch);
    }
    m_yaw += turn;
    m_pitch = std::clamp(m_pitch + tilt, -std::numbers::pi_v<float> * 0.5f, std::numbers::pi_v<float> * 0.5f);
    glm::vec2 pan(input.pan[0], input.pan[1]);
    if (pan != glm::vec2(0.0f)) {
        // The game's clamp to length 1 is on the frame's input, which never reaches it.
        pan *= (k_min_pan_speed + m_zoom * (k_max_pan_speed - k_min_pan_speed)) * std::max(dt, 0.0f);
        const float c = std::cos(m_yaw), s = std::sin(m_yaw);
        m_translation += glm::vec2(pan.x * c + pan.y * s, pan.y * c - pan.x * s);
        correct_bounds();
    }
    const float range = m_data.max_height - m_data.min_height;
    if (m_ground && range > 0.0f) {
        const glm::vec2 at = glm::vec2(m_target) + m_translation;
        const float delta = (m_ground(at.x, at.y) - m_land) * std::clamp(k_height_force * dt, 0.0f, 1.0f);
        m_land += delta;
        m_zoom -= delta / range;
    }
    m_zoom = std::clamp(m_zoom - input.zoom * k_zoom_speed, 0.0f, std::max(m_zoom, 1.0f));
    place();
    m_position = m_end;
}

glm::vec3 WorldMapCamera::ray(float x, float y) const {
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch), cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    const glm::vec3 forward(sy * cp, cy * cp, -sp), right(cy, -sy, 0.0f);
    const glm::vec3 up = glm::cross(right, forward);
    const float top = std::tan(glm::radians(k_fov_degrees) * 0.5f) * k_reference_aspect;
    return glm::normalize(forward + right * ((x * 2.0f - 1.0f) * top * m_aspect) + up * ((1.0f - y * 2.0f) * top));
}

WorldMapCamera::Projection WorldMapCamera::project(const glm::vec3& point) const {
    // NiCamera world-to-screen: divides by |w|, so points behind the camera mirror; the frustum is
    // tan(fov / 2) x 0.5625 high and that x aspect wide.
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch), cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    const glm::vec3 forward(sy * cp, cy * cp, -sp), right(cy, -sy, 0.0f);
    const glm::vec3 up = glm::cross(right, forward);
    const glm::vec3 v = point - m_position;
    const float depth = glm::dot(v, forward);
    const float w = std::max(std::abs(depth), 1e-3f);
    const float top = std::tan(glm::radians(k_fov_degrees) * 0.5f) * k_reference_aspect;
    Projection p;
    p.x = (glm::dot(v, right) / w / (top * m_aspect) + 1.0f) * 0.5f;
    p.y = 1.0f - (glm::dot(v, up) / w / top + 1.0f) * 0.5f;
    p.depth = depth;
    return p;
}

} // namespace spud
