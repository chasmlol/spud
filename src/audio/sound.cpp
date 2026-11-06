#include "audio/sound.hpp"

#include "audio/binaural.hpp"
#include "audio/device.hpp"
#include "core/log.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <windows.h>
#include <x3daudio.h>
#include <xaudio2.h>

namespace spud::audio {

namespace {

constexpr float k_pi = 3.14159265358979f;
constexpr size_t k_stream_frames = 16384; // per queued buffer
constexpr size_t k_stream_queue = 3;
// The emitter's inner radius: ~70 units = 1 m (69.99 units in the game's constructor).
constexpr float k_inner_radius = 69.99f * 0.0142875f;

// The game: matrices used when the output model doesn't define speaker
// levels (row per destination FL FR FC LFE BL BR, column per source channel).
constexpr float k_mono_matrix[6] = {1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.5f};
constexpr float k_stereo_matrix[12] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                       0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.5f};

// X3DAudio's coordinate frame is left-handed with Y up; the game passes (x, z, y) of its Z-up positions
X3DAUDIO_VECTOR to_x3d(const glm::vec3& v) {
    return {v.x, v.z, v.y};
}

} // namespace

float effective_volume(const std::vector<CategoryState>& categories, uint32_t index) {
    float v = 1.0f;
    for (int depth = 0; index != osd::k_none && index < categories.size() && depth < 32; ++depth) {
        const auto& c = categories[index];
        v *= c.static_volume * c.volume;
        index = c.parent;
    }
    return v;
}

float effective_frequency(const std::vector<CategoryState>& categories, uint32_t index) {
    float f = 1.0f;
    for (int depth = 0; index != osd::k_none && index < categories.size() && depth < 32; ++depth) {
        f *= categories[index].frequency;
        index = categories[index].parent;
    }
    return f;
}

