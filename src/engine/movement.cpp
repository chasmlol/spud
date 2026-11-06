#include "engine/movement.hpp"

#include "core/units.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace spud::engine::movement {

Settings Settings::from_rules(const osr::Rules& rules) {
    Settings s;
    auto take = [&](const char* name, float& out) {
        if (auto v = rules.setting(name)) {
            out = static_cast<float>(*v);
        }
    };
    take("fJumpHeightMin", s.jump_height_min);
    take("fJumpSwimmingMult", s.jump_swimming_mult);
    take("fJumpMoveBase", s.jump_move_base);
    take("fJumpMoveMult", s.jump_move_mult);
    take("fJumpFallHeightMin", s.jump_fall_height_min);
    take("fJumpFallHeightMult", s.jump_fall_height_mult);
    take("fJumpFallHeightExponent", s.jump_fall_height_exponent);
    take("fMoveWeightMin", s.move_weight_min);
    take("fMoveWeightMax", s.move_weight_max);
    take("fMoveEncumEffect", s.move_encum_effect);
    take("fMoveEncumEffectNoWeapon", s.move_encum_effect_no_weapon);
    take("fSprintStaminaDrainMult", s.sprint_stamina_drain_mult);
    take("fSprintStaminaWeightBase", s.sprint_stamina_weight_base);
    take("fSprintStaminaWeightMult", s.sprint_stamina_weight_mult);
    take("fOutOfBreathStaminaRegenDelay", s.out_of_breath_stamina_regen_delay);
    take("fDamagedStaminaRegenDelay", s.damaged_stamina_regen_delay);
    take("fStaminaRegenDelayMax", s.stamina_regen_delay_max);
    take("fDamagedHealthRegenDelay", s.damaged_health_regen_delay);
    take("fHealthRegenDelayMax", s.health_regen_delay_max);
    take("fCombatStaminaRegenRateMult", s.combat_stamina_regen_rate_mult);
    take("fJogInterpolationBetweenWalkAndRun", s.jog_interpolation);
    take("fActorSwimBreathBase", s.actor_swim_breath_base);
    take("fActorSwimBreathMult", s.actor_swim_breath_mult);
    take("fActorSwimBreathDamage", s.actor_swim_breath_damage);
    take("fWaterKnockdownVelocity", s.water_knockdown_velocity);
    take("fWaterKnockdownSizeNormal", s.water_knockdown_size_normal);
    take("fJumpFallVelocityMin", s.jump_fall_velocity_min);
    return s;
}

bool can_run(const Body& body) {
    return body.inventory_weight <= body.carry_weight;
}

Speeds actor_speeds(const osr::MovementType& type, const Body& body, const Settings& s) {
    // The movement type's speeds (metres in the rules file) back in units, times scale and SpeedMult; run entries
    // lose speed with equipped weight but never drop below walking; an actor that can't run walks.
    const float k = body.scale * (body.speed_mult == 0 ? 1.0f : std::abs(body.speed_mult) / 100.0f) /
                    units::k_skyrim_unit_m;
    const float effect = body.weapon_drawn ? s.move_encum_effect : s.move_encum_effect_no_weapon;
    const float span = std::max(s.move_weight_max - s.move_weight_min, 1e-3f);
    const float e = 1.0f - (std::min(s.move_weight_max, body.equipped_weight) + s.move_weight_min) / span * effect;
    const bool run = can_run(body);
    Speeds out;
    auto pair = [&](const float in[2], float o[2]) {
        o[0] = in[0] * k;
        o[1] = run ? std::max(e * in[1] * k, o[0]) : o[0];
    };
    pair(type.left, out.left);
    pair(type.right, out.right);
    pair(type.forward, out.forward);
    pair(type.back, out.back);
    out.rotate_in_place[0] = type.rotate_in_place[0];
    out.rotate_in_place[1] = run ? type.rotate_in_place[1] : type.rotate_in_place[0];
    out.rotate_while_moving = type.rotate_while_moving;
    return out;
}

