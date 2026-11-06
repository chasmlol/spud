#include "engine/camera3.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spud::engine::camera3 {

namespace {

constexpr float k_pi = std::numbers::pi_v<float>;
constexpr float k_two_pi = 2.0f * k_pi;
constexpr float k_half_pi = 0.5f * k_pi;
constexpr float k_not_recovering = std::numeric_limits<float>::max();

// The camera's axes for a heading and a pitch (positive looking down): x right, y forward, z up.
struct Axes {
    glm::vec3 right, forward, up;
};
Axes axes(float heading, float pitch) {
    const float sh = std::sin(heading), ch = std::cos(heading), sp = std::sin(pitch), cp = std::cos(pitch);
    return {{ch, -sh, 0.0f}, {sh * cp, ch * cp, -sp}, {sh * sp, ch * sp, cp}};
}

// Moves `value` toward `target` by at most `step`, landing on it.
float slew(float value, float target, float step) {
    if (std::abs(step) > std::abs(target - value)) {
        return target;
    }
    return target >= value ? value + step : value - step;
}

} // namespace

void ThirdPerson::begin(const Target& t) {
    // Offset straight to its target, the zoom from the minimum toward the saved zoom (the minimum until the player
    // zooms: the game).
    m_offset_target = t.weapon_drawn ? glm::vec3(m_settings.over_shoulder_combat_pos_x,
                                                 m_settings.over_shoulder_combat_add_y,
                                                 m_settings.over_shoulder_combat_pos_z)
                                     : glm::vec3(m_settings.over_shoulder_pos_x, m_settings.over_shoulder_add_y, m_settings.over_shoulder_pos_z);
    m_offset = m_offset_target;
    if (!m_saved) {
        m_saved_zoom = m_settings.min_current_zoom;
        m_saved = true;
    }
    m_zoom_target = m_saved_zoom;
    m_zoom = m_settings.min_current_zoom;
    m_yaw_target = m_yaw = t.heading;
    m_pitch = t.pitch;
    m_recovery = k_not_recovering;
}

std::optional<ThirdPerson::Facing> ThirdPerson::set_free_rotation(bool want, bool moving, const Target& t) {
    const bool blocked = moving || t.weapon_drawn;
    const bool on = want && !blocked;
    if (on == m_free) {
        return std::nullopt;
    }
    std::optional<Facing> handed;
    if (blocked || !want) {
        // The player turns to where the camera looks.
        handed = Facing{std::fmod(t.heading + m_free_yaw + k_two_pi, k_two_pi), t.pitch - m_free_pitch};
    }
    m_free_yaw = m_free_pitch = 0;
    m_free = on;
    return handed;
}

void ThirdPerson::zoom(float notches) {
    m_zoom_target = std::clamp(m_zoom_target + notches * m_settings.mouse_wheel_zoom_increment,
                               m_settings.min_current_zoom, 1.0f);
}

