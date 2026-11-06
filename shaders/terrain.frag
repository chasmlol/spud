#version 460
// The game's landscape pixel shader, type 8 with specular: the quadrant's six
// texture slots blended by their vertex weights with no normalisation, a per-pixel specular power from the slots'
// LTEX values, then the generic lighting and output stages.
#include "common.glsl"
#include "shadows.glsl"
#include "terrain_far.glsl"
#extension GL_EXT_control_flow_attributes : require

layout(location = 0) centroid in vec3 v_rel;
layout(location = 1) in vec2 v_uv; // not centroid: its derivatives pick the mip
layout(location = 2) centroid in vec3 v_t;
layout(location = 3) centroid in vec3 v_b;
layout(location = 4) centroid in vec3 v_n;
layout(location = 5) centroid in vec3 v_color;
layout(location = 6) centroid in vec4 v_w0;
layout(location = 7) centroid in vec2 v_w1;
layout(location = 8) centroid in vec4 v_fog;
layout(location = 9) flat in uint v_quad;

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_normal;

vec3 fog_and_clamp(vec3 c, float clamp_value, out vec3 removed) {
    FrameRef f = pc.frame;
    vec3 fogged = mix(c, v_fog.rgb, v_fog.a);
    vec3 x = c - fogged * f.fog_color.w;
    removed = x * f.frame_params.y;
    return min(c, x * f.frame_params.y + clamp_value);
}

