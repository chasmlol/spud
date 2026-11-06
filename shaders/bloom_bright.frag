#version 460
// ISBrightPassBlur (ImageSpace ps 83..95): the vertical half of the bloom, thresholded. pc.mode n: 2n + 1 taps.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec2 uv = target_uv();
    float texel = 1.0 / slot_size(SRC0).y;
    int n = int(pc.mode);
    vec4 b = pc.post.bloom;
    vec3 sum = vec3(0.0);
    for (int i = -n; i <= n; ++i) {
        vec3 c = sample_linear(SRC0, uv + vec2(0.0, float(i) * texel)).rgb * b.w;
        sum += max(c - b.x, vec3(0.0)) * b.y * pc.post.gauss[abs(i)].x;
    }
    o_color = vec4(sum * b.z, 1.0);
}
