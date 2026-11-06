#pragma once

// Skyrim's shared random generator, reproduced: the one most of the game's random draws come from (file
// choice, pitch and dB variance, music picks, region one-shots and their placement, flickering lights,
// particles,...)., "The random draws in the running game", has the
// algorithm, its sources and the measurements.
//
// Two things make the game's draws differ from a uniform source, and both are reproduced:
// - The twist is a defective Mersenne Twister: it only ever copies the words' upper 21 bits around.
// - The game draws from it about 6,400 times a second from several threads, with no lock. Most of its
//   twists run on several threads at once, and the copies they make collapse the state. Draws come out
//   biased, by an amount that differs from session to session and grows through one. advance() stands in
//   for those draws: the measured rate, and racing twists from a model fitted to watched ones.
//
// Systems that want the game's random behaviour draw from GameRandom::shared() (thread-safe). When a system
// that draws in the game starts drawing here too, its share comes out of k_background_draws_per_second.

#include <array>
#include <cstdint>
#include <mutex>
#include <random>

namespace spud {

// What generators outside GameRandom seed from: the OS's entropy, unless a run that must replay (a perf tour, `seed` in
// a bench scenario) fixed it before they were created. Each call gives a different value, in a fixed order when fixed.
void set_session_seed(uint32_t seed);
uint32_t session_seed();

class GameRandom {
  public:
    static constexpr uint32_t k_words = 624;

    // Measured at Riverwood at night: 781,838 and 752,379 draws in two 120 s watches (the game's
    // ap_rngwatch, counted from the index), with 922 of 1,247 and 716 of 1,186 twists racing.
    static constexpr double k_background_draws_per_second = 6390.0;
    static constexpr double k_racing_share = 1638.0 / 2433.0;

    explicit GameRandom(uint32_t seed) { this->seed(seed); }

    // The process-wide generator, seeded from the clock (_time64) on first use as the game's is:
    // The game seeds it when its byte is clear.
    static GameRandom& shared();
    // What the audio thread draws from: shared(), but its own in a run that must replay (set_session_seed), since it draws
    // on real time and would shift everyone else's draws.
    static GameRandom& audio();

    // Seeds the generator, and the racing model's own source with the same value, so a run can be repeated.
    void seed(uint32_t seed);
    uint32_t next_u32();
    // r mod n; 0 when n is 0.
    uint32_t next_below(uint32_t n);
    // u32 x 2^-32, in [0, 1): what the game's chance tests compare against.
    double next_unit();
    // The draws the rest of the game would have made over this time, with their twists (some racing).
    void advance(double seconds);

    // The game's state layout: 624 words, then the index (which the twist's last step reads as a 625th word).
    struct State {
        std::array<uint32_t, k_words> words;
        uint32_t index;
    };
    State state() const;
    void set_state(const State& s);
    // One twist of the words, as the game does when the index wraps to 0.
    void twist();

  private:
    void twist_words();
    void racing_twist();

    std::array<uint32_t, k_words + 1> m_mt{}; // [624] is the index's slot in the game's layout: 0 at a twist
    uint32_t m_index = 0;
    double m_pending = 0.0; // background draws not yet made
    std::mt19937 m_race;    // the racing model's choices: not the game's generator
    mutable std::mutex m_mutex;
};

} // namespace spud