namespace {

float move_graph(float a, const Settings& s) {
    glm::vec2 prev(0.0f);
    for (const glm::vec2& p : s.move_graph) {
        if (a <= p.x) {
            return prev.y + (p.y - prev.y) * (a - prev.x) / std::max(p.x - prev.x, 1e-6f);
        }
        prev = p;
    }
    return prev.y + (1.0f - prev.y) * (a - prev.x) / std::max(1.0f - prev.x, 1e-6f);
}

} // namespace

Motion requested_motion(const Speeds& sp, glm::vec2 move, bool run, bool gamepad, const Settings& s) {
    const bool can_strafe = sp.left[0] > 0 && sp.left[1] > 0 && sp.right[0] > 0 && sp.right[1] > 0;
    if (!can_strafe) {
        move.x = 0;
    }
    const float m = glm::length(move);
    if (m < 1e-6f) {
        return {};
    }
    const glm::vec2 u = move / m;
    const float a = std::min(m, 1.0f);
    const float g = gamepad ? move_graph(a, s) : a;
    const float reference = g * ((run || gamepad) ? sp.forward[1] : sp.forward[0]);
    // Only the run column caps each axis; the walk column enters through the reference speed.
    const float side_cap = u.x >= 0 ? sp.right[1] : sp.left[1];
    const float fore_cap = u.y >= 0 ? sp.forward[1] : sp.back[1];
    const float speed = (std::min(side_cap, reference * std::abs(u.x)) + std::min(fore_cap, reference * std::abs(u.y))) /
                        (std::abs(u.x) + std::abs(u.y));
    Motion out;
    out.speed = speed <= 5.0f ? 0.0f : speed;
    out.direction = std::atan2(u.x, u.y);
    return out;
}

