#version 460
// ISDownsampleCS (ImageSpace cs 161) and the first ISIBLensFlares (ps 166): the brightest of four taps, kept at x 0.7 when
// a channel passes 1. The game's jitter compensation averages it with last frame's; with no jitter a still view gives
// the same. SRC0 the scene, SRC1 its depth. The game's sky is saturated by then and spud's composite leaves it alone.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec2 uv = target_uv();
    vec2 t = 1.0 / slot_size(SRC0);
    const vec2 taps[4] = vec2[4](vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));
    vec3 best = vec3(0.0);
    float brightest = -1.0;
    for (int i = 0; i < 4; ++i) {
        vec2 q = uv + taps[i] * t;
        vec3 c = sample_linear(SRC0, q).rgb;
        if (sample_point(SRC1, q).x <= 0.0) {
            c = min(c, vec3(1.0));
        }
        float l = dot(c, vec3(0.3, 0.59, 0.11));
        if (l > brightest) {
            brightest = l;
            best = c;
        }
    }
    vec3 over = clamp(best - 1.0, 0.0, 1.0);
    o_color = vec4(0.0);
    if (over.x + over.y + over.z > 0.0) {
        o_color = vec4(clamp(best, 0.0, 1.0) * 0.7, 0.0);
        if (pc.post.lens_flag.value == 0u) {
            pc.post.lens_flag.value = 1u;
        }
    }
}
