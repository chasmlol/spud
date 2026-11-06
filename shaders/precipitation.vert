#version 460
// The game ENVCUBE SNOW and RAIN: 1000 particles on a jittered 10 x 10 x 10 grid,
// wrapped in a box half a box ahead of the camera. The game fills a vertex buffer from its global random stream
// this hashes the same distributions from a seed, so a run repeats. pc.mode: the
// emitter | pass << 8.
#include "common.glsl"
layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_alpha;
layout(location = 2) out vec3 v_occlusion;

uint hash(uint x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
float unit(inout uint s) {
    s = hash(s + 0x9E3779B9u);
    return float(s) * 2.3283064e-10;
}

void main() {
    Precip e = pc.post.precip[pc.mode & 0xFFu];
    uint pass_index = pc.mode >> 8;
    float box = e.box.x;
    uint particle = uint(gl_VertexIndex) / 6u;
    const uint corners[6] = uint[6](1u, 0u, 2u, 2u, 0u, 3u); // (v1, v0, v2), (v2, v0, v3)
    uint corner = corners[uint(gl_VertexIndex) % 6u];
    uint s = e.info.x ^ (particle * 0x85EBCA6Bu);
    uvec3 g = uvec3(particle % 10u, (particle / 10u) % 10u, particle / 100u);
    float c = box / 9.0;
    vec3 jitter = vec3(unit(s), unit(s), unit(s)) * 2.0 - 1.0;
    vec3 position = box + vec3(g) * c + jitter * c * 0.5;
    if (any(equal(g, uvec3(0u))) || any(equal(g, uvec3(9u)))) {
        position = mod(position, box) + box;
    }
    float theta = (unit(s) - 0.5) * e.shape.w;
    float cs = cos(theta), sn = sin(theta);
    vec2 offset = unit(s) * e.shape.z * vec2(cs, -sn);
    uint m = uint(unit(s) * float(e.info.z * e.info.w)) % (e.info.z * e.info.w);
    vec2 du = vec2(1.0 / float(e.info.z), 1.0 / float(e.info.w));
    vec2 uv0 = vec2(float(m % e.info.z), float(m / e.info.z)) * du;
    vec2 sign_xy = corner == 0u ? vec2(1, 1) : corner == 1u ? vec2(1, -1) : corner == 2u ? vec2(-1, -1) : vec2(-1, 1);
    vec2 local = sign_xy * e.shape.xy;
    vec2 rotated = vec2(local.x * cs + local.y * sn, local.y * cs - local.x * sn);
    v_uv = uv0 + (sign_xy * 0.5 + 0.5) * du;

    vec3 q = (position + e.scroll.xyz - e.forward.xyz + e.instance[pass_index].xyz) / box;
    vec3 wrapped = q - trunc(q);
    vec3 p = wrapped * box + e.forward.xyz - box * 0.5;

    const float metres = 0.01428750; // units::k_skyrim_unit_m
    FrameRef f = pc.frame;
    vec4 clip;
    if (e.box.y != 0.0) { // snow: rotates, constant world size, square on screen
        float r = e.scroll.w, cr = cos(r), sr = sin(r);
        vec2 a = vec2(rotated.x * cr - rotated.y * sr, rotated.x * sr + rotated.y * cr);
        vec2 b = vec2(offset.x * cr - offset.y * sr, offset.x * sr + offset.y * cr);
        clip = f.view_proj * vec4(p * metres, 1.0);
        clip.xy += (a * e.box.zw + b) * vec2(1.0, -1.0) * metres;
    } else { // rain: a streak along the velocity, its corners in raw clip units
        float t = (rotated.y + 1.0) * 0.5;
        vec4 tail = f.view_proj * vec4((p - e.velocity.xyz) * metres, 1.0);
        vec4 head = f.view_proj * vec4(p * metres, 1.0);
        clip = mix(tail, head, t);
        clip.xy += rotated * vec2(1.0, -1.0) * metres;
    }
    gl_Position = clip;
    vec4 o = pc.post.precip_occlusion * vec4(p * metres, 1.0);
    v_occlusion = vec3(o.xy * 0.5 + 0.5, o.z);
    v_alpha = clamp(e.forward.w - float(pass_index), 0.0, 1.0);
}
