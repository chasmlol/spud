// The far node's vertices, for vertex stages.
#include "terrain_far.glsl"

// render::GpuFarNode
struct FarNode {
    int column;
    int row;
    uint step;
    float skirt;
    float morph_start;
    float morph_scale;
    vec2 pad;
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer FarNodeRef { FarNode n[]; };

// The far node's vertex, absolute: 17 x 17 grid vertices, then the skirts' south, north, west and east.
vec3 far_position() {
    const FarLand fl = pc.frame.far_land;
    const FarNode n = FarNodeRef(pc.terrain_cells).n[gl_InstanceIndex];
    const uint v = uint(gl_VertexIndex);
    int column, row;
    float drop = 0.0;
    if (v < 289u) {
        row = int(v / 17u);
        column = int(v % 17u);
    } else {
        const uint e = v - 289u, side = e / 17u;
        const int k = int(e % 17u);
        row = side == 0u ? 0 : side == 1u ? 16 : k;
        column = side < 2u ? k : side == 2u ? 0 : 16;
        drop = n.skirt;
    }
    const int step = int(n.step);
    const ivec2 s = ivec2(n.column + column * step, n.row + row * step);
    float h = land_height(s);
    const vec2 xy = fl.samples.xy + vec2(s) * fl.samples.z;
    const bool odd_column = (column & 1) != 0, odd_row = (row & 1) != 0;
    if (n.morph_scale > 0.0 && (odd_column || odd_row)) {
        const float m = clamp((distance(vec3(xy, h), pc.frame.camera_pos.xyz) - n.morph_start) * n.morph_scale, 0.0, 1.0);
        h = mix(h, parent_height(s, odd_column, odd_row, step), m);
    }
    return vec3(xy, h - drop);
}
