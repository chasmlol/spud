#version 460
#include "common.glsl"

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 w = pc.frame.inv_view_proj * vec4(v_ndc, 1.0, 1.0);
    vec3 dir = normalize(w.xyz / w.w);
    o_color = vec4(sky_color(dir, true), 1.0);
}
