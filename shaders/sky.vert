#version 460
#include "common.glsl"

layout(location = 0) out vec2 v_ndc;

void main() {
    // Fullscreen triangle on the far plane (depth 0 with reverse-Z).
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    v_ndc = p;
    gl_Position = vec4(p, 0.0, 1.0);
}
