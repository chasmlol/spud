#version 460
// The resolved UI layer over the scene, premultiplied.
layout(set = 0, binding = 0) uniform texture2D u_layer;
layout(set = 1, binding = 0) uniform sampler u_samplers[4];

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() { o_color = texelFetch(sampler2D(u_layer, u_samplers[0]), ivec2(gl_FragCoord.xy), 0); }
