#include "audio/hrtf.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <numbers>
#include <utility>

namespace spud::audio {

namespace {

struct DataRing {
    float elevation;
    uint32_t count;
};

#include "audio/hrtf_data.inc"

static_assert(k_hrtf_taps == HrtfSet::k_taps);

constexpr float k_pi = std::numbers::pi_v<float>;
constexpr float k_slew_seconds = 0.02f; // a turn of the head glides over the audio passes' 33 ms steps
constexpr float k_center_gain = 0.70710678f; // both ears, the same power as the filters
constexpr float k_inaudible = 2e-5f;         // -94 dB, below 16-bit audio's floor

// A direction change smaller than this keeps the filter (cosine of about 0.25 degrees).
constexpr float k_same_direction = 0.99999f;

// `taps` at k_hrtf_rate re-sampled to `rate` <= k_hrtf_rate: a Hann-windowed sinc, scaled to keep the response.
void resample(const float* in, float* out, uint32_t rate) {
    constexpr int k_half_width = 16;
    const double step = double(k_hrtf_rate) / rate; // input samples per output sample
    const double cutoff = 1.0 / step;
    for (uint32_t m = 0; m < k_hrtf_taps; ++m) {
        const double center = m * step;
        double sum = 0.0;
        const int first = std::max(0, int(std::ceil(center - k_half_width)));
        const int last = std::min(int(k_hrtf_taps) - 1, int(std::floor(center + k_half_width)));
        for (int k = first; k <= last; ++k) {
            const double t = k - center;
            const double sinc = t == 0.0 ? 1.0
                                         : std::sin(std::numbers::pi * cutoff * t) / (std::numbers::pi * cutoff * t);
            const double window = 0.5 + 0.5 * std::cos(std::numbers::pi * t / k_half_width);
            sum += in[k] * cutoff * sinc * window;
        }
        out[m] = float(sum * step);
    }
}

} // namespace

HrtfSet::HrtfSet(uint32_t sample_rate) {
    uint32_t first = 0;
    for (const DataRing& r : k_hrtf_rings) {
        m_rings.push_back({r.elevation, r.count, first});
        first += r.count;
    }
    m_filters.resize(first);
    const float itd_scale = sample_rate * 1e-6f;
    for (uint32_t d = 0; d < first; ++d) {
        Filter& f = m_filters[d];
        f.itd = k_hrtf_itd_us[d] * itd_scale;
        for (uint32_t ear = 0; ear < 2; ++ear) {
            const int16_t* src = k_hrtf_filters + (size_t(d) * 2 + ear) * k_taps;
            float taps[k_taps];
            for (uint32_t k = 0; k < k_taps; ++k) {
                taps[k] = src[k] * k_hrtf_scale;
            }
            if (sample_rate == k_hrtf_rate) {
                std::copy(std::begin(taps), std::end(taps), f.ear[ear]);
            } else {
                resample(taps, f.ear[ear], sample_rate);
            }
        }
    }
}

void HrtfSet::lookup(const glm::vec3& direction, Filter& out) const {
    // Azimuth grows toward the left, as the measurements are labelled.
    float azimuth = std::atan2(-direction.x, direction.y) * (180.0f / k_pi);
    if (azimuth < 0.0f) {
        azimuth += 360.0f;
    }
    const float elevation = std::asin(std::clamp(direction.z, -1.0f, 1.0f)) * (180.0f / k_pi);

    // Below the lowest ring and above the highest the nearest ring stands in.
    size_t upper = 0;
    while (upper < m_rings.size() && m_rings[upper].elevation < elevation) {
        ++upper;
    }
    const size_t a = upper == 0 ? 0 : upper - 1;
    const size_t b = std::min(upper, m_rings.size() - 1);
    const float span = m_rings[b].elevation - m_rings[a].elevation;
    const float along = span > 0.0f ? std::clamp((elevation - m_rings[a].elevation) / span, 0.0f, 1.0f) : 0.0f;

    struct Part {
        const Filter* filter;
        float weight;
    };
    Part parts[4];
    int count = 0;
    for (const auto& [index, ring_weight] : {std::pair{a, 1.0f - along}, std::pair{b, along}}) {
        if (ring_weight <= 0.0f) {
            continue;
        }
        const Ring& ring = m_rings[index];
        const float position = azimuth * ring.count / 360.0f;
        const uint32_t i = uint32_t(position) % ring.count;
        const float t = position - std::floor(position);
        parts[count++] = {&m_filters[ring.first + i], ring_weight * (1.0f - t)};
        parts[count++] = {&m_filters[ring.first + (i + 1) % ring.count], ring_weight * t};
    }

    std::memset(out.ear, 0, sizeof(out.ear));
    out.itd = 0.0f;
    for (int p = 0; p < count; ++p) {
        const float w = parts[p].weight;
        const Filter& f = *parts[p].filter;
        out.itd += w * f.itd;
        for (uint32_t ear = 0; ear < 2; ++ear) {
            for (uint32_t k = 0; k < k_taps; ++k) {
                out.ear[ear][k] += w * f.ear[ear][k];
            }
        }
    }
}

namespace {

uint64_t pack(const glm::vec3& d, float focus) {
    auto s16 = [](float v) { return uint64_t(uint16_t(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)))); };
    return s16(d.x) | s16(d.y) << 16 | s16(d.z) << 32 | s16(focus) << 48;
}

