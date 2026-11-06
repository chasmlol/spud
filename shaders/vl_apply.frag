#version 460
// ISApplyVolumetricLighting (ImageSpace ps 135) without its history: the summed volume at the pixel's depth, dithered.
// SRC0 the scene's depth. vl0: width, height, depth, intensity / depth; vl1.x the range.
#include "post.glsl"
layout(location = 0) out vec4 o_color;

float bayer() {
    uvec2 p = uvec2(gl_FragCoord.xy) & 7u;
    uint a = p.x ^ p.y, y = p.y;
    uint v = ((a & 1u) << 5) | ((y & 1u) << 4) | ((a & 2u) << 2) | ((y & 2u) << 1) | ((a & 4u) >> 1) | ((y & 4u) >> 2);
    return float(v) / 255.0 / 32.0 - 1.0 / 128.0;
}

void main() {
    vec4 g = pc.post.vl0;
    float z = min(game_depth(texelFetch(sampler2D(u_textures[SRC0], u_samplers[0]), ivec2(gl_FragCoord.xy), 0).r), 1.0);
    float s = clamp(pow(max(z, 0.0), pc.post.vl1.x), 0.0, 0.9999);
    // Depth wraps in this sampler: held inside the first and last slices' centres, as a clamp would.
    s = clamp(s, 0.5 / g.z, 1.0 - 0.5 / g.z);
    float v = textureLod(sampler3D(u_volumes[pc.post.post_slots.y], u_samplers[0]), vec3(target_uv(), s), 0.0).x;
    // The blur's depth rides along: 1 - z keeps the precision where the game's z crowds towards 1.
    o_color = vec4(v * g.w + bayer(), 1.0 - z, 0.0, 0.0);
}
