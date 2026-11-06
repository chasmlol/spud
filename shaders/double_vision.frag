#version 460
// ISDoubleVision (ImageSpace ps 13): the image twice, offset, leaning to its blurred copy towards the edges. SRC0 the
// image, SRC1 the blurred quarter image.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec2 uv = target_uv();
    vec4 dv = pc.post.dv0;
    vec2 q = 2.0 * (uv - 0.5);
    float r = min(pc.post.dv1.z * length(q * vec2(pc.post.dv1.x, 1.0)), 1.0);
    vec4 twin = (sample_linear(SRC0, uv + dv.xy) + sample_linear(SRC0, uv - dv.xy)) * 0.5;
    o_color = (1.0 - r) * twin + r * sample_linear(SRC1, uv);
}
