#version 460
// The water's own target into the scene, one sample per pixel of
// the shading but the scene's samples for the edge.
#include "common.glsl"

layout(location = 0) out vec4 o_color;

void main() {
    o_color = vec4(texelFetch(sampler2D(u_textures[pc.water.slots3.y], u_samplers[0]), ivec2(gl_FragCoord.xy), 0).rgb, 1.0);
}
