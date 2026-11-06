#pragma once

// The output graph, built like Skyrim's:
//
//   source voices --(matrix: X3DAudio or static)--> mastering voice, 6 channels (5.1), device sample rate
//                 --(reverb send)--> reverb submix, 2 channels, XAudio2 reverb effect --> mastering voice
//
// Skyrim uses XAudio2 2.7 and X3DAudio 1.7 from the DirectX SDK; spud uses XAudio2 2.9 and its X3DAudio,
// which ship with Windows. The equivalence of the two (down-mix, reverb, 3D maths) is measured, not assumed:
// and. Only the audio thread calls into a Device.

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SubmixVoice;
struct IXAudio2SourceVoice;
struct IXAudio2VoiceCallback;

namespace spud::audio {

class BinauralVoice;
class CaptureTap;
class HrtfSet;

// I3DL2 parameters as the game fills them from a game object.
struct ReverbI3DL2 {
    float wet_dry_mix = 100.0f;
    int32_t room = -10000;
    int32_t room_hf = 0;
    float room_rolloff = 0.0f;
    float decay_time = 1.0f;
    float decay_hf_ratio = 0.5f;
    int32_t reflections = -10000;
    float reflections_delay = 0.02f;
    int32_t reverb = -10000;
    float reverb_delay = 0.04f;
    float diffusion = 100.0f;
    float density = 100.0f;
    float hf_reference = 5000.0f;
};

class Device {
  public:
    // Opens the default render device. With `silent`, nothing reaches the speakers (the mastering voice's
    // volume is 0); the capture tap still sees the full mix. Throws Error when XAudio2 is unavailable.
    explicit Device(bool silent);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    IXAudio2* xaudio() const { return m_xaudio; }
    IXAudio2MasteringVoice* master() const { return m_master; }
    IXAudio2SubmixVoice* reverb_voice() const { return m_reverb; }
    uint32_t master_channels() const { return k_master_channels; }
    uint32_t sample_rate() const { return m_sample_rate; }
    uint32_t device_channels() const { return m_device_channels; }
    uint32_t device_channel_mask() const { return m_device_channel_mask; }
    const uint8_t* x3d() const { return m_x3d; }
    const uint8_t* x3d_mono() const { return m_x3d_mono; } // one speaker: levels with no panning in them
    std::string description() const;

    // A source voice for `channels` x 16-bit PCM at `sample_rate`, sending to the master and the reverb
    // submix with per-send filters. Returns nullptr on
    // failure.
    IXAudio2SourceVoice* create_source(uint32_t channels, uint32_t sample_rate,
                                       IXAudio2VoiceCallback* callback);

    void set_reverb(const ReverbI3DL2& params);
    // The game and the game: after every XAudio2 processing
    // pass, every I3DL2 field moves linearly from the last completed set to the target over `ms`
    // (GetTickCount time; the four mB fields truncated). A new target mid-way restarts from the last
    // completed set.
    void transition_reverb(const ReverbI3DL2& target, uint32_t ms);
    void on_processing_pass_end(); // XAudio2's thread
    void set_silent(bool silent);

    // Headphone HRTF (binaural.hpp): new positioned sounds go through a BinauralVoice while on. The filters are
    // built the first time it is turned on.
    void set_hrtf(bool on);
    bool hrtf() const { return m_hrtf_on; }
    // A voice for one sound; nullptr if XAudio2 refuses one, and the sound keeps the game's mix.
    std::unique_ptr<BinauralVoice> create_binaural();

    CaptureTap& capture() { return *m_capture; }

    static constexpr uint32_t k_master_channels = 6; // SPEAKER_5POINT1
    static constexpr uint32_t k_master_channel_mask = 0x3F;
    static constexpr float k_max_frequency_ratio = 2.0f;
    // 24041.564 Skyrim units per second, in metres.
    static constexpr float k_speed_of_sound = 24041.564453125f * 0.0142875f;

  private:
    IXAudio2* m_xaudio = nullptr;
    IXAudio2MasteringVoice* m_master = nullptr;
    IXAudio2SubmixVoice* m_reverb = nullptr;
    std::unique_ptr<CaptureTap> m_capture;
    std::unique_ptr<HrtfSet> m_hrtf;
    bool m_hrtf_on = false;
    uint32_t m_sample_rate = 0;
    uint32_t m_device_channels = 0;
    uint32_t m_device_channel_mask = 0;
    bool m_com = false;
    struct EngineCallback;
    std::unique_ptr<EngineCallback> m_engine_callback;
    std::mutex m_reverb_mutex;
    ReverbI3DL2 m_reverb_done;   // the last completed set
    ReverbI3DL2 m_reverb_target;
    uint32_t m_reverb_start = 0, m_reverb_end = 0;
    bool m_reverb_pending = false;
    uint8_t m_x3d[20] = {}; // X3DAUDIO_HANDLE
    uint8_t m_x3d_mono[20] = {};
};

} // namespace spud::audio
