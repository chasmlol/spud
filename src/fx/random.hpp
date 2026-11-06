#pragma once

// The two random number generators Skyrim's particle code uses.
// Each effect instance owns its own pair, seeded from one value, so a run is reproducible.

#include <array>
#include <cstdint>

namespace spud::fx {

// The C runtime's rand as the game calls it (__imp_rand, MSVC): a 32-bit LCG, 15-bit results.
class CrtRandom {
public:
    static constexpr int k_max = 0x7FFF;

    explicit CrtRandom(uint32_t seed = 1) : m_state(seed) {}
    void seed(uint32_t s) { m_state = s; }

    int next() {
        m_state = m_state * 214013u + 2531011u;
        return static_cast<int>((m_state >> 16) & 0x7FFF);
    }

    // The game's spellings: rand() * (1/32767) in [0, 1], rand() * (2/32767) - 1 in [-1, 1].
    float unit() { return static_cast<float>(next()) * 0.000030518509f; }
    float symmetric() { return static_cast<float>(next()) * 0.000061037019f - 1.0f; }

    uint32_t state() const { return m_state; }

private:
    uint32_t m_state;
};

// The engine's own generator (global): a Mersenne-Twister variant with
// the game's seeding and twist, which differ from MT19937. Seeding it also reseeds the CRT generator.
class BsRandom {
public:
    BsRandom() = default;

    void seed(uint32_t s, CrtRandom& crt);

    // y % n, or y & n when n + 1 has no bits outside bit 15 (the game's test).
    uint32_t next(uint32_t n);

    // next(0xFFFFFFFF) as an unsigned value * 2^-32 * (hi - lo) + lo.
    float range(float lo, float hi);

    uint64_t hash() const;
    // The state index ( in the game's object), for comparing draw counts with the game.
    uint32_t index() const { return m_mt[624]; }

private:
    // [0, 624) the state, [624] the index: the game keeps the index directly after the state and its twist
    // reads mt[i + 1] for i = 623 from there.
    std::array<uint32_t, 625> m_mt{};
    bool m_seeded = false;
};

} // namespace spud::fx
