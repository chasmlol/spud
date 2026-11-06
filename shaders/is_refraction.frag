#version 460
// ISRefraction: the scene displaced where the refraction target says so, into a
// separate HDR target. ImageSpace ps 11.
#include "common.glsl"

#define SCENE_SLOT 65535u
#define REFRACTION_SLOT 65533u

layout(location = 0) out vec4 o_color;

void main() {
    FrameRef f = pc.frame;
    vec2 size = f.lod_fade.yz;
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec2 uv = gl_FragCoord.xy / size;
    vec4 n = texelFetch(sampler2D(u_textures[REFRACTION_SLOT], u_samplers[0]), px, 0);
    vec4 c0 = texelFetch(sampler2D(u_textures[SCENE_SLOT], u_samplers[0]), px, 0);
    float s = 0.05 * n.b;
    vec2 p = vec2(uv.x - 2.0 * s * (n.r - 0.5), uv.y + 2.0 * s * (n.g - 0.5));
    // Near the screen edges the offset is squeezed, blended in by the strength.
    vec2 c = p;
    c = mix(c, 0.85 + 0.78 * (c - 0.85), greaterThan(c, vec2(0.85)));
    c = mix(c, 0.15 - 0.78 * (0.15 - c), lessThan(c, vec2(0.15)));
    vec2 q = clamp(p + n.b * (c - p), 0.0, 1.0);
    float a = texelFetch(sampler2D(u_textures[REFRACTION_SLOT], u_samplers[0]),
                         clamp(ivec2(q * size), ivec2(0), ivec2(size) - 1), 0).a;
    vec4 c1 = textureLod(sampler2D(u_textures[SCENE_SLOT], u_samplers[0]), q, 0.0);
    // A displaced sample is used only where it lands on refraction-marked pixels.
    vec4 col = a != 0.0 ? c1 : c0;
    // The Tint branch (alpha strictly between 0.8 and 1; Tint (0, 0, 0, 0.3)): no refraction writer produces it.
    if (n.a > 0.8 && n.a < 1.0) {
        col.rgb = 0.7 * col.rgb + 0.3 * dot(c1.rgb, vec3(0.299, 0.587, 0.114)) * vec3(0.0);
    }
    o_color = col;
}