float unpack(uint64_t packed, int shift) {
    return int16_t(uint16_t(packed >> shift)) / 32767.0f;
}

} // namespace

BinauralRenderer::BinauralRenderer(const HrtfSet& set, uint32_t sample_rate)
    : m_set(set), m_slew_seconds(k_slew_seconds * sample_rate) {
    set_target({0, 1, 0}, 1.0f);
}

void BinauralRenderer::prepare(uint32_t max_frames) {
    m_input.assign(k_history + max_frames, 0.0f);
    for (auto& d : m_delayed) {
        d.assign(HrtfSet::k_taps - 1 + max_frames, 0.0f);
    }
    m_from.assign(max_frames, 0.0f);
    m_to.assign(max_frames, 0.0f);
}

void BinauralRenderer::set_target(const glm::vec3& direction, float focus) {
    m_target.store(pack(direction, focus), std::memory_order_relaxed);
}

// Sums taps four at a time, so each pass over the block does four multiplies per store.
void BinauralRenderer::filter(const float* delayed, const float* taps, float* out, uint32_t frames) const {
    const float* x = delayed + HrtfSet::k_taps - 1;
    std::memset(out, 0, frames * sizeof(float));
    for (uint32_t k = 0; k < HrtfSet::k_taps; k += 4) {
        const float h0 = taps[k], h1 = taps[k + 1], h2 = taps[k + 2], h3 = taps[k + 3];
        const float* __restrict x0 = x - k;
        const float* __restrict x1 = x0 - 1;
        const float* __restrict x2 = x0 - 2;
        const float* __restrict x3 = x0 - 3;
        float* __restrict y = out;
        for (uint32_t n = 0; n < frames; ++n) {
            y[n] += h0 * x0[n] + h1 * x1[n] + h2 * x2[n] + h3 * x3[n];
        }
    }
}

