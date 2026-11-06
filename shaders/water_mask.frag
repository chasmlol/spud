#version 460
// Water's stencil technique: before the reflections trace, the surface's
// depth, and the cosine of the view against it in the mask the blend and the traced reflections read.
#include "common.glsl"

layout(location = 0) in vec3 v_rel;
layout(location = 9) flat in float v_quad;
layout(location = 0) out float o_mask;

void main() {
    // The wading quad's depth stands alone: the cells under it are drawn on another grid.
    vec2 place = v_rel.xy + pc.water.eye.xy;
    if (v_quad == 0.0 && all(greaterThan(place, pc.water.quad_rect.xy)) && all(lessThan(place, pc.water.quad_rect.zw))) {
        discard;
    }
    vec3 n = normalize(cross(dFdxCoarse(v_rel), dFdyCoarse(v_rel)));
    o_mask = abs(dot(normalize(v_rel), n));
}
