#version 460
// every sky object is camera-centred, in game units, at the far plane or just
// in front of it (the depth the game's bias gives it).
#include "common.glsl"
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out float v_horizon;
void main() {
    SkyDraw d = pc.post.sky_draws.d[pc.mode];
    vec4 world = d.world * vec4(a_position, 1.0);
    vec4 clip = pc.frame.view_proj * vec4(world.xyz, 1.0);
    clip.z = d.params.y * clip.w;
    gl_Position = clip;
    float k = pc.post.misc.z; // VParams.w: g_FramebufferRangeRcp
    uint tech = d.info.x;
    v_uv = a_uv + d.uv_offset.xy;
    if (tech == 2u) {
        v_color = vec4(vec3(k), 1.0);
    } else if (tech == 3u) {
        v_color = vec4(d.color[0].rgb * k, d.color[0].a);
    } else {
        v_color = vec4((d.color[0].rgb * a_color.r + d.color[1].rgb * a_color.g + d.color[2].rgb * a_color.b) * k,
                       a_color.a * d.color[0].a);
    }
    v_horizon = clamp(world.z * (1.0 / 17.0), 0.0, 1.0);
}