std::optional<glm::vec3> water_current(std::span<const osw::CurrentQuad> quads, const glm::vec3& p) {
    glm::vec3 acc(0.0f);
    bool hit = false;
    for (size_t i = 0; i + 2 < quads.size();) {
        const auto& q0 = quads[i].v;
        const auto& q1 = quads[i + 1].v;
        const auto& q2 = quads[i + 2].v;
        int32_t type = 0;
        std::memcpy(&type, &q2[3], 4);
        const size_t size = (type == 2 || type == 3) ? 4 : 3;
        if (type == 0) {
            acc += glm::vec3(q0[0], q0[1], q0[2]);
            hit = true;
        } else if (i + size <= quads.size()) {
            // Zones: only the horizontal distance counts; the weight falls from 1 at the centre to k at the edge.
            glm::vec2 nearest(q2[0], q2[1]);
            float radius = q2[2];
            bool inside = true;
            if (type == 2) {
                const glm::vec2 a(q2[0], q2[1]), b(quads[i + 3].v[0], quads[i + 3].v[1]);
                const glm::vec2 ab = b - a;
                const float len2 = glm::dot(ab, ab);
                const float f = len2 > 0 ? std::clamp(glm::dot(glm::vec2(p) - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
                nearest = a + f * ab;
            } else if (type == 3) {
                const auto& q3 = quads[i + 3].v;
                radius = q3[3];
                inside = glm::dot(p - glm::vec3(q2[0], q2[1], q2[2]), glm::vec3(q3[0], q3[1], q3[2])) <= 0.0f;
            }
            const glm::vec2 off = glm::vec2(p) - nearest;
            const float d = glm::length(off);
            if (inside && radius > 0 && d <= radius) {
                const float k = q1[3];
                const float w = k + (1.0f - k) * (1.0f - d / radius);
                glm::vec3 u(q0[0], q0[1], q0[2]);
                if (q0[3] != 0.0f) { // polar: tangential (clockwise seen from above) and radial speeds
                    const glm::vec2 r = d > 0 ? off / d : glm::vec2(0.0f);
                    const glm::vec2 t(r.y, -r.x);
                    u = glm::vec3(q0[0] * t + q0[1] * r, q0[2]);
                }
                acc = w * u + (1.0f - w) * acc;
                hit = true;
            }
        }
        i += size;
    }
    if (!hit) {
        return std::nullopt;
    }
    return acc;
}

glm::vec3 applied_current(const glm::vec3& current_ms, const glm::vec3& move, float run_speed, bool swimming,
                          bool falling_fast, const Settings& s) {
    const float threshold = s.water_knockdown_velocity * s.water_knockdown_size_normal;
    const float strength = glm::length(current_ms);
    if (!swimming && !falling_fast && strength <= threshold) {
        return glm::vec3(0.0f);
    }
    glm::vec3 c = current_ms / units::k_skyrim_unit_m;
    if (strength <= threshold) {
        const glm::vec3 total = move + c;
        const float speed = glm::length(total);
        if (speed > run_speed && speed > 0) {
            c.x = total.x / speed * run_speed - move.x;
            c.y = total.y / speed * run_speed - move.y;
        }
    }
    return c;
}

glm::vec2 stick_deadzone(glm::vec2 stick, float deadzone, float deadzone_max) {
    stick = glm::clamp(glm::round(stick * 32767.0f) * 0.000030518509f, -1.0f, 1.0f);
    const float len = glm::length(stick);
    if (len <= 0.0f) {
        return glm::vec2(0.0f);
    }
    return stick * (std::clamp((len - deadzone) / (deadzone_max - deadzone), 0.0f, 1.0f) / len);
}

glm::vec2 gamepad_look(glm::vec2 s, float dt, float& full_time, const Settings& c) {
    if (c.invert_y) {
        s.y = -s.y;
    }
    if (c.look_maxed_out_acceleration) {
        full_time = std::abs(glm::dot(s, s) - 1.0f) > 0.0001f ? 0.0f : full_time + dt;
    }
    if (c.look_snap_to_axis && c.look_snap_strength > 0 && c.look_snap_strength < 1) {
        const float f = c.look_snap_strength;
        const glm::vec2 in = s;
        for (int i = 0; i < 2; ++i) {
            const float other = std::abs(in[1 - i]);
            const float d = (1.0f - std::max((f - other) / f, 0.0f)) * f;
            const float sign = in[i] >= 0 ? 1.0f : -1.0f;
            s[i] = std::max(std::abs(in[i]) - d, 0.0f) * sign / (1.0f - d);
        }
    }
    const float r = glm::length(s);
    if (r > 0) {
        s /= r;
    }
    const float r2 = r * r;
    s *= (c.look_graph[3] * r2 + c.look_graph[2] * r) * r2 + (c.look_graph[1] * r2 + c.look_graph[0] * r);
    if (r > 0) {
        s *= std::pow(r, c.look_mult_exponent);
    }
    if (c.look_maxed_out_acceleration && full_time > 0) {
        const float k = c.gamepad_sensitivity_default / c.gamepad_sensitivity;
        const float ta = c.look_accel_sec * k;
        const float ramp = std::min(full_time, ta) / ta;
        s.x *= (std::max(c.look_accel_yaw_mult * k, 1.0f) - 1.0f) * ramp + 1.0f;
        s.y *= (std::max(c.look_accel_pitch_mult * k, 1.0f) - 1.0f) * ramp + 1.0f;
    }
    float sens = c.gamepad_sensitivity;
    if (c.look_sensitivity_threshold && c.look_curve_threshold > 0) {
        sens = std::clamp((r - c.look_curve_threshold) / (1.0f - c.look_curve_threshold), 0.0f, 1.0f) *
                   (c.gamepad_sensitivity - 1.0f) + 1.0f;
    }
    s *= sens;
    s.x *= c.gamepad_x_scale;
    s.y *= c.gamepad_y_scale;
    if (c.look_time_normalize) {
        s.y *= std::min(1.0f / c.look_floor_framerate, dt) / (1.0f / c.look_target_framerate);
    }
    return s;
}

MoveBuffer::Out MoveBuffer::push(float direction, float speed, float dt, const Settings& s) {
    constexpr unsigned n = k_size;
    auto wrapped = [](float a) { return std::remainder(a, 2.0f * std::numbers::pi_v<float>); };
    const int newest = m_count > 0 ? int((m_tail + m_count - 1) % n) : -1;
    if (speed < 5.0f && newest >= 0) {
        direction = m_samples[newest].direction;
    }
    // A sample is "changing" when it moves faster than the thresholds over the sample threshold allow
    // (10 degrees or 16.7 units/s per update at 60 Hz with the defaults).
    const float angle_threshold = glm::radians(s.freeze_direction_angle);
    bool changing = false;
    if (newest >= 0 && s.controller_sample_threshold > 0) {
        const Sample& last = m_samples[newest];
        changing = std::abs(wrapped(direction - last.direction)) >= angle_threshold / s.controller_sample_threshold * dt ||
                   std::abs(speed - last.speed) >= s.freeze_direction_speed / s.controller_sample_threshold * dt;
    }
    if (changing && !m_changing) {
        m_change_start = newest; // the last sample before the change
    } else if (!changing && m_changing) {
        m_change_end = newest; // the last changing sample
        m_check = true;
    }
    m_changing = changing;

    bool freeze_end = false;
    auto emit = [&](Sample& e) {
        if (!(e.flags & 16)) {
            freeze_end = freeze_end || (e.flags & 2);
            e.flags |= 16;
        }
    };
    if (m_count == n) { // full: the oldest goes
        emit(m_samples[m_tail]);
        m_time -= m_samples[m_tail].dt;
        m_tail = (m_tail + 1) % n;
        --m_count;
    }
    m_samples[(m_tail + m_count) % n] = {direction, speed, dt, 0};
    ++m_count;
    m_time += dt;
    while (m_count > 1 && m_time - m_samples[m_tail].dt > s.controller_buffer_depth) {
        emit(m_samples[m_tail]);
        m_time -= m_samples[m_tail].dt;
        m_tail = (m_tail + 1) % n;
        --m_count;
    }
    if (m_check) {
        m_check = false;
        if (m_change_start >= 0 && m_change_end >= 0) {
            Sample& a = m_samples[m_change_start];
            Sample& b = m_samples[m_change_end];
            if (std::abs(wrapped(b.direction - a.direction)) > angle_threshold) {
                a.flags |= 1;
                b.flags |= 2;
            }
            if (std::abs(b.speed - a.speed) > s.freeze_direction_speed) {
                a.flags |= 4;
                b.flags |= 8;
            }
        }
    }
    Sample& out = m_samples[m_tail];
    emit(out);
    Out o;
    o.direction = out.direction;
    o.velocity = glm::vec2(std::sin(out.direction), std::cos(out.direction)) * out.speed;
    o.freeze_end = freeze_end && s.freeze_direction_on_large_delta;
    return o;
}

float jog_speed(const Speeds& sp, const Settings& s) {
    const float k = 0.5f * (s.jog_interpolation + 1.0f);
    return (1.0f - k) * sp.forward[0] + k * sp.forward[1];
}

float sprint_drain_per_second(float equipped_weight, const Settings& s) {
    return (s.sprint_stamina_weight_mult * equipped_weight + s.sprint_stamina_weight_base) *
           s.sprint_stamina_drain_mult;
}

float regen_per_second(float max_value, float rate, float rate_mult, bool in_combat,
                               const Settings& s) {
    return max_value * rate / 100.0f * rate_mult / 100.0f *
           (in_combat ? s.combat_stamina_regen_rate_mult : 1.0f);
}

float fall_damage(float distance, const Settings& s) {
    if (distance <= s.jump_fall_height_min) {
        return 0.0f;
    }
    return std::pow((distance - s.jump_fall_height_min) * s.jump_fall_height_mult, s.jump_fall_height_exponent);
}

float jump_speed(float scale, bool swimming, float gravity_units, const Settings& s) {
    const float h = s.jump_height_min * scale * (swimming ? s.jump_swimming_mult : 1.0f);
    return std::sqrt(2.0f * gravity_units * h);
}

float air_control_gain(const Settings& s) {
    return s.jump_move_base + 1.0f * s.jump_move_mult;
}

glm::vec2 mouse_look(glm::vec2 counts, float frame_dt, const Settings& s) {
    if (frame_dt <= 0) {
        return glm::vec2(0.0f);
    }
    const float dy = s.invert_y ? counts.y : -counts.y;
    return {counts.x * s.mouse_heading_sensitivity * s.mouse_heading_x_scale / frame_dt,
            dy * s.mouse_heading_sensitivity * s.mouse_heading_y_scale};
}

float pitch_delta(float look_y, float fov_deg, const Settings& s) {
    return -look_y * s.looking_speed * std::min(1.0f, fov_deg / s.default_world_fov);
}

} // namespace spud::engine::movement
