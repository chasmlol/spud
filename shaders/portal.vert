#version 460
#extension GL_EXT_buffer_reference_uvec2 : require
#include "common.glsl"

// A doorway's depth (renderer_portal.cpp): clip-space positions read from the frame's arena.
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer ClipPositionRef { vec4 p[]; };

void main() {
    gl_Position = ClipPositionRef(pc.pass.xy).p[gl_VertexIndex];
}
