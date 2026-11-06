#pragma once

// Binaural rendering for headphones: an upgrade over the game, which has no such thing ("HRTF" in its output
// models only names X3DAudio's speaker panning). docs/AUDIO.md "Headphone spatial audio" is the overview.
//
// The filters are measured ones (tools/hrtf/build_hrtf.py, hrtf_data.inc): each direction is a minimum-phase
// impulse response per ear plus the interaural time difference. Keeping the delay apart is what lets directions
// be interpolated: blending raw impulse responses would comb where their arrival times differ.
//
// No XAudio2 here: BinauralVoice (binaural.hpp) runs a renderer as an effect of a voice.

#include <glm/vec3.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

namespace spud::audio {

// Every measured direction, at the device's sample rate. Immutable once built, so voices share one.
class HrtfSet {
  public:
    static constexpr uint32_t k_taps = 64;

    struct Filter {
        alignas(16) float ear[2][k_taps]; // left, right
        // Samples the left ear lags the right, negative when the right ear lags, on top of the delay the two
        // minimum-phase filters have of their own.
        float itd = 0.0f;
    };

    explicit HrtfSet(uint32_t sample_rate);

    // The filter toward a unit vector in the head's frame (x right, y forward, z up), interpolated between the
    // four surrounding measurements.
    void lookup(const glm::vec3& direction, Filter& out) const;

  private:
    struct Ring {
        float elevation; // degrees
        uint32_t count;  // measurements, evenly spaced from azimuth 0 toward the left
        uint32_t first;
    };
    std::vector<Ring> m_rings; // by rising elevation
    std::vector<Filter> m_filters;
};

// One sound's HRTF filter. set_target comes from the audio thread, process from XAudio2's.
class BinauralRenderer {
  public:
    BinauralRenderer(const HrtfSet& set, uint32_t sample_rate);

    // Sizes the buffers for blocks of up to `max_frames`, before the first process.
    void prepare(uint32_t max_frames);

    // `direction`: a unit vector in the head's frame. `focus`: 1 for a sound with a direction, down to 0 for one
    // at the listener, where direction loses its meaning and the sound sits evenly in both ears (what X3DAudio's
    // inner radius does on speakers).
    void set_target(const glm::vec3& direction, float focus);

    // `in`: `frames` mono samples, nullptr when silent. `out`: interleaved stereo. Returns false, with `out`
    // left unwritten, once the filter has run out of sound.
    bool process(const float* in, float* out, uint32_t frames);

  private:
    static constexpr uint32_t k_max_delay = 64; // samples: past the widest head's ITD at 48 kHz
    // The fractional delay reads two samples ahead of its position, so every path runs that much late.
    static constexpr uint32_t k_lookahead = 2;
    static constexpr uint32_t k_history = k_max_delay + k_lookahead + 2;

    void filter(const float* delayed, const float* taps, float* out, uint32_t frames) const;

    const HrtfSet& m_set;
    float m_slew_seconds;
    std::atomic<uint64_t> m_target{0}; // packed: audio thread to mixer thread without a lock
    bool m_started = false;
    glm::vec3 m_direction{0, 1, 0}; // where the current filter points
    float m_focus = 1.0f;
    HrtfSet::Filter m_filter{}, m_next{};
    std::vector<float> m_input;      // k_history + frames
    std::vector<float> m_delayed[2]; // per ear: k_taps - 1 + frames
    std::vector<float> m_from, m_to; // filter outputs of one ear
    uint32_t m_quiet_frames = 0;     // input frames since the last sound
};

} // namespace spud::audio
