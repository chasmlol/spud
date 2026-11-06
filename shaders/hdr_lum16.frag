#version 460
// ISHDRDownSample16Lum (ImageSpace ps 43): 16 point taps at +-0.5, +-1.5 texels.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec2 uv = target_uv();
    vec2 t = 1.0 / slot_size(SRC0);
    float l = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            l += sample_point(SRC0, uv + (vec2(x, y) - 1.5) * t).x;
        }
    }
    o_color = vec4(vec3(l / 16.0), 1.0);
}
