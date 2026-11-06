#include "core/game_random.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <vector>

namespace spud {

namespace {

// One step of the twist loop at word i, reading memory as it is now (the loop reloads mt[i], mt[i+1] and
// mt[(i+397) mod 624] every iteration): y = (mt[i] & 1) +
// (mt[i+1] mod 624), not the standard upper/lower bit split.
void twist_step(std::array<uint32_t, GameRandom::k_words + 1>& mt, uint32_t i) {
    const uint32_t y = (mt[i + 1] % GameRandom::k_words) + (mt[i] & 1u);
    uint32_t v = mt[(i + 397) % GameRandom::k_words] ^ (2u * y);
    if (y & 1u) {
        v ^= 0x9908B0DFu;
    }
    mt[i] = v;
}

} // namespace

namespace {
std::atomic<bool> g_session_seed_fixed{false};
std::atomic<uint32_t> g_session_seed{0};
} // namespace

void set_session_seed(uint32_t seed) {
    g_session_seed = seed;
    g_session_seed_fixed = true;
    GameRandom::shared().seed(seed);
}

uint32_t session_seed() {
    if (!g_session_seed_fixed) {
        return std::random_device{}();
    }
    return g_session_seed.fetch_add(0x9E3779B1u);
}

GameRandom& GameRandom::shared() {
    static GameRandom instance(static_cast<uint32_t>(std::time(nullptr)));
    return instance;
}

GameRandom& GameRandom::audio() {
    static GameRandom own(0xA0D10u);
    return g_session_seed_fixed ? own : shared();
}

// mt[i] = 1812433253 x (mt[i-1] XOR (i + (mt[i-1] >> 30))), the index left at 623.
void GameRandom::seed(uint32_t seed) {
    std::lock_guard lock(m_mutex);
    m_mt[0] = seed;
    for (uint32_t i = 1; i < k_words; ++i) {
        const uint32_t p = m_mt[i - 1];
        m_mt[i] = 1812433253u * (p ^ (i + (p >> 30)));
    }
    m_index = k_words - 1;
    m_pending = 0.0;
    m_race.seed(seed);
}

void GameRandom::twist() {
    std::lock_guard lock(m_mutex);
    twist_words();
}

void GameRandom::twist_words() {
    m_mt[k_words] = 0;
    for (uint32_t i = 0; i < k_words; ++i) {
        twist_step(m_mt, i);
    }
}

// Several threads find the index at 0 together and run the twist loop over the same words, each from its own
// start and at its own speed; their iterations land in time order. The model and its parameters were fitted
// to 1,638 racing twists watched in the game: two threads, each of
// two more joining with probability 0.9; the others start within one loop's time of the first; each loop's
// duration is log-uniform within 4x.
void GameRandom::racing_twist() {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    uint32_t threads = 2;
    for (int extra = 0; extra < 2; ++extra) {
        threads += unit(m_race) < 0.9 ? 1 : 0;
    }
    struct Step {
        double time;
        uint32_t word;
    };
    std::vector<Step> steps;
    steps.reserve(threads * k_words);
    for (uint32_t t = 0; t < threads; ++t) {
        const double start = t == 0 ? 0.0 : unit(m_race);
        const double duration = std::pow(4.0, unit(m_race) * 2.0 - 1.0);
        for (uint32_t i = 0; i < k_words; ++i) {
            steps.push_back({start + duration * (i + 0.5) / k_words, i});
        }
    }
    std::stable_sort(steps.begin(), steps.end(),
                     [](const Step& a, const Step& b) { return a.time < b.time; });
    m_mt[k_words] = 0;
    for (const auto& s : steps) {
        twist_step(m_mt, s.word);
    }
}

void GameRandom::advance(double seconds) {
    std::lock_guard lock(m_mutex);
    m_pending += seconds * k_background_draws_per_second;
    auto draws = static_cast<uint64_t>(m_pending);
    m_pending -= static_cast<double>(draws);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    while (draws > 0) {
        if (m_index == 0) {
            if (unit(m_race) < k_racing_share) {
                racing_twist();
            } else {
                twist_words();
            }
        }
        const uint64_t run = std::min<uint64_t>(draws, k_words - m_index);
        m_index = static_cast<uint32_t>((m_index + run) % k_words);
        draws -= run;
    }
}

uint32_t GameRandom::next_u32() {
    std::lock_guard lock(m_mutex);
    if (m_index == 0) {
        twist_words();
    }
    uint32_t y = m_mt[m_index];
    m_index = (m_index + 1) % k_words;
    y ^= y >> 11;
    y ^= (y & 0xFF3A58ADu) << 7;
    y ^= (y & 0xFFFFDF8Cu) << 15;
    y ^= y >> 18;
    return y;
}

// For n = 0x7FFF and 0xFFFFFFFF the game masks instead of dividing; for 0 it returns 0 without drawing.
uint32_t GameRandom::next_below(uint32_t n) {
    if (!n) {
        return 0;
    }
    const uint32_t r = next_u32();
    return ((n + 1) & 0xFFFF7FFFu) == 0 ? r & n : r % n;
}

double GameRandom::next_unit() {
    return static_cast<double>(next_u32()) * (1.0 / 4294967296.0);
}

GameRandom::State GameRandom::state() const {
    std::lock_guard lock(m_mutex);
    State s{};
    for (uint32_t i = 0; i < k_words; ++i) {
        s.words[i] = m_mt[i];
    }
    s.index = m_index;
    return s;
}

void GameRandom::set_state(const State& s) {
    std::lock_guard lock(m_mutex);
    for (uint32_t i = 0; i < k_words; ++i) {
        m_mt[i] = s.words[i];
    }
    m_index = s.index % k_words;
}

} // namespace spud
