#version 460
// The screen blood, for the game, which isn't specified: each splat a quadrant of the
// colour and alpha atlases, its flare at the same point added over it. pc.mode splats; SRC0 alpha, SRC1 colour, SRC2 flare.
#include "post.glsl"

struct Splat {
    vec4 rect;  // centre, half size in x, in y
    vec4 flare; // the same for its flare
    vec4 uv;    // the atlas quadrant's corner, opacity, the flare's
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer SplatRef {
    Splat s[];
};
layout(location = 0) out vec4 o_color;

void main() {
    vec2 at = target_uv();
    vec3 c = vec3(0.0);
    float a = 0.0;
    for (uint i = 0u; i < pc.mode; ++i) {
        Splat s = SplatRef(pc.particles).s[i];
        vec2 q = (at - s.rect.xy) / s.rect.zw;
        if (all(lessThan(abs(q), vec2(1.0)))) {
            vec2 uv = q * 0.25 + 0.25 + s.uv.xy;
            float alpha = sample_linear(SRC0, uv).a * s.uv.z;
            c = sample_linear(SRC1, uv).rgb * alpha + c * (1.0 - alpha);
            a = alpha + a * (1.0 - alpha);
        }
        vec2 f = (at - s.flare.xy) / s.flare.zw;
        if (all(lessThan(abs(f), vec2(1.0)))) {
            vec4 flare = sample_linear(SRC2, f * 0.5 + 0.5);
            c += flare.rgb * flare.a * s.uv.w * a;
        }
    }
    if (a <= 0.0) {
        discard;
    }
    o_color = vec4(c / a, a);
}
