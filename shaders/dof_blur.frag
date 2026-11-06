#version 460
// ISDepthOfFieldBlurHCS / VCS (ImageSpace cs 146, 147): five taps on the quarter-size image, kept back at depth edges.
// SRC0 the source, SRC1 the scene's depth; pc.mode 0 horizontal, 1 vertical.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec2 size = target_size();
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 dir = pc.mode == 0u ? ivec2(1, 0) : ivec2(0, 1);
    const float w[5] = float[5](0.06136, 0.24477, 0.38774, 0.24477, 0.06136);
    vec4 blur = vec4(0.0), c0 = vec4(0.0);
    float d0 = 0.0, others = 0.0;
    for (int i = -2; i <= 2; ++i) {
        vec2 uv = min((vec2(p + dir * i) + 0.5) / size, 1.0 - 1.0 / size);
        vec4 c = sample_linear(SRC0, uv);
        float d = game_depth(sample_point(SRC1, uv).r);
        blur += c * w[i + 2];
        if (i == 0) {
            c0 = c;
            d0 = d;
        } else {
            others += d;
        }
    }
    float e = abs(4.0 * d0 - others);
    o_color = mix(blur, c0, 0.5 * smooth01(min(25.0 * e, 1.0)));
}
