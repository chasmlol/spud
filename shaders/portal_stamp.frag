#version 460
// A door's far frame over the finished image (renderer_portal.cpp), by how much of the pixel shows the doorway: each
// sample of the main view's depth at or behind the doorway's plane, inside its edges. SRC0 the far frame's image, SRC1
// the main view's depth (multisampled when pc.mode bit 0 is set), SRC2 the main view's scene. What the main frame added
// in front of the doorway (rain, smoke, the near air's fog, a lens flare) stays: it is that frame's tone of its scene
// there above the tone of the far scene alone. pc.mode >> 1: the adapted values' slot, 0 without an image space.
#include "post.glsl"
layout(set = 0, binding = 0) uniform texture2DMS u_ms_textures[];
layout(location = 0) out vec4 o_color;

#define PORTAL_SCENE_SLOT (65536u - 7u - 64u + 62u) // Bindless::k_portal_scene_slot

bool behind_doorway(float depth, vec3 ray) {
    vec4 plane = pc.post.portal[4];
    return depth * plane.w <= pc.post.camera.w * dot(plane.xyz, ray) * 1.004;
}

void main() {
    vec2 uv = target_uv();
    vec3 ray = vec3(uv * 2.0 - 1.0, 1.0);
    for (int i = 0; i < 4; ++i) {
        if (dot(pc.post.portal[i].xyz, ray) < 0.0) {
            discard;
        }
    }
    ivec2 px = ivec2(gl_FragCoord.xy);
    float covered = 0.0;
    if ((pc.mode & 1u) != 0u) {
        int samples = textureSamples(sampler2DMS(u_ms_textures[SRC1], u_samplers[0]));
        for (int s = 0; s < samples; ++s) {
            float d = texelFetch(sampler2DMS(u_ms_textures[SRC1], u_samplers[0]), px, s).r;
            covered += behind_doorway(d, ray) ? 1.0 : 0.0;
        }
        covered /= float(samples);
    } else {
        covered = behind_doorway(texelFetch(sampler2D(u_textures[SRC1], u_samplers[0]), px, 0).r, ray) ? 1.0 : 0.0;
    }
    if (covered <= 0.0) {
        discard;
    }
    vec3 c = texelFetch(sampler2D(u_textures[SRC0], u_samplers[0]), px, 0).rgb;
    uint adapted = pc.mode >> 1;
    if (adapted != 0u) {
        vec2 a = sample_linear(adapted, uv).xy;
        vec3 here = texelFetch(sampler2D(u_textures[SRC2], u_samplers[0]), px, 0).rgb;
        vec3 far = texelFetch(sampler2D(u_textures[PORTAL_SCENE_SLOT], u_samplers[0]), px, 0).rgb;
        c += max(image_space_tone(here, vec3(0.0), a) - image_space_tone(far, vec3(0.0), a), vec3(0.0));
    }
    o_color = vec4(c, covered);
}
