#version 460
// ISHDRDownSample16LightAdapt (ImageSpace ps 51): the scene's luminance and two adapted values, each moving at least
// 1/256 a frame towards it and never past it. SRC0 the last luminance level, SRC1 the previous adapted target.
// adapt.xy the two rates, adapt.z 1 to jump to the scene.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec2 uv = target_uv();
    vec2 t = 1.0 / slot_size(SRC0);
    float l = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            l += sample_linear(SRC0, uv + (vec2(x, y) - 1.5) * t).x;
        }
    }
    l /= 16.0;
    vec4 a = pc.post.adapt;
    if (a.w != 0.0) {
        o_color = vec4(a.w, a.w, l, 0.0); // held for the history audit (set hold_effect_state)
        return;
    }
    if (a.z != 0.0) {
        o_color = vec4(l, l, l, 0.0);
        return;
    }
    vec2 prev = sample_point(SRC1, uv).xy;
    if (isnan(l)) {
        o_color = vec4(prev, 0.0, 0.0);
        return;
    }
    vec2 d = vec2(l) - prev;
    vec2 r = d * a.yx; // x takes the slower rate (Param.w), y Param.z
    vec2 step_size = sign(r) * min(abs(d), max(abs(r), vec2(1.0 / 256.0)));
    o_color = vec4(prev + step_size, l, 0.0);
}
