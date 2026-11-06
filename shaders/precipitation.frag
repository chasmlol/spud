#version 460
// The game ENVCUBE: the texture at the framebuffer range, no lighting or fog, nothing below the occlusion map.
#include "common.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_alpha;
layout(location = 2) in vec3 v_occlusion;
layout(location = 0) out vec4 o_color;
#define PRECIP_OCCLUSION_SLOT 65506u // Bindless::k_precip_occlusion_slot
void main() {
    if (pc.post.misc.w != 0.0 && all(greaterThanEqual(v_occlusion.xy, vec2(0.0))) &&
        all(lessThan(v_occlusion.xy, vec2(1.0)))) {
        float d = texelFetch(sampler2D(u_textures[PRECIP_OCCLUSION_SLOT], u_samplers[0]),
                             ivec2(v_occlusion.xy * 512.0), 0).r;
        if (d > v_occlusion.z) { // reverse-Z: the surface is nearer the sky than the particle
            discard;
        }
    }
    Precip e = pc.post.precip[pc.mode & 0xFFu];
    vec4 t = texture(sampler2D(u_textures[nonuniformEXT(e.info.y)], u_samplers[SAMPLER_WRAP]), v_uv);
    o_color = vec4(t.rgb * pc.post.misc.z, t.a * v_alpha);
}
