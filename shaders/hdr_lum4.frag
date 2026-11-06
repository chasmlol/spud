#version 460
// ISHDRDownSample4RGB2Lum (ImageSpace ps 45): the same taps, reduced to luminance.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec2 uv = target_uv();
    vec2 t = 1.0 / slot_size(SRC0);
    float l = luminance(sample_linear(SRC0, uv + vec2(-t.x, -t.y)).rgb) + luminance(sample_linear(SRC0, uv + vec2(t.x, -t.y)).rgb) +
              luminance(sample_linear(SRC0, uv + vec2(-t.x, t.y)).rgb) + luminance(sample_linear(SRC0, uv + vec2(t.x, t.y)).rgb);
    o_color = vec4(vec3(l * 0.25), 1.0);
}
