// Shared by the image-space passes. Each pass is a fullscreen triangle: pc.pass holds its
// source slots and the size of the target it draws, pc.post the frame's constants.
#ifndef POST_GLSL
#define POST_GLSL
#include "common.glsl"

#define SRC0 pc.pass.x
#define SRC1 pc.pass.y
#define SRC2 pc.pass.z

vec2 target_size() { return vec2(float(pc.pass.w & 0xFFFFu), float(pc.pass.w >> 16)); }
vec2 target_uv() { return gl_FragCoord.xy / target_size(); }

vec4 sample_linear(uint slot, vec2 uv) { return textureLod(sampler2D(u_textures[slot], u_samplers[0]), uv, 0.0); }
vec2 slot_size(uint slot) { return vec2(textureSize(sampler2D(u_textures[slot], u_samplers[0]), 0)); }
// A point-filtered read of level 0 at uv.
vec4 sample_point(uint slot, vec2 uv) {
    ivec2 size = textureSize(sampler2D(u_textures[slot], u_samplers[0]), 0);
    return texelFetch(sampler2D(u_textures[slot], u_samplers[0]), clamp(ivec2(floor(uv * vec2(size))), ivec2(0), size - 1), 0);
}

// spud's reverse-Z depth -> view distance in game units; 0 (the far plane) is infinite.
float view_units(float depth) { return depth > 0.0 ? pc.post.camera.w / depth * pc.post.camera.z : 1e30; }
// The same depth as the game's depth buffer would hold it (near, far = pc.post.camera.xy).
float game_depth(float depth) {
    float n = pc.post.camera.x, f = pc.post.camera.y;
    float v = view_units(depth);
    return f * (v - n) / (v * (f - n));
}
// ISSAOCompositeSAOFog, ISDistantBlur: the distance they derive from a depth value.
float game_view_z(float z) {
    float n = pc.post.camera.x, f = pc.post.camera.y;
    float zp = 1.01 * z - 0.01;
    return 2.0 * n * f / ((n + f) - (2.0 * zp - 1.0) * (f - n));
}
float luminance(vec3 c) { return dot(c, vec3(0.2125, 0.7154, 0.0721)); }
float smooth01(float t) { return t * t * (3.0 - 2.0 * t); }
// ISHDRTonemapBlendCinematic: a scene colour, its bloom and the adapted values to the image's colour, before the fade.
vec3 image_space_tone(vec3 c, vec3 bloom, vec2 a) {
    vec4 param = pc.post.tone, cin = pc.post.cinematic, tint = pc.post.tint;
    float l = max(luminance(c), 0.00001);
    float k = a.y / a.x;
    float ls = k * l;
    float lt;
    if (param.z > 0.5) {
        float x = max(ls - 0.004, 0.0);
        lt = param.y * pow(x * (x * 6.2 + 0.5) / (x * (x * 6.2 + 1.7) + 0.06), 2.2);
    } else {
        lt = ls * (ls * param.y + 1.0) / (ls + 1.0);
    }
    vec3 r = c * (lt / l) + bloom * clamp(param.x - lt, 0.0, 1.0);
    float g = luminance(r);
    r = g + cin.x * (r - g);
    r += tint.w * (g * tint.rgb - r);
    r = cin.z * (cin.w * r - a.x) + a.x;
    r = pow(clamp(r, 0.0, 1.0), vec3(pc.post.misc.y));
    return r;
}
#endif
