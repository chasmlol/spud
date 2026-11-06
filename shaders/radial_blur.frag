#version 460
// ISRadialBlurHigh (ImageSpace ps 31; iRadialBlurLevel 2): 21 taps along the line to the centre. SRC0 the scene.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec2 uv = target_uv();
    vec4 p = pc.post.radial0, c = pc.post.radial1;
    vec2 v = c.xy - uv;
    float r = length(v);
    float up = r > p.z ? p.x * (1.0 - 1.0 / (1.0 + p.y * (r - p.z))) : 0.0;
    float down = r > c.z ? p.x * (1.0 - 1.0 / (1.0 + p.w * (r - c.z))) : 0.0;
    float a = max(up - down, 0.0);
    vec2 dir = r > 0.0 ? v / r : vec2(0.0);
    vec4 sum = vec4(0.0);
    for (int k = -10; k <= 10; ++k) {
        sum += sample_linear(SRC0, uv + float(k) / 10.0 * a * dir);
    }
    o_color = sum / 21.0;
}
