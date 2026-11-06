#version 460
// Simple decals as a projected box, in the game's decal pass: before the composite, which fogs
// them with the scene. A surface takes the decal inside the box where its face passes the angle test, as the game's triangle
// clip keeps faces within the IPCT's angle of the axis. Lit as the lighting shader lights a plain mesh (mesh.frag), or
// unlit as the effect shader when the texture set has no normal map. SRC0 the depth (multisampled when pc.mode is 1), SRC1
// the normal target.
#include "post.glsl"
#include "shadows.glsl"
#include "decal.glsl"

layout(set = 0, binding = 0) uniform texture2DMS u_ms_textures[];
layout(location = 0) flat in uint v_decal;
layout(location = 0) out vec4 o_color;

vec3 relative(vec2 frag, float depth) {
    FrameRef f = pc.frame;
    vec2 ndc = vec2(2.0, -2.0) * frag / target_size() + vec2(-1.0, 1.0);
    vec3 forward = cross(f.camera_up.xyz, f.camera_right.xyz);
    return (f.camera_right.xyz * (ndc.x * pc.post.sao_proj.z) + f.camera_up.xyz * (ndc.y * pc.post.sao_proj.w) + forward) *
           (view_units(depth) / pc.post.camera.z);
}

float depth_at(ivec2 p) { return texelFetch(sampler2D(u_textures[DEPTH_SLOT], u_samplers[0]), p, 0).r; }

// The face under a pixel, whatever a normal map does to it: from the depth, on the side that stays on the surface.
vec3 face_normal(ivec2 px) {
    const ivec2 x = ivec2(1, 0), y = ivec2(0, 1);
    float c = depth_at(px);
    float l = depth_at(px - x), r = depth_at(px + x), u = depth_at(px - y), d = depth_at(px + y);
    vec2 at = vec2(px) + 0.5;
    vec3 p = relative(at, c);
    vec3 dx = abs(l - c) < abs(r - c) ? p - relative(at - vec2(x), l) : relative(at + vec2(x), r) - p;
    vec3 dy = abs(u - c) < abs(d - c) ? p - relative(at - vec2(y), u) : relative(at + vec2(y), d) - p;
    vec3 n = normalize(cross(dx, dy));
    return dot(n, p) > 0.0 ? -n : n;
}

// The target's spheremap back to a world normal (encode_normal).
vec3 scene_normal(ivec2 px) {
    FrameRef f = pc.frame;
    vec2 e = texelFetch(sampler2D(u_textures[SRC1], u_samplers[0]), px, 0).xy * 4.0 - 2.0;
    float q = dot(e, e);
    vec2 xy = e * sqrt(max(1.0 - q * 0.25, 0.0));
    return normalize(f.camera_right.xyz * xy.x + f.camera_up.xyz * xy.y +
                     cross(f.camera_up.xyz, f.camera_right.xyz) * (q * 0.5 - 1.0));
}

void add_light(vec3 N, vec3 V, vec3 L, vec3 C, float atten, float shadow, float shininess, inout vec3 diffuse, inout vec3 spec) {
    diffuse += clamp(dot(N, L), 0.0, 1.0) * C * shadow * atten;
    if (shininess > 0.0) {
        spec += pow(clamp(dot(normalize(V + L), N), 0.0, 1.0), shininess) * C * atten * shadow;
    }
}

