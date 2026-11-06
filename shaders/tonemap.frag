#version 460
// ISHDRTonemapBlendCinematic and ...Fade (ImageSpace ps 35, 37): exposure from the adapted values, Reinhard (or the
// filmic curve), bloom where the result is dim, then saturation, tint, brightness and contrast, and the gamma.
// SRC0 the bloom, SRC1 the scene, SRC2 the adapted luminance. pc.mode 1: no image space, the scene clamped.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    vec3 c = texelFetch(sampler2D(u_textures[SRC1], u_samplers[0]), ivec2(gl_FragCoord.xy), 0).rgb;
    if (pc.mode == 1u) {
        o_color = vec4(clamp(c, 0.0, 1.0), 1.0);
        return;
    }
    vec2 uv = target_uv();
    vec3 bloom = sample_linear(SRC0, uv).rgb;
    vec2 a = sample_linear(SRC2, uv).xy;
    vec3 r = image_space_tone(c, bloom, a);
    vec4 fade = pc.post.fade;
    if (fade.w > 0.0) {
        r = mix(r, fade.rgb, fade.w);
    }
    o_color = vec4(r, 1.0);
}
