#version 460
// ISBlur (ImageSpace ps 139) over the streaks, with the capture's kernels: 15 taps across, 5 down, a texel apart. pc.mode
// bit 0 down, bit 1 the wider streak's weights (sqrt(10) x the narrow one's), bit 2 adds SRC1. SRC0 the source.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    const float across[8] = float[8](0.03721868, 0.03573012, 0.03161228, 0.02577651, 0.01937046, 0.0134154, 0.008562753,
                                     0.00503699);
    const float down[3] = float[3](0.1273196, 0.07722329, 0.0172308);
    float scale = (pc.mode & 2u) != 0u ? 3.1622777 : 1.0;
    bool vertical = (pc.mode & 1u) != 0u;
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 last = ivec2(slot_size(SRC0)) - 1;
    ivec2 dir = vertical ? ivec2(0, 1) : ivec2(1, 0);
    int reach = vertical ? 2 : 7;
    vec4 sum = vec4(0.0);
    for (int i = -reach; i <= reach; ++i) {
        float w = vertical ? down[abs(i)] : across[abs(i)];
        sum += texelFetch(sampler2D(u_textures[SRC0], u_samplers[0]), clamp(p + dir * i, ivec2(0), last), 0) * w;
    }
    sum *= scale;
    if ((pc.mode & 4u) != 0u) {
        sum += sample_linear(SRC1, target_uv());
    }
    o_color = sum;
}
