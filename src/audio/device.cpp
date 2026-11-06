#include "audio/device.hpp"

#include "audio/binaural.hpp"
#include "audio/capture.hpp"
#include "audio/hrtf.hpp"
#include "core/error.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <format>

#include <objbase.h>
#include <windows.h>
#include <x3daudio.h>
#include <xaudio2.h>
#include <xaudio2fx.h>
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace spud::audio {

struct Device::EngineCallback final : IXAudio2EngineCallback {
    Device* device;
    explicit EngineCallback(Device* d) : device(d) {}
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override { device->on_processing_pass_end(); }
    void STDMETHODCALLTYPE OnCriticalError(HRESULT) override {}
};

namespace {

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw Error(std::format("audio: {} failed (0x{:08X})", what, static_cast<uint32_t>(hr)));
    }
}

} // namespace

Device::Device(bool silent) {
    m_com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    check(XAudio2Create(&m_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR), "XAudio2Create");

    // The game asks for 6 input channels at the device's rate clamped to [20000, 48000] Hz.
    check(m_xaudio->CreateMasteringVoice(&m_master, k_master_channels, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr,
                                         nullptr, AudioCategory_GameEffects),
          "CreateMasteringVoice");
    XAUDIO2_VOICE_DETAILS details{};
    m_master->GetVoiceDetails(&details);
    m_sample_rate = details.InputSampleRate;
    if (m_sample_rate > 48000 || m_sample_rate < 20000) {
        m_master->DestroyVoice();
        m_sample_rate = std::clamp<uint32_t>(m_sample_rate, 20000, 48000);
        check(m_xaudio->CreateMasteringVoice(&m_master, k_master_channels, m_sample_rate, 0, nullptr, nullptr,
                                             AudioCategory_GameEffects),
              "CreateMasteringVoice");
    }
    DWORD mask = 0;
    m_master->GetChannelMask(&mask);
    m_device_channel_mask = mask;
    m_device_channels = static_cast<uint32_t>(std::popcount(static_cast<uint32_t>(mask)));

    // X3DAudio with the 5.1 speaker layout, whatever the device (the mastering voice down-mixes).
    check(X3DAudioInitialize(k_master_channel_mask, k_speed_of_sound, m_x3d), "X3DAudioInitialize");
    check(X3DAudioInitialize(SPEAKER_FRONT_CENTER, k_speed_of_sound, m_x3d_mono), "X3DAudioInitialize");

    // The reverb submix: 2 channels, the AudioReverb effect (enabled, 2 output channels), default sends.
    IUnknown* reverb = nullptr;
    check(XAudio2CreateReverb(&reverb, 0), "XAudio2CreateReverb");
    XAUDIO2_EFFECT_DESCRIPTOR effect{reverb, TRUE, 2};
    XAUDIO2_EFFECT_CHAIN chain{1, &effect};
    check(m_xaudio->CreateSubmixVoice(&m_reverb, 2, m_sample_rate, 0, 0, nullptr, &chain),
          "CreateSubmixVoice");
    reverb->Release();
    // Before any SetReverb both sets hold the I3DL2 default preset (Room -10000: silent).
    set_reverb(ReverbI3DL2{});
    m_engine_callback = std::make_unique<EngineCallback>(this);
    check(m_xaudio->RegisterForCallbacks(m_engine_callback.get()), "RegisterForCallbacks");

    // The capture tap sits on the mastering voice and sees the 6-channel game mix.
    m_capture = std::make_unique<CaptureTap>(k_master_channels, m_sample_rate);
    IUnknown* tap = m_capture->xapo();
    XAUDIO2_EFFECT_DESCRIPTOR tap_effect{tap, TRUE, k_master_channels};
    XAUDIO2_EFFECT_CHAIN tap_chain{1, &tap_effect};
    check(m_master->SetEffectChain(&tap_chain), "SetEffectChain (capture)");

    set_silent(silent);
    log::info("audio: {}", description());
}

Device::~Device() {
    if (m_xaudio && m_engine_callback) {
        m_xaudio->UnregisterForCallbacks(m_engine_callback.get());
    }
    if (m_reverb) {
        m_reverb->DestroyVoice();
    }
    if (m_master) {
        m_master->DestroyVoice();
    }
    if (m_xaudio) {
        m_xaudio->Release();
    }
    m_capture.reset();
    if (m_com) {
        CoUninitialize();
    }
}

std::string Device::description() const {
    return std::format("XAudio2 2.9, mastering voice {} ch at {} Hz, device {} ch (mask 0x{:X})",
                       k_master_channels, m_sample_rate, m_device_channels, m_device_channel_mask);
}

