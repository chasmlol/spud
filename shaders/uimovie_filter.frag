#version 460
#include "uimovie_filter.glsl"

layout(set = 0, binding = 0) uniform texture2D u_a; // the input (or the object, for the shadow combine)
layout(set = 1, binding = 0) uniform texture2D u_b; // the blurred alpha (shadow combine)
layout(set = 2, binding = 0) uniform sampler u_samplers[4];

layout(location = 0) out vec4 o_color;

vec4 fetch(texture2D t, ivec2 p) {
    // Outside the layer's own region is transparent: the atlas packs other layers next to it, and a blur reaching
    // past the region's edge would pick up their pixels (the Stats Menu's 64 stacked PerkName fields drew a line).
    const ivec2 lo = ivec2(fp.rect.xy + fp.offset), hi = ivec2(fp.rect.zw + fp.offset);
    if (any(lessThan(p, lo)) || any(greaterThanEqual(p, hi))) {
        return vec4(0.0);
    }
    return texelFetch(sampler2D(t, u_samplers[0]), p, 0);
}

// One pass of Flash's box blur: the centre and m taps each side at full weight, then one tap each side at the
// fractional weight; normalised by the box width.
vec4 blur(ivec2 p, ivec2 dir) {
    vec4 sum = fetch(u_a, p);
    for (int i = 1; i <= fp.m; ++i) {
        sum += fetch(u_a, p + dir * i) + fetch(u_a, p - dir * i);
    }
    sum += (fetch(u_a, p + dir * (fp.m + 1)) + fetch(u_a, p - dir * (fp.m + 1))) * fp.alpha;
    return sum / (1.0 + 2.0 * float(fp.m) + 2.0 * fp.alpha);
}

void main() {
    const ivec2 p = ivec2(gl_FragCoord.xy + fp.offset);
    if (fp.mode == 0) {
        o_color = blur(p, ivec2(1, 0));
    } else if (fp.mode == 1) {
        o_color = blur(p, ivec2(0, 1));
    } else if (fp.mode == 2 || fp.mode == 3) {
        o_color = vec4(blur(p, fp.mode == 2 ? ivec2(1, 0) : ivec2(0, 1)).a);
    } else if (fp.mode == 4) {
        // Drop shadow / glow: the object's blurred alpha, moved by the shadow offset, scaled by the strength.
        const vec4 object = fetch(u_a, p);
        const float blurred = fetch(u_b, p - ivec2(round(fp.shadow))).a;
        const bool inner = (fp.flags & 1) != 0, knockout = (fp.flags & 2) != 0, composite = (fp.flags & 4) != 0;
        float s = inner ? clamp((1.0 - blurred) * fp.strength, 0.0, 1.0) * object.a : clamp(blurred * fp.strength, 0.0, 1.0);
        s *= fp.color.a;
        const vec4 shadow = vec4(fp.color.rgb * s, s);
        if (inner) {
            o_color = knockout ? shadow : (composite ? object * (1.0 - s) + shadow : shadow);
        } else {
            o_color = knockout ? shadow * (1.0 - object.a) : (composite ? object + shadow * (1.0 - object.a) : shadow);
        }
    } else if (fp.mode == 5) {
        vec4 c = fetch(u_a, p);
        c = c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
        vec4 r = vec4(dot(fp.matrix[0], c), dot(fp.matrix[1], c), dot(fp.matrix[2], c), dot(fp.matrix[3], c)) + fp.matrix[4];
        r = clamp(r, 0.0, 1.0);
        o_color = vec4(r.rgb * r.a, r.a);
    } else {
        // A finished layer over its parent, with the layer's colour transform.
        vec4 c = fetch(u_a, p);
        c = c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
        c = clamp(c * fp.mul + fp.add, 0.0, 1.0);
        o_color = vec4(c.rgb * c.a, c.a);
    }
}
