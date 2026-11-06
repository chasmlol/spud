#version 460
// ISMinify / ISMinifyContrast (ImageSpace ps 195, 197). SRC0 the level above. pc.mode bit 0: the sampler is linear
// (levels past the fourth: a 2x2 average at the corner of four texels), bit 1: the contrast weights, bit 2: SRC0 is the
// scene's depth, as ISSAOCameraZ (ps 186) would have turned it into level 0: the first level picks from it directly.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
const float k_weights[25] = float[25](0.3, 0.4, 0.5, 0.4, 0.3, 0.4, 2.0, 2.5, 2.0, 0.4, 0.5, 2.5, 3.5, 2.5, 0.5,
                                      0.4, 2.0, 2.5, 2.0, 0.4, 0.3, 0.4, 0.5, 0.4, 0.3);
float tap(vec2 uv) {
    if ((pc.mode & 4u) != 0u) {
        return min(view_units(sample_point(SRC0, uv).x), pc.post.sao_screen2.x);
    }
    return (pc.mode & 1u) != 0u ? sample_linear(SRC0, uv).x : sample_point(SRC0, uv).x;
}
void main() {
    vec2 uv = target_uv();
    vec2 size = slot_size(SRC0);
    float z;
    if (max(size.x, size.y) == 3.0) {
        vec2 parity = vec2(ivec2(size) & 1);
        vec2 o = (parity * 0.166667 + 0.5) / size;
        z = (tap(uv + o) + tap(uv + vec2(-o.x, o.y)) + tap(uv - o) + tap(uv + vec2(o.x, -o.y))) * 0.25;
    } else if ((pc.mode & 1u) != 0u) {
        z = tap(uv);
    } else {
        z = tap(uv - 0.5 / size); // the child (2x, 2y) at texel centres
    }
    if ((pc.mode & 2u) != 0u) {
        ivec2 c = ivec2(uv * 5.0);
        z *= pc.post.misc.x * k_weights[c.y * 5 + c.x];
    }
    o_color = vec4(z);
}
