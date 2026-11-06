#version 460
// ReflectionBlurHCS / VCS (ImageSpace cs 142, 143): nine taps two texels apart. SRC0 the source; pc.mode 1 vertical.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    const float weights[5] = float[5](0.202360, 0.179044, 0.124009, 0.067234, 0.028532);
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 last = ivec2(slot_size(SRC0)) - 1;
    ivec2 dir = pc.mode == 0u ? ivec2(2, 0) : ivec2(0, 2);
    vec4 sum = texelFetch(sampler2D(u_textures[SRC0], u_samplers[0]), p, 0) * weights[0];
    for (int i = 1; i < 5; ++i) {
        sum += (texelFetch(sampler2D(u_textures[SRC0], u_samplers[0]), clamp(p - dir * i, ivec2(0), last), 0) +
                texelFetch(sampler2D(u_textures[SRC0], u_samplers[0]), clamp(p + dir * i, ivec2(0), last), 0)) * weights[i];
    }
    o_color = sum;
}
