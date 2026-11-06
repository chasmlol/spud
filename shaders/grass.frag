#version 460
// Grass: the game's RunGrass pixel stage, the alpha test and colour in one pass. No fog,
// no directional ambient: the sun and ambient colours as the weather gives them.
#include "common.glsl"
#include "shadows.glsl"

layout(location = 0) in vec3 v_rel;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec3 v_tint;
layout(location = 3) in float v_ndotl;
layout(location = 4) in float v_fade;
layout(location = 5) flat in uint v_material;

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_normal;

void main() {
    FrameRef f = pc.frame;
    Material m = pc.materials.m[v_material];
    const vec4 albedo = tex(m.tex[TEX_DIFFUSE], m.smp, v_uv);
    const float alpha = v_fade * albedo.a;
    if (alpha - m.alpha_threshold < 0.0) {
        discard;
    }
    const float s = sun_shadow(v_rel);
    const float shadow = s + (1.0 - s) * 0.3;
    vec3 color = (v_ndotl * v_tint * f.sun_color.rgb * albedo.rgb * shadow + v_tint * f.far_land.ambient.rgb * albedo.rgb) *
                 0.8333333;
    color = debug_view(color, albedo.rgb, vec3(0.0, 0.0, 1.0), vec3(1.0));
    o_color = vec4(color, 1.0);
    // Flat view-facing normal: the game derives one from the position's derivatives, for the ambient occlusion.
    const vec3 n = normalize(cross(dFdx(v_rel), dFdy(v_rel)));
    o_normal = encode_normal(n);
    o_normal.zw = vec2(0.0);
}
