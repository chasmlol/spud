#version 460
// Land beyond the full-resolution ring into the god-ray map, as shadow_land.vert.
#include "common.glsl"
#include "terrain_far_node.glsl"

void main() {
    vec4 p = pc.frame.view_proj * vec4(far_position() - pc.frame.camera_pos.xyz, 1.0);
    p.z = max(p.z, 0.0);
    gl_Position = p;
}
