#version 460
// ISVolumetricLightingBlurHCS / VCS (ImageSpace cs 140, 141). SRC0 the source (value, 1 - depth); pc.mode bit 0
// vertical. Five taps 6 pixels apart; depth edges keep the centre. Bit 2 adds the lens flare in SRC2, bit 3 only that. Bit 1: the result as ps 153 adds it to the scene.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec3 lens = (pc.mode & 4u) != 0u ? sample_linear(SRC2, target_uv()).rgb : vec3(0.0);
    if ((pc.mode & 8u) != 0u) {
        o_color = vec4(lens, 0.0); // ISCompositeLensFlare: no volumetric lighting this frame
        return;
    }
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 size = ivec2(target_size()) - 1;
    ivec2 dir = (pc.mode & 1u) == 0u ? ivec2(6, 0) : ivec2(0, 6);
    const float w[5] = float[5](0.178400, 0.210431, 0.222338, 0.210431, 0.178400);
    float sum = 0.0, c0 = 0.0, d0 = 0.0, others = 0.0;
    for (int i = -2; i <= 2; ++i) {
        vec2 t = texelFetch(sampler2D(u_textures[SRC0], u_samplers[0]), clamp(p + dir * i, ivec2(0), size), 0).xy;
        sum += t.x * w[i + 2];
        if (i == 0) {
            c0 = t.x;
            d0 = t.y;
        } else {
            others += t.y;
        }
    }
    float v = abs(4.0 * d0 - others) > 0.002 ? c0 : sum;
    o_color = (pc.mode & 2u) != 0u ? vec4(pc.post.vl2.rgb * v + lens, 0.0) : vec4(v, d0, 0.0, 0.0);
}
