#version 460
// Alpha-tested shadow casters: discarded where the diffuse
// alpha, times the vertex alpha with vertex colours (not on leaf-animated geometry), is below the AlphaTestRef.
#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_alpha;
layout(location = 2) flat in uvec2 v_texture;
layout(location = 4) flat in float v_threshold;
layout(location = 3) in float v_side;

void main() {
    if (v_side < 0.0) {
        discard;
    }
    if (v_threshold >= 0.0 && tex(v_texture.x, v_texture.y, v_uv).a * v_alpha < v_threshold) {
        discard;
    }
}