void ThirdPerson::update(const Target& t, glm::vec2 look, float dt, const SphereCast& cast) {
    const Settings& s = m_settings;

    // The look input (HandleLookInput): free rotation turns the camera; looking down pulls it back.
    float view_pitch_neg; // minus the camera's pitch
    if (m_free) {
        m_free_yaw += s.free_rotation_speed * look.x * dt;
        m_free_pitch = std::clamp(m_free_pitch + s.free_rotation_speed * look.y * dt, t.pitch - k_half_pi,
                                  t.pitch + k_half_pi);
        view_pitch_neg = m_free_pitch - t.pitch;
    } else {
        view_pitch_neg = -t.pitch;
    }
    if (view_pitch_neg < 0.0f) {
        m_pitch_zoom = -view_pitch_neg * (2.0f / k_pi) * s.pitch_zoom_out_max_dist;
    }

    // Smoothing (UpdateSmoothing): zoom at a fixed rate, the yaw chasing last frame's target, the shoulder offset
    // easing exponentially.
    m_zoom = slew(m_zoom, m_zoom_target, s.mouse_wheel_zoom_speed * dt);
    if (m_yaw != m_yaw_target) {
        if (m_yaw_target - m_yaw > k_half_pi) {
            m_yaw_target -= k_two_pi;
        } else if (m_yaw - m_yaw_target > k_half_pi) {
            m_yaw_target += k_two_pi;
        }
        m_yaw = slew(m_yaw, m_yaw_target, s.chase_camera_speed * dt);
        const float max_lag = s.chase_camera_max_angle * k_pi / 180.0f;
        if (std::abs(m_yaw_target - m_yaw) > max_lag) {
            m_yaw = m_yaw_target < m_yaw ? m_yaw_target + max_lag : m_yaw_target - max_lag;
        }
        if (m_yaw > k_two_pi) {
            m_yaw = std::fmod(m_yaw, k_two_pi);
        } else if (m_yaw < 0.0f) {
            m_yaw = std::fmod(m_yaw, k_two_pi) + k_two_pi;
        }
    }
    m_offset_target = t.weapon_drawn ? glm::vec3(s.over_shoulder_combat_pos_x, s.over_shoulder_combat_add_y,
                                                 s.over_shoulder_combat_pos_z)
                                     : glm::vec3(s.over_shoulder_pos_x, s.over_shoulder_add_y, s.over_shoulder_pos_z);
    if (m_offset != m_offset_target) {
        const glm::vec3 step = s.shoulder_dolly_speed * (m_offset_target - m_offset) * dt;
        const glm::vec3 rest = m_offset_target - m_offset;
        m_offset = glm::dot(step, step) <= glm::dot(rest, rest) ? m_offset + step : m_offset_target;
    }

    // Rotation (UpdateRotation): the target yaw is the heading; free rotation adds its own and is never behind.
    m_yaw_target = t.heading;
    m_pitch = t.pitch;
    if (m_free) {
        m_yaw_target += m_free_yaw;
        m_yaw = m_yaw_target;
        m_pitch = t.pitch - m_free_pitch;
    }

    // Position (UpdatePosition): behind the pivot by the zoomed distance plus the pitch zoom, in camera axes.
    const Axes a = axes(m_yaw, m_pitch);
    const float distance = s.vanity_mode_min_dist + (s.vanity_mode_max_dist - s.vanity_mode_min_dist) * m_zoom;
    const glm::vec3 local(m_offset.x, m_offset.y - distance - m_pitch_zoom, m_offset.z);
    glm::vec3 pos = t.root + glm::vec3(0, 0, t.height) + a.right * local.x + a.forward * local.y + a.up * local.z;

    // Collision (CollideCameraPosition): a sphere from above the player's current position; a nearer hit moves
    // the camera to the hit point's projection on that line. After a collision the camera eases back out.
    bool collided = false;
    if (cast) {
        const glm::vec3 origin = t.position + glm::vec3(0, 0, t.height);
        if (const auto hit = cast(origin, pos, s.camera_caster_size)) {
            const glm::vec3 to_origin = origin - pos;
            const float free_d2 = glm::dot(to_origin, to_origin);
            const glm::vec3 hit_d = origin - *hit;
            if (glm::dot(hit_d, hit_d) < free_d2) {
                const float len = std::sqrt(free_d2);
                const glm::vec3 dir = len <= 1e-6f ? glm::vec3(0.0f) : to_origin / len;
                pos += glm::dot(*hit - pos, dir) * dir;
                collided = true;
            }
        }
    }
    if (collided) {
        m_collided = pos;
        m_recovery = 0;
    } else if (m_recovery != k_not_recovering) {
        const glm::vec3 d = pos - m_collided;
        if (glm::dot(d, d) <= s.collision_recovery_min_dist) {
            m_recovery = k_not_recovering;
        } else {
            m_recovery = std::min(m_recovery + s.collision_recovery_speed * dt, 1.0f);
            if (m_recovery >= 1.0f) {
                m_recovery = k_not_recovering;
            } else {
                pos = m_collided + m_recovery * d;
            }
        }
    }
    m_position = pos;
}

} // namespace spud::engine::camera3
