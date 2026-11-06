#version 460
// ISDownsample (ImageSpace ps 158): the brightest of a row of taps, narrowing the image. pc.mode 0: four taps, then the
// second ISIBLensFlares (ps 166, no threshold) at x 0.7; 1: two taps. SRC0 the source.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec2 uv = target_uv();
    float t = 1.0 / slot_size(SRC0).x;
    int taps = pc.mode == 0u ? 4 : 2;
    vec3 best = vec3(0.0);
    float brightest = 0.0;
    for (int i = 0; i < taps; ++i) {
        vec3 c = sample_linear(SRC0, uv + vec2((float(i) - 0.5 * float(taps - 1)) * t, 0.0)).rgb;
        float l = dot(c, vec3(0.3, 0.59, 0.11));
        if (l > brightest) {
            brightest = l;
            best = c;
        }
    }
    o_color = vec4(pc.mode == 0u ? clamp(best, 0.0, 1.0) * 0.7 : best, 0.0);
}
