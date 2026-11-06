#version 460
#include "uimovie_common.glsl"

layout(set = 0, binding = 0) uniform texture2D u_bitmap;
layout(set = 1, binding = 0) uniform sampler u_samplers[4];

layout(location = 0) in vec2 v_fill;
layout(location = 1) flat in uint v_draw;
layout(location = 0) out vec4 o_color;

vec4 unpack(uint c) {
    return vec4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u), float(c >> 24)) / 255.0;
}

vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
vec3 to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }

vec4 gradient(MovieDraw d, float t) {
    const uint spread = uint(d.fill_t.z);
    if (spread == 1u) {
        t = 1.0 - abs(mod(t, 2.0) - 1.0);
    } else if (spread == 2u) {
        t = fract(t);
    }
    t = clamp(t, 0.0, 1.0);
    MovieStops stops = MovieStops(d.stops);
    const bool lin = (d.flags & 2u) != 0u;
    vec4 c = unpack(stops.s[0].color);
    for (uint i = 1u; i < d.stop_count; ++i) {
        const GradientStop a = stops.s[i - 1u], b = stops.s[i];
        if (t <= a.ratio) {
            break;
        }
        vec4 ca = unpack(a.color), cb = unpack(b.color);
        if (lin) {
            ca.rgb = to_linear(ca.rgb);
            cb.rgb = to_linear(cb.rgb);
        }
        const float f = b.ratio > a.ratio ? clamp((t - a.ratio) / (b.ratio - a.ratio), 0.0, 1.0) : 1.0;
        c = mix(ca, cb, f);
        if (lin) {
            c.rgb = to_srgb(c.rgb);
        }
    }
    return c;
}

void main() {
    const MovieDraw d = pc.draws.d[v_draw];
    const uint paint = uint(d.xform_t.w);
    vec4 c; // straight colour
    if (paint == 0u) {
        c = d.color;
    } else if (paint == 1u) {
        c = gradient(d, (v_fill.x + 1.0) * 0.5);
    } else if (paint == 2u) {
        c = gradient(d, length(v_fill));
    } else if (paint == 3u) {
        // Focal radial: the ray from the focal point (f, 0) through this point meets the unit circle at the edge.
        const float f = clamp(d.fill_t.w, -0.99, 0.99);
        const vec2 p = v_fill - vec2(f, 0.0);
        const vec2 dir = normalize(p + vec2(1e-9, 0.0));
        const float b = dot(vec2(f, 0.0), dir);
        const float edge = -b + sqrt(max(b * b - (f * f - 1.0), 0.0));
        c = gradient(d, length(p) / max(edge, 1e-6));
    } else {
        const vec4 texel = texture(sampler2D(u_bitmap, u_samplers[d.sampler_index]), v_fill);
        // Flag 4: a straight-alpha texture (an engine image), filtered as it is; else premultiplied (the movies'
        // own bitmaps).
        c = (d.flags & 4u) != 0u ? texel : texel.a > 0.0 ? vec4(texel.rgb / texel.a, texel.a) : vec4(0.0);
    }
    c = clamp(c * d.mul + d.add, 0.0, 1.0);
    o_color = vec4(c.rgb * c.a, c.a);
}
