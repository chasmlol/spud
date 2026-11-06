#version 460
// ISSnowSSS (ImageSpace ps 212): snow scatters light sideways, more in red-less channels' weights below. SRC0 the scene,
// SRC1 the normal and snow target, SRC2 the scene's depth. Only snow pixels are written.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
const vec4 k_taps[10] = vec4[10](vec4(0.000051, 0.000185, 0.004717, -2.0), vec4(0.000842, 0.002820, 0.019283, -1.28),
                                 vec4(0.006437, 0.013100, 0.036390, -0.72), vec4(0.020926, 0.035861, 0.082190, -0.32),
                                 vec4(0.079380, 0.113491, 0.077180, -0.08), vec4(0.079380, 0.113491, 0.077180, 0.08),
                                 vec4(0.020926, 0.035861, 0.082190, 0.32), vec4(0.006437, 0.013100, 0.036390, 0.72),
                                 vec4(0.000842, 0.002820, 0.019283, 1.28), vec4(0.000051, 0.000185, 0.004717, 2.0));
float camera_z(vec2 uv) { return min(view_units(sample_point(SRC2, uv).x), pc.post.sao_screen2.x); }
void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    float mask = texelFetch(sampler2D(u_textures[SRC1], u_samplers[0]), px, 0).w;
    if (mask <= 0.00001) {
        discard;
    }
    vec2 uv = target_uv();
    vec3 orig = sample_linear(SRC0, uv).rgb;
    float z0 = camera_z(uv);
    vec4 dark = pc.post.sss[0], p = pc.post.sss[1];
    vec2 step_uv = p.x * vec2(0.078125, 0.13889) / z0;
    float diff = p.y * 0.1;
    vec3 acc = orig * vec3(0.784728, 0.669086, 0.560479);
    for (int i = 0; i < 10; ++i) {
        vec2 q = clamp(uv + k_taps[i].w * step_uv, vec2(0.0), vec2(1.0));
        vec3 t = sample_linear(SRC0, q).rgb;
        float keep = min(abs(z0 - camera_z(q)) * diff, 1.0);
        acc += k_taps[i].rgb * mix(t, orig, keep);
    }
    float lum = dot(acc, vec3(0.3, 0.59, 0.11));
    vec3 lit = max(mix(dark.rgb * dark.w, orig, lum), vec3(0.0));
    o_color = vec4(mix(orig, mix(lit, acc, lum), mask), 1.0);
}
