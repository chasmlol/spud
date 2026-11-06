#version 460
// The game's vertex shaders: a grid of pc.mode x pc.mode quads per surface, from the instance.
// Positions and everything derived from them are in game units, camera relative.
#include "common.glsl"
out float gl_ClipDistance[1];

layout(location = 0) out vec3 v_rel;
layout(location = 1) out vec4 v_fog;
layout(location = 2) out vec4 v_noise01;
layout(location = 3) out vec2 v_noise2;
layout(location = 4) out vec4 v_flow;
layout(location = 5) out vec2 v_cell;
layout(location = 6) flat out uint v_material;
layout(location = 7) flat out float v_flow_grid;
layout(location = 8) flat out vec4 v_plane;
layout(location = 9) flat out float v_quad;
layout(location = 10) out float v_depth;

invariant gl_Position;

void main() {
    WaterRef w = pc.water;
    WaterInstance s;
    vec3 rel;
    vec2 t;
    vec4 plane = vec4(0.0); // placed water's own
    v_depth = -1.0;
    bool vertex_uv = false;
    if (pc.mode == 0u) { // placed water: its own triangles
        WaterVertex p = w.placed.v[gl_VertexIndex];
        s.flow = vec4(0.0);
        s.flow_cell = vec4(0.0);
        s.info = uvec4(p.material, 0u, 0u, 0u);
        rel = p.position.xyz - pc.frame.camera_pos.xyz;
        t = vec2(p.uv.x, 1.0 - p.uv.y);
        plane = p.plane;
        v_depth = p.position.w;
        vertex_uv = p.vertex_uv != 0u;
    } else {
        s = w.instances.i[gl_InstanceIndex];
        const uint quad = uint(gl_VertexIndex) / 6u, corner = uint(gl_VertexIndex) % 6u;
        const vec2 corners[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
        t = (vec2(float(quad % pc.mode), float(quad / pc.mode)) + corners[corner]) / float(pc.mode);
        rel = s.origin.xyz + vec3(t * s.origin.w, 0.0) - pc.frame.camera_pos.xyz;
    }
    WaterMaterial m = w.materials.m[s.info.x];
    gl_Position = pc.frame.view_proj * vec4(rel, 1.0);
    gl_ClipDistance[0] = dot(pc.frame.clip_plane, vec4(rel, 1.0));
    vec3 units = rel / w.camera_z.y;
    v_rel = units;
    v_material = s.info.x;
    v_plane = pc.mode == 0u ? vec4(plane.xyz, (dot(plane.xyz, pc.frame.camera_pos.xyz) - plane.w) / w.camera_z.y)
                            : vec4(0.0, 0.0, 1.0, -units.z);
    v_flow_grid = s.flow.x;

    vec4 clip = pc.frame.fog_view_proj * vec4(rel, 1.0);
    vec4 fp = pc.frame.fog_param;
    float f = min(pow(clamp(length(clip.xyz) * fp.y - fp.x, 0.0, 1.0), fp.z), fp.w);
    v_fog = vec4(mix(pc.frame.fog_near_color.rgb, pc.frame.fog_far_color.rgb, f), f);

    // Texture coordinates of the game's mesh: v runs south. A cell's are its own plus its cell coordinates; the wading
    // quad's follow the player it follows: both are fixed to the world.
    vec2 uv = vec2(t.x, 1.0 - t.y);
    // The noise lies on the world, or along a stream's own coordinates, a tile being a thousandth as much there.
    vec2 base = vertex_uv ? uv : units.xy + w.eye.xy;
    vec3 tile = m.uv_scale.xyz * (vertex_uv ? 0.001 : 1.0);
    v_noise01 = vec4(base / tile.x + m.uv_offset[0].xy, base / tile.y + m.uv_offset[0].zw);
    v_noise2 = base / tile.z + m.uv_offset[1].xy;
    v_cell = s.flow.w > 0.0 ? uv * 0.5 + s.flow.yz - 0.25 : uv + s.flow.yz;
    // The wading quad's flow map window is 10 % wider than a cell, across half a cell; its centre is the player's.
    v_quad = s.flow.w;
    v_flow = vec4(uv, s.flow.w > 0.0 ? s.flow_cell.xy + (uv - 0.5) * 0.1 : s.flow_cell.xy + uv / 11.0);
}
