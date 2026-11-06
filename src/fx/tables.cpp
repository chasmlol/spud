#include "fx/tables.hpp"

#include <bit>
#include <cmath>

namespace spud::fx {

namespace {

MathTables build() {
    MathTables t;
    // The game: 512 steps of a float accumulator. The game evaluates both with
    // its own vectorised sincos; this uses correctly rounded double-precision values (see the design).
    float x = 0.0f;
    for (size_t i = 0; i < 512; ++i) {
        t.sin[i] = static_cast<float>(std::sin(static_cast<double>(x)));
        t.cos[i] = static_cast<float>(std::cos(static_cast<double>(x)));
        x += 0.012271847f;
    }
    // The game: sqrtss of exact inputs, so std::sqrt(float) matches bit for bit.
    for (uint32_t i = 0; i < 128; ++i) {
        t.sqrt_mantissa[i] =
            std::bit_cast<uint32_t>(std::sqrt(std::bit_cast<float>((i | 0x3F80u) << 16))) & 0x7FFFFF;
        t.sqrt_mantissa[128 + i] =
            std::bit_cast<uint32_t>(std::sqrt(std::bit_cast<float>((i | 0x4000u) << 16))) & 0x7FFFFF;
    }
    return t;
}

} // namespace

const MathTables& math_tables() {
    static const MathTables tables = build();
    return tables;
}

float fast_sqrt(float x) {
    uint32_t bits = std::bit_cast<uint32_t>(x);
    uint32_t e = (bits >> 23) & 0xFFFF;
    uint32_t m = bits & 0x7FFFFF;
    uint32_t index_bits = ((e - 1) & 1) == 0 ? m : (m | 0x800000);
    uint32_t exponent = ((((e << 22) - 532676608u) & 0xFF800000u) + 0x3F800000u);
    return std::bit_cast<float>(exponent | math_tables().sqrt_mantissa[index_bits >> 16]);
}

} // namespace spud::fx
