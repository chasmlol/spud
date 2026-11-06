#version 460
// A tree impostor: the three frames blended, lit like the land around it.
#include "common.glsl"

layout(location = 0) in vec2 v_local[3];
layout(location = 3) flat in vec2 v_origin[3];
layout(location = 6) flat in vec3 v_weights;
layout(location = 7) flat in vec4 v_rotation;
layout(location = 8) flat in uvec3 v_maps;
layout(location = 9) in vec3 v_rel;
layout(location = 10) in vec4 v_fog;

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_normal;

vec3 rotate(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

vec3 fog_and_clamp(vec3 c, float clamp_value, out vec3 removed) {
    FrameRef f = pc.frame;
    vec3 fogged = mix(c, v_fog.rgb, v_fog.a);
    vec3 x = c - fogged * f.fog_color.w;
    removed = x * f.frame_params.y;
    return min(c, x * f.frame_params.y + clamp_value);
}

void main() {
    FrameRef f = pc.frame;
    const float inv = 1.0 / float(v_maps.z);
    vec4 albedo = vec4(0.0);
    vec2 uvs[3], dxs[3], dys[3];
    float cover[3];
    for (int k = 0; k < 3; ++k) {
        const vec2 local = clamp(v_local[k], 0.0, 1.0);
        dxs[k] = dFdx(v_local[k]) * inv;
        dys[k] = dFdy(v_local[k]) * inv;
        uvs[k] = (v_origin[k] + local) * inv;
        cover[k] = 0.0;
        if (v_weights[k] > 0.0) { // the same for the whole tree
            const vec4 c = tex_grad(v_maps.x, SAMPLER_ISOTROPIC, uvs[k], dxs[k], dys[k]);
            // Colour weighs by coverage: the atlas's empty texels hold only spread colour.
            cover[k] = (local == v_local[k] ? v_weights[k] : 0.0) * c.a;
            albedo += vec4(c.rgb, 1.0) * cover[k];
        }
    }
    // Keeps the shape's coverage as the mips average it (a foliage card's alpha test is sharp at any distance).
    const float width = max(fwidth(albedo.a), 0.0001);
    const float sharp = clamp((albedo.a - 0.35) / width + 0.5, 0.0, 1.0);
    // Where it stands beside real trees it thins as they do, its coverage as it is: the sharp edge reads dark and dense.
    const float alpha = mix(albedo.a, sharp, smoothstep(300.0, 600.0, length(v_rel)));
    if (alpha <= 0.0) {
        discard;
    }
    vec3 n = vec3(0.0);
    for (int k = 0; k < 3; ++k) {
        if (v_weights[k] > 0.0) {
            n += (tex_grad(v_maps.y, SAMPLER_ISOTROPIC, uvs[k], dxs[k], dys[k]).rgb * 2.0 - 1.0) * cover[k];
        }
    }
    const vec3 base = albedo.rgb / max(albedo.a, 0.001);
    const vec3 N = normalize(rotate(v_rotation, n));
    vec3 diffuse = clamp(dot(N, f.sun_dir.xyz), 0.0, 1.0) * f.dir_light_color.rgb * f.dir_light_color.w;
    vec3 ambient = vec3(dot(f.dir_ambient[0], vec4(N, 1.0)), dot(f.dir_ambient[1], vec4(N, 1.0)),
                        dot(f.dir_ambient[2], vec4(N, 1.0)));
    vec3 light = diffuse + ambient;
    light = f.ibl.yzw * f.ibl.x + light;
    if (f.flags.y != 0u) {
        light = vec3(1.0);
    }
    vec3 removed;
    vec3 color = fog_and_clamp(base * light, f.output_clamp.x, removed);
    color = fog_and_clamp(color, f.output_clamp.z, removed);
    color -= removed * f.frame_params.z;
    color = debug_view(color, base, N, light);
    if (f.flags.x == VIEW_FORM) {
        o_color = vec4(0.0, 0.0, 0.0, length(v_rel));
        return;
    }
    if (f.flags.x == 0u) {
        color = clamp(color, vec3(0.0), vec3(f.params.w)); // the composite's saturate, ahead of its 1 / 1.2
    }
    o_color = vec4(color, alpha);
    o_normal = encode_normal(N);
    o_normal.zw = vec2(0.0);
}
