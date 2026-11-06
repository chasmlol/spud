#version 460
// ISSAORawAONoTemporal (ImageSpace ps 201): the game's history-free path, 15 samples, changed so that nothing depends on
// where a pixel falls on a screen grid: that crawls in motion with no history to hide it.
// SRC1 the normals, SRC2 the scene's depth.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec4 proj = pc.post.sao_proj, info = pc.post.sao_info, screen = pc.post.sao_screen, info2 = pc.post.sao_info2;
    vec2 uv = target_uv();
    ivec2 ip = ivec2(uv * screen.xy);
    float z = min(view_units(texelFetch(sampler2D(u_textures[SRC2], u_samplers[0]), ip, 0).x), pc.post.sao_screen2.x);
    vec3 c = vec3((vec2(ip) + 0.5) * proj.xy + proj.zw, 1.0) * z;
    float t = clamp(z * 0.000143, 0.0, 1.0);
    float key_hi = floor(t * 256.0);
    vec4 outv = vec4(0.0, key_hi * 0.00390625, t * 256.0 - key_hi, 1.0);
    // The normal over the pixels around this one, four linear taps: a single pixel of a pebbled normal map is a dark speck
    // that blinks as the surface slides under it.
    const vec2 d = screen.zw * 0.5;
    vec2 e = (sample_linear(SRC1, uv + vec2(-d.x, -d.y)).xy + sample_linear(SRC1, uv + vec2(d.x, -d.y)).xy +
              sample_linear(SRC1, uv + vec2(-d.x, d.y)).xy + sample_linear(SRC1, uv + d).xy) - 2.0;
    float g = dot(e, e);
    vec2 exy = sqrt(max(1.0 - 0.25 * g, 0.0)) * e;
    vec3 n = vec3(-exy.x, exy.y, -(1.0 - 0.5 * g));
    float ssr = info.y / z;
    vec2 q = uv * 2.0 - 1.0;
    float bias = 2.0 * dot(q, q) + max(t - 0.3, 0.0) * 10.0 + info.z;
    float sum = 0.0;
    // Past 7000 units the result is 1 whatever the samples say (below): nothing to gather.
    int samples = t < 1.0 ? 15 : 0;
    // Angle (i + 0.5) x 3.349334, the same at every pixel: the game's per-pixel rotation is noise fixed to the screen, which
    // crawls over the scene as the camera moves. A constant step, so each sample rotates the last.
    vec2 axis = vec2(cos(1.674667), sin(1.674667));
    const vec2 turn = vec2(-0.978499, -0.206250); // cos, sin 3.349334
    for (int i = 0; i < samples; ++i) {
        float fi = float(i) + 0.5;
        float r = ssr * fi * 0.066667;
        vec2 o = r * axis;
        axis = vec2(axis.x * turn.x - axis.y * turn.y, axis.x * turn.y + axis.y * turn.x);
        ivec2 sp = ip + ivec2(o);
        float zq = min(view_units(texelFetch(sampler2D(u_textures[SRC2], u_samplers[0]),
                                             clamp(sp, ivec2(0), ivec2(screen.xy) - 1), 0).x), pc.post.sao_screen2.x);
        vec3 v = vec3((vec2(sp) + 0.5) * proj.xy + proj.zw, 1.0) * zq - c;
        float vv = dot(v, v);
        float f = max(info2.y - vv, 0.0);
        float a = max((dot(v, n) - bias) / (vv + 0.01), 0.0) * f * f * f;
        sum += zq >= info2.w ? a : 0.0;
    }
    float ao = max(1.0 - sum * info.w * 0.333333, 0.0);
    float ex = clamp(info2.x, 0.00001, 0.99999);
    ao = ex < 0.5 ? pow(ao, 2.0 * ex) : pow(ao, 1.0 / (1.0 - (ex - 0.5) * 2.0));
    outv.x = t == 1.0 ? 1.0 : ao;
    o_color = outv;
}
