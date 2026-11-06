#version 460
// Land into the god-ray map: the terrain's vertices, one instance per cell, flattened onto the
// near plane like the sun's casters (shadow.vert).
#include "common.glsl"

void main() {
    uint base = pc.terrain_cells.base[gl_InstanceIndex];
    TerrainVertex tv = pc.terrain_vertices.v[base + uint(gl_VertexIndex)];
    vec4 p = pc.frame.view_proj * vec4(tv.pos - pc.frame.camera_pos.xyz, 1.0);
    p.z = max(p.z, 0.0);
    gl_Position = p;
}
