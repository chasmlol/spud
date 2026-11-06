#version 460
// Far land: the game's LOD landscape pixel shader, type 18, on the macro maps: diffuse
// lighting from the map's normal, no specular, no shadow mask.
#include "common.glsl"
#include "terrain_far.glsl"

layout(location = 0) in vec3 v_rel;
layout(location = 1) in vec4 v_fog;

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_normal;

vec3 fog_and_clamp(vec3 c, float clamp_value, out vec3 removed) {
    FrameRef f = pc.frame;
    vec3 fogged = mix(c, v_fog.rgb, v_fog.a);
    vec3 x = c - fogged * f.fog_color.w;
    removed = x * f.frame_params.y;
    return min(c, x * f.frame_params.y + clamp_value);
}

void main() {
    FrameRef f = pc.frame;
    const vec2 world = v_rel.xy + f.camera_pos.xy;
    const vec3 albedo = far_albedo(world);
    const vec3 N = far_normal(world);
    vec3 diffuse = clamp(dot(N, f.sun_dir.xyz), 0.0, 1.0) * f.dir_light_color.rgb * f.dir_light_color.w;
    vec3 ambient = vec3(dot(f.dir_ambient[0], vec4(N, 1.0)), dot(f.dir_ambient[1], vec4(N, 1.0)),
                        dot(f.dir_ambient[2], vec4(N, 1.0)));
    vec3 light = diffuse + ambient;
    light = f.ibl.yzw * f.ibl.x + light;
    if (f.flags.y != 0u) {
        light = vec3(1.0);
    }
    vec3 removed;
    vec3 color = fog_and_clamp(albedo * light, f.output_clamp.x, removed);
    color = fog_and_clamp(color, f.output_clamp.z, removed);
    color -= removed * f.frame_params.z;
    color = debug_view(color, albedo, N, light);
    if (f.flags.x == VIEW_MATERIAL) {
        color = vec3(200.0, 0.0, 0.0);
    } else if (f.flags.x == VIEW_FORM) {
        o_color = vec4(2047.0, 2047.0, 1023.0, length(v_rel));
        return;
    } else if (f.flags.x == VIEW_TECHNIQUE) {
        color = vec3(0.0, 0.0, float(TYPE_LANDSCAPE << 2));
    } else if (f.flags.x == VIEW_SHADOW) {
        color = vec3(1.0);
    }
    if (f.flags.x == 0u) {
        color = clamp(color, vec3(0.0), vec3(f.params.w)); // the composite's saturate, ahead of its 1 / 1.2
    }
    o_color = vec4(color, 0.0);
    o_normal = encode_normal(N);
    o_normal.zw = vec2(0.0);
}
