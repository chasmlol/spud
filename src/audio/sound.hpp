#pragma once

// One playing sound, with Skyrim's per-sound rules:
// file choice, static and random attenuation, frequency shift and variance, the volume formula in hundredths
// of a dB, X3DAudio spatialisation with the output model's attenuation curve, distance low-pass filter,
// reverb send, looping and envelopes. Lives on the audio thread.

#include "audio/clip.hpp"
#include "formats/osd.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

struct IXAudio2SourceVoice;

namespace spud::audio {

class BinauralVoice;
class Device;
struct CategoryState;

struct Listener {
    glm::vec3 position{0};
    glm::vec3 forward{0, 1, 0};
    glm::vec3 up{0, 0, 1};
    glm::vec3 velocity{0};
};

// Fields set when a sound is built from its descriptor.
struct SoundSetup {
    uint32_t descriptor = osd::k_none;
    std::filesystem::path file;
    uint32_t flags = 0; // The game flags: 1 HRTF attenuating, 2 none, 4 speaker attenuating, 0x800 loop..
    uint8_t frequency_base = 100;   // percent: (100 + shift) clamped to [10, 200]
    uint8_t frequency_variance = 0; // percent
    uint8_t priority = 0;
    uint16_t static_attenuation = 0; // hundredths of a dB, including the random dB variance
    const osd::OutputModel* output = nullptr;
    uint32_t category = osd::k_none;
    std::optional<glm::vec3> position;
    bool follow_listener = false;         // positioned on the listener (flag 0x400)
    std::optional<glm::vec3> segment_end; // line emitters (flag 0x4000): slide toward this point
    float volume = 1.0f;
};

class GameSound {
  public:
    GameSound(uint32_t id, SoundSetup setup);
    ~GameSound();

    uint32_t id() const { return m_id; }
    const SoundSetup& setup() const { return m_setup; }
    bool started() const { return m_voice != nullptr; }
    bool finished() const { return m_finished; }

    // Creates the voice and starts playback from a loaded clip or an open stream.
    bool start(Device& device, std::shared_ptr<const Clip> clip, std::unique_ptr<ClipStream> stream,
               float frequency_ratio);
    // Per audio pass: distance culling, streaming, volume; every >= 66 ms the voice poll (3D, end detection).
    void update(Device& device, const Listener& listener, float master_volume,
                const std::vector<CategoryState>& categories, double now);
    void recompute_3d(Device& device, const Listener& listener) { update_3d(device, listener); }
    // Moves a playing sound onto or off the HRTF path to match the device, when the option changes.
    void apply_hrtf(Device& device, const Listener& listener);
    void stop(); // envelope sounds play their release; others stop now
    void pause();
    void resume();
    void destroy();

    void set_position(const glm::vec3& position) { m_setup.position = position; }
    void set_category(uint32_t category) { m_setup.category = category; }
    void set_volume(float volume) { m_setup.volume = std::clamp(volume, 1e-5f, 1.0f); }
    float volume() const { return m_setup.volume; }
    // The fade attenuation (hundredths of a dB): fades, ducking and the category's
    // attenuation at play time all write it.
    uint16_t fade_attenuation() const { return m_fade_attenuation; }
    void set_fade_attenuation(uint16_t mb) { m_fade_attenuation = mb; }
    // Playback position in milliseconds and seeking (music loops and cue points).
    uint32_t position_ms() const;
    void seek_ms(uint32_t ms);
    uint32_t duration_ms() const;

    // Diagnostics.
    float last_gain() const { return m_last_gain; }
    float distance() const { return m_distance; }
    uint16_t distance_attenuation() const { return m_distance_attenuation; }

  private:
    void init_outputs(Device& device);
    void update_3d(Device& device, const Listener& listener);
    bool wants_binaural(const Device& device) const;
    void attach_binaural(Device& device);
    glm::vec3 emitter_position(const Listener& listener) const;
    void update_direction(const Listener& listener);
    float compute_volume(float master_volume, const std::vector<CategoryState>& categories) const;
    void submit_stream();
    void submit_clip(uint32_t play_begin);

    uint32_t m_id;
    SoundSetup m_setup;
    IXAudio2SourceVoice* m_voice = nullptr;
    std::unique_ptr<BinauralVoice> m_binaural; // only with HRTF on, on positioned 3D sounds
    struct Callback;
    std::unique_ptr<Callback> m_callback;
    std::shared_ptr<const Clip> m_clip;
    std::unique_ptr<ClipStream> m_stream;
    std::vector<std::vector<int16_t>> m_stream_buffers;
    std::vector<std::vector<int16_t>> m_retired_buffers; // flushed by a seek, freed with the voice
    size_t m_stream_next = 0;
    bool m_stream_ended = false;
    uint32_t m_channels = 0;
    uint32_t m_sample_rate = 0;

    // X3DAudio state (the emitter defaults of the game's constructor).
    struct Curve {
        float distance;
        float value;
    };
    Curve m_curve[6];
    float m_min_distance = 0;
    float m_max_distance = 0;
    uint16_t m_distance_attenuation = 10000; // hundredths of a dB
    uint16_t m_fade_attenuation = 0;
    uint64_t m_position_base = 0;            // frames: where the last seek put the stream
    uint64_t m_samples_at_seek = 0;          // XAudio2's SamplesPlayed at that seek
    std::vector<float> m_matrix;
    float m_distance = 0;
    float m_last_gain = -1;
    bool m_stopping = false;
    bool m_culled = false; // stopped by distance culling (state 0x20000)
    double m_last_poll = -1.0;
    bool m_finished = false;
};

// Runtime state of a sound category.
struct CategoryState {
    uint32_t parent = osd::k_none;
    float static_volume = 1.0f;
    float volume = 1.0f;      // runtime volume (menu slider)
    float frequency = 1.0f;   // runtime frequency multiplier
    uint16_t attenuation = 0; // runtime attenuation, hundredths of a dB
    bool paused = false;
    bool muted = false;
};

float effective_volume(const std::vector<CategoryState>& categories, uint32_t index);
float effective_frequency(const std::vector<CategoryState>& categories, uint32_t index);

} // namespace spud::audio
