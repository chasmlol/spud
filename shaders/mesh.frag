#version 460
// Skyrim's lighting pixel shader. One program covers the
// game's technique permutations: the material's technique bits select the features (uniform per draw).
// Variants: ALPHA_TEST (discard; alpha-to-coverage when multisampling), BLEND (outputs alpha).
#include "common.glsl"
#include "shadows.glsl"
#if defined(WORLD_MAP)
#include "world_map.glsl"
#endif

layout(location = 0) centroid in vec3 v_rel;
layout(location = 1) in vec2 v_uv;
layout(location = 2) centroid in vec3 v_b;
layout(location = 3) centroid in vec3 v_t;
layout(location = 4) centroid in vec3 v_n;
layout(location = 5) centroid in vec4 v_color;
layout(location = 6) centroid in vec4 v_fog;
layout(location = 7) in vec3 v_view_shape;
layout(location = 13) in vec2 v_uvp;
layout(location = 16) in vec3 v_eye_normal;

// The draw instance: the instance's light list, fade metric and FormID and the material are read through it here,
// which costs less than passing them on from every vertex.
layout(location = 19) flat in uint v_draw;
layout(location = 0) out vec4 o_color;
#if !defined(BLEND)
layout(location = 1) out vec4 o_normal; // absent in the passes without a normal target: the write is dropped
#endif

float smoothstep01(float x) { return x * x * (3.0 - 2.0 * x); }

// Everything one light adds (5.3, 5.4): diffuse and the rim/soft/back effects into `diffuse`, specular into `spec`.
// Type 14's sparkle (14.4.3): the view vector mirrored about N, and the summed sparkle texture value.
vec3 g_sparkle_r = vec3(0.0);
float g_sparkle = 0.0;

// `shadow` scales the direct diffuse and the specular only: the game's shaders add the rim, soft and back terms
// with the unshadowed light colour (5.3).
void add_light(Material m, uint tech, vec3 N, vec3 V, vec3 L, vec3 C, float atten, float shadow, vec3 rim_soft,
               vec3 back, inout vec3 diffuse, inout vec3 spec) {
    float ndl = dot(N, L);
    vec3 d = clamp(ndl, 0.0, 1.0) * C * shadow;
    if ((tech & TECH_RIM) != 0u) {
        float rim = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), m.effect.y) * clamp(dot(V, -L), 0.0, 1.0);
        d += rim * C * rim_soft;
    }
    if ((tech & TECH_SOFT) != 0u) {
        float w = clamp((ndl + m.effect.x) / (m.effect.x + 1.0), 0.0, 1.0);
        float soft = clamp(smoothstep01(w) - smoothstep01(clamp(ndl, 0.0, 1.0)), 0.0, 1.0);
        d += soft * C * rim_soft;
    }
    if ((tech & TECH_BACK) != 0u) {
        d += clamp(-ndl, 0.0, 1.0) * back * C;
    }
    diffuse += d * atten;
    if ((tech & TECH_SPECULAR) != 0u) {
        vec3 H = normalize(V + L);
        float h = clamp(dot(H, N), 0.0, 1.0);
        spec += pow(h, m.specular.w) * C * atten * shadow;
        if (g_sparkle > 0.0) {
            float glint = pow(clamp(dot(g_sparkle_r, L), 0.0, 1.0), m.sparkle.w) * g_sparkle >= 0.5 ? 1.0 : 0.0;
            spec += glint * h * C * atten * shadow;
        }
    }
}

// The shader's final stage (5.6): fog, then the per-frame colour clamp, in the game's order of operations.
vec3 fog_and_clamp(vec3 c, float clamp_value, out vec3 removed) {
    FrameRef f = pc.frame;
    vec3 fogged = mix(c, v_fog.rgb, v_fog.a);
    vec3 x = c - fogged * f.fog_color.w;
    removed = x * f.frame_params.y;
    return min(c, x * f.frame_params.y + clamp_value);
}

