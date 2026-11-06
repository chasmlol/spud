#version 460
// The game's pixel techniques.
#include "common.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in float v_horizon;
layout(location = 0) out vec4 o_color;

// the 8x8 Bayer matrix, stored as bytes 0..63.
float dither() {
    uvec2 p = uvec2(gl_FragCoord.xy) & 7u;
    uint a = p.x ^ p.y, y = p.y;
    uint v = ((a & 1u) << 5) | ((y & 1u) << 4) | ((a & 2u) << 2) | ((y & 2u) << 1) | ((a & 4u) >> 1) | ((y & 4u) >> 2);
    return float(v) / 255.0 / 32.0 - 1.0 / 128.0;
}

void main() {
    SkyDraw d = pc.post.sky_draws.d[pc.mode];
    uint tech = d.info.x;
    float py = d.params.z; // PParams.y: the image space's Sky Scale
    vec4 t = vec4(1.0);
    if (tech != 8u) {
        t = textureLod(sampler2D(u_textures[nonuniformEXT(d.info.y)], u_samplers[SAMPLER_WRAP]), v_uv, 0.0);
    }
    if (tech == 5u) {
        vec4 t1 = textureLod(sampler2D(u_textures[nonuniformEXT(d.info.z)], u_samplers[SAMPLER_WRAP]), v_uv, 0.0);
        t = mix(t, t1, d.params.x);
    }
    if (tech == 8u) {
        o_color = vec4(v_color.rgb + dither() + py, v_color.a);
    } else if (tech == 2u) {
        if (t.a < 0.50196081) {
            discard;
        }
        o_color = t;
    } else if (tech == 3u) {
        o_color = vec4(1.5 * (v_color.rgb * t.rgb + py), t.a * v_color.a * v_horizon);
    } else if (tech == 6u) {
        o_color = vec4(v_color.rgb * t.rgb + py, t.a * d.params.x * v_color.a);
    } else {
        o_color = vec4(v_color.rgb * t.rgb + py, t.a * v_color.a);
    }
    if (o_color.a <= 0.0) {
        discard; // both blends leave the target as it was: the empty parts of a cloud layer
    }
}
