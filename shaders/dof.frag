#version 460
// ISDistantBlur (ImageSpace ps 19): the blurred quarter image beyond the focus. SRC0 the tonemapped image, SRC1 the
// blurred one, SRC2 the scene's depth; the focus is the camera-Z pyramid's last level. pc.mode 1: a plain copy.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
void distant_blur() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4 image = texelFetch(sampler2D(u_textures[SRC0], u_samplers[0]), px, 0);
    if (pc.mode == 1u) {
        o_color = image;
        return;
    }
    vec2 uv = target_uv();
    vec4 p1 = pc.post.dof1, p2 = pc.post.dof2, p3 = pc.post.dof3;
    float strength, range, focus;
    if (p3.z > 0.0) {
        float a = sample_point(pc.post.post_slots.x, vec2(0.0)).x;
        float phi = clamp(a * p3.z - p3.w, 0.0, 1.0);
        strength = mix(p2.x, p2.w, phi);
        range = mix(p1.x, p1.y, phi);
        focus = a;
    } else {
        strength = p2.x;
        range = p1.x;
        focus = p1.y;
    }
    float z = game_depth(texelFetch(sampler2D(u_textures[SRC2], u_samplers[0]), px, 0).r);
    float sky = 0.0;
    if (p2.w != 0.0 && z > 0.999999) {
        // Sky: the mean of nine depths at +-3 pixels; the more of them are sky, the less blur.
        vec2 size = target_size();
        ivec2 isize = ivec2(size) - 1;
        float sum = z, count = 1.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0) {
                    continue;
                }
                float d = game_depth(texelFetch(sampler2D(u_textures[SRC2], u_samplers[0]),
                                                clamp(px + ivec2(x, y) * 3, ivec2(0), isize), 0).r);
                d = min(d, 1.0);
                sum += d;
                count += d > 0.999999 ? 1.0 : 0.0;
            }
        }
        if (count >= 9.0) {
            o_color = image;
            return;
        }
        sky = count / 9.0;
        z = min(sum, 9.0) / 9.0;
    }
    if (z <= 0.00001) {
        o_color = image;
        return;
    }
    float w = clamp((game_view_z(z) - focus) / range, 0.0, 1.0) * strength * (1.0 - 0.5 * sky);
    o_color = mix(image, sample_linear(SRC1, uv), w);
}

void main() {
    distant_blur();
    // ISDistantBlurFogged: the fog of the water the camera is in.
    // Straddling the surface, only what's below the waterline: the game's MaskedFogged, with its mask's hard edge.
    if (pc.post.water_fog.a > 0.0 && target_uv().y >= pc.post.water_fog2.z) {
        float z = game_depth(texelFetch(sampler2D(u_textures[SRC2], u_samplers[0]), ivec2(gl_FragCoord.xy), 0).r);
        float v = z >= 1.0 ? pc.post.camera.y : game_view_z(z);
        float far = pc.post.water_fog2.x, near = pc.post.water_fog2.y;
        float f = clamp((v - near) / (far - near), 0.0, 1.0) * pc.post.water_fog.a;
        o_color.rgb = mix(o_color.rgb, pc.post.water_fog.rgb, f);
    }
}
