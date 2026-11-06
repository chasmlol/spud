// Skyrim's shadow mask, evaluated at the shaded point instead of from a depth buffer: the
// game's deferred pass (Utility ps 0x282002) reconstructs the same camera-relative position from its depth at the
// pixel. Included after common.glsl.
//
// The tap loops stay rolled, like the game's, and every mask goes through one of two of them, at one site before the
// rest of the shading: unrolled, or with a copy per case, this code costs the lighting shaders more than the samples
// themselves do.

#extension GL_EXT_control_flow_attributes : require

// The first 16 entries of the mask shader's sample table, in its order (taps 2i and 2i + 1 per loop step).
const vec2 k_shadow_taps[16] = vec2[](
    vec2(0.493393, 0.394269), vec2(0.798547, 0.885922), vec2(0.247322, 0.926450), vec2(0.051454, 0.140782),
    vec2(0.831843, 0.009552), vec2(0.428632, 0.017151), vec2(0.015656, 0.749779), vec2(0.758385, 0.496170),
    vec2(0.223487, 0.562151), vec2(0.011628, 0.406995), vec2(0.241462, 0.304636), vec2(0.430311, 0.727226),
    vec2(0.981811, 0.278359), vec2(0.407056, 0.500534), vec2(0.123478, 0.463546), vec2(0.809534, 0.682272));

// The game's mask is an RGBA8 target.
float shadow_mask_store(float m) { return round(clamp(m, 0.0, 1.0) * 255.0) * (1.0 / 255.0); }

// 1 lit, 0 in the sun's shadow. `rel` is the camera-relative position (metres).
float sun_shadow(vec3 rel) {
    FrameRef f = pc.frame;
    uint count = f.shadow_info.x;
    if (count == 0u) {
        return 1.0;
    }
    float z = dot(rel, cross(f.camera_up.xyz, f.camera_right.xyz)); // view depth
    if (z > f.shadow_split.z) {
        return 1.0;
    }
    // The distance fade: 1 - (|P|^2 / D^2)^8.
    float q = clamp(dot(rel, rel) * f.shadow_params.w, 0.0, 1.0);
    q *= q;
    q *= q;
    float fade = 1.0 - q * q;
    uint c = count > 2u && z > f.shadow_split.y ? 2u : z > f.shadow_split.x ? 1u : 0u;
    vec4 P = vec4(rel, 1.0);
    uint slot = f.shadow_info.y;
    float radius = f.shadow_filter.x;
    // The cascade, then in the overlap cascade 1 as well, blended in by smoothstep over [start of cascade 1, end of
    // cascade 0]. The game interpolates its depth-buffer values, which are linear in 1 / z: the same ratio in z.
    uint last = c == 0u && z > f.shadow_params.x ? 1u : c;
    float s = 0.0, first = 0.0;
    [[dont_unroll]] for (uint i = c; i <= last; ++i) {
        vec2 uv = vec2(dot(f.shadow_rows[3u * i], P), dot(f.shadow_rows[3u * i + 1u], P));
        float ref = dot(f.shadow_rows[3u * i + 2u], P) - (i == 0u ? f.shadow_params.y : f.shadow_params.z);
        first = s;
        s = 0.0;
        [[dont_unroll]] for (int t = 0; t < 16; ++t) {
            vec2 p = uv + (k_shadow_taps[t] * 2.0 - 1.0) * radius;
            s += texture(sampler2DArrayShadow(u_arrays[slot], u_samplers[SAMPLER_SHADOW]), vec4(p, float(i), ref));
        }
        s *= 0.0625;
    }
    if (last != c) {
        float t = clamp((1.0 / f.shadow_params.x - 1.0 / z) / (1.0 / f.shadow_params.x - 1.0 / f.shadow_split.x), 0.0,
                        1.0);
        t = t * t * (3.0 - 2.0 * t);
        s = t * (s - first) + first;
    }
    return shadow_mask_store(fade * (s - 1.0) + 1.0);
}

