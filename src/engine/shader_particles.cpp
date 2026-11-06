#include "engine/shader_particles.hpp"

#include "core/units.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace spud::engine {

namespace {

// The quad's corners go into clip space through 1 / (tan x 1.3032254), as the weather's particles do.
constexpr float k_quad_scale = 1.0f / 1.3032254f;

} // namespace

ShaderParticles::ShaderParticles(const ParticleLook& look, uint32_t seed, uint32_t slots) : m_look(look), m_state(seed | 1u) {
    m_slots.resize(slots);
    // fixed per slot, not drawn again at a rebirth.
    const float life = std::max(look.lifetime, 1e-3f);
    const float speed = std::abs(look.speed) > 1e-6f ? look.speed : 1.0f;
    for (Slot& s : m_slots) {
        s.age_mult = 1.0f + random(-1.0f, 1.0f) * look.lifetime_range / life;
        s.speed_mult = 1.0f + random(-1.0f, 1.0f) * look.speed_range / speed;
        s.rotation = look.rotation + random(-1.0f, 1.0f) * look.rotation_range;
        s.rotation_speed = look.rotation_speed + random(-1.0f, 1.0f) * look.rotation_speed_range;
    }
}

float ShaderParticles::random(float low, float high) {
    m_state ^= m_state << 13;
    m_state ^= m_state >> 17;
    m_state ^= m_state << 5;
    return low + (high - low) * (float(m_state >> 8) / 16777216.0f);
}

// _GetBirth: a point on the line from a node's parent to it, going out round the bone (x, 0, z).
void ShaderParticles::birth(Slot& s, std::span<const BoneSegment> bones) {
    const BoneSegment& b = bones[std::min(size_t(random(0.0f, float(bones.size()))), bones.size() - 1)];
    const float t = random(0.0f, 1.0f);
    glm::vec3 around(random(-1.0f, 1.0f), 0.0f, random(-1.0f, 1.0f));
    const float length = glm::length(around);
    around = length > 1e-5f ? around / length : glm::vec3(1.0f, 0.0f, 0.0f);
    s.direction = b.rotation * around;
    s.position = b.from + (b.to - b.from) * t + s.direction * (m_look.birth_offset + (random(0.0f, 1.0f) - 0.5f) * m_look.birth_offset_range);
    s.age = 0.0f;
    s.frame = float(m_look.start_frame) + random(0.0f, float(m_look.start_variation));
}

void ShaderParticles::step(float dt, uint32_t target, std::span<const BoneSegment> bones) {
    const float life = std::max(m_look.lifetime, 1e-3f);
    target = std::min<uint32_t>(target, uint32_t(m_slots.size()));
    m_budget += std::abs(dt) / life * float(target);
    int budget = int(m_budget);
    m_budget -= float(budget);
    m_live = 0;
    glm::vec3 sum(0.0f);
    for (const Slot& s : m_slots) {
        m_live += s.age >= 0.0f;
    }
    uint32_t live = 0;
    for (Slot& s : m_slots) {
        if (s.age >= 0.0f) {
            s.age += dt;
            if (s.age * s.age_mult > life) {
                s.age = -1.0f;
                --m_live;
            } else if (m_look.animated) {
                s.frame += float(m_look.frame_count) / life * dt;
                if (s.frame > float(m_look.end_frame)) {
                    s.frame = float(m_look.loop_start) + random(0.0f, float(m_look.loop_variation));
                }
            }
        } else if (budget > 0 && m_live < target) {
            --budget; // a failed emission spends it too
            if (!bones.empty()) {
                birth(s, bones);
                ++m_live;
            }
        }
        if (s.age >= 0.0f) {
            sum += s.position;
            ++live;
        }
    }
    m_centre = live ? sum / float(live) : m_centre;
}

void ShaderParticles::collect(std::vector<render::GpuParticle>& out, uint32_t system) const {
    const ParticleLook& l = m_look;
    const float life = std::max(l.lifetime, 1e-3f);
    float system_bits;
    std::memcpy(&system_bits, &system, 4);
    for (const Slot& s : m_slots) {
        if (s.age < 0.0f) {
            continue;
        }
        const float a = s.age * s.age_mult, t = a / life;
        // The Particle vertex shader, as designed: scale from 0 by two keys back to 0.
        float low = 0.0f, high = l.scale_key[0], from = 0.0f, to = l.scale_time[0];
        if (t > l.scale_time[1]) {
            low = l.scale_key[1], high = 0.0f, from = l.scale_time[1], to = 1.0f;
        } else if (t > l.scale_time[0]) {
            low = l.scale_key[0], high = l.scale_key[1], from = l.scale_time[0], to = l.scale_time[1];
        }
        const float scale = low + (t - from) / std::max(to - from, 1e-6f) * (high - low);
        // Colour by three keys; alpha from 0 at the start, and to 0 at the end.
        const auto& c = l.colours;
        const auto& k = l.colour_times;
        glm::vec4 first = c[0], last = c[0];
        float start = 0.0f, end = k[0];
        first.a = 0.0f;
        if (t > k[2]) {
            first = c[2], last = glm::vec4(glm::vec3(c[2]), 0.0f), start = k[2], end = 1.0f;
        } else if (t > k[1]) {
            first = c[1], last = c[2], start = k[1], end = k[2];
        } else if (t > k[0]) {
            first = c[0], last = c[1], start = k[0], end = k[1];
        }
        const glm::vec4 colour = first + (last - first) * ((t - start) / std::max(end - start, 1e-6f));
        const glm::vec3 place = s.position + (s.direction * (l.speed * s.speed_mult) + l.velocity) * a +
                                (s.direction * l.acceleration_along_normal + l.acceleration) * (a * a * 0.5f);
        render::GpuParticle q{};
        q.position = glm::vec4(place * units::k_skyrim_unit_m, s.rotation + s.rotation_speed * s.age);
        q.size_system = glm::vec4(scale * k_quad_scale * units::k_skyrim_unit_m, system_bits, 0.0f, 0.0f);
        q.color = colour;
        const uint32_t cells = std::max(l.cells_u * l.cells_v, 1u);
        const uint32_t frame = uint32_t(s.frame) % cells;
        const float du = 1.0f / float(std::max(l.cells_u, 1u)), dv = 1.0f / float(std::max(l.cells_v, 1u));
        q.uv = glm::vec4(float(frame % std::max(l.cells_u, 1u)) * du, du, float(frame / std::max(l.cells_u, 1u)) * dv, dv);
        out.push_back(q);
    }
}

} // namespace spud::engine