struct GameSound::Callback final : IXAudio2VoiceCallback {
    std::atomic<int> buffers_ended{0};
    std::atomic<bool> stream_ended{false};
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override { stream_ended = true; }
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { ++buffers_ended; }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

GameSound::GameSound(uint32_t id, SoundSetup setup) : m_id(id), m_setup(std::move(setup)) {
    // The default volume curve of the game's constructor, replaced by the output model's.
    static constexpr Curve k_default[6] = {{0.0f, 1.0f}, {0.2f, 0.8f}, {0.4f, 0.6f},
                                           {0.6f, 0.4f}, {0.8f, 0.2f}, {1.0f, 0.0f}};
    std::copy(std::begin(k_default), std::end(k_default), m_curve);
    if (const auto* out = m_setup.output; out && (out->flags & osd::output_attenuates_with_distance)) {
        // SetAttenuationCurve then SetMinMaxDistance ().
        for (int i = 0; i < 5; ++i) {
            m_curve[i + 1].value = out->curve[i] * 0.01f;
        }
        m_curve[0].value = m_curve[1].value;
        m_min_distance = out->min_distance;
        m_max_distance = out->max_distance;
        const float mn = m_min_distance, mx = m_max_distance;
        auto norm = [&](float d) { return mx > 0 ? std::clamp(d / mx, 0.0f, 1.0f) : 1.0f; };
        m_curve[1].distance = norm(mn);
        m_curve[2].distance = norm((mx - mn) * 0.25f + mn);
        m_curve[3].distance = norm((mx - mn) * 0.5f + mn);
        m_curve[4].distance = norm((mx - mn) * 0.75f + mn);
        m_curve[5].distance = 1.0f;
    }
}

GameSound::~GameSound() {
    destroy();
}

void GameSound::destroy() {
    if (m_voice) {
        m_voice->DestroyVoice();
        m_voice = nullptr;
    }
    m_binaural.reset(); // after the source voice: a voice can't go while another still sends to it
    m_finished = true;
}

bool GameSound::start(Device& device, std::shared_ptr<const Clip> clip, std::unique_ptr<ClipStream> stream,
                      float frequency_ratio) {
    const ClipInfo& info = clip ? clip->info : stream->info();
    m_channels = info.channels;
    m_sample_rate = info.sample_rate;
    m_callback = std::make_unique<Callback>();
    m_voice = device.create_source(m_channels, m_sample_rate, m_callback.get());
    if (!m_voice) {
        m_finished = true;
        return false;
    }
    m_clip = std::move(clip);
    m_stream = std::move(stream);
    if (wants_binaural(device)) {
        attach_binaural(device);
    }
    init_outputs(device);
    m_voice->SetFrequencyRatio(
        std::clamp(frequency_ratio, XAUDIO2_MIN_FREQ_RATIO, Device::k_max_frequency_ratio));
    if (m_clip) {
        submit_clip(0);
        if (m_finished) {
            return false;
        }
    } else {
        m_stream_buffers.assign(k_stream_queue, std::vector<int16_t>(k_stream_frames * m_channels));
        submit_stream();
    }
    return SUCCEEDED(m_voice->Start(0));
}

void GameSound::submit_clip(uint32_t play_begin) {
    XAUDIO2_BUFFER buffer{};
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes = static_cast<UINT32>(m_clip->pcm.size() * 2);
    buffer.pAudioData = reinterpret_cast<const BYTE*>(m_clip->pcm.data());
    const uint32_t frames = static_cast<uint32_t>(m_clip->info.frames);
    buffer.PlayBegin = frames > 0 ? std::min(play_begin, frames - 1) : 0;
    // Loop and envelope sounds loop [loop begin, loop end) until stopped:
    // only while the start lies before the loop end.
    const uint32_t loop_flags =
        osd::descriptor_loop | osd::descriptor_envelope_fast | osd::descriptor_envelope_slow;
    if ((m_setup.flags & loop_flags) && frames > 0) {
        const uint32_t end = std::min<uint32_t>(m_clip->info.loop_end, frames);
        const uint32_t begin = std::min(m_clip->info.loop_begin, end > 0 ? end - 1 : 0);
        if (buffer.PlayBegin < end) {
            buffer.LoopBegin = begin;
            buffer.LoopLength = end - begin;
            buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
        }
    }
    if (FAILED(m_voice->SubmitSourceBuffer(&buffer))) {
        destroy();
    }
}

uint32_t GameSound::position_ms() const {
    if (!m_voice || m_sample_rate == 0) {
        return 0;
    }
    XAUDIO2_VOICE_STATE state{};
    m_voice->GetState(&state);
    const uint64_t frames = m_position_base + (state.SamplesPlayed - m_samples_at_seek);
    return static_cast<uint32_t>(frames * 1000 / m_sample_rate);
}

uint32_t GameSound::duration_ms() const {
    const ClipInfo* info = m_clip ? &m_clip->info : m_stream ? &m_stream->info() : nullptr;
    return info && info->sample_rate ? static_cast<uint32_t>(info->frames * 1000 / info->sample_rate) : 0;
}

void GameSound::seek_ms(uint32_t ms) {
    if (!m_voice) {
        return;
    }
    const uint64_t frame = static_cast<uint64_t>(ms) * m_sample_rate / 1000;
    m_voice->Stop(0);
    m_voice->FlushSourceBuffers();
    XAUDIO2_VOICE_STATE state{};
    m_voice->GetState(&state);
    m_samples_at_seek = state.SamplesPlayed;
    m_position_base = frame;
    if (m_clip) {
        submit_clip(static_cast<uint32_t>(frame));
    } else if (m_stream) {
        // Flushed buffers may still be read by XAudio2 until their callbacks: keep them, use fresh ones.
        for (auto& b : m_stream_buffers) {
            m_retired_buffers.push_back(std::move(b));
        }
        m_stream_buffers.assign(k_stream_queue, std::vector<int16_t>(k_stream_frames * m_channels));
        m_stream_next = 0;
        m_stream->seek(frame);
        m_stream_ended = false;
        m_callback->stream_ended = false;
        submit_stream();
    }
    if (m_voice) {
        m_voice->Start(0);
    }
}

// Sounds with a place in the world. Those on the listener have no direction to give, and stay the game's mix.
bool GameSound::wants_binaural(const Device& device) const {
    return device.hrtf() && m_setup.output && (m_setup.flags & 1) != 0 && !m_setup.follow_listener &&
           m_setup.position.has_value();
}

void GameSound::attach_binaural(Device& device) {
    auto binaural = device.create_binaural();
    if (!binaural) {
        return;
    }
    XAUDIO2_SEND_DESCRIPTOR sends[2] = {
        {XAUDIO2_SEND_USEFILTER, binaural->voice()},
        {XAUDIO2_SEND_USEFILTER, device.reverb_voice()},
    };
    XAUDIO2_VOICE_SENDS send_list{2, sends};
    if (SUCCEEDED(m_voice->SetOutputVoices(&send_list))) {
        m_binaural = std::move(binaural);
    }
}

void GameSound::apply_hrtf(Device& device, const Listener& listener) {
    if (!m_voice || wants_binaural(device) == (m_binaural != nullptr)) {
        return;
    }
    if (m_binaural) {
        XAUDIO2_SEND_DESCRIPTOR sends[2] = {
            {XAUDIO2_SEND_USEFILTER, device.master()},
            {XAUDIO2_SEND_USEFILTER, device.reverb_voice()},
        };
        XAUDIO2_VOICE_SENDS send_list{2, sends};
        if (FAILED(m_voice->SetOutputVoices(&send_list))) {
            return;
        }
        m_binaural.reset();
    } else {
        attach_binaural(device);
    }
    init_outputs(device);
    update_3d(device, listener);
}

void GameSound::init_outputs(Device& device) {
    const auto* out = m_setup.output;
    if (!out) {
        return;
    }
    if (m_binaural) {
        // Silent until the first Update3D, like the game's voices before it: that is what sets the gain.
        const std::vector<float> none(m_channels, 0.0f);
        m_voice->SetOutputMatrix(m_binaural->voice(), m_channels, 1, none.data());
        const std::vector<float> reverb(static_cast<size_t>(m_channels) * 2, out->reverb_send);
        m_voice->SetOutputMatrix(device.reverb_voice(), m_channels, 2, reverb.data());
        return;
    }
    const uint32_t src = m_channels, dst = device.master_channels();
    m_matrix.assign(static_cast<size_t>(src) * dst, 0.0f);
    if (out->type == osd::OutputType::defined_speaker_output && src <= 2) {
        // Rows of ONAM: mono, or stereo left / right; the first `dst` columns (FL FR C LFE RL RR).
        for (uint32_t s = 0; s < src; ++s) {
            const uint8_t* row = out->speakers[src == 2 ? s + 1 : 0];
            for (uint32_t d = 0; d < dst; ++d) {
                m_matrix[d * src + s] = row[d] * 0.01f;
            }
        }
    } else if (src == 1) {
        std::copy(std::begin(k_mono_matrix), std::end(k_mono_matrix), m_matrix.begin());
    } else if (src == 2) {
        std::copy(std::begin(k_stereo_matrix), std::end(k_stereo_matrix), m_matrix.begin());
    }
    m_voice->SetOutputMatrix(device.master(), src, dst, m_matrix.data());
    const float send = out->reverb_send;
    std::vector<float> reverb(static_cast<size_t>(src) * 2, send);
    m_voice->SetOutputMatrix(device.reverb_voice(), src, 2, reverb.data());
}

void GameSound::submit_stream() {
    if (!m_stream || m_stream_ended) {
        return;
    }
    XAUDIO2_VOICE_STATE state{};
    m_voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    while (state.BuffersQueued < k_stream_queue && !m_stream_ended) {
        auto& buf = m_stream_buffers[m_stream_next];
        size_t got = m_stream->read(buf.data(), k_stream_frames);
        const bool loops = (m_setup.flags & osd::descriptor_loop) != 0;
        if (got < k_stream_frames && loops && m_stream->info().frames > 0) {
            // Streamed loops wrap to the loop start.
            while (got < k_stream_frames) {
                m_stream->seek(m_stream->info().loop_begin);
                const size_t more = m_stream->read(buf.data() + got * m_channels, k_stream_frames - got);
                if (more == 0) {
                    break;
                }
                got += more;
            }
        }
        XAUDIO2_BUFFER b{};
        b.AudioBytes = static_cast<UINT32>(got * m_channels * 2);
        b.pAudioData = reinterpret_cast<const BYTE*>(buf.data());
        if (got < k_stream_frames) {
            b.Flags = XAUDIO2_END_OF_STREAM;
            m_stream_ended = true;
        }
        if (b.AudioBytes == 0) {
            b.AudioBytes = static_cast<UINT32>(m_channels * 2);
            std::fill_n(buf.data(), m_channels, int16_t{0});
        }
        m_voice->SubmitSourceBuffer(&b);
        m_stream_next = (m_stream_next + 1) % k_stream_queue;
        ++state.BuffersQueued;
    }
}

glm::vec3 GameSound::emitter_position(const Listener& listener) const {
    glm::vec3 pos = m_setup.follow_listener ? listener.position : *m_setup.position;
    if (m_setup.segment_end) {
        // Line emitters sound from the point of their segment closest to the listener (Update3D; the start
        // point is restored after each calculation, so this is the clamped projection from the start).
        const glm::vec3 v = *m_setup.segment_end - pos;
        const float along = glm::dot(listener.position - pos, v);
        if (along > 0.0f) {
            const float len2 = glm::dot(v, v);
            pos = len2 > along ? pos + v * (along / len2) : *m_setup.segment_end;
        }
    }
    return pos;
}

// Where the sound is from the head. Every audio pass, not just on the voice poll: turning the head has to move
// the sound as smoothly as the filters can follow.
void GameSound::update_direction(const Listener& listener) {
    const glm::vec3 to = emitter_position(listener) - listener.position;
    const float distance = glm::length(to);
    if (distance < 1e-3f) {
        m_binaural->set_direction({0, 1, 0}, 0.0f);
        return;
    }
    const glm::vec3 right = glm::normalize(glm::cross(listener.forward, listener.up));
    const glm::vec3 head(glm::dot(to, right), glm::dot(to, listener.forward), glm::dot(to, listener.up));
    m_binaural->set_direction(head / distance, std::min(distance / k_inner_radius, 1.0f));
}

void GameSound::update_3d(Device& device, const Listener& listener) {
    const auto* out = m_setup.output;
    const bool hrtf = (m_setup.flags & 1) != 0;
    const bool speakers = (m_setup.flags & 4) != 0;
    if (!out || !(hrtf || speakers)) {
        return;
    }
    // Without a position the 3D update never runs: an attenuating sound then keeps the constructor's 10000 mB
    // distance attenuation and stays silent (the game logs "no position has been set").
    if (!m_setup.follow_listener && !m_setup.position) {
        return;
    }
    const glm::vec3 pos = emitter_position(listener);
    m_distance = glm::length(pos - listener.position);
    if (hrtf) {
        if (m_distance >= m_max_distance) {
            m_distance_attenuation = 10000;
            return;
        }
        m_distance_attenuation = 0;
        X3DAUDIO_LISTENER l{};
        l.OrientFront = to_x3d(listener.forward);
        l.OrientTop = to_x3d(listener.up);
        l.Position = to_x3d(listener.position);
        l.Velocity = to_x3d(listener.velocity);
        X3DAUDIO_CONE cone{0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        X3DAUDIO_DISTANCE_CURVE_POINT points[6];
        for (int i = 0; i < 6; ++i) {
            points[i] = {m_curve[i].distance, m_curve[i].value};
        }
        X3DAUDIO_DISTANCE_CURVE curve{points, 6};
        X3DAUDIO_EMITTER e{};
        e.pCone = &cone;
        e.OrientFront = {0.0f, 0.0f, 1.0f};
        e.OrientTop = {0.0f, 1.0f, 0.0f};
        e.Position = to_x3d(pos);
        e.Velocity = {0.0f, 0.0f, 0.0f};
        e.InnerRadius = k_inner_radius;
        e.InnerRadiusAngle = k_pi;
        e.ChannelCount = m_channels;
        e.ChannelRadius = 0.0f;
        static float azimuths[8] = {};
        e.pChannelAzimuths = azimuths;
        e.pVolumeCurve = &curve;
        e.CurveDistanceScaler = m_max_distance;
        e.DopplerScaler = 1.0f;
        X3DAUDIO_DSP_SETTINGS dsp{};
        m_matrix.assign(static_cast<size_t>(m_channels) * device.master_channels(), 0.0f);
        dsp.pMatrixCoefficients = m_matrix.data();
        dsp.SrcChannelCount = m_channels;
        dsp.DstChannelCount = device.master_channels();
        X3DAudioCalculate(device.x3d(), &l, &e,
                          X3DAUDIO_CALCULATE_MATRIX | X3DAUDIO_CALCULATE_LPF_DIRECT |
                              X3DAUDIO_CALCULATE_LPF_REVERB | X3DAUDIO_CALCULATE_REVERB |
                              X3DAUDIO_CALCULATE_DOPPLER,
                          &dsp);
        IXAudio2Voice* direct = device.master();
        if (m_binaural) {
            // The level without the panning, from X3DAudio on a single speaker. The 5.1 matrix can't give it: straight
            // above, below or inside the inner radius, the sound spreads over five speakers at a fifth of the
            // amplitude each, 7 dB down in power (measured), which is about speakers and not about the sound.
            direct = m_binaural->voice();
            std::vector<float> levels(m_channels, 0.0f);
            X3DAUDIO_DSP_SETTINGS mono{};
            mono.pMatrixCoefficients = levels.data();
            mono.SrcChannelCount = m_channels;
            mono.DstChannelCount = 1;
            X3DAudioCalculate(device.x3d_mono(), &l, &e, X3DAUDIO_CALCULATE_MATRIX, &mono);
            m_voice->SetOutputMatrix(direct, m_channels, 1, levels.data());
            update_direction(listener);
        } else {
            m_voice->SetOutputMatrix(direct, m_channels, device.master_channels(), m_matrix.data());
        }
        std::vector<float> reverb(static_cast<size_t>(m_channels) * 2, out->reverb_send * dsp.ReverbLevel);
        m_voice->SetOutputMatrix(device.reverb_voice(), m_channels, 2, reverb.data());
        XAUDIO2_FILTER_PARAMETERS filter{LowPassFilter,
                                         2.0f * std::sin(dsp.LPFDirectCoefficient * (k_pi / 6.0f)), 1.0f};
        m_voice->SetOutputFilterParameters(direct, &filter);
        return;
    }
    // Defined speaker output: the five-point curve as linear gain between min and max distance, converted to
    // hundredths of a dB.
    const float mn = m_min_distance, mx = m_max_distance;
    if (std::fabs(mx - mn) <= 1.0f * 0.0142875f) {
        return; // the game skips models whose range is under one unit
    }
    const float p[5] = {mn, (mx - mn) * 0.25f + mn, (mx - mn) * 0.5f + mn, (mx - mn) * 0.75f + mn, mx};
    float gain;
    if (m_distance >= mx) {
        gain = 1e-5f;
    } else if (m_distance <= p[0]) {
        gain = 1.0f;
    } else {
        int seg = 3;
        while (seg > 0 && m_distance <= p[seg]) {
            --seg;
        }
        const float t = (m_distance - p[seg]) / (p[seg + 1] - p[seg]);
        gain = t * (m_curve[seg + 2].value - m_curve[seg + 1].value) + m_curve[seg + 1].value;
    }
    // A gain of exactly 0 converts to an out-of-range integer, which the game stores as 0 dB (kept as is).
    m_distance_attenuation =
        gain == 0.0f
            ? 0
            : static_cast<uint16_t>(static_cast<int32_t>(std::fabs(std::log10(gain) * 20.0f) * 100.0f));
}

float GameSound::compute_volume(float master_volume, const std::vector<CategoryState>& categories) const {
    // The game: everything in hundredths of a dB, clamped to [-100 dB, 0 dB].
    float v = master_volume * m_setup.volume;
    if (m_setup.category != osd::k_none) {
        v *= effective_volume(categories, m_setup.category);
    }
    const float level = std::log10(std::clamp(v, 1.1920929e-7f, 1.0f));
    int32_t mb = static_cast<int16_t>(static_cast<int32_t>(level * 2000.0f)) - m_fade_attenuation -
                 m_setup.static_attenuation;
    if (m_setup.flags & (1 | 4)) {
        mb -= m_distance_attenuation;
    }
    mb = std::clamp(mb, -10000, 0);
    return std::pow(10.0f, mb * 0.0005f);
}

void GameSound::update(Device& device, const Listener& listener, float master_volume,
                       const std::vector<CategoryState>& categories, double now) {
    if (!m_voice) {
        return;
    }
    // The game: attenuating loops without auto-release (envelope-fast
    // ones excepted) stop beyond their max distance and play again when back in range.
    const uint32_t f = m_setup.flags;
    if ((f & (1 | 4)) && !(f & 0x10) && (f & 0x3800) && !(f & osd::descriptor_envelope_fast) && m_clip) {
        if (!m_culled && m_max_distance < m_distance) {
            m_culled = true;
            if (f & osd::descriptor_envelope_slow) {
                m_voice->ExitLoop();
            } else {
                m_voice->Stop(0); // a plain loop pauses in place
            }
        } else if (m_culled && m_max_distance > m_distance) {
            m_culled = false;
            if (f & osd::descriptor_envelope_slow) {
                m_voice->Stop(0);
                m_voice->FlushSourceBuffers();
                submit_clip(0); // restarts from the attack
            }
            m_voice->Start(0);
        }
    }
    submit_stream();
    // The voice poll runs at most every 66 ms: Update3D, then the end
    // check.
    if (m_last_poll < 0.0 || now - m_last_poll >= 0.066) {
        m_last_poll = now;
        update_3d(device, listener);
        if (m_callback->stream_ended.load() && !m_culled) {
            XAUDIO2_VOICE_STATE state{};
            m_voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (state.BuffersQueued == 0) {
                destroy();
                return;
            }
        }
    }
    if (m_binaural) {
        update_direction(listener);
    }
    const float gain = compute_volume(master_volume, categories);
    if (gain != m_last_gain) {
        m_voice->SetVolume(gain);
        m_last_gain = gain;
    }
}

void GameSound::pause() {
    if (m_voice) {
        m_voice->Stop(0);
    }
}

void GameSound::resume() {
    if (m_voice) {
        m_voice->Start(0);
    }
}

void GameSound::stop() {
    if (!m_voice || m_stopping) {
        return;
    }
    m_stopping = true;
    const uint32_t envelope = m_setup.flags & (osd::descriptor_envelope_fast | osd::descriptor_envelope_slow);
    if (envelope && m_clip) {
        if (m_setup.flags & osd::descriptor_envelope_fast) {
            // Jump to the loop end, then let the release play.
            const uint32_t end =
                std::min<uint32_t>(m_clip->info.loop_end, static_cast<uint32_t>(m_clip->info.frames));
            m_voice->Stop(0);
            m_voice->FlushSourceBuffers();
            XAUDIO2_BUFFER b{};
            b.Flags = XAUDIO2_END_OF_STREAM;
            b.AudioBytes = static_cast<UINT32>(m_clip->pcm.size() * 2);
            b.pAudioData = reinterpret_cast<const BYTE*>(m_clip->pcm.data());
            b.PlayBegin = std::min(end, static_cast<uint32_t>(m_clip->info.frames) - 1);
            m_voice->SubmitSourceBuffer(&b);
            m_voice->Start(0);
        } else {
            m_voice->ExitLoop();
        }
        return;
    }
    destroy();
}

} // namespace spud::audio
