#version 460
// The refraction surfaces' only pass: Utility ps 0x202 with vs 0x61B / 0x21B.
// Writes the encoded screen offset (rg), the strength (b) and 1 (a) into the refraction target.
#include "common.glsl"

layout(location = 0) in vec3 v_rel;
layout(location = 1) in vec2 v_uv;
layout(location = 4) in vec3 v_n;
layout(location = 5) in vec4 v_color;
layout(location = 8) flat in uint v_material;
layout(location = 14) flat in float v_fade;
layout(location = 15) in vec2 v_refraction;

layout(location = 0) out vec4 o_refraction;

void main() {
    FrameRef f = pc.frame;
    Material m = load_material(v_material);
    float fade = 1.0;
    if ((m.flags & MAT_NO_FADE) == 0u) {
        // Beyond the refraction fade's end the object gets no passes at all (13.2.2).
        if (v_fade > f.fade_decal_refr.w) {
            discard;
        }
        fade = distance_fade(v_fade, f.fade_decal_refr.z, f.fade_decal_refr.w);
    }
    vec2 t = tex(m.tex[TEX_DIFFUSE], m.smp, v_uv).rg;
    // The world-space normal (with the object's scale) in the camera's right/up axes.
    vec2 view_n = vec2(dot(v_n, f.camera_right.xyz), dot(v_n, f.camera_up.xyz));
    vec2 o;
    if ((m.flags & MAT_REFRACTION_CLAMP) != 0u) {
        o = (clamp(2.0 * (t - 0.5), -0.1, 0.1) * 0.9 + clamp(view_n, -0.1, 0.1)) / v_refraction.x;
    } else {
        o = ((t - 0.5) * 1.8 + view_n) / v_refraction.x;
    }
    float factor = (m.flags & MAT_FIRE_REFRACTION) != 0u ? v_refraction.y * fade : fade;
    o_refraction = vec4(0.5 + 0.5 * o, factor * m.extra.z * v_color.a, 1.0);
}
