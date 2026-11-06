#version 460
// ISBlur (ImageSpace ps 55..67): the horizontal half. pc.mode n: 2n + 1 taps.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec2 uv = target_uv();
    int n = int(pc.mode & 0xFFu);
    // Bit 8: the vertical half of the full-screen blur.
    vec2 texel = (pc.mode & 0x100u) != 0u ? vec2(0.0, 1.0 / slot_size(SRC0).y) : vec2(1.0 / slot_size(SRC0).x, 0.0);
    vec4 b = pc.post.bloom;
    vec3 sum = vec3(0.0);
    for (int i = -n; i <= n; ++i) {
        sum += sample_linear(SRC0, uv + float(i) * texel).rgb * b.w * pc.post.gauss[abs(i)].x;
    }
    o_color = vec4(sum * b.z, 1.0);
}
