#include "audio/capture.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

#include <windows.h>
#include <xapobase.h>
#pragma comment(lib, "xapobase.lib")

namespace spud::audio {

namespace {

// {6F70656E-736B-7963-6170-747572650001}
constexpr GUID k_capture_clsid = {
    0x6f70656e, 0x736b, 0x7963, {0x61, 0x70, 0x74, 0x75, 0x72, 0x65, 0x00, 0x01}};

const XAPO_REGISTRATION_PROPERTIES k_registration = {
    k_capture_clsid,
    L"spud capture tap",
    L"spud",
    1,
    0,
    XAPO_FLAG_CHANNELS_MUST_MATCH | XAPO_FLAG_FRAMERATE_MUST_MATCH | XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH |
        XAPO_FLAG_BUFFERCOUNT_MUST_MATCH | XAPO_FLAG_INPLACE_SUPPORTED | XAPO_FLAG_INPLACE_REQUIRED,
    1,
    1,
    1,
    1,
};

class CaptureXapo final : public CXAPOBase {
  public:
    explicit CaptureXapo(CaptureTap* tap) : CXAPOBase(&k_registration), m_tap(tap) {}

    STDMETHOD(LockForProcess)(UINT32 inputs, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* input,
                              UINT32 outputs, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* output) override {
        m_silence.assign(static_cast<size_t>(input[0].MaxFrameCount) * input[0].pFormat->nChannels, 0.0f);
        return CXAPOBase::LockForProcess(inputs, input, outputs, output);
    }

    STDMETHOD_(void, Process)(UINT32, const XAPO_PROCESS_BUFFER_PARAMETERS* input, UINT32,
                              XAPO_PROCESS_BUFFER_PARAMETERS* output, BOOL) override {
        const auto* frames = static_cast<const float*>(input[0].pBuffer);
        // A silent buffer's contents are undefined; record zeros.
        if (input[0].BufferFlags == XAPO_BUFFER_SILENT) {
            frames = m_silence.data();
        }
        m_tap->push(frames, input[0].ValidFrameCount);
        output[0].ValidFrameCount = input[0].ValidFrameCount;
        output[0].BufferFlags = input[0].BufferFlags;
        if (m_tap->muted()) {
            std::memset(output[0].pBuffer, 0, m_silence.size() * sizeof(float));
            output[0].BufferFlags = XAPO_BUFFER_SILENT;
        }
    }

  private:
    CaptureTap* m_tap;
    std::vector<float> m_silence;
};

void write_header(std::ofstream& f, uint32_t channels, uint32_t rate, uint32_t data_bytes) {
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + data_bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(3); // IEEE float
    u16(static_cast<uint16_t>(channels));
    u32(rate);
    u32(rate * channels * 4);
    u16(static_cast<uint16_t>(channels * 4));
    u16(32);
    f.write("data", 4);
    u32(data_bytes);
}

} // namespace

CaptureTap::CaptureTap(uint32_t channels, uint32_t sample_rate)
    : m_channels(channels), m_sample_rate(sample_rate) {
    m_ring_frames = static_cast<size_t>(sample_rate) * 8;
    m_ring.assign(m_ring_frames * channels, 0.0f);
    m_xapo = static_cast<IXAPO*>(new CaptureXapo(this));
    m_thread = std::jthread([this] { writer(); });
}

CaptureTap::~CaptureTap() {
    m_quit = true;
    m_thread = {};
    if (m_xapo) {
        m_xapo->Release();
    }
}

IUnknown* CaptureTap::xapo() const {
    return m_xapo;
}

bool CaptureTap::start(const std::filesystem::path& file, double seconds) {
    std::lock_guard lock(m_mutex);
    if (m_state.load() != 0) {
        return false;
    }
    m_file = file;
    m_frames = 0;
    m_target = static_cast<uint64_t>(seconds * m_sample_rate);
    m_read = m_write.load();
    m_state = 1;
    return true;
}

void CaptureTap::push(const float* frames, uint32_t count) {
    if (m_state.load(std::memory_order_relaxed) == 0) {
        m_write += count; // keep the clock running so a capture starts at "now"
        m_read = m_write.load(std::memory_order_relaxed);
        return;
    }
    uint64_t w = m_write.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < count; ++i, ++w) {
        std::memcpy(&m_ring[(w % m_ring_frames) * m_channels], frames + static_cast<size_t>(i) * m_channels,
                    m_channels * sizeof(float));
    }
    m_write.store(w, std::memory_order_release);
}

void CaptureTap::writer() {
    std::ofstream out;
    uint64_t written = 0;
    while (!m_quit) {
        if (m_state.load() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!out.is_open()) {
            std::lock_guard lock(m_mutex);
            std::filesystem::create_directories(m_file.parent_path());
            out.open(m_file, std::ios::binary | std::ios::trunc);
            write_header(out, m_channels, m_sample_rate, 0);
            written = 0;
        }
        const uint64_t w = m_write.load(std::memory_order_acquire);
        uint64_t r = m_read.load();
        if (w - r > m_ring_frames) {
            log::warn("audio capture: overrun, {} frames lost", w - r - m_ring_frames);
            r = w - m_ring_frames;
        }
        const uint64_t want = m_target.load() - written;
        const uint64_t n = std::min(w - r, want);
        for (uint64_t i = 0; i < n; ++i) {
            out.write(reinterpret_cast<const char*>(&m_ring[((r + i) % m_ring_frames) * m_channels]),
                      m_channels * sizeof(float));
        }
        m_read = r + n;
        written += n;
        m_frames = written;
        if (written >= m_target.load()) {
            out.seekp(0);
            write_header(out, m_channels, m_sample_rate, static_cast<uint32_t>(written * m_channels * 4));
            out.close();
            m_state = 0;
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace spud::audio
