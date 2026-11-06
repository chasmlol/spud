#version 460
// A door's far scene: kept at the end of its frame, then put into the main frame's scene (renderer_portal.cpp), where
// pc.mode 1 scales it for the main composite's 1 / 1.2. SRC0 the source.
#include "post.glsl"
layout(location = 0) out vec4 o_color;

void main() {
    vec4 c = texelFetch(sampler2D(u_textures[SRC0], u_samplers[0]), ivec2(gl_FragCoord.xy), 0);
    o_color = vec4(c.rgb * (pc.mode == 1u ? 1.2 : 1.0), c.a);
}
