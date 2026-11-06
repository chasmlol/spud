#pragma once

// An effect shader's particles: slots born on an actor's bones and left in the
// world, where the game's Particle vertex shader works out each one's place, size and colour from its age.

#include "render/gpu_types.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::engine {

// What an EFSH says of its particles, in game units, seconds and radians.
struct ParticleLook {
    std::string texture, palette; // ICO2, NAM9, cache paths
    uint32_t technique = 0;
    uint32_t blend_src = 6, blend_dst = 0;
    float lifetime = 1, lifetime_range = 0;
    float speed = 0, speed_range = 0, acceleration_along_normal = 0;
    glm::vec3 velocity{0.0f}, acceleration{0.0f};
    float scale_key[2] = {1, 1}, scale_time[2] = {0, 1};
    std::array<glm::vec4, 3> colours{};
    std::array<float, 3> colour_times{};
    float colour_scale = 1;
    float rotation = 0, rotation_range = 0, rotation_speed = 0, rotation_speed_range = 0;
    float birth_offset = 0, birth_offset_range = 0;
    uint32_t cells_u = 1, cells_v = 1;
    bool animated = false;
    uint32_t start_frame = 0, start_variation = 0, end_frame = 0, loop_start = 0, loop_variation = 0;
    uint32_t frame_count = 0, frame_variation = 0;
};

// A line of the skeleton, world space, game units: its birth points lie on it, its rotation turns their directions.
struct BoneSegment {
    glm::vec3 from{0.0f}, to{0.0f};
    glm::mat3 rotation{1.0f};
};

class ShaderParticles {
public:
    ShaderParticles(const ParticleLook& look, uint32_t seed, uint32_t slots);
    // `target` is how many may live at once.
    void step(float dt, uint32_t target, std::span<const BoneSegment> bones);
    // What the Particle vertex shader makes of each live slot, in metres.
    void collect(std::vector<render::GpuParticle>& out, uint32_t system) const;
    bool empty() const { return m_live == 0; }
    glm::vec3 centre() const { return m_centre; }

private:
    struct Slot {
        glm::vec3 position{0.0f}, direction{0.0f};
        float age = -1.0f; // below 0: free
        float age_mult = 1, speed_mult = 1, rotation = 0, rotation_speed = 0;
        float frame = 0;
    };
    float random(float low, float high);
    void birth(Slot& s, std::span<const BoneSegment> bones);

    ParticleLook m_look;
    std::vector<Slot> m_slots;
    uint32_t m_state;
    float m_budget = 0.5f;
    uint32_t m_live = 0;
    glm::vec3 m_centre{0.0f};
};

} // namespace spud::engine
