#version 460
#include "uimovie_common.glsl"

layout(location = 0) out vec2 v_fill;
layout(location = 1) flat out uint v_draw;

// Scale-9: corners keep their on-screen size, the middle takes up the rest (Flash 8 DefineScalingGrid).
float scale9(float x, float b0, float b1, float g0, float g1, float s) {
    s = max(s, 1e-4);
    const float left = (g0 - b0) / s, right = (b1 - g1) / s;
    if (x < g0) {
        return b0 + (x - b0) / s;
    }
    if (x > g1) {
        return b1 - (b1 - x) / s;
    }
    const float t = g1 > g0 ? (x - g0) / (g1 - g0) : 0.0;
    return b0 + left + t * ((b1 - b0) - left - right);
}

void main() {
    const MovieDraw d = pc.draws.d[pc.draw];
    vec2 p;
    if (uint(d.xform_t.z) == 1u) {
        const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
        p = mix(d.rect.xy, d.rect.zw, corners[gl_VertexIndex % 6]);
    } else {
        p = MovieVertices(d.vertices).v[gl_VertexIndex];
    }
    v_fill = vec2(d.fill.x * p.x + d.fill.z * p.y + d.fill_t.x, d.fill.y * p.x + d.fill.w * p.y + d.fill_t.y);
    if ((d.flags & 1u) != 0u) {
        const vec2 q = vec2(d.inner.x * p.x + d.inner.z * p.y + d.inner_t.x,
                            d.inner.y * p.x + d.inner.w * p.y + d.inner_t.y);
        p = vec2(scale9(q.x, d.s9_bounds.x, d.s9_bounds.z, d.s9_grid.x, d.s9_grid.z, d.inner_t.z),
                 scale9(q.y, d.s9_bounds.y, d.s9_bounds.w, d.s9_grid.y, d.s9_grid.w, d.inner_t.w));
    }
    const vec2 px = vec2(d.xform.x * p.x + d.xform.z * p.y + d.xform_t.x, d.xform.y * p.x + d.xform.w * p.y + d.xform_t.y);
    gl_Position = vec4(px * pc.to_clip - 1.0, 0.0, 1.0);
    v_draw = pc.draw;
}
