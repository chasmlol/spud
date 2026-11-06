#version 460
// Grass: the game's RunGrass vertex stage, in game units. PushConstants::terrain_cells holds
// the instances and terrain_vertices the forms for this draw.
#include "common.glsl"
#include "grass.glsl"
out float gl_ClipDistance[1];

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_normal;
layout(location = 3) in vec4 in_tangent;
layout(location = 4) in vec4 in_color;

layout(location = 0) out vec3 v_rel;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec3 v_tint;
layout(location = 3) out float v_ndotl;
layout(location = 4) out float v_fade;
layout(location = 5) flat out uint v_material;

const float k_unit_m = 0.0142875;

void main() {
    FrameRef f = pc.frame;
    const GrassBlade r = GrassRef(pc.terrain_cells).b[gl_InstanceIndex];
    const vec2 h0 = unpackHalf2x16(r.w[0]), h1 = unpackHalf2x16(r.w[1]), h2 = unpackHalf2x16(r.w[2]),
               h3 = unpackHalf2x16(r.w[3]), h4 = unpackHalf2x16(r.w[4]), h5 = unpackHalf2x16(r.w[5]),
               h6 = unpackHalf2x16(r.w[6]);
    const vec3 inst = vec3(h0, h1.x);
    const float brightness = h1.y;
    const vec3 row0 = vec3(h2, h3.x), row1 = vec3(h4, h5.x);
    const vec3 row2 = vec3(h6.x, h3.y, h5.y);
    const vec3 land_normal = vec3(h3.x, h5.x, h5.y);
    const float d = h6.y;
    const uint packed = r.packed;
    const GrassForm form = GrassFormRef(pc.terrain_vertices).f[packed & 0xFFu];
    const vec2 block = vec2(float((packed >> 8) & 0xFFu) - 128.0, float((packed >> 16) & 0xFFu) - 128.0) * 49152.0;

    const vec3 p = in_pos.xyz / k_unit_m;
    const vec3 scaled = p * (1.0 + d * vec3(form.uniform_scale, form.uniform_scale, 1.0));
    const vec3 rotated = vec3(dot(row0, scaled), dot(row1, scaled), dot(row2, scaled));
    const float timer = (f.camera_pos.w * 0.0016666667 * 6.2831802) * form.wave_period;
    const float t = 0.4 * (timer - (inst.x + inst.y) * 0.0078125);
    const float st = sin(t);
    const float wave = (sin(3.1415927 * st) + sin(6.2831855 * st)) * 0.3 + cos(3.1415927 * cos(t)) * 0.2;
    const float bend = 0.5 * in_color.a * in_color.a * wave * f.far_land.grass.z;
    const vec3 world = rotated + vec3(0.0, bend, 0.0) + inst + vec3(block, 0.0);
    const vec3 rel = world * k_unit_m - f.camera_pos.xyz;
    v_rel = rel;
    v_uv = in_uv;
    v_tint = in_color.rgb * brightness;
    v_ndotl = clamp(dot(f.sun_dir.xyz, land_normal), 0.0, 1.0);
    const vec4 clip = f.fog_view_proj * vec4(rel, 1.0);
    v_fade = 1.0 - clamp((length(clip.xyz) - f.far_land.grass.x) / f.far_land.grass.y, 0.0, 1.0);
    v_fade *= GrassFormRef(pc.terrain_vertices).f[256u + (packed >> 24)].pad; // the cell's fade-in
    v_material = form.material;
    gl_Position = f.view_proj * vec4(rel, 1.0);
    gl_ClipDistance[0] = dot(f.clip_plane, vec4(rel, 1.0));
}