void main() {
    FrameRef f = pc.frame;
    Decal d = DecalRef(pc.particles).d[v_decal];
    ivec2 px = ivec2(gl_FragCoord.xy);
    float depth = pc.mode == 1u ? texelFetch(sampler2DMS(u_ms_textures[SRC0], u_samplers[0]), px, gl_SampleID).r
                                : texelFetch(sampler2D(u_textures[SRC0], u_samplers[0]), px, 0).r;
    if (depth <= 0.0) {
        discard; // the sky
    }
    vec3 rel = relative(gl_FragCoord.xy, depth);
    vec3 to = rel - d.position.xyz;
    vec3 box = vec3(dot(to, d.axes[0].xyz) * d.axes[0].w, dot(to, d.axes[1].xyz) * d.axes[1].w, dot(to, d.axes[2].xyz) * d.axes[2].w);
    if (any(greaterThan(abs(box), vec3(1.0)))) {
        discard;
    }
    vec3 axis = normalize(d.axes[2].xyz);
    if (dot(face_normal(px), axis) <= d.position.w) {
        discard;
    }
    // One quadrant of the atlas, or the whole texture. The level from the pixel's size on the surface: the box's
    // own derivatives break at every edge of what it lands on.
    float scale = d.params.y;
    vec2 uv = box.xy * scale + scale + d.params.zw;
    uint diffuse_slot = d.textures.x;
    vec2 size = vec2(textureSize(sampler2D(u_textures[nonuniformEXT(diffuse_slot)], u_samplers[0]), 0));
    float pixel = length(rel) * 2.0 * pc.post.sao_proj.w / target_size().y;
    float lod = log2(max(pixel * scale * inversesqrt(1.0 / d.axes[0].w) * max(size.x, size.y), 1e-6));
    vec4 albedo = textureLod(sampler2D(u_textures[nonuniformEXT(diffuse_slot)], u_samplers[SAMPLER_WRAP]), uv, lod);
    uint flags = d.textures.w;
    if ((flags & DECAL_ALPHA_TEST) != 0u && albedo.a <= 128.0 / 255.0) {
        discard;
    }
    float alpha = ((flags & DECAL_ALPHA_BLEND) != 0u ? albedo.a : 1.0) * d.color.a;
    vec3 c;
    if (d.textures.y == 0u) {
        c = albedo.rgb * d.color.rgb * f.effect_light.rgb;
    } else {
        vec4 nmap = textureLod(sampler2D(u_textures[nonuniformEXT(d.textures.y)], u_samplers[SAMPLER_WRAP]), uv, lod);
        vec3 surface = scene_normal(px);
        vec3 t = normalize(d.axes[0].xyz - surface * dot(d.axes[0].xyz, surface));
        vec3 N = normalize(mat3(t, cross(surface, t), surface) * (nmap.xyz * 2.0 - 1.0));
        vec3 V = normalize(-rel);
        float shininess = (flags & DECAL_SPECULAR) != 0u ? d.params.x : 0.0;
        vec3 diffuse = vec3(0.0), spec = vec3(0.0);
        const vec4 masks = shadow_light_masks(d.lights, rel);
        add_light(N, V, f.sun_dir.xyz, f.dir_light_color.rgb, 1.0, f.dir_light_color.w * sun_shadow(rel), shininess, diffuse, spec);
        uint used = 0u;
        for (uint i = 0u; i < 8u && used < 7u; ++i) {
            uint li = light_index(d.lights, i);
            if (li == 0xFFFFu) {
                break;
            }
            PointLight pl = pc.lights.l[li];
            if (pl.color.w == 0.0) {
                continue;
            }
            ++used;
            vec3 l = pl.position.xyz - rel;
            float dist = length(l);
            float a = clamp(dist / pl.position.w, 0.0, 1.0);
            float shadow = pl.color.w >= 2.0 ? masks[uint(pl.color.w) - 2u] : 1.0;
            add_light(N, V, l / dist, pl.color.rgb, 1.0 - a * a, shadow, shininess, diffuse, spec);
        }
        vec3 ambient = vec3(dot(f.dir_ambient[0], vec4(N, 1.0)), dot(f.dir_ambient[1], vec4(N, 1.0)), dot(f.dir_ambient[2], vec4(N, 1.0)));
        vec3 emit = vec3(0.0);
        if (d.textures.z != 0u) {
            emit = textureLod(sampler2D(u_textures[nonuniformEXT(d.textures.z)], u_samplers[SAMPLER_WRAP]), uv, lod).rgb;
        }
        vec3 light = diffuse + ambient + emit + f.ibl.yzw * f.ibl.x;
        c = albedo.rgb * d.color.rgb * light + spec * nmap.a;
    }
    o_color = vec4(min(c, vec3(f.params.w)), alpha); // the composite's saturate, ahead of its 1 / 1.2
}
