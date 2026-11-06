#version 460
// ISWorldMap (ImageSpace ps 23), first on the linear image: a 7 x 7 box whose step grows away from the focal depth
// SRC0 the scene, SRC1 the depth.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void main() {
    const vec2 uv = target_uv();
    const float focus = 45000.0, max_blur = 0.45, scale = 0.3 * 3e-5, near_scale = 4.0; // fWorldMap*:MapMenu
    float z = game_view_z(game_depth(texelFetch(sampler2D(u_textures[SRC1], u_samplers[0]), ivec2(gl_FragCoord.xy), 0).r));
    float blur = min(abs(z - focus) * scale, max_blur) * (z < focus ? near_scale : 1.0);
    vec2 step = blur * 0.5 / target_size();
    vec4 sum = vec4(0.0);
    for (int y = -3; y <= 3; ++y) {
        for (int x = -3; x <= 3; ++x) {
            sum += sample_linear(SRC0, uv + vec2(x, y) * step);
        }
    }
    o_color = sum / 49.0;
}
