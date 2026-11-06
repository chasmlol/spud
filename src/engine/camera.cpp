#include "engine/camera.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spud::engine {

namespace {

constexpr float k_max_pitch = glm::radians(89.5f);

float wrap_angle(float a) {
    const float two_pi = 2.0f * std::numbers::pi_v<float>;
    a = std::fmod(a, two_pi);
    return a < 0 ? a + two_pi : a;
}

} // namespace

void FreeCamera::set_pitch(float p) { pitch = std::clamp(p, -k_max_pitch, k_max_pitch); }

glm::vec3 FreeCamera::forward() const {
    return {std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
}

void FreeCamera::update(const CameraInput& in, float dt) {
    yaw = wrap_angle(yaw + in.look_dx * sensitivity);
    set_pitch(pitch - in.look_dy * sensitivity);
    if (in.wheel != 0) {
        base_speed = std::clamp(base_speed * std::pow(1.2f, in.wheel), 0.5f, 2000.0f);
    }
    const glm::vec3 fwd = forward();
    const glm::vec3 right(std::cos(yaw), -std::sin(yaw), 0.0f);
    glm::vec3 move = fwd * in.forward + right * in.right + glm::vec3(0, 0, 1) * in.up;
    if (glm::dot(move, move) > 1.0f) {
        move = glm::normalize(move);
    }
    const float speed = base_speed * (in.fast ? fast_multiplier : 1.0f);
    position += move * speed * dt;
}

void FirstPersonLag::begin(const glm::vec3& feet) {
    m_reference = feet;
    m_velocity = glm::vec3(0);
    m_weight = 1.0f;
}

glm::vec3 FirstPersonLag::step(const glm::vec3& feet, float dt, bool fade_out, const Settings& s) {
    constexpr float k_reset_distance = 300.0f, k_max_velocity = 500.0f, k_weight_rate = 1.5f;
    const glm::vec3 behind = m_reference - feet;
    const bool reset = glm::length(behind) > k_reset_distance;
    m_weight = fade_out || glm::length(m_velocity) > k_max_velocity ? std::max(m_weight - dt * k_weight_rate, 0.0f)
                                                                    : std::min(m_weight + dt * k_weight_rate, 1.0f);
    glm::vec3 lag(0);
    if (!reset) {
        const float horz = std::pow(s.horz_spring, dt), vert = std::pow(s.vert_spring, dt);
        lag = m_weight * (glm::vec3(behind.x * horz, behind.y * horz, behind.z * vert) +
                          dt * glm::vec3(s.horz_velocity, s.horz_velocity, s.vert_velocity) * m_velocity);
    }
    m_reference = feet + lag;
    m_velocity = reset || dt <= 0.0f ? glm::vec3(0) : (lag - behind) / dt;
    return lag;
}

render::View FreeCamera::view(float aspect) const {
    // The game adds the offset to its own FOV, which is horizontal at 16:9.
    const float fov_16_9 = 2.0f * std::atan(std::tan(fov_y * 0.5f) / 0.5625f) + glm::radians(fov_offset);
    const float shown_fov_y = fov_offset != 0 ? 2.0f * std::atan(std::tan(fov_16_9 * 0.5f) * 0.5625f) : fov_y;
    render::View v = render::View::make(position, yaw, pitch, shown_fov_y, aspect, 0.1f);
    if (sway != glm::mat3(1.0f)) {
        v.turn(glm::mat4(sway));
    }
    if (shake != glm::vec3(0)) {
        // View space looks down -z with y up: forward's roll turns the other way about z.
        v.turn(glm::rotate(glm::mat4(1), -shake.x, glm::vec3(1, 0, 0)) * glm::rotate(glm::mat4(1), shake.y, glm::vec3(0, 0, 1)) *
               glm::rotate(glm::mat4(1), -shake.z, glm::vec3(0, 1, 0)));
    }
    return v;
}

void FreeCamera::set_skyrim_angles(float heading_deg, float pitch_deg) {
    yaw = wrap_angle(glm::radians(heading_deg));
    set_pitch(-glm::radians(pitch_deg));
}

float FreeCamera::skyrim_heading_deg() const { return glm::degrees(yaw); }
float FreeCamera::skyrim_pitch_deg() const { return -glm::degrees(pitch); }

} // namespace spud::engine
