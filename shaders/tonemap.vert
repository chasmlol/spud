#version 460
// Fullscreen triangle for the tonemap pass.
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    gl_Position = vec4(p, 0.0, 1.0);
}
