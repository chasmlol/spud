#version 460
// The full-screen blur's last pass (IMAD blur radius): the blurred quarter image over the scene, up to fully at radius 1.
// SRC0 the scene, SRC1 the blurred image; radial1.w the radius.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec4 c = texelFetch(sampler2D(u_textures[SRC0], u_samplers[0]), ivec2(gl_FragCoord.xy), 0);
    o_color = mix(c, sample_linear(SRC1, target_uv()), min(pc.post.radial1.w, 1.0));
}