// centroid: with MSAA a triangle that covers a sample but not the pixel's centre is otherwise shaded at the centre, outside it, and
// on a thin sliver of land the blend weights and vertex colours overshoot by thousands: a lone pixel that bloom turns into a white ball.
void main() {
    FrameRef f = pc.frame;
    // The shadow masks first, with nothing else live through their loops (mesh.frag): land always takes them
    const float sun_mask = sun_shadow(v_rel);
    const vec4 light_masks = shadow_light_masks(pc.quads.q[v_quad].lights, v_rel);
    const uint after_masks = floatBitsToUint(sun_mask + light_masks.x + light_masks.y + light_masks.z + light_masks.w) >> 31;
    TerrainQuad q = pc.quads.q[v_quad + after_masks];
    // Slots are sampled under divergent branches: explicit gradients.
    vec2 dx = dFdx(v_uv);
    vec2 dy = dFdy(v_uv);
    float w[6] = float[6](v_w0.x, v_w0.y, v_w0.z, v_w0.w, v_w1.x, v_w1.y);
    // The snow technique (7.5.5) is chosen per quadrant: any slot with a snow land texture.
    bool snow_quad = false;
    [[unroll]] for (uint i = 0u; i < 6u; ++i) {
        if (i == 0u || i <= q.count) {
            snow_quad = snow_quad || pc.land_textures.t[i == 0u ? q.base : q.layers[i - 1u]].is_snow == 1.0;
        }
    }
    vec3 albedo = vec3(0.0);
    vec3 nts = vec3(0.0);
    float spec_mask = 0.0;
    float power = 0.0;
    float snow = 0.0;
    [[unroll]] for (uint i = 0u; i < 6u; ++i) {
        // A zero weight adds exactly nothing, so its textures need not be read.
        if (w[i] > 0.0 && (i == 0u || i <= q.count)) {
            LandTexture lt = pc.land_textures.t[i == 0u ? q.base : q.layers[i - 1u]];
            vec4 d = tex_grad(lt.diffuse, SAMPLER_WRAP, v_uv, dx, dy);
            vec4 m = tex_grad(lt.normal, SAMPLER_WRAP, v_uv, dx, dy);
            vec3 n = m.rgb * 2.0 - 1.0;
            if (snow_quad) {
                // Snow shader: the diffuse alpha is the snow mask; layers with one get a detail normal, the
                // reoriented blend of the normal map with itself at a quarter of the UV.
                float mask = d.a;
                if (mask > 1e-5) {
                    vec3 m2 = tex_grad(lt.normal, SAMPLER_WRAP, v_uv * 0.25, dx * 0.25, dy * 0.25).rgb;
                    vec3 t = vec3(m.xy * 2.0 - 1.0, m.z * 2.0);
                    vec3 u = (m2 * 2.0 - 1.0) * vec3(-1.0, -1.0, 1.0);
                    n = normalize(t * dot(t, u) - u * t.z);
                }
                snow += w[i] * lt.is_snow * mask;
            }
            albedo += w[i] * d.rgb;
            nts += w[i] * n;
            spec_mask += w[i] * m.a;
            power += w[i] * lt.specular_power;
        }
    }
    vec3 N = normalize(nts.x * v_t + nts.y * v_b + nts.z * v_n);
    // Into the far land's look before the ring ends, like the game's own LOD blend.
    const FarLand fl = f.far_land;
    const float far_blend = fl.ring.y > 0.0 ? clamp((length(v_rel.xy) - fl.ring.x) * fl.ring.y, 0.0, 1.0) : 0.0;
    vec3 tint = v_color;
    if (far_blend > 0.0) {
        const vec2 world = v_rel.xy + f.camera_pos.xy;
        albedo = mix(albedo * v_color, far_albedo(world), far_blend);
        tint = vec3(1.0);
        N = normalize(mix(N, far_normal(world), far_blend));
        spec_mask *= 1.0 - far_blend;
    }
    vec3 V = normalize(-v_rel);
    vec3 L = f.sun_dir.xyz;
    vec3 C = f.dir_light_color.rgb * (f.dir_light_color.w * sun_mask);
    vec3 diffuse = clamp(dot(N, L), 0.0, 1.0) * C;
    vec3 H = normalize(V + L);
    vec3 spec = pow(clamp(dot(H, N), 0.0, 1.0), power) * C;
    if (snow > 0.0) {
        spec = vec3(snow_rim_light(f.snow_rim, normalize(v_n), N, V));
    }
    // Point lights (7.5.2, 7.5.3, 9.4): the quadrant's list, the first 7 lit this frame.
    uint used = 0u;
    for (uint i = 0u; i < 8u && used < 7u; ++i) {
        uint li = light_index(q.lights, i);
        if (li == 0xFFFFu) {
            break;
        }
        PointLight pl = pc.lights.l[li];
        if (pl.color.w == 0.0) {
            continue;
        }
        ++used;
        vec3 d = pl.position.xyz - v_rel;
        float dist = length(d);
        vec3 Lp = d / dist;
        float a = clamp(dist / pl.position.w, 0.0, 1.0);
        a = 1.0 - a * a;
        a *= pl.color.w >= 2.0 ? light_masks[uint(pl.color.w) - 2u] : 1.0;
        diffuse += clamp(dot(N, Lp), 0.0, 1.0) * pl.color.rgb * a;
        spec += pow(clamp(dot(normalize(V + Lp), N), 0.0, 1.0), power) * pl.color.rgb * a;
    }
    // Doors open onto another space: the light that comes through (mesh.frag).
    for (uint i = 0u; i < f.door_info.x; ++i) {
        const DoorLight d = f.doors[i];
        const vec3 to = d.position.xyz - v_rel;
        const float dist = max(length(to), 0.01);
        const float front = dot(-to, d.normal.xyz) / dist;
        if (front > 0.0 && dist < d.position.w) {
            const float a = dist / d.position.w;
            diffuse += clamp(dot(N, to / dist), 0.0, 1.0) * d.colour.rgb * (d.normal.w * (1.0 - a * a) * front);
        }
    }

    vec3 ambient = vec3(dot(f.dir_ambient[0], vec4(N, 1.0)), dot(f.dir_ambient[1], vec4(N, 1.0)),
                        dot(f.dir_ambient[2], vec4(N, 1.0)));
    vec3 light = diffuse + ambient; // + EmitColor (0 for land)
    light = f.ibl.yzw * f.ibl.x + light;
    if (f.flags.y != 0u) {
        light = vec3(1.0);
    }
    vec3 c = albedo * light * tint;

    vec3 removed;
    vec3 color = fog_and_clamp(c, f.output_clamp.x, removed);
    // With snow present the specular goes to a separate target composited by the image-space pass (another
    // workstream), not into the colour (7.5.5).
    if (snow <= 0.0) {
        color += spec * spec_mask; // x MaterialData.y (1) x SpecularColor (1, 1, 1)
    }
    color = fog_and_clamp(color, f.output_clamp.z, removed);
    color -= removed * f.frame_params.z;
    color = debug_view(color, albedo, N, light);
    if (f.flags.x == VIEW_MATERIAL) {
        color = vec3(200.0, 0.0, 0.0);
    } else if (f.flags.x == VIEW_FORM) {
        o_color = vec4(2047.0, 2047.0, 1023.0, length(v_rel));
        return;
    } else if (f.flags.x == VIEW_TECHNIQUE) {
        color = vec3(0.0, 0.0, float(TYPE_LANDSCAPE << 2)); // the landscape's own path (type 8)
    } else if (f.flags.x == VIEW_SHADOW) {
        color = vec3(sun_mask, light_masks.xy);
    }
    if (f.flags.x == 0u) {
        color = clamp(color, vec3(0.0), vec3(f.params.w)); // the composite's saturate, ahead of its 1 / 1.2
    }
    o_color = vec4(color, 0.0);
    o_normal = encode_normal(N);
    o_normal.zw = vec2(snow > 0.0 ? dot(spec, vec3(0.3, 0.59, 0.11)) : ssr_mask(spec_mask), snow);
}
