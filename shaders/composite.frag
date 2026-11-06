#version 460
// ISSAOCompositeSAOFog (ImageSpace ps 193) as a blend into the multisampled scene, per sample: c * alpha + rgb is
// lerp(ao * c, fog, f) / 1.2, and sky pixels (depth at the far plane) only take the ambient occlusion. The saturate
// that ends the game's shader comes from the opaque shaders' limit (GpuFrame::params.w). SRC0 the ambient occlusion,
// SRC1 the scene's depth (the multisampled target when pc.mode is 1), SRC2 the scene, one sample per pixel.
#include "post.glsl"
#include "snoise.glsl"
#include "shadows.glsl"
layout(set = 0, binding = 0) uniform texture2DMS u_ms_textures[];
layout(location = 0) out vec4 o_color;
void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    const bool plain = (pc.mode & 2u) != 0u; // no occlusion or reflections were made
    float depth = (pc.mode & 1u) != 0u ? texelFetch(sampler2DMS(u_ms_textures[SRC1], u_samplers[0]), px, gl_SampleID).r
                                : texelFetch(sampler2D(u_textures[SRC1], u_samplers[0]), px, 0).r;
    float z = game_depth(depth);
    float ao = plain ? 1.0 : texelFetch(sampler2D(u_textures[SRC0], u_samplers[0]), px, 0).x;
    // The snow target: its specular joins the colour, and occlusion stops on snow (bDeactivateAOOnSnow).
    vec2 snow = texelFetch(sampler2D(u_textures[pc.post.post_slots.z], u_samplers[0]), px, 0).zw;
    float snow_spec = snow.x * snow.y;
    if (snow.y > 0.00001) {
        ao = min(ao + pc.post.sparkle[2].x, 1.0);
        // Read first: every snow sample storing to one address serialises the whole pass on it.
        if (pc.post.snow_flag.value == 0u) {
            pc.post.snow_flag.value = 1u;
        }
    }
    if (z >= 0.999999) {
        if (ao >= 1.0) {
            discard; // sky without occlusion: the blend would change nothing
        }
        o_color = vec4(0.0, 0.0, 0.0, ao);
        return;
    }
    vec4 fp = pc.post.fog_param;
    float d = game_view_z(z);
    float fo = min(pow(clamp(d * fp.y - fp.x, 0.0, 1.0), fp.z), fp.w);
    vec3 fog = mix(pc.post.fog_near.rgb, pc.post.fog_far.rgb, fo);
    float k = pc.post.fog_near.w;
    float a = ao * (1.0 - fo) * k;
    vec3 add = vec3(0.0);
    if (snow.y != 0.0 && pc.post.sparkle[1].z > 0.00001) {
        // Sparkles: simplex noise fixed in the world, thresholded, fading with distance, less in the sun's shadow.
        vec4 s1 = pc.post.sparkle[0], s2 = pc.post.sparkle[1], s3 = pc.post.sparkle[2], s4 = pc.post.sparkle[3];
        FrameRef f = pc.frame;
        vec2 ndc = vec2(2.0, -2.0) * target_uv() + vec2(-1.0, 1.0);
        vec3 forward = cross(f.camera_up.xyz, f.camera_right.xyz);
        vec3 rel = (f.camera_right.xyz * (ndc.x * pc.post.sao_proj.z) + f.camera_up.xyz * (ndc.y * pc.post.sao_proj.w) +
                    forward) * view_units(depth);
        vec3 world = rel + f.camera_pos.xyz * pc.post.camera.z;
        float n = 2.0 * 21.0 * snoise(world * s2.x * 0.07);
        float q = clamp((n + 1.0) * 0.5, 0.0, 1.0);
        float p = pow(q, s1.z);
        p = p < s1.w ? 0.0 : p * s1.x;
        float fade = 1.0 - smooth01(clamp(length(rel) / s3.y, 0.0, 1.0));
        if (p > 0.0) {
            float lit = mix(sun_shadow(rel / pc.post.camera.z), 1.0, s4.x);
            add = snow.y * lit * p * fade * s4.yzw;
        }
    }
    // Reflections and snow join the colour before the occlusion and the fog, and the game's saturate follows them. Both
    // depend on the colour under the blend, which a blend can't read: the single-sampled scene stands in.
    vec3 base = fog * fo * k;
    vec3 extra = snow_spec * a + add;
    bool reflects = !plain && snow.y <= 0.00001 && snow.x > 0.00001 && pc.post.ssr.z > 0.0;
    if (reflects || extra != vec3(0.0)) {
        vec3 c = texelFetch(sampler2D(u_textures[SRC2], u_samplers[0]), px, 0).rgb;
        if (reflects) {
            vec4 r = sample_linear(pc.post.post_slots.w, target_uv());
            extra += min(c * pc.post.ssr.w, max(r.rgb * r.a * pc.post.ssr.z, vec3(0.0))) * a;
        }
        extra = min(extra, max(1.0 - (c * a + base), vec3(0.0)));
    }
    o_color = vec4(base + extra, a);
}