// The game's environment cube maps, sampled through D3D11 on the reference machine, lose their anisotropy in
// every 2x2 quad whose top-left, top-right and bottom-left pixels see different faces (lighting spec 4.4).
// Those are the pixels coarse derivatives difference: a non-zero coarse derivative of the face index.
vec4 tex_env_cube(uint id, uint smp, vec3 dir) {
    vec3 a = abs(dir);
    float face = a.x >= a.y && a.x >= a.z ? (dir.x > 0.0 ? 0.0 : 1.0)
               : a.y >= a.z               ? (dir.y > 0.0 ? 2.0 : 3.0)
                                          : (dir.z > 0.0 ? 4.0 : 5.0);
    uint s = dFdxCoarse(face) != 0.0 || dFdyCoarse(face) != 0.0 ? SAMPLER_ISOTROPIC : smp; // varies by quad
    return texture(samplerCube(u_cubes[nonuniformEXT(id)], u_samplers[nonuniformEXT(s)]), dir);
}

void main() {
    FrameRef f = pc.frame;
    const uvec2 draw = pc.draw_instances.v[v_draw];
    const Instance inst = pc.instances.v[draw.x];
    // The LOD fade metric of the reference's fade node, its bound centre's distance to the camera (2.2).
    const float fade =
        lod_fade_metric(inst.sphere.xyz, f.camera_pos.xyz, inst.flags, f.lod_fade, f.lod_fade_types);
    Material m = load_material(draw.y);
    const uint tech = m.technique & (k_spec_technique | 0xFF000000u);
    const uint type = tech >> 24;
    vec2 uv = v_uv;
    vec3 V = normalize(-v_rel);

    if (is_type(type, TYPE_PARALLAX)) {
        // The game offsets by the shape-space view vector's xy, not a tangent-space one (5.7).
        vec3 vs = normalize(v_view_shape);
        float h = tex(m.tex[TEX_HEIGHT], m.smp, uv).r;
        uv = vs.xy * (h * 0.08 - 0.04) + uv;
    }

    float spec_energy = 1.0; // specular anti-aliasing's dimming, below
    vec4 albedo = tex(m.tex[TEX_DIFFUSE], m.smp, uv);
    if (pc.mode == 0x4C4Du) {
        // The local map's scene: objects are Utility ps 0x2000003 and 0x2000083, the diffuse as it is, alpha tested.
        if (albedo.a - m.alpha_threshold < 0.0 && ((m.flags & MAT_ALPHA_TEST) != 0u)) {
            discard;
        }
        o_color = albedo;
        return;
    }
    if (is_type(type, TYPE_FACE)) {
        // Face (5.8): soft-light blend with the tint map (t3), then the detail map (t4) scaled by 255/64.
        vec3 detail = (tex(m.tex[TEX_ENV], m.smp, uv).rgb + vec3(1.0 / 255.0, 0.0, 1.0 / 255.0)) * 3.984375;
        vec3 tint = tex(m.tex[TEX_HEIGHT], m.smp, uv).rgb;
        vec3 at = albedo.rgb * tint;
        at = at + at;
        albedo.rgb = detail * (albedo.rgb * albedo.rgb + (-at * albedo.rgb + at));
    } else if (is_type(type, TYPE_FACE_TINT)) {
        vec3 at = albedo.rgb * m.tint.rgb;
        at = at + at;
        albedo.rgb = (albedo.rgb * albedo.rgb + (-at * albedo.rgb + at)) * vec3(1.01171875, 0.99609375, 1.01171875);
    }
    vec4 nmap = tex(m.tex[TEX_NORMAL], m.smp, uv);
    vec3 N;
    float spec_mask;
    if ((tech & TECH_MSN) != 0u) {
        // Model-space normal maps store (x, z, y); the game uses them without normalising. v_b/v_t/v_n carry the
        // object's unit axes for these materials.
        vec3 nm = nmap.xzy * 2.0 - 1.0;
        N = nm.x * normalize(v_b) + nm.y * normalize(v_t) + nm.z * normalize(v_n);
        spec_mask = tex(m.tex[TEX_SPECULAR], m.smp, uv).r;
    } else {
        vec3 nm = nmap.xyz * 2.0 - 1.0;
#if defined(WORLD_MAP)
        // ps 0x0D040001: the overlay takes the normal map's place, tangent space as it is.
        const vec3 rock = world_map_triplanar(m.tex[TEX_ENV_MASK], world_map_position(v_rel) * k_world_map_overlay_scale,
                                              world_map_weights(normalize(v_n)));
        const vec3 overlay = normalize(2.0 * (rock - 0.5));
        nm = normalize(overlay + k_world_map_normal_mix * (nm - overlay));
#endif
        N = normalize(nm.x * v_b + nm.y * v_t + nm.z * v_n);
        spec_mask = nmap.a;
        if (f.flags.w != 0u) {
            // Specular anti-aliasing: a mip-filtered normal
            // shorter than 1 widens the highlight (Toksvig), and dims it by the Blinn-Phong normalisation (n + 8): the same
            // light spread wider. Without that a stream's water, gloss 202, came out a broad white sheen.
            float len = min(length(nm), 1.0);
            const float sharp = m.specular.w;
            m.specular.w *= len / (len + m.specular.w * (1.0 - len));
            spec_energy = (m.specular.w + 8.0) / (sharp + 8.0);
        }
    }

    bool snow_pixel = false;
    float snow_mask = 1.0; // SV_Target3.y (11.5.6, 14.5.3)
    if (is_type(type, TYPE_SPARKLE)) {
        // The multi-pass directional material's pass (11.2.3, 14.4.2): coverage from the undampened
        // normal, lighting with the normal dampened in xy, the MATO's diffuse at the projected UV.
        vec3 nm = nmap.xyz * 2.0 - 1.0;
        vec3 nu = normalize(nm.x * v_b + nm.y * v_t + nm.z * v_n);
        float noise = textureLod(sampler2D(u_textures[nonuniformEXT(f.projected_textures.x)], u_samplers[SAMPLER_WRAP]),
                                 v_uvp * m.projected.z, 0.0).r;
        float s = dot(nu, normalize(m.projection[2].xyz)) * v_color.a - m.projected.w - m.projected.x * noise;
        if (s < 0.0) {
            discard;
        }
        N = normalize(nm.x * m.projected_color.x * v_b + nm.y * m.projected_color.x * v_t + nm.z * v_n);
        albedo = tex(m.tex[TEX_DIFFUSE], m.smp, v_uvp * m.projected_color.y);
        snow_pixel = (tech & TECH_SNOW) != 0u;
        if (!snow_pixel && m.sparkle.z > 0.0) {
            // Sparkle (14.4.3): three lookups of ProjectedNormalDetail's blue at the object UV.
            float k = abs(m.sparkle.x);
            uint t10 = f.projected_textures.w;
            float b1 = tex(t10, SAMPLER_WRAP, v_uv * pow(k, 1.3)).b;
            float b2 = tex(t10, SAMPLER_WRAP, v_uv * pow(k, 1.6)).b;
            float b3 = tex(t10, SAMPLER_WRAP, v_uv * pow(k, 1.9)).b;
            g_sparkle = m.sparkle.z * (pow(min(abs(b1), 1.0), m.sparkle.y) + pow(min(abs(b2), 1.0), m.sparkle.y) +
                                       pow(min(abs(b3), 1.0), m.sparkle.y));
            g_sparkle_r = 2.0 * dot(N, V) * N - V;
        }
    }

    // LOD fades of the reference's fade node (2.2.6): the specular fade whatever No_Fade says, the
    // environment fade only for Environment_Mapping (flags1.7), the decal fade unless No_Fade or flags2.9.
    float spec_fade = distance_fade(fade, f.fade_spec_env.x, f.fade_spec_env.y);
    float env_fade =
        (m.flags & MAT_ENV_MAP) != 0u ? distance_fade(fade, f.fade_spec_env.z, f.fade_spec_env.w) : 1.0;
    float alpha = albedo.a * m.emit.w;
    if ((m.flags & MAT_DECAL) != 0u && (m.flags & MAT_NO_DECAL_FADE) == 0u) {
        alpha *= distance_fade(fade, f.fade_decal_refr.x, f.fade_decal_refr.y);
    }
    // Trees and LOD objects keep other data in vertex alpha (5.5).
    if (!is_type(type, TYPE_TREE) && !is_type(type, TYPE_LOD_OBJECTS) &&
        !is_type(type, TYPE_LOD_OBJECTS_HD)) {
        alpha *= v_color.a;
    }
#if defined(ALPHA_TEST)
    const float alpha_edge = max(fwidth(alpha), 1e-4);
    if (alpha - m.alpha_threshold < 0.0) {
        discard;
    }
    // The game's test is all or nothing. Coverage only softens the edge: an alpha above the threshold is solid however low it is,
    // where its raw value would show the scene through (a Vampire Lord's body, alpha near its threshold of 30).
    alpha = clamp(0.5 + (alpha - m.alpha_threshold) / alpha_edge, 0.0, 1.0);
#elif defined(BLEND)
    if ((m.flags & MAT_ALPHA_TEST) != 0u && alpha - m.alpha_threshold < 0.0) {
        discard;
    }
#endif

    // The sun's shadow mask, for materials that take it.
    // It scales only the direct diffuse and the specular, so a back-facing point without specular needs none.
    bool sun_mask = (m.flags & MAT_RECEIVE_SHADOWS) != 0u &&
                    (dot(N, f.sun_dir.xyz) > 0.0 || (tech & TECH_SPECULAR) != 0u || f.flags.x == VIEW_SHADOW);
    float sun = f.dir_light_color.w * (sun_mask ? sun_shadow(v_rel) : 1.0);
    const vec4 light_masks = (m.flags & MAT_RECEIVE_SHADOWS) != 0u ? shadow_light_masks(inst.lights, v_rel) : vec4(1.0);
    // Everything loaded above would otherwise stay in registers through the masks' loops (149 instead of 80 in the
    // general variant): the material and the light list come again, from an address the compiler can't see through.
    const uint after_masks = floatBitsToUint(sun + light_masks.x + light_masks.y + light_masks.z + light_masks.w) >> 31;
    {
        const float power = m.specular.w;
        m = load_material(draw.y + after_masks);
        m.specular.w = power;
    }
    const uvec4 lights = pc.instances.v[draw.x + after_masks].lights;

    // Projected UV (11.5): the snow amount from the world normal's z, the vertex alpha and the noise; the layer
    // replaces the albedo (single colour) or blends a projected texture over it. The lighting keeps the object's
    // own normal.
    if ((tech & TECH_PROJECTED_UV) != 0u && !is_type(type, TYPE_SPARKLE)) {
        vec4 P = m.projected;
        vec2 uvn = v_uvp * P.z;
        float noise = textureLod(sampler2D(u_textures[nonuniformEXT(f.projected_textures.x)],
                                           u_samplers[SAMPLER_WRAP]), uvn, 0.0).r;
        float s = dot(N, m.projection[2].xyz) * v_color.a - P.w - P.x * noise;
        if (f.projected_params.w <= 0.5) {
            if (s > 0.0) {
                albedo.rgb = m.projected_color.rgb;
            }
            snow_pixel = s > 0.0;
            snow_mask = max(s, 0.0);
        } else {
            vec2 uvd = uvn * f.projected_params.x;
            float k = clamp((s + 0.1) * 5.0, 0.0, 1.0);
            float w = k * k * (3.0 - 2.0 * k);
            vec3 layer = textureLod(sampler2D(u_textures[nonuniformEXT(f.projected_textures.y)],
                                              u_samplers[SAMPLER_WRAP]), uvd, 0.0).rgb;
            albedo.rgb = mix(albedo.rgb, layer * m.projected_color.rgb, w);
            snow_pixel = true;
            snow_mask = w;
        }
        snow_pixel = snow_pixel && (tech & TECH_SNOW) != 0u;
    } else if (!is_type(type, TYPE_SPARKLE)) {
        // The snow shader without a projected layer (snow drifts, snow-flagged statics): every pixel is snow,
        // its mask the albedo's raw alpha (14.5.3).
        snow_pixel = (tech & TECH_SNOW) != 0u;
        snow_mask = albedo.a;
    }

    vec3 rim_soft = vec3(0.0);
    if ((tech & (TECH_RIM | TECH_SOFT)) != 0u) {
        rim_soft = tex(m.tex[TEX_RIM_SOFT], m.smp, uv).rgb;
    }
    vec3 back = vec3(0.0);
    if ((tech & TECH_BACK) != 0u) {
        back = tex(m.tex[TEX_BACK], m.smp, uv).rgb;
    }

    vec3 diffuse = vec3(0.0);
    vec3 spec = vec3(0.0);
    add_light(m, tech, N, V, f.sun_dir.xyz, f.dir_light_color.rgb, 1.0, sun, rim_soft, back, diffuse, spec);
    if (snow_pixel) {
        // Without specular the shader has no view vector: a constant stands in (11.5.6).
        spec = vec3(snow_rim_light(f.snow_rim, normalize(v_n), N, (tech & TECH_SPECULAR) != 0u ? V : vec3(0.57735026)));
    }
    // Point lights (5.3, 9.4): the object's list in order, the first 7 that are lit this frame.
    uint used = 0u;
    for (uint i = 0u; i < 8u && used < 7u; ++i) {
        uint li = light_index(lights, i);
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
        float a = clamp(dist / pl.position.w, 0.0, 1.0);
        float shadow = pl.color.w >= 2.0 ? light_masks[uint(pl.color.w) - 2u] : 1.0;
        add_light(m, tech, N, V, d / dist, pl.color.rgb, 1.0 - a * a, shadow, rim_soft, back, diffuse, spec);
    }

    // Eyes take their ambient and reflection from the eyeball sphere's normal, the rest from the normal map (15.4).
    vec3 Na = is_type(type, TYPE_EYE) ? normalize(v_eye_normal) : N;
    vec3 ambient = vec3(dot(f.dir_ambient[0], vec4(Na, 1.0)), dot(f.dir_ambient[1], vec4(Na, 1.0)),
                        dot(f.dir_ambient[2], vec4(Na, 1.0)));
    // Doors open onto another space. A leaf that has swung through one takes the side it came from's ambient light at
    // the door's plane, going over to this space's with the distance, on the faces that look back that way. The rest
    // takes the light that comes through.
    if (f.door_info.x != 0u) {
        const float door_leaf = pc.instances.v[draw.x + after_masks].leaf_frequency;
        if (door_leaf < 0.0) {
            const DoorLight d = f.doors[uint(-door_leaf) - 1u];
            const float past = dot(v_rel - d.position.xyz, d.normal.xyz);
            const float t = clamp(past / 1.4, 0.0, 1.0);
            // Only what looks back through the doorway: a strap's edge on a door facing the room stays in the room's light.
            const float own = mix(1.0, t * t * (3.0 - 2.0 * t), clamp(-2.0 * dot(Na, d.normal.xyz), 0.0, 1.0));
            const vec3 other = vec3(dot(d.ambient[0], vec4(Na, 1.0)), dot(d.ambient[1], vec4(Na, 1.0)),
                                    dot(d.ambient[2], vec4(Na, 1.0)));
            ambient = mix(other, ambient, own);
            diffuse *= own;
            spec *= own;
        } else {
            for (uint i = 0u; i < f.door_info.x; ++i) {
                const DoorLight d = f.doors[i];
                const vec3 to = d.position.xyz - v_rel;
                const float dist = max(length(to), 0.01);
                const float front = dot(-to, d.normal.xyz) / dist;
                if (front > 0.0 && dist < d.position.w) {
                    const float a = dist / d.position.w;
                    add_light(m, tech, N, V, to / dist, d.colour.rgb * d.normal.w, (1.0 - a * a) * front, 1.0, rim_soft,
                              back, diffuse, spec);
                }
            }
        }
    }
    vec3 emit = m.emit.rgb;
    if (is_type(type, TYPE_GLOWMAP)) {
        emit *= tex(m.tex[TEX_GLOW], m.smp, uv).rgb;
    }
    vec3 light = diffuse + (ambient + emit);
    light = f.ibl.yzw * f.ibl.x + light;
    if (f.flags.y != 0u) {
        light = vec3(1.0);
    }

    vec3 c = albedo.rgb * light;
    if (is_type(type, TYPE_HAIR)) {
        c *= (m.tint.rgb - 1.0) * v_color.g + 1.0; // hair tint, weighted by vertex green (5.8)
    } else {
        c *= v_color.rgb;
    }
    if (is_type(type, TYPE_MULTILAYER)) {
        // Multilayer parallax (5.9): an inner layer seen through the surface, offset along the reflected
        // tangent-space view vector, blended in where the surface faces the eye and the diffuse alpha is low.
        float h = tex(m.tex[TEX_INNER], m.smp, v_uv).a * m.layer.x;
        vec3 vt = vec3(dot(v_b, V), dot(v_t, V), dot(v_n, V));
        vec3 nl = vec3(0.0, 0.0, 1.0) + m.layer.y * (nmap.xyz * 2.0 - vec3(1.0, 1.0, 2.0));
        float d = dot(-vt, nl);
        vec3 r = nl * -(d + d) - vt;
        vec2 inner_uv = v_uv * m.layer.zw + r.xy * (h / abs(r.z) * 0.0009765625);
        vec3 inner = tex(m.tex[TEX_INNER], m.smp, inner_uv).rgb * v_color.rgb;
        float f_inner = clamp(dot(N, V), 0.0, 1.0) * (1.0 - albedo.a);
        c = mix(c, (diffuse + ambient) * inner, f_inner);
    }
    if (is_type(type, TYPE_ENVMAP) || is_type(type, TYPE_EYE) || is_type(type, TYPE_MULTILAYER)) {
        float mask = mix(spec_mask, tex(m.tex[TEX_ENV_MASK], m.smp, uv).r, m.effect.w);
        float strength = mask * (m.effect.z * env_fade); // EnvmapData.x * MaterialData.x
        vec3 R = 2.0 * dot(Na, V) * Na - V;
        c += tex_env_cube(m.tex[TEX_ENV], m.smp, R).rgb * strength * light;
    }

    vec3 removed;
    vec3 color = fog_and_clamp(c, f.output_clamp.x, removed);
    // On a snow pixel the specular (the sun's replaced by the snow rim) goes to the snow target for the image-space
    // passes, not into the colour (11.5).
    if ((tech & TECH_SPECULAR) != 0u && !snow_pixel) {
        color += spec * (spec_mask * spec_fade * spec_energy) * m.specular.rgb; // MaterialData.y: the specular fade
        color = fog_and_clamp(color, f.output_clamp.z, removed);
    }
#if defined(BLEND)
    // Blended lit draws come after the composite, decals apart, and nothing limits them later: the game's cb12[42].z is 1 for
    // exactly these (5.6), which leaves the fogged colour in the framebuffer's 1 / 1.2 range, 1 at most.
    color -= removed * ((m.flags & MAT_DECAL) != 0u ? f.frame_params.z : 1.0);
#else
    color -= removed * f.frame_params.z;
#endif

    color = debug_view(color, albedo.rgb, N, light);
    if (f.flags.x == VIEW_MATERIAL) {
        // for analysis: which material class drew each pixel (exact small integers in the HDR target).
        color = vec3(float(type), float(m.flags & 0x7FFu), (m.flags & MAT_ALPHA_TEST) != 0u ? 1.0 : 0.0);
    } else if (f.flags.x == VIEW_FORM) {
        const uint id = inst.form_id;
        color = vec3(float(id & 0x7FFu), float((id >> 11) & 0x7FFu), float(id >> 22));
        alpha = length(v_rel); // distance from the camera, metres
    } else if (f.flags.x == VIEW_TECHNIQUE) {
        color = vec3(float(tech & 0x7FFu), float((tech >> 11) & 0x7FFu), float(tech >> 22));
    }
    if (f.flags.x == VIEW_SHADOW) {
        color = vec3(sun, light_masks.xy);
    }
    if (f.flags.x == VIEW_UV) {
        vec2 q = fract(v_uv) * 256.0;
        o_color = vec4(floor(q.x), fract(q.x) * 1024.0, floor(q.y), fract(q.y) * 1024.0);
        return;
    }
#if !defined(BLEND)
    if (f.flags.x == 0u) {
        color = clamp(color, vec3(0.0), vec3(f.params.w)); // the composite's saturate, ahead of its 1 / 1.2
    }
    o_normal = encode_normal(N);
    if (snow_pixel) {
        // SV_Target3: the specular light's luminance and the mask; base snow (bit 19) adds the albedo's alpha to a
        // projected layer's (11.5.7).
        const bool base_snow = (tech & (1u << 19)) != 0u && (tech & TECH_PROJECTED_UV) != 0u && !is_type(type, TYPE_SPARKLE);
        o_normal.zw = vec2(dot(spec, vec3(0.3, 0.59, 0.11)), base_snow ? min(snow_mask + albedo.a, 1.0) : snow_mask);
    } else if ((tech & TECH_SPECULAR) != 0u) {
        o_normal.z = ssr_mask(spec_mask) * spec_fade; // SSRParams.w: off without specular, else the specular fade
    }
#endif
    o_color = vec4(color, alpha);
}
