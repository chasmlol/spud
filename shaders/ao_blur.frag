#version 460
// ISSAOBlurH / ISSAOBlurV (ImageSpace ps 180, 182): depth-aware 7 taps. SRC0 the source; pc.mode 0 horizontal, 1
// vertical (which writes the result to every channel).
#include "post.glsl"
layout(location = 0) out vec4 o_color;
float key(vec4 s) { return dot(s.yz, vec2(0.996109, 0.003891)); }
void main() {
    vec2 uv = target_uv();
    vec2 texel = pc.post.sao_screen.zw;
    vec2 dir = pc.mode == 0u ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    vec4 c = sample_linear(SRC0, uv);
    float k0 = key(c);
    if (k0 == 1.0) {
        o_color = vec4(0.0, c.yz, 0.0);
        return;
    }
    // Between two pixels, where the game's taps are on one: each tap then reads both, and an outlier doesn't print the
    // kernel's every-other-pixel lattice on the image.
    const float offsets[6] = float[6](-5.5, -3.5, -1.5, 1.5, 3.5, 5.5);
    const float weights[6] = float[6](0.392902, 0.422649, 0.444893, 0.444893, 0.422649, 0.392902);
    float sum = c.x * 0.153170, total = 0.153170;
    for (int i = 0; i < 6; ++i) {
        vec4 s = sample_linear(SRC0, uv + dir * offsets[i]);
        float w = max(1.0 - 2000.0 * abs(key(s) - k0), 0.0) * weights[i];
        sum += s.x * w;
        total += w;
    }
    float ao = sum / (total + 0.0001);
    o_color = pc.mode == 0u ? vec4(ao, c.yz, c.w) : vec4(ao);
}
