#include "anim/clip.hpp"

#include "core/units.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>

namespace spud::anim {

namespace {

constexpr uint32_t k_rotation_align[] = {4, 1, 2, 1, 2, 4};
constexpr uint32_t k_rotation_size[] = {4, 5, 6, 3, 2, 16};

struct Cursor {
    const uint8_t* data;
    size_t at;

    void align(size_t n) { at = (at + n - 1) & ~(n - 1); }
    template <typename T>
    T take() {
        T v;
        std::memcpy(&v, data + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
};

glm::vec4 smallest_three(const uint32_t (&values)[3], uint32_t drop, bool negative, float scale, int32_t bias) {
    float q[4] = {};
    float sum = 0.0f;
    int i = 0;
    for (uint32_t slot = 0; slot < 4; ++slot) {
        if (slot != drop) {
            q[slot] = float(int32_t(values[i++]) - bias) * scale;
            sum += q[slot] * q[slot];
        }
    }
    const float w = std::sqrt(std::max(0.0f, 1.0f - sum));
    q[drop] = negative ? -w : w;
    return {q[0], q[1], q[2], q[3]};
}

// A control point's quaternion as x, y, z, w (the table of readers in the design).
glm::vec4 read_rotation(uint32_t type, const uint8_t* b) {
    switch (type) {
    case 0: {
        uint32_t v;
        std::memcpy(&v, b, 4);
        const uint32_t r = (v >> 18) & 0x3FF, t = v & 0x3FFFF;
        const float w = 1.0f - std::pow(float(r) * 0.00097751711f, 2.0f);
        const auto a = static_cast<uint32_t>(std::sqrt(float(t)));
        const float phi = a ? (float(t - a * a) / float(a * 2)) * 0.5f * 3.14159265f : 0.0f;
        const float theta = float(a) * 0.0019569471f * 0.5f * 3.14159265f;
        const float s = std::sqrt(std::max(0.0f, 1.0f - w * w));
        float q[4] = {std::cos(phi) * std::sin(theta) * s, std::sin(phi) * std::sin(theta) * s, std::cos(theta) * s, w};
        for (int i = 0; i < 4; ++i) {
            if (v & (0x10000000u << i)) {
                q[i] = -q[i];
            }
        }
        return {q[0], q[1], q[2], q[3]};
    }
    case 1: {
        const uint32_t v[3] = {uint32_t(b[0]) | uint32_t(b[1] & 0xF) << 8, uint32_t(b[1] >> 4) | uint32_t(b[2]) << 4,
                               uint32_t(b[3]) | uint32_t(b[4] & 0xF) << 8};
        return smallest_three(v, (b[4] >> 4) & 3, b[4] & 0x40, std::bit_cast<float>(968170391u), 2047);
    }
    case 2: {
        uint16_t x, y, z;
        std::memcpy(&x, b, 2);
        std::memcpy(&y, b + 2, 2);
        std::memcpy(&z, b + 4, 2);
        const uint32_t v[3] = {uint32_t(x & 0x7FFF), uint32_t(y & 0x7FFF), uint32_t(z & 0x7FFF)};
        const uint32_t drop = (((x >> 1) & 0x4000) | (y & 0x8000)) >> 14;
        return smallest_three(v, drop, z & 0x8000, std::bit_cast<float>(942999495u), 0x3FFF);
    }
    case 3: {
        const uint32_t v[3] = {uint32_t(b[0] & 0x7F), uint32_t(b[1] & 0x7F), uint32_t(b[2] & 0x7F)};
        const uint32_t drop = (((b[0] >> 1) & 0x40) | (b[1] & 0x80)) >> 6;
        return smallest_three(v, drop, b[2] & 0x80, std::bit_cast<float>(1010295941u), 63);
    }
    case 4: {
        glm::vec4 q(float(b[0] & 0xF) - 7.0f, float(b[0] >> 4) - 7.0f, float(b[1] & 0xF) - 7.0f, float(b[1] >> 4) - 7.0f);
        q /= 7.0f;
        const float n = glm::length(q);
        return n > 0.0f ? q / n : q;
    }
    default: {
        float q[4];
        std::memcpy(q, b, 16);
        return {q[0], q[1], q[2], q[3]};
    }
    }
}

// The span of a spline read at the cursor: its header, and which control points and knots evaluate it.
struct Span {
    uint32_t n = 0;
    uint32_t degree = 0;
    uint32_t span = 0;
    float knots[8] = {}; // the 2 * degree values around the span, in seconds
};

Span read_span(Cursor& c, uint8_t key, float knot_scale) {
    Span s;
    s.n = c.take<uint16_t>();
    s.degree = c.take<uint8_t>();
    const uint8_t* knots = c.data + c.at;
    c.at += s.n + s.degree + 2;
    if (key >= knots[s.n + 1]) {
        s.span = s.n;
    } else if (key <= knots[0]) {
        s.span = s.degree;
    } else {
        uint32_t lo = s.degree, hi = s.n + 1;
        for (;;) {
            const uint32_t mid = (lo + hi) / 2;
            if (key < knots[mid]) {
                hi = mid;
            } else if (key < knots[mid + 1]) {
                s.span = mid;
                break;
            } else {
                lo = mid;
            }
        }
    }
    for (uint32_t i = 0; i < 2 * s.degree && i < 8; ++i) {
        s.knots[i] = float(knots[s.span - s.degree + 1 + i]) * knot_scale;
    }
    return s;
}

// de Boor over `count` components.
template <size_t N>
std::array<float, N> de_boor(float u, const Span& s, std::array<float, N>* d) {
    const uint32_t p = s.degree;
    for (uint32_t r = 1; r <= p; ++r) {
        for (uint32_t j = p; j >= r; --j) {
            const float lo = s.knots[j - 1];
            const float hi = s.knots[j + p - r];
            const float a = hi != lo ? (u - lo) / (hi - lo) : 0.0f;
            for (size_t k = 0; k < N; ++k) {
                d[j][k] = (1.0f - a) * d[j - 1][k] + a * d[j][k];
            }
        }
    }
    return d[p];
}

// A vector track (translation or scale), or a float track as its X.
glm::vec3 read_vector(Cursor& c, uint8_t key, float u, uint8_t flags, uint32_t quant, float fallback, float knot_scale) {
    glm::vec3 lo(fallback), hi(fallback), value(fallback);
    Span s;
    const bool spline = flags & 0xF0;
    if (spline) {
        s = read_span(c, key, knot_scale);
    }
    c.align(4);
    for (int axis = 0; axis < 3; ++axis) {
        if (flags & (1 << axis)) {
            value[axis] = c.take<float>();
        } else if (flags & (0x10 << axis)) {
            lo[axis] = c.take<float>();
            hi[axis] = c.take<float>();
        }
    }
    if (!spline) {
        return value;
    }
    c.align(2);
    const size_t size = quant ? 2 : 1;
    uint32_t axes = 0;
    for (int axis = 0; axis < 3; ++axis) {
        axes += (flags >> (4 + axis)) & 1;
    }
    std::array<float, 3> d[5];
    const size_t start = c.at;
    for (uint32_t i = 0; i <= s.degree; ++i) {
        size_t at = start + size_t(s.span - s.degree + i) * axes * size;
        for (int axis = 0; axis < 3; ++axis) {
            if (flags & (0x10 << axis)) {
                float q;
                if (quant) {
                    uint16_t v;
                    std::memcpy(&v, c.data + at, 2);
                    q = float(v) / 65535.0f;
                } else {
                    q = float(c.data[at]) / 255.0f;
                }
                at += size;
                d[i][axis] = lo[axis] + q * (hi[axis] - lo[axis]);
            } else {
                d[i][axis] = value[axis];
            }
        }
    }
    c.at = start + size_t(s.n + 1) * axes * size;
    const auto r = de_boor<3>(u, s, d);
    return {r[0], r[1], r[2]};
}

glm::quat read_quaternion(Cursor& c, uint8_t key, float u, uint8_t flags, uint32_t type, float knot_scale) {
    if (flags & 0xF0) {
        const Span s = read_span(c, key, knot_scale);
        c.align(k_rotation_align[type]);
        const size_t start = c.at;
        const uint32_t size = k_rotation_size[type];
        std::array<float, 4> d[5];
        for (uint32_t i = 0; i <= s.degree; ++i) {
            const glm::vec4 q = read_rotation(type, c.data + start + size_t(s.span - s.degree + i) * size);
            d[i] = {q.x, q.y, q.z, q.w};
        }
        c.at = start + size_t(s.n + 1) * size;
        const auto r = de_boor<4>(u, s, d);
        const glm::vec4 q(r[0], r[1], r[2], r[3]);
        const float n = glm::length(q);
        return n > 0.0f ? glm::quat(q.w / n, q.x / n, q.y / n, q.z / n) : glm::quat(1, 0, 0, 0);
    }
    if (flags & 0x0F) {
        c.align(k_rotation_align[type]);
        const glm::vec4 q = read_rotation(type, c.data + c.at);
        c.at += k_rotation_size[type];
        return {q.w, q.x, q.y, q.z};
    }
    return {1.0f, 0.0f, 0.0f, 0.0f};
}

} // namespace

Skeleton Skeleton::from(const osk::Skeleton& s) {
    Skeleton out;
    out.names.reserve(s.bones.size());
    for (size_t i = 0; i < s.bones.size(); ++i) {
        const osk::Bone& b = s.bones[i];
        out.names.push_back(b.name);
        out.parents.push_back(static_cast<int16_t>(b.parent));
        Qs q;
        q.t = glm::vec3(b.translation[0], b.translation[1], b.translation[2]) / units::k_skyrim_unit_m;
        glm::mat3 r;
        for (int row = 0; row < 3; ++row) {
            for (int c = 0; c < 3; ++c) {
                r[c][row] = b.rotation[row][c];
            }
        }
        q.r = glm::quat_cast(r);
        q.s = glm::vec3(b.scale);
        out.reference.push_back(q);
        out.lock_translation.push_back(b.lock_translation);
        std::string lower = b.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        out.by_name.emplace(std::move(lower), static_cast<int32_t>(i));
    }
    return out;
}

glm::vec3 ClipMotion::translation_at(float time) const {
    if (translation.empty()) {
        return glm::vec3(0.0f);
    }
    const float t = std::clamp(time, 0.0f, translation.back().time);
    float before = 0.0f;
    glm::vec3 from(0.0f);
    for (const auto& k : translation) {
        if (k.time >= t) {
            const float span = k.time - before;
            const float f = span <= 1.2e-7f ? 1.0f : (t - before) / span;
            return from + (k.translation - from) * f;
        }
        before = k.time;
        from = k.translation;
    }
    return from;
}

glm::quat ClipMotion::rotation_at(float time) const {
    if (rotation.empty()) {
        return {1.0f, 0.0f, 0.0f, 0.0f};
    }
    const float t = std::clamp(time, 0.0f, rotation.back().time);
    float before = 0.0f;
    glm::quat from(1.0f, 0.0f, 0.0f, 0.0f);
    for (const auto& k : rotation) {
        if (k.time >= t) {
            const float span = k.time - before;
            const float f = span <= 1.2e-7f ? 1.0f : (t - before) / span;
            return glm::slerp(from, k.rotation, f);
        }
        before = k.time;
        from = k.rotation;
    }
    return from;
}

int32_t Skeleton::bone(std::string_view name) const {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    auto it = by_name.find(lower);
    return it != by_name.end() ? it->second : -1;
}

void Clip::sample(float time, std::span<Qs> out, std::span<float> floats) const {
    const osn::Clip& a = data;
    if (!a.raw.empty() && a.frames != 0) { // uncompressed: between the two frames either side
        const float f = a.duration > 0.0f ? std::clamp(time, 0.0f, a.duration) / a.duration * float(a.frames - 1) : 0.0f;
        const uint32_t frame = std::min(static_cast<uint32_t>(f), a.frames - 1), next = std::min(frame + 1, a.frames - 1);
        const float fraction = f - float(frame);
        const auto at = [&](uint32_t n, uint32_t t) { return a.raw.data() + (size_t(n) * a.transform_tracks + t) * 12; };
        for (uint32_t t = 0; t < std::min<uint32_t>(a.transform_tracks, static_cast<uint32_t>(out.size())); ++t) {
            const float *x = at(frame, t), *y = at(next, t);
            glm::quat from(x[7], x[4], x[5], x[6]), to(y[7], y[4], y[5], y[6]);
            if (glm::dot(from, to) < 0.0f) {
                to = -to;
            }
            out[t].t = glm::mix(glm::vec3(x[0], x[1], x[2]), glm::vec3(y[0], y[1], y[2]), fraction);
            out[t].r = glm::normalize(glm::quat(glm::mix(from.w, to.w, fraction),
                                                glm::mix(from.x, to.x, fraction), glm::mix(from.y, to.y, fraction),
                                                glm::mix(from.z, to.z, fraction)));
            out[t].s = glm::mix(glm::vec3(x[8], x[9], x[10]), glm::vec3(y[8], y[9], y[10]), fraction);
        }
        return;
    }
    if (a.frames == 0 || a.blocks == 0) {
        return;
    }
    // Picking the block and time.
    const float f = a.duration > 0.0f ? std::clamp(time, 0.0f, a.duration) / a.duration * float(a.frames - 1) : 0.0f;
    uint32_t frame = static_cast<uint32_t>(f);
    float fraction = std::clamp(f - float(frame), 0.0f, 1.0f);
    if (frame + 2 > a.frames) {
        frame = a.frames >= 2 ? a.frames - 2 : 0;
        fraction = a.frames >= 2 ? 1.0f : 0.0f;
    }
    const uint32_t p = a.max_frames_per_block - 1;
    const uint32_t block = std::min(p ? frame / p : 0, a.blocks - 1);
    const float block_time = (float(frame - block * p) + fraction) * a.frame_duration;
    const auto key = static_cast<uint8_t>(block_time * a.block_inverse_duration * float(p));
    const float knot_scale = a.frame_duration;

    const uint8_t* start = a.data.data() + a.block_offsets[block];
    Cursor c{start, a.mask_size};
    const uint32_t tracks = std::min<uint32_t>(a.transform_tracks, static_cast<uint32_t>(out.size()));
    for (uint32_t t = 0; t < a.transform_tracks; ++t) {
        const uint8_t* mask = start + size_t(t) * 4;
        const uint8_t quant = mask[0];
        const uint32_t translation_quant = quant & 3, rotation_type = (quant >> 2) & 0xF, scale_quant = (quant >> 6) & 3;
        Qs q;
        q.t = read_vector(c, key, block_time, mask[1], translation_quant, 0.0f, knot_scale);
        c.align(4);
        q.r = read_quaternion(c, key, block_time, mask[2], rotation_type, knot_scale);
        c.align(4);
        q.s = read_vector(c, key, block_time, mask[3], scale_quant, 1.0f, knot_scale);
        c.align(4);
        if (t < tracks) {
            out[t] = q;
        }
    }
    if (!floats.empty() && a.float_tracks) {
        const uint8_t* masks = start + size_t(a.transform_tracks) * 4;
        Cursor fc{start, a.float_block_offsets[block]};
        for (uint32_t t = 0; t < a.float_tracks; ++t) {
            const uint8_t m = masks[t];
            const uint32_t quant = (m >> 1) & 3;
            const auto flags = static_cast<uint8_t>(m & ~6u);
            const float v = read_vector(fc, key, block_time, flags, quant, 0.0f, knot_scale).x;
            fc.align(4);
            if (t < floats.size()) {
                floats[t] = v;
            }
        }
    }
}

} // namespace spud::anim
