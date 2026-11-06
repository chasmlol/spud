#include "audio/binaural.hpp"

#include "audio/hrtf.hpp"
#include "core/error.hpp"

#include <format>
#include <vector>

#include <windows.h>
#include <xapobase.h>
#include <xaudio2.h>

namespace spud::audio {

namespace {

// {6F70656E-736B-7968-7274-660000000001}
constexpr GUID k_binaural_clsid = {
    0x6f70656e, 0x736b, 0x7968, {0x72, 0x74, 0x66, 0x00, 0x00, 0x00, 0x00, 0x01}};

// One channel in, two out: nothing in place, and the channel counts differ on purpose.
const XAPO_REGISTRATION_PROPERTIES k_registration = {
    k_binaural_clsid,
    L"spud HRTF",
    L"spud",
    1,
    0,
    XAPO_FLAG_FRAMERATE_MUST_MATCH | XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH | XAPO_FLAG_BUFFERCOUNT_MUST_MATCH,
    1,
    1,
    1,
    1,
};

} // namespace

class BinauralXapo final : public CXAPOBase {
  public:
    BinauralXapo(const HrtfSet& set, uint32_t sample_rate)
        : CXAPOBase(&k_registration), m_renderer(set, sample_rate) {}

    STDMETHOD(LockForProcess)(UINT32 inputs, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* input, UINT32 outputs,
                              const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* output) override {
        m_renderer.prepare(input[0].MaxFrameCount);
        return CXAPOBase::LockForProcess(inputs, input, outputs, output);
    }

    STDMETHOD_(void, Process)(UINT32, const XAPO_PROCESS_BUFFER_PARAMETERS* input, UINT32,
                              XAPO_PROCESS_BUFFER_PARAMETERS* output, BOOL) override {
        const uint32_t frames = input[0].ValidFrameCount;
        const bool silent = input[0].BufferFlags == XAPO_BUFFER_SILENT;
        const bool sound = m_renderer.process(silent ? nullptr : static_cast<const float*>(input[0].pBuffer),
                                              static_cast<float*>(output[0].pBuffer), frames);
        output[0].ValidFrameCount = frames;
        output[0].BufferFlags = sound ? XAPO_BUFFER_VALID : XAPO_BUFFER_SILENT;
    }

    void set_direction(const glm::vec3& direction, float focus) { m_renderer.set_target(direction, focus); }

  private:
    BinauralRenderer m_renderer;
};

BinauralVoice::BinauralVoice(IXAudio2* xaudio, IXAudio2MasteringVoice* master, uint32_t master_channels,
                             const HrtfSet& set, uint32_t sample_rate) {
    m_xapo = new BinauralXapo(set, sample_rate);
    XAUDIO2_EFFECT_DESCRIPTOR effect{static_cast<IXAPO*>(m_xapo), TRUE, 2};
    XAUDIO2_EFFECT_CHAIN chain{1, &effect};
    XAUDIO2_SEND_DESCRIPTOR send{0, master};
    XAUDIO2_VOICE_SENDS sends{1, &send};
    const HRESULT hr = xaudio->CreateSubmixVoice(&m_voice, 1, sample_rate, 0, 0, &sends, &chain);
    if (FAILED(hr)) {
        m_xapo->Release();
        m_xapo = nullptr;
        throw Error(std::format("audio: CreateSubmixVoice (HRTF) failed (0x{:08X})", static_cast<uint32_t>(hr)));
    }
    // Left to FL, right to FR, nothing else.
    std::vector<float> matrix(size_t(master_channels) * 2, 0.0f);
    matrix[0] = 1.0f;
    matrix[3] = 1.0f;
    m_voice->SetOutputMatrix(master, 2, master_channels, matrix.data());
}

BinauralVoice::~BinauralVoice() {
    if (m_voice) {
        m_voice->DestroyVoice();
    }
    if (m_xapo) {
        m_xapo->Release();
    }
}

void BinauralVoice::set_direction(const glm::vec3& direction, float focus) {
    m_xapo->set_direction(direction, focus);
}

} // namespace spud::audio
