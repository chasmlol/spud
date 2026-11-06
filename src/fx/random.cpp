#include "fx/random.hpp"

#include <bit>

namespace spud::fx {

void BsRandom::seed(uint32_t s, CrtRandom& crt) {
    // The game: mt[i] = 1812433253 * (mt[i-1] ^ (i + (mt[i-1] >> 30))), index 623, srand(s).
    m_mt[0] = s;
    for (uint32_t i = 1; i < 624; ++i) {
        m_mt[i] = 1812433253u * (m_mt[i - 1] ^ (i + (m_mt[i - 1] >> 30)));
    }
    m_mt[624] = 623;
    crt.seed(s);
    m_seeded = true;
}

uint32_t BsRandom::next(uint32_t n) {
    // The game seeds lazily from _time64(); callers seed explicitly before use, so an unseeded generator
    // here behaves as if seeded with 0 (deterministic, never time-based).
    if (!m_seeded) {
        CrtRandom unused;
        seed(0, unused);
    }
    if (n == 0) {
        return 0;
    }
    uint32_t index = m_mt[624];
    if (index == 0) {
        for (uint32_t i = 0; i < 624; ++i) {
            uint32_t r9 = m_mt[i + 1] % 624u + (m_mt[i] & 1u);
            uint32_t v = m_mt[(i + 397) % 624] ^ (r9 * 2u);
            if (r9 & 1u) {
                v ^= 0x9908B0DFu;
            }
            m_mt[i] = v;
        }
        m_mt[624] = 0;
    }
    uint32_t y = m_mt[index];
    m_mt[624] = (index + 1) % 624;
    y ^= y >> 11;
    y ^= (y & 0xFF3A58ADu) << 7;
    y ^= (y & 0xFFFFDF8Cu) << 15;
    y ^= y >> 18;
    if (((n + 1) & 0xFFFF7FFFu) != 0) {
        return y % n;
    }
    return y & n;
}

float BsRandom::range(float lo, float hi) {
    uint32_t y = next(0xFFFFFFFFu);
    return static_cast<float>(static_cast<uint64_t>(y)) * std::bit_cast<float>(0x2F800000u) * (hi - lo) + lo;
}

uint64_t BsRandom::hash() const {
    uint64_t h = 1469598103934665603ull;
    for (auto v : m_mt) {
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

} // namespace spud::fx
