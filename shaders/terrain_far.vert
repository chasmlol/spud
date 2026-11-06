#version 460
// Far land: the game's LOD landscape vertex stage on the shared node grid.
#include "common.glsl"
#include "terrain_far_node.glsl"
out float gl_ClipDistance[1];

layout(location = 0) out vec3 v_rel;
layout(location = 1) out vec4 v_fog;

void main() {
    vec3 rel = far_position() - pc.frame.camera_pos.xyz;
    v_rel = rel;
    vec4 clip = pc.frame.fog_view_proj * vec4(rel, 1.0);
    vec4 fp = pc.frame.fog_param;
    float f = min(pow(clamp(length(clip.xyz) * fp.y - fp.x, 0.0, 1.0), fp.z), fp.w);
    v_fog = vec4(mix(pc.frame.fog_near_color.rgb, pc.frame.fog_far_color.rgb, f), f);
    gl_Position = pc.frame.view_proj * vec4(rel, 1.0);
    gl_ClipDistance[0] = dot(pc.frame.clip_plane, vec4(rel, 1.0));
}