bool BinauralRenderer::process(const float* in, float* out, uint32_t frames) {
    // A loop out of range keeps playing at -100 dB: nothing to hear, so nothing to filter. Most voices in a scene
    // are like that.
    if (in) {
        float peak = 0.0f;
        for (uint32_t n = 0; n < frames; ++n) {
            peak = std::max(peak, std::fabs(in[n]));
        }
        if (peak < k_inaudible) {
            in = nullptr;
        }
    }
    // Nothing coming in and the history has run out: stay quiet until the next sound.
    constexpr uint32_t k_tail = k_history + HrtfSet::k_taps;
    if (!in && m_quiet_frames >= k_tail) {
        return false;
    }
    m_quiet_frames = in ? 0 : m_quiet_frames + frames;

    // Move toward the target, then decide whether this block needs a new filter.
    const uint64_t packed = m_target.load(std::memory_order_relaxed);
    glm::vec3 target(unpack(packed, 0), unpack(packed, 16), unpack(packed, 32));
    const float length = glm::length(target);
    target = length > 1e-3f ? target / length : m_direction;
    const float target_focus = unpack(packed, 48);
    float focus = target_focus;
    glm::vec3 direction = target;
    if (m_started) {
        const float a = 1.0f - std::exp(-float(frames) / m_slew_seconds);
        const glm::vec3 moved = m_direction + (target - m_direction) * a;
        const float moved_length = glm::length(moved);
        // Straight through the head there is no way to glide: the target takes over at once.
        direction = moved_length > 1e-3f ? moved / moved_length : target;
        focus = m_focus + (target_focus - m_focus) * a;
    } else {
        m_set.lookup(direction, m_filter);
        m_direction = direction;
        m_focus = focus;
        m_started = true;
    }
    const bool moving = glm::dot(direction, m_direction) < k_same_direction;
    if (moving) {
        m_set.lookup(direction, m_next);
    }
    const HrtfSet::Filter& next = moving ? m_next : m_filter;

    // The input, after its history.
    float* x = m_input.data() + k_history;
    if (in) {
        std::memcpy(x, in, frames * sizeof(float));
    } else {
        std::memset(x, 0, frames * sizeof(float));
    }

    const float frame_step = 1.0f / float(frames);
    for (uint32_t ear = 0; ear < 2; ++ear) {
        // Each ear's delay, gliding across the block: only the far ear lags. Third-order Lagrange interpolation.
        const float sign = ear == 0 ? 1.0f : -1.0f;
        const float from = std::clamp(std::max(m_filter.itd * sign, 0.0f), 0.0f, float(k_max_delay));
        const float to = std::clamp(std::max(next.itd * sign, 0.0f), 0.0f, float(k_max_delay));
        float* delayed = m_delayed[ear].data() + HrtfSet::k_taps - 1;
        for (uint32_t n = 0; n < frames; ++n) {
            const float delay = from + (to - from) * (float(n) * frame_step) + float(k_lookahead);
            const float position = float(n) - delay;
            const float base = std::floor(position);
            const float f = position - base;
            const float* p = x + int(base);
            const float c0 = -f * (f - 1.0f) * (f - 2.0f) * (1.0f / 6.0f);
            const float c1 = (f + 1.0f) * (f - 1.0f) * (f - 2.0f) * 0.5f;
            const float c2 = -(f + 1.0f) * f * (f - 2.0f) * 0.5f;
            const float c3 = (f + 1.0f) * f * (f - 1.0f) * (1.0f / 6.0f);
            delayed[n] = c0 * p[-1] + c1 * p[0] + c2 * p[1] + c3 * p[2];
        }
        filter(m_delayed[ear].data(), m_filter.ear[ear], m_from.data(), frames);
        if (moving) {
            filter(m_delayed[ear].data(), next.ear[ear], m_to.data(), frames);
        }
        // Old filter into new across the block, and out of the filters into the centre as the focus falls.
        for (uint32_t n = 0; n < frames; ++n) {
            const float t = (float(n) + 0.5f) * frame_step;
            const float wet = moving ? m_from[n] + (m_to[n] - m_from[n]) * t : m_from[n];
            const float w = m_focus + (focus - m_focus) * t;
            out[n * 2 + ear] = w * wet + (1.0f - w) * k_center_gain * x[int(n) - int(k_lookahead)];
        }
        std::memmove(m_delayed[ear].data(), m_delayed[ear].data() + frames, (HrtfSet::k_taps - 1) * sizeof(float));
    }
    if (moving) {
        m_filter = m_next;
        m_direction = direction;
    }
    m_focus = focus;
    std::memmove(m_input.data(), m_input.data() + frames, k_history * sizeof(float));
    return true;
}

} // namespace spud::audio
