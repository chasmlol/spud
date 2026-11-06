#version 460
// Skyrim's effect shader. Variants: ALPHA_TEST, BLEND.
#include "common.glsl"

layout(location = 0) centroid in vec3 v_rel;
layout(location = 1) in vec2 v_uv;
layout(location = 4) centroid in vec3 v_normal;
layout(location = 5) centroid in vec4 v_color;
layout(location = 6) centroid in vec4 v_fog;
#ifdef BLEND
layout(location = 2) in vec3 v_b;
layout(location = 3) in vec3 v_t;
layout(location = 13) in vec2 v_uvp; // a membrane: the mesh's own UV
#endif
layout(location = 8) flat in uint v_material;
layout(location = 10) centroid in float v_falloff;
layout(location = 11) centroid in float v_view_depth;
layout(location = 12) flat in uvec4 v_lights;

layout(location = 18) flat in vec4 v_emittance;
layout(location = 19) flat in uint v_draw;
layout(location = 0) out vec4 o_color;

#ifdef BLEND
// An effect shader over a lighting mesh (10.4.7; Effect ps 0x8043, 0x88443, 0x108443, 0x188443, 0x208443, 0x808443).
// Per frame: emit is PropertyColor, base_color MembraneRimColor, falloff MembraneVars, uv TexcoordOffsetMembrane.
vec4 membrane(Material m, uint t) {
    if ((t & EFX_BASE_ALPHA) != 0u && tex(m.tex[TEX_GLOW], m.smp, v_uvp).a * v_color.a < m.layer.x) {
        discard;
    }
    vec3 N = normalize(v_normal);
    if ((m.flags & MAT_MODEL_SPACE_NORMALS) != 0u) {
        vec3 nm = tex(m.tex[TEX_NORMAL], m.smp, v_uvp).xzy * 2.0 - 1.0;
        N = nm.x * normalize(v_b) + nm.y * normalize(v_t) + nm.z * normalize(v_normal);
    }
    vec4 R = pow(clamp(1.0 - dot(N, normalize(-v_rel)), 0.0, 1.0), m.falloff.x) * m.base_color;
    vec4 T = tex(m.tex[TEX_DIFFUSE], m.smp, v_uv);
    vec4 property = m.emit;
    float fill = v_color.a * ((t & (EFX_IGNORE_ALPHA | EFX_PALETTE_ALPHA)) != 0u ? 1.0 : T.a);
    float a = property.a * fill + R.a;
    if ((t & EFX_PALETTE_ALPHA) != 0u) {
        a = textureLod(sampler2D(u_textures[nonuniformEXT(m.palette)], u_samplers[0]), vec2(T.a, a), 0.0).a;
    }
    if ((t & EFX_PALETTE_COLOR) != 0u) {
        vec2 at = vec2(T.g, v_color.r * property.r);
        vec3 c = textureLod(sampler2D(u_textures[nonuniformEXT(m.palette)], u_samplers[0]), at, 0.0).rgb;
        return vec4(c * m.falloff.z, a);
    }
    return vec4((T.rgb + property.rgb) * (property.a * fill) + R.rgb * R.a, a);
}
#endif

