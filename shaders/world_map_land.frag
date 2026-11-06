#version 460
// The world map's land: the game type 18 with the world-map bit.
// The LOD map's own normal and colour, with a triplanar relief over them and the map's colour remap.
#include "common.glsl"
#include "world_map.glsl"

layout(location = 0) in vec3 v_rel;
layout(location = 1) in vec2 v_uv;
layout(location = 6) in vec4 v_fog;
layout(location = 8) flat in uint v_material;

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
    Material m = load_material(v_material);
    const vec3 t0 = tex(m.tex[TEX_DIFFUSE], m.smp, v_uv).rgb;
    const vec3 t1 = tex(m.tex[TEX_NORMAL], m.smp, v_uv).rgb;
    const vec3 N = normalize(2.0 * (t1.rbg - 0.5));
    const float s = smoothstep(0.4, 1.0, 0.55 * (t0.r + t0.g + t0.b));
    const float noise = tex(m.tex[TEX_BACK], SAMPLER_WRAP, v_uv * 3.0).r;
    const vec3 c = t0 * (mix(noise, 0.37, s) * (5.0 / 6.0) + 0.37);

    const vec3 P = world_map_position(v_rel);
    const vec3 w = world_map_weights(N);
    const vec3 rock = world_map_triplanar(m.tex[TEX_ENV_MASK], P * k_world_map_overlay_scale, w);
    const vec3 snow = world_map_triplanar(m.tex[TEX_INNER], P * k_world_map_snow_scale, w);
    const vec3 o = normalize(2.0 * (mix(rock, snow, s) - 0.5));
    const vec3 Tn = normalize(vec3(N.z, 0.0, -N.x));
    const vec3 B = normalize(cross(Tn, N));
    const vec3 W = normalize(o.x * Tn + o.y * B + o.z * N);
    const float u = clamp((2.0 * (t1.g - 0.5) - 0.95) * 20.0, 0.0, 1.0);
    const float k = clamp(1.5 - u * u * (3.0 - 2.0 * u), 0.0, 1.0);
    const vec3 Nl = mix(N, W, k);

    const vec3 albedo = world_map_remap(c, s, 0.3 + 0.2 * s);
    vec3 diffuse = clamp(dot(Nl, f.sun_dir.xyz), 0.0, 1.0) * f.dir_light_color.rgb;
    vec3 ambient = vec3(dot(f.dir_ambient[0], vec4(Nl, 1.0)), dot(f.dir_ambient[1], vec4(Nl, 1.0)),
                        dot(f.dir_ambient[2], vec4(Nl, 1.0)));
    vec3 light = f.ibl.yzw * f.ibl.x + diffuse + ambient;

    vec3 removed;
    vec3 color = fog_and_clamp(albedo * light, f.output_clamp.x, removed);
    color -= removed * f.frame_params.z;
    color = debug_view(color, albedo, Nl, light);
    if (f.flags.x == 0u) {
        color = min(color, vec3(f.params.w)); // the composite's saturate, ahead of its 1 / 1.2
    }
    o_color = vec4(color, 0.0);
    o_normal = encode_normal(Nl);
    o_normal.zw = vec2(0.0);
}
