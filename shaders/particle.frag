#version 460
// The game on a particle system: base colour, vertex
// colour, texture, palette, soft fade, effect lighting, fog by blend mode, the framebuffer's range.
#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec4 v_fog;
layout(location = 3) in float v_view_depth;
layout(location = 4) flat in uint v_system;
layout(location = 0) out vec4 o_color;

void main() {
    FrameRef f = pc.frame;
    ParticleSystem m = pc.particles.systems.s[v_system];
    const uint t = m.info.y;
    vec2 uv = v_uv * m.uv.zw + m.uv.xy;
    vec4 c = m.base_color * v_color;
    vec4 base = texture(sampler2D(u_textures[nonuniformEXT(m.info.x)], u_samplers[SAMPLER_WRAP]), uv);
    c.rgb *= base.rgb;
    c.a *= (t & EFX_PALETTE_ALPHA) != 0u ? 1.0 : base.a;
    if ((t & EFX_PALETTE_COLOR) != 0u) {
        float row = v_color.r * m.base_color.r;
        c.rgb = textureLod(sampler2D(u_textures[nonuniformEXT(m.info.z)], u_samplers[0]), vec2(base.g, row), 0.0).rgb * m.params.y;
    }
    float s = 1.0;
    if ((t & EFX_SOFT) != 0u && f.depth_params.y != 0.0) {
        float d = texelFetch(sampler2D(u_textures[DEPTH_SLOT], u_samplers[0]), ivec2(gl_FragCoord.xy), 0).r;
        float scene = d > 0.0 ? f.depth_params.x / d : 1e30;
        s = clamp((scene - v_view_depth) / (m.params.x * 0.0142875), 0.0, 1.0);
    }
    float a = c.a * s * m.base_color.a; // x PropertyColor.a, an effect's base alpha again
    if ((t & EFX_PALETTE_ALPHA) != 0u) {
        a = textureLod(sampler2D(u_textures[nonuniformEXT(m.info.z)], u_samplers[0]), vec2(base.a, a), 0.0).a;
    }
    if (a < 0.003) {
        discard;
    }
    vec3 L = (t & EFX_LIGHTING) != 0u ? f.effect_light.rgb : vec3(1.0);
    c.rgb *= mix(vec3(1.0), L, m.params.z);
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
    if ((t & (EFX_MULTIPLY | EFX_PREMULT)) == 0u) {
        c.rgb *= f.fog_color.w; // the framebuffer range, 1/1.2
    }
    o_color = vec4(c.rgb, a);
}
