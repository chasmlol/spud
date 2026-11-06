#pragma once

// Records the game mix where Skyrim's comparison capture taps it: an effect on the mastering voice, so the
// 6-channel (5.1) signal is recorded before the down-mix to the device, and recording works with the output
// muted. Used by the `audio_capture` script command and the comparisons.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct IUnknown;

namespace spud::audio {

class CaptureTap {
  public:
    CaptureTap(uint32_t channels, uint32_t sample_rate);
    ~CaptureTap();

    IUnknown* xapo() const; // the XAPO to put in the mastering voice's effect chain

    // Starts recording `seconds` of the mix into a 32-bit float WAV. Returns false if one is running.
    bool start(const std::filesystem::path& file, double seconds);
    bool busy() const { return m_state.load() != 0; }
    // Frames recorded by the running (or last) capture.
    uint64_t frames() const { return m_frames.load(); }

    // Silent output: the tap records the mix, then hands the device zeros (the mastering voice applies its
    // own volume before its effects, so muting it would also mute the recording).
    void set_mute(bool mute) { m_mute = mute; }
    bool muted() const { return m_mute.load(std::memory_order_relaxed); }

    // Called by the XAPO on XAudio2's thread: never blocks.
    void push(const float* frames, uint32_t count);

  private:
    void writer();

    uint32_t m_channels;
    uint32_t m_sample_rate;
    IUnknown* m_xapo = nullptr;
    std::vector<float> m_ring; // m_ring_frames * channels
    size_t m_ring_frames = 0;
    std::atomic<uint64_t> m_write{0}; // frames written into the ring, total
    std::atomic<uint64_t> m_read{0};
    std::atomic<uint64_t> m_target{0};
    std::atomic<uint64_t> m_frames{0};
    std::atomic<int> m_state{0}; // 0 idle, 1 recording
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_mute{false};
    std::mutex m_mutex;
    std::filesystem::path m_file;
    std::jthread m_thread;
};

} // namespace spud::audio
