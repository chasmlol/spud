#pragma once

// Which placed lights reach an object, in the order the game hands them to its shader
// The lists are built from every light of the world, not from the
// loaded ones, so an object's list never depends on streaming history.

#include "formats/osw.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace spud::render {

inline constexpr uint32_t k_light_list_size = 8; // u16 light indices per object (the shader uses the first 7 lit)
inline constexpr uint16_t k_no_light = 0xFFFF;
// Slots after World::lights in the frame's light array, in every list with room: what moves through the world (a
// projectile's light, an explosion's), which no list built at load could name. An unlit slot costs the shader nothing but
// the step past it.
inline constexpr uint32_t k_moving_lights = 2;

class LightGrid {
public:
    explicit LightGrid(const osw::World& world);

    // The lights reaching the sphere (|P - C| - R < r), as World::lights indices packed two per u32 (low half
    // first), k_no_light padded. Returns how many lights reach it (more than k_light_list_size are dropped).
    // `unlit`, sorted, are lights that stay out whatever their reach.
    uint32_t list(const glm::vec3& centre, float radius, bool landscape, uint32_t out[4],
                  std::span<const uint16_t> unlit = {}) const;

private:
    struct Candidate {
        uint16_t light;
        float k; // (|P - C| - R) / r
    };
    const osw::World& m_world;
    int32_t m_min[2] = {0, 0};
    int32_t m_size[2] = {0, 0};
    float m_cell_size = 1.0f;
    std::vector<std::vector<uint16_t>> m_cells; // lights whose sphere touches each grid cell
};

} // namespace spud::render
