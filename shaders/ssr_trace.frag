#version 460
// ReflectionsRayTracing (ImageSpace ps 176) with ISDownsampleIgnoreBrightest's normal downsample (ps 160) in front of it:
// 15 steps along the reflected ray out to fReflectionMarchingRadius of the screen, then 16 back between the last two for
// a surface within fReflectionRayThickness. SRC0 the normal target, SRC1 the scene's depth, SRC2 the scene. The game
// also adds last frame's blended effects (t3): spud has no such target, and the image keeps no history.
#include "post.glsl"
layout(location = 0) out vec4 o_color;
float ray_depth(vec2 uv) { return min(game_depth(sample_point(SRC1, uv).x), 1.0); }
void main() {
    vec2 uv = target_uv();
    vec2 t = 1.0 / slot_size(SRC0);
    vec4 n = 0.25 * (sample_linear(SRC0, uv + vec2(-t.x, -t.y)) + sample_linear(SRC0, uv + vec2(-t.x, t.y)) +
                     sample_linear(SRC0, uv + vec2(t.x, -t.y)) + sample_linear(SRC0, uv + vec2(t.x, t.y)));
    // The mask shares the snow target's specular channel, which only a snow pixel uses.
    float mask = n.w <= 0.00001 ? n.z : 0.0;
    float depth = sample_point(SRC1, uv).x;
    // Water (pc.mode 1): the stencil technique's pixels trace from the surface, along its normal.
    bool water = false;
    if (pc.mode == 1u) {
        uint m = pc.water.slots3.z;
        water = 0.25 * (sample_linear(m, uv + vec2(-t.x, -t.y)).x + sample_linear(m, uv + vec2(-t.x, t.y)).x +
                        sample_linear(m, uv + vec2(t.x, -t.y)).x + sample_linear(m, uv + vec2(t.x, t.y)).x) >= 0.00001;
        mask = water ? 1.0 : mask;
    }
    o_color = vec4(0.0);
    if (mask < 0.00001 || depth <= 0.0) {
        return;
    }
    vec2 e = n.xy * 4.0 - 2.0;
    float g = dot(e, e);
    vec3 N = water ? pc.water.default_normal.xyz : normalize(vec3(e * sqrt(1.0 - g * 0.25), -(1.0 - g * 0.5)));
    vec2 ndc = vec2(2.0, -2.0) * uv + vec2(-1.0, 1.0);
    float view_z = view_units(depth);
    vec3 P = vec3(ndc * pc.post.sao_proj.zw, 1.0) * view_z;
    vec3 to_eye = normalize(-P);
    vec3 R = reflect(-to_eye, N);
    if (dot(to_eye, R) > 0.0 || R.z < 0.0) {
        return; // back towards the camera
    }
    vec3 Q = P + R;
    float n_game = pc.post.camera.x, f_game = pc.post.camera.y;
    float qz = f_game * (Q.z - n_game) / (Q.z * (f_game - n_game));
    vec2 quv = vec2(0.5, -0.5) * (Q.xy / (pc.post.sao_proj.zw * Q.z)) + 0.5;
    float start_z = min(game_depth(depth), 1.0);
    vec3 start = vec3(uv, start_z);
    vec3 d = vec3(quv, qz) - start;
    if (d.y > 0.0 || d.z < 0.0) {
        return; // down the screen, or nearer than the surface
    }
    vec3 end = start + d * (pc.post.ssr.x / length(d.xy));
    end.xy = clamp(end.xy, vec2(0.0), vec2(1.0));
    vec3 delta = end - start;
    vec3 before = start, after = start;
    bool crossed = false;
    for (int i = 1; i < 16 && !crossed; ++i) {
        before = after;
        after = start + delta * (float(i) * 0.0625);
        crossed = ray_depth(after.xy) < after.z;
    }
    if (!crossed) {
        return;
    }
    vec3 hit = vec3(0.0);
    bool found = false;
    for (int k = 0; k < 16 && !found; ++k) {
        hit = before + (after - before) * (float(k) * 0.0625);
        float s = ray_depth(hit.xy);
        found = s < hit.z && hit.z < s + pc.post.ssr.y;
    }
    if (!found) {
        return;
    }
    float edge = min(2.0 * distance(hit.xy, vec2(0.5)), 1.0);
    float a = (1.0 - distance(hit.xy, uv) * 2.5) * (1.0 - edge * edge) * mask;
    o_color = vec4(sample_linear(SRC2, hit.xy).rgb, a);
}