void main() {
    FrameRef f = pc.frame;
    Material m = load_material(v_material);
    const uint t = m.technique;

    vec4 c;
    float a;
#ifdef BLEND
    if ((m.effect_flags & EFFECT_MEMBRANE) != 0u) {
        c = membrane(m, t);
        a = c.a;
    } else
#endif
    {
        c = m.base_color;
        if ((t & EFX_VC) != 0u) {
            c *= v_color;
        }
        vec4 base = vec4(1.0);
        if ((t & EFX_TEXTURE) != 0u) {
            base = tex(m.tex[TEX_DIFFUSE], m.smp, v_uv);
            c.rgb *= base.rgb;
            // With palette alpha the texture's alpha is the palette's column, not a factor of the alpha (10.4.2).
            c.a *= ((t & EFX_PALETTE_ALPHA) != 0u ? 1.0 : base.a) * v_falloff;
        } else {
            c.a *= v_falloff;
        }
        if ((t & EFX_PALETTE_COLOR) != 0u) {
            // The base texture's green indexes the palette; vertex red x base red picks the row (10.4.2).
            float row = ((t & EFX_VC) != 0u ? v_color.r : 1.0) * m.base_color.r;
            c.rgb = textureLod(sampler2D(u_textures[nonuniformEXT(m.palette)], u_samplers[0]), vec2(base.g, row), 0.0).rgb *
                    m.extra.y;
        }
        if ((t & EFX_WEAPON_BLOOD) != 0u) {
            // Weapon blood (10.4.7): the threshold grows it over the mask; layer.y, z are the falloff and the alpha-to-RGB scale.
            vec4 vc = (t & EFX_VC) != 0u ? v_color : vec4(1.0);
            float mask = base.g * vc.g * m.base_color.g;
            float g = clamp(mask - m.alpha_threshold, 0.0, 1.0), w = base.b * vc.b * m.base_color.b * vc.a;
            float x = g < m.layer.y ? g / m.layer.y * w : w;
            c = vec4((1.0 - x * m.layer.z) * vec3(clamp(2.0 - x, 0.0, 1.0), vec2(clamp(1.0 - x, 0.0, 1.0))), mask);
        }
        float s = 1.0;
        if ((t & EFX_SOFT) != 0u && f.depth_params.y != 0.0) {
            // Soft fade against the opaque scene's depth (10.4.3).
            float d = texelFetch(sampler2D(u_textures[DEPTH_SLOT], u_samplers[0]), ivec2(gl_FragCoord.xy), 0).r;
            float scene = d > 0.0 ? f.depth_params.x / d : 1e30;
            s = clamp((scene - v_view_depth) / (m.effect.y * 0.0142875), 0.0, 1.0);
            if (c.a * s < 0.003) {
                discard;
            }
        }
        a = c.a * s * m.emit.w;
        if ((t & EFX_PALETTE_ALPHA) != 0u) {
            a = textureLod(sampler2D(u_textures[nonuniformEXT(m.palette)], u_samplers[0]), vec2(base.a, a), 0.0).a;
        }
#if defined(ALPHA_TEST) || defined(BLEND)
        if ((t & EFX_OPAQUE) == 0u && (m.flags & MAT_ALPHA_TEST) != 0u && a < m.alpha_threshold) {
            discard;
        }
#endif
    }
    // Lighting influence: the external emittance colour (white unless a reference sets one) or effect lighting:
    // the sun's Effect Lighting colour plus up to 4 point lights, non-directional (10.4.5). A reference's emittance
    // colours its External_Emittance shapes and turns their effect lighting off (10.4.6).
    vec3 L = vec3(1.0);
    const bool emitted = (m.flags & MAT_EXTERNAL_EMITTANCE) != 0u && v_emittance.w > 0.0;
    if (emitted) {
        L = v_emittance.rgb;
    } else if ((t & EFX_LIGHTING) != 0u) {
        L = f.effect_light.rgb;
        uint used = 0u;
        for (uint i = 0u; i < 8u && used < 4u; ++i) {
            uint li = light_index(v_lights, i);
            if (li == 0xFFFFu) {
                break;
            }
            PointLight pl = pc.lights.l[li];
            if (pl.color.w == 0.0) {
                continue;
            }
            ++used;
            vec3 d = pl.position.xyz - v_rel;
            L += (1.0 - clamp(dot(d, d) / (pl.position.w * pl.position.w), 0.0, 1.0)) * pl.color.rgb;
        }
    }
    c.rgb *= mix(vec3(1.0), L, m.effect.x);

    if ((t & EFX_OPAQUE) == 0u) {
        if ((t & EFX_ADDITIVE) != 0u) {
            c.rgb *= 1.0 - v_fog.a;
        } else if ((t & (EFX_MULTIPLY | EFX_PREMULT)) != 0u) {
            c.rgb = mix(c.rgb, vec3(1.0), clamp(1.5 * v_fog.a, 0.0, 1.0));
            if ((t & EFX_PREMULT) != 0u) {
                c.rgb *= a;
            }
        } else {
            c.rgb = mix(c.rgb, v_fog.rgb, v_fog.a);
        }
    }
    if ((t & (EFX_MULTIPLY | EFX_PREMULT)) == 0u) {
        c.rgb *= f.fog_color.w; // the framebuffer range, 1/1.2
    }
    c.rgb = debug_view(c.rgb, c.rgb, normalize(v_normal), vec3(1.0));
    if (f.flags.x == VIEW_MATERIAL) {
        c.rgb = vec3(100.0, float(m.flags & 0x7FFu), (m.flags & MAT_ALPHA_TEST) != 0u ? 1.0 : 0.0);
    } else if (f.flags.x == VIEW_FORM || f.flags.x == VIEW_TECHNIQUE) {
        discard; // blended: these views show what lies behind
    }
    const Instance inst = pc.instances.v[pc.draw_instances.v[v_draw].x];
    if ((inst.flags & (1u << 5)) != 0u) {
        a *= inst.leaf_amplitude; // a weather's sky model fades with the hour and the transition
    }
    if ((inst.flags & 0xC0u) != 0u) { // a sky static, with the weathers that list it
        a *= ((inst.flags & 0x40u) != 0u ? f.clock.y : 0.0) + ((inst.flags & 0x80u) != 0u ? 1.0 - f.clock.y : 0.0);
    }
    o_color = vec4(c.rgb, a);
}