// The shadow light n's mask at `rel` (Utility ps 0x482102 spot, 0x882002 hemisphere, 0x1082002 omni): 1 lit. The
// game draws each into a channel of the mask; here it is evaluated where the light is shaded.
float light_shadow(uint n, vec3 rel) {
    FrameRef f = pc.frame;
    vec4 P = vec4(rel, 1.0);
    vec4 q = vec4(dot(f.shadow_light_rows[4u * n], P), dot(f.shadow_light_rows[4u * n + 1u], P),
                  dot(f.shadow_light_rows[4u * n + 2u], P), dot(f.shadow_light_rows[4u * n + 3u], P));
    vec4 p = f.shadow_light_params[n]; // type, layer, bias, radius or falloff
    uint slot = f.shadow_info.z;
    float radius = f.shadow_filter.x;
    float d = clamp(dot(rel, rel) * f.shadow_light_fade[n], 0.0, 1.0);
    d *= d;
    d *= d;
    float fade = 1.0 - d * d;
    vec3 v = q.xyz / q.w;
    vec2 uv;
    float ref, side = 0.0; // side: an omni light's halves share a layer, the front's above
    if (p.x == 0.0) {
        uv = v.xy * 0.5 + 0.5;
        ref = v.z - p.z;
    } else {
        bool back = 0.5 * q.z + 0.5 < 0.0;
        if (p.x == 1.0 && back) {
            return shadow_mask_store(fade); // a hemisphere's map stops at its plane
        }
        float len = length(v);
        vec3 dir = normalize(v / len + vec3(0.0, 0.0, back ? -1.0 : 1.0));
        uv = dir.xy / dir.z * 0.5 + 0.5;
        ref = clamp(len / p.w, 0.0, 1.0) - p.z;
        if (p.x == 2.0) {
            uv.y = back ? 1.0 - uv.y * 0.5 : uv.y * 0.5;
            side = uv.y >= 0.5 ? 1.0 : -1.0;
        }
    }
    float s = 0.0;
    [[dont_unroll]] for (int i = 0; i < 8; ++i) {
        vec2 a = k_shadow_taps[2 * i] * 2.0 - 1.0, b = k_shadow_taps[2 * i + 1] * 2.0 - 1.0;
        // In an omni light's map a pair of taps keeps its vertical offset only when the larger of its two offsets
        // stays in the half (the test adds the unscaled offset).
        float rv = side == 0.0 || (max(abs(a.y), abs(b.y)) + uv.y) * side >= side * 0.5 ? radius : 0.0;
        vec2 scale = vec2(radius, rv);
        s += texture(sampler2DArrayShadow(u_arrays[slot], u_samplers[SAMPLER_SHADOW]), vec4(uv + a * scale, p.y, ref));
        s += texture(sampler2DArrayShadow(u_arrays[slot], u_samplers[SAMPLER_SHADOW]), vec4(uv + b * scale, p.y, ref));
    }
    s *= 0.0625;
    if (p.x == 0.0) {
        s -= pow(length(v.xy), p.w) * s; // the spot's cone: the vignette 1 - |ndc|^falloff
    }
    return shadow_mask_store(fade * s);
}

// The masks of the shadow lights on a point-light list, by mask slot; 1 for the others.
vec4 shadow_light_masks(uvec4 list, vec3 rel) {
    vec4 masks = vec4(1.0);
    if (pc.frame.shadow_info.w != 0u) {
        [[dont_unroll]] for (uint i = 0u; i < 8u; ++i) {
            uint li = light_index(list, i);
            if (li == 0xFFFFu) {
                break;
            }
            float w = pc.lights.l[li].color.w;
            if (w >= 2.0) {
                uint n = uint(w) - 2u;
                masks[n] = light_shadow(n, rel);
            }
        }
    }
    return masks;
}
