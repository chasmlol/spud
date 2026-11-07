#version 460
// The wading ripples: a 512 x 512 window of 4 units a texel, following the player. xy height and
// velocity around 0.5, zw the slope the water reads. pc.mode 0 clears; 1 is ISWaterDisplacementTexOffset, with
// WadingRipple's stamps along the wader's segment; 3 the same without the move or the stamps (a frame's later steps); 2 is
// ISWaterWadingHeightmap, a step of the simulation. SRC0 the state.
#include "post.glsl"
layout(location = 0) out vec4 o_color;

// Clamped: the window ends at its edges. Wrapped, what left one edge as the player moved came back in at the other,
// and the waves ran across from edge to edge, a hard line through the water round the wading quad.
vec4 state(vec2 uv) { return sample_linear(SRC0, uv); }
float height(vec2 uv) { return state(uv).x; }

void main() {
    vec2 uv = target_uv();
    if (pc.mode == 0u) {
        o_color = vec4(0.5);
    } else if (pc.mode == 1u || pc.mode == 3u) {
        // What comes in at the leading edge has no waves yet.
        vec2 from = uv + (pc.mode == 1u ? pc.water.ripple.xy : vec2(0.0));
        vec4 s = all(greaterThanEqual(from, vec2(0.0))) && all(lessThanEqual(from, vec2(1.0))) ? state(from) : vec4(0.5);
        float edge = clamp((length(uv - 0.5) - 0.4) * 10.000001, 0.0, 1.0);
        s.zw = mix(s.zw, vec2(0.5), edge);
        // A strip along each segment, on its left only, 10.24 units wide (ISWaterDisplacementWadingRipple).
        for (uint i = 0u; i < (pc.mode == 1u ? uint(pc.water.ripple.w) : 0u); ++i) {
            vec2 a = pc.water.ripple_segments[i].xy, ab = pc.water.ripple_segments[i].zw - a;
            float len = length(ab);
            vec2 along = ab / max(len, 1e-9), d = uv - a;
            float t = dot(d, along), side = dot(d, vec2(along.y, -along.x));
            if (len > 0.0 && t >= 0.0 && t <= len && side >= 0.0 && side <= pc.water.ripple.z) {
                s = vec4(1.0, 0.5, 0.5, 0.5);
            }
        }
        o_color = s;
    } else {
        const float t = 0.000977; // a half texel of the window's quarter: the game's taps
        vec2 c = state(uv).xy;
        float a = height(uv + vec2(-t, -t)), b = height(uv + vec2(-t, t)), d = height(uv + vec2(t, -t)), e = height(uv + vec2(t, t));
        float lap = a + b + d + e - 4.0 * c.x;
        vec2 h = c - 0.5;
        float velocity = 0.2 * lap + h.y;
        float position = 1.7 * velocity + h.x;
        vec2 next = max(vec2(position, velocity) * 0.99, vec2(-1.0));
        o_color = vec4(next, (a + b - d - e) * 0.5, (a - b + d - e) * 0.5) + 0.5;
    }
}
