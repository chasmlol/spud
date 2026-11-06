#version 460
// The game's landscape vertex shader (type 8).
#include "common.glsl"
#include "terrain_far.glsl"
out float gl_ClipDistance[1];

layout(location = 0) out vec3 v_rel;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec3 v_t; // the game's frame rows: normal-map x along T, y along B, z along N
layout(location = 3) out vec3 v_b;
layout(location = 4) out vec3 v_n;
layout(location = 5) out vec3 v_color;
layout(location = 6) out vec4 v_w0; // weights of texture slots 0-3
layout(location = 7) out vec2 v_w1; // slots 4-5
layout(location = 8) out vec4 v_fog;
layout(location = 9) flat out uint v_quad;

void main() {
    // Vertex pulling: one instance per visible cell, the shared index template per LOD.
    uint base = pc.terrain_cells.base[gl_InstanceIndex];
    TerrainVertex tv = pc.terrain_vertices.v[base + uint(gl_VertexIndex)];
    vec3 rel = tv.pos - pc.frame.camera_pos.xyz;
    // The ring's outer vertices settle onto the far land's first level.
    const FarLand fl = pc.frame.far_land;
    const float m = fl.ring.w > 0.0 ? clamp((length(rel.xy) - fl.ring.z) * fl.ring.w, 0.0, 1.0) : 0.0;
    if (m > 0.0) {
        const ivec2 s = ivec2(round((tv.pos.xy - fl.samples.xy) / fl.samples.z));
        const bool odd_column = (s.x & 1) != 0, odd_row = (s.y & 1) != 0;
        if (odd_column || odd_row) {
            rel.z += (parent_height(s, odd_column, odd_row, 1) - land_height(s)) * m;
        }
    }
    v_rel = rel;
    vec4 nrm = unpackUnorm4x8(tv.normal);
    vec4 bin = unpackUnorm4x8(tv.binormal);
    v_n = nrm.xyz * 2.0 - 1.0;
    v_t = vec3(tv.tangent_x, nrm.w * 2.0 - 1.0, bin.w * 2.0 - 1.0);
    v_b = bin.xyz * 2.0 - 1.0;
    v_color = unpackUnorm4x8(tv.color).rgb;
    v_w0 = unpackUnorm4x8(tv.weight0);
    vec4 w1 = unpackUnorm4x8(tv.weight1);
    v_w1 = w1.xy;
    v_uv = vec2(float((tv.weight1 >> 16) & 0xFFu), float(tv.weight1 >> 24)) * 0.75;
    v_quad = tv.quad;

    vec4 clip = pc.frame.fog_view_proj * vec4(rel, 1.0);
    vec4 fp = pc.frame.fog_param;
    float f = min(pow(clamp(length(clip.xyz) * fp.y - fp.x, 0.0, 1.0), fp.z), fp.w);
    v_fog = vec4(mix(pc.frame.fog_near_color.rgb, pc.frame.fog_far_color.rgb, f), f);

    gl_Position = pc.frame.view_proj * vec4(rel, 1.0);
    gl_ClipDistance[0] = dot(pc.frame.clip_plane, vec4(rel, 1.0));
}