IXAudio2SourceVoice* Device::create_source(uint32_t channels, uint32_t sample_rate,
                                           IXAudio2VoiceCallback* callback) {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = static_cast<WORD>(channels);
    wfx.nSamplesPerSec = sample_rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(channels * 2);
    wfx.nAvgBytesPerSec = sample_rate * wfx.nBlockAlign;
    XAUDIO2_SEND_DESCRIPTOR sends[2] = {
        {XAUDIO2_SEND_USEFILTER, m_master},
        {XAUDIO2_SEND_USEFILTER, m_reverb},
    };
    XAUDIO2_VOICE_SENDS send_list{2, sends};
    IXAudio2SourceVoice* voice = nullptr;
    if (FAILED(m_xaudio->CreateSourceVoice(&voice, &wfx, 0, k_max_frequency_ratio, callback, &send_list,
                                           nullptr))) {
        return nullptr;
    }
    // Nothing reaches the reverb until a send level is set.
    float zero[2 * 2] = {};
    voice->SetOutputMatrix(m_reverb, channels, 2, zero);
    return voice;
}

void Device::set_reverb(const ReverbI3DL2& p) {
    XAUDIO2FX_REVERB_I3DL2_PARAMETERS i3dl2{};
    i3dl2.WetDryMix = p.wet_dry_mix;
    i3dl2.Room = p.room;
    i3dl2.RoomHF = p.room_hf;
    i3dl2.RoomRolloffFactor = p.room_rolloff;
    i3dl2.DecayTime = p.decay_time;
    i3dl2.DecayHFRatio = p.decay_hf_ratio;
    i3dl2.Reflections = p.reflections;
    i3dl2.ReflectionsDelay = p.reflections_delay;
    i3dl2.Reverb = p.reverb;
    i3dl2.ReverbDelay = p.reverb_delay;
    i3dl2.Diffusion = p.diffusion;
    i3dl2.Density = p.density;
    i3dl2.HFReference = p.hf_reference;
    XAUDIO2FX_REVERB_PARAMETERS native{};
    // FALSE selects the stereo/5.1 defaults (rear delay 5 ms), the values of the XAudio2 2.7 SDK conversion
    // compiled into the game.
    ReverbConvertI3DL2ToNative(&i3dl2, &native, FALSE);
    m_reverb->SetEffectParameters(0, &native, sizeof(native));
}

void Device::transition_reverb(const ReverbI3DL2& target, uint32_t ms) {
    std::lock_guard lock(m_reverb_mutex);
    m_reverb_target = target;
    m_reverb_start = GetTickCount();
    m_reverb_end = m_reverb_start + ms;
    m_reverb_pending = true;
}

void Device::on_processing_pass_end() {
    std::unique_lock lock(m_reverb_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_reverb_pending) {
        return;
    }
    const uint32_t now = GetTickCount();
    if (now >= m_reverb_end || m_reverb_end == m_reverb_start) {
        m_reverb_done = m_reverb_target;
        m_reverb_pending = false;
        set_reverb(m_reverb_done);
        return;
    }
    const float t = float(now - m_reverb_start) / float(m_reverb_end - m_reverb_start);
    const auto& a = m_reverb_done;
    const auto& b = m_reverb_target;
    auto lerp = [t](float x, float y) { return x + (y - x) * t; };
    auto lerp_mb = [t](int32_t x, int32_t y) { return static_cast<int32_t>(x + (float(y) - float(x)) * t); };
    ReverbI3DL2 p;
    p.wet_dry_mix = 100.0f;
    p.room = lerp_mb(a.room, b.room);
    p.room_hf = lerp_mb(a.room_hf, b.room_hf);
    p.room_rolloff = 0.0f;
    p.decay_time = lerp(a.decay_time, b.decay_time);
    p.decay_hf_ratio = lerp(a.decay_hf_ratio, b.decay_hf_ratio);
    p.reflections = lerp_mb(a.reflections, b.reflections);
    p.reflections_delay = lerp(a.reflections_delay, b.reflections_delay);
    p.reverb = lerp_mb(a.reverb, b.reverb);
    p.reverb_delay = lerp(a.reverb_delay, b.reverb_delay);
    p.diffusion = lerp(a.diffusion, b.diffusion);
    p.density = lerp(a.density, b.density);
    p.hf_reference = lerp(a.hf_reference, b.hf_reference);
    set_reverb(p);
}

void Device::set_silent(bool silent) {
    m_capture->set_mute(silent);
}

void Device::set_hrtf(bool on) {
    if (on && !m_hrtf) {
        m_hrtf = std::make_unique<HrtfSet>(m_sample_rate);
    }
    m_hrtf_on = on;
}

std::unique_ptr<BinauralVoice> Device::create_binaural() {
    try {
        return std::make_unique<BinauralVoice>(m_xaudio, m_master, k_master_channels, *m_hrtf, m_sample_rate);
    } catch (const std::exception& e) {
        log::warn("{}", e.what());
        return nullptr;
    }
}

} // namespace spud::audio
