#version 460
// World particle systems: one camera-facing quad per particle. The system's material is
// The game's, so the fragment shader follows effect.frag.
#include "common.glsl"
out float gl_ClipDistance[1];

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec4 v_fog;
layout(location = 3) out float v_view_depth;
layout(location = 4) flat out uint v_system;

void main() {
    Particle p = pc.particles.particles.p[gl_InstanceIndex];
    const vec2 corners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 c = corners[gl_VertexIndex];
    float s = sin(p.position.w), k = cos(p.position.w);
    vec2 r = vec2(c.x * k - c.y * s, c.x * s + c.y * k) * p.size_system.x;
    vec3 rel = p.position.xyz - pc.frame.camera_pos.xyz + pc.frame.camera_right.xyz * r.x + pc.frame.camera_up.xyz * r.y;
    v_system = floatBitsToUint(p.size_system.y);
    v_color = p.color;
    v_uv = mix(p.uv.xz, p.uv.xz + p.uv.yw, c * vec2(0.5, -0.5) + 0.5);

    vec4 clip = pc.frame.fog_view_proj * vec4(rel, 1.0);
    vec4 fp = pc.frame.fog_param;
    float f = min(pow(clamp(length(clip.xyz) * fp.y - fp.x, 0.0, 1.0), fp.z), fp.w);
    v_fog = vec4(mix(pc.frame.fog_near_color.rgb, pc.frame.fog_far_color.rgb, f), f);

    gl_Position = pc.frame.view_proj * vec4(rel, 1.0);
    gl_ClipDistance[0] = dot(pc.frame.clip_plane, vec4(rel, 1.0));
    v_view_depth = gl_Position.w;
}
