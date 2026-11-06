#pragma once

// Gamebryo's math lookup tables as the game builds them at start-up.

#include <array>
#include <cstdint>

namespace spud::fx {

struct MathTables {
    std::array<float, 512> sin{}; // g_SinTable512: sin(x_i), x_i = x_{i-1} + 0.012271847f (float sum)
    std::array<float, 512> cos{}; // g_CosTable512
    std::array<uint32_t, 256> sqrt_mantissa{}; // g_FastSqrtTable: [i] sqrt(1+i/128), [128+i] sqrt(2(1+i/128))
};

const MathTables& math_tables();

// Table lookup used for particle emission angles: (int)(angle * 81.487328) & 511.
inline uint32_t angle_index(float angle) {
    return static_cast<uint32_t>(static_cast<int>(angle * 81.487328f)) & 0x1FF;
}

// NiFastSqrt as inlined in the game: exponent halved, 7 mantissa bits looked up.
float fast_sqrt(float x);

} // namespace spud::fx
