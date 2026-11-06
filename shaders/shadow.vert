#version 460
// Shadow casters: the lit pass's vertex position (leaf animation, billboards facing the
// main camera, as the game's shadow culls leave them) into the shadow map's clip space, pc.frame.view_proj.
#include "common.glsl"

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_normal;
layout(location = 3) in vec4 in_tangent;
layout(location = 4) in vec4 in_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_alpha;
layout(location = 2) flat out uvec2 v_texture; // diffuse and sampler
layout(location = 4) flat out float v_threshold; // AlphaTestRef; below 0 no alpha test
layout(location = 3) out float v_side; // paraboloids: below 0 on the other side of the plane (discarded)

#include "mesh_transform.glsl"

void main() {
    uvec2 di = pc.draw_instances.v[gl_InstanceIndex];
    Instance inst = pc.instances.v[di.x];
    Material m = load_material(di.y);
    const MeshXform x = mesh_transform(m, inst, m.technique >> 24, in_pos, in_normal, in_color);
    vec4 p = pc.frame.view_proj * vec4(x.rel, 1.0);
    v_side = 1.0;
    if (pc.mode == SHADOW_PASS_SUN) {
        // Utility vs 0xC000: vertices sunwards of the near plane are flattened onto it (depth clamped to 0 per
        // vertex), so casters above the cascade still cast.
        p.z = max(p.z, 0.0);
    } else if (pc.mode == SHADOW_PASS_PARABOLOID) {
        // Utility vs 0x14006: the direction's paraboloid projection, depth the distance over the radius. frame
        // shadow_filter: y 1 / radius, z 1 for the front half, -1 for the back.
        vec3 v = p.xyz / p.w;
        float side = pc.frame.shadow_filter.z;
        v_side = 0.5 * p.z * side + 0.5;
        float len = length(v);
        vec3 d = normalize(v / len + vec3(0.0, 0.0, side));
        p = vec4(d.x / d.z, -d.y / d.z, len * pc.frame.shadow_filter.y, 1.0); // Vulkan's y is down
    }
    gl_Position = p;
    v_uv = in_uv * m.uv.zw + m.uv.xy;
    // Utility ps 0x4083 takes the vertex alpha, 0x4082 not: leaf-animated geometry keeps its animation
    // weight there, and its shadow technique drops it.
    v_alpha = (m.technique & TECH_VC) != 0u && (m.flags & MAT_TREE_ANIM) == 0u ? in_color.a : 1.0;
    v_texture = uvec2(m.tex[TEX_DIFFUSE], m.smp);
    v_threshold = (m.flags & MAT_ALPHA_TEST) != 0u ? m.alpha_threshold : -1.0;
}
