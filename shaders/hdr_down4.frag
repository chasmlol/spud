#version 460
// ISHDRDownSample4 (ImageSpace ps 41): four bilinear taps one texel from the centre, a 4x4 box at a 4:1 ratio.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec2 uv = target_uv();
    vec2 t = 1.0 / slot_size(SRC0);
    o_color = (sample_linear(SRC0, uv + vec2(-t.x, -t.y)) + sample_linear(SRC0, uv + vec2(t.x, -t.y)) +
               sample_linear(SRC0, uv + vec2(-t.x, t.y)) + sample_linear(SRC0, uv + vec2(t.x, t.y))) * 0.25;
}
