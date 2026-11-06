#version 460
// The game's pixel shaders: the surface seen from above (cell and placed water, with or
// without a flow map), the far surface (LOD) and the surface seen from below (UNDER). Everything is in game units,
// camera relative, as the game's shader has it. The water lands in its own target, with the blend of ISWaterBlend
// over its own last image; water_resolve.frag then takes it into the scene.
#include "common.glsl"

layout(location = 0) in vec3 v_rel;
layout(location = 1) in vec4 v_fog;
layout(location = 2) in vec4 v_noise01;
layout(location = 3) in vec2 v_noise2;
layout(location = 4) in vec4 v_flow; // xy the displacement's texcoord, zw the flow map's
layout(location = 5) in vec2 v_cell; // the cell's texcoord
layout(location = 6) flat in uint v_material;
layout(location = 7) flat in float v_flow_grid;
layout(location = 8) flat in vec4 v_plane; // xyz the surface normal, w the eye's height over it, in units
layout(location = 9) flat in float v_quad;  // 1 for the wading quad
layout(location = 10) in float v_depth;     // from the vertices of a shape with Vertex Alpha Depth; < 0: from the scene
layout(location = 0) out vec4 o_color;

#define WRAP u_samplers[SAMPLER_WRAP]
#define CLAMP u_samplers[0]

vec3 noise(uint slot, vec2 uv) { return texture(sampler2D(u_textures[slot], WRAP), uv).xyz; }

// The scene's distance from the camera along a screen position: the game's depth in world units.
float scene_distance(vec2 uv) {
    WaterRef w = pc.water;
    ivec2 size = textureSize(sampler2D(u_textures[w.slots.y], CLAMP), 0);
    float d = texelFetch(sampler2D(u_textures[w.slots.y], CLAMP), clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1), 0).x;
    float z = d > 0.0 ? w.camera_z.x / d : 1e30; // reverse-Z: view Z in game units
    vec2 ndc = uv * 2.0 - 1.0;
    return z * length(vec3(ndc * w.camera_z.zw, 1.0));
}

vec2 screen_uv(vec3 rel_units) {
    vec4 clip = pc.frame.view_proj * vec4(rel_units * pc.water.camera_z.y, 1.0);
    return clip.xy / clip.w * 0.5 + 0.5;
}

#ifdef FLOW
// One flow-map tap: the local normal offset in the direction the cell's flow vector points, advanced by the flow's own
// clock.
vec2 flow_tap(vec2 flow_uv, vec2 scaled, float clock, float rate, float shift) {
    ivec2 texel = ivec2(floor(fract(flow_uv) * 704.0));
    vec4 f = texelFetch(sampler2D(u_textures[pc.water.slots2.z], CLAMP), texel, 0);
    vec2 dir = f.xy * 2.0 - 1.0;
    vec2 s = sqrt(1.01 - f.z) * scaled;
    vec2 uv = vec2(dot(vec2(dir.x, -dir.y), s), dot(dir.yx, s));
    uv -= vec2(clock * f.w * rate, 0.0);
    return texture(sampler2D(u_textures[pc.water.slots2.w], WRAP), uv + shift).xy;
}

vec2 flow_normal(WaterMaterial m) {
    vec2 tri = 1.0 - abs(fract(64.0 * v_flow_grid * v_flow.zw) * 2.0 - 1.0);
    float half_texel = 1.0 / (v_flow_grid * 128.0);
    vec2 scaled = v_cell * 64.0;
    float clock = m.c[2].w * 0.001;
    vec2 base = v_flow.zw;
    vec2 a = flow_tap(base + vec2(half_texel), scaled, clock, 9.92, 0.0);
    vec2 b = flow_tap(base + vec2(0.0, half_texel), scaled, clock, 10.64, 0.27);
    vec2 c = flow_tap(base, scaled, clock, 8.0, 0.0);
    vec2 d = flow_tap(base + vec2(half_texel, 0.0), scaled, clock, 8.48, 0.62);
    vec2 inv = 1.0 - tri;
    vec2 n = (mix(a, b, tri.x) * inv.y + mix(d, c, tri.x) * tri.y) - 0.5;
    return n / (sqrt(tri.x * tri.x + inv.x * inv.x) * sqrt(tri.y * tri.y + inv.y * inv.y));
}
#endif

void main() {
    WaterRef w = pc.water;
    WaterMaterial m = w.materials.m[v_material];
    vec3 V = normalize(v_rel);
    float dist = length(v_rel);
    vec2 uv = gl_FragCoord.xy * w.screen.xy;
    float range = w.sun_dir_range.w; // 1 / the framebuffer's range (0.8333)
    vec3 sun = w.sun_color.rgb * w.sun_color.a;
#ifndef LOD
    // The wading quad replaces the cells under it (the game's stencil id).
    vec2 place = v_rel.xy + w.eye.xy;
    if (v_quad == 0.0 && all(greaterThan(place, w.quad_rect.xy)) && all(lessThan(place, w.quad_rect.zw))) {
        discard;
    }
#endif

#ifdef LOD
    vec3 n = normalize(vec3(0.0, 0.0, 1.0) + m.c[6].x * (noise(m.noise.x, v_noise01.xy) * 2.0 - vec3(1.0, 1.0, 2.0)));
    vec3 nr = m.c[7].y * n + vec3(0.0, 0.0, 1.0 - m.c[7].y);
    vec3 refl = texture(samplerCube(u_cubes[w.slots.z], CLAMP), reflect(V, nr)).rgb * m.c[7].w;
    refl = mix(m.c[2].rgb, refl, m.c[5].y);
    if (w.params.z > 0.5) {
        refl = m.c[2].rgb * m.c[5].y; // the local map's technique 0x4803 has no cube
    }
    float fresnel = m.c[3].x + (1.0 - m.c[3].x) * pow(1.0 - clamp(dot(-V, n), 0.0, 1.0), 5.0);
    vec3 body = mix(m.c[0].rgb, m.c[1].rgb, fresnel);
    float ndl = clamp(dot(w.sun_dir_range.xyz, n), 0.0, 1.0);
    float fade = clamp((dist - 8192.0) / (m.c[7].x - 8192.0), 0.0, 1.0);
    fade = fade * (1.0 - w.params.y) + w.params.y;
    vec3 lit = body * ndl;
    vec3 color = lit + (1.0 + fade * (fresnel - 1.0)) * (refl - lit);
#elif defined(UNDER)
    float fade = clamp((dist - 8192.0) / (m.c[7].x - 8192.0), 0.0, 1.0);
    fade = fade * (1.0 - w.params.y) + w.params.y;
#ifdef FLOW
    vec2 fn = flow_normal(m);
    vec3 near_n = vec3(fn, sqrt(max(1.0 - dot(fn, fn), 0.0)));
    vec3 n = normalize(mix(noise(m.noise.x, v_noise01.xy) * 2.0 - 1.0, near_n, fade));
#else
    vec3 n = vec3(0.0, 0.0, 1.0) + m.c[6].x * (noise(m.noise.x, v_noise01.xy) * 2.0 - vec3(1.0, 1.0, 2.0));
    n += m.c[6].y * (noise(m.noise.y, v_noise01.zw) * 2.0 - 1.0);
    n += m.c[6].z * (noise(m.noise.z, v_noise2) * 2.0 - 1.0);
    n = normalize(n);
#endif
    vec2 ssr_uv = clamp(uv + n.xy * m.c[13].x, vec2(0.0), vec2(1.0));
    vec4 ssr = mix(texture(sampler2D(u_textures[w.slots2.y], CLAMP), ssr_uv),
                   texture(sampler2D(u_textures[w.slots2.x], CLAMP), ssr_uv), m.c[12].y);
    vec3 nr = m.c[7].y * n + vec3(0.0, 0.0, 1.0 - m.c[7].y);
    vec3 cube = texture(samplerCube(u_cubes[w.slots.z], CLAMP), reflect(V, nr)).rgb * m.c[7].w;
    vec3 refl = mix(cube, ssr.rgb, clamp(fade * m.c[12].x * ssr.a, 0.0, 1.0));
    refl = mix(m.c[2].rgb, refl, m.c[5].y);
    refl = (refl + m.c[0].rgb) * 0.5;
    vec3 through = texture(sampler2D(u_textures[w.slots.x], CLAMP), screen_uv(v_rel + vec3(m.c[5].w * n.xy, 0.0))).rgb * m.c[7].w;
    float f = pow(1.0 - clamp(dot(V, n), 0.0, 1.0), 5.0);
    f = 1.0 - (m.c[3].x + (1.0 - m.c[3].x) * f);
    vec3 color = refl + clamp(1.0 - dist * 0.002, 0.0, 1.0) * f * (through - refl);
#else
    // The path under the surface to what lies behind it, in units: along the ray, and straight down.
    float d = scene_distance(uv);
    vec3 P = -d * V;
    float along = dot(P, v_plane.xyz);
    float past = 1.0 - v_plane.w / along;
    float ray = length(P) * past, vertical = abs(along) * past;
    vec4 depth = clamp(vec4(ray, ray, vertical, vertical) / m.c[10].z, 0.0, 1.0);
    if (v_depth >= 0.0) {
        depth = vec4(clamp(v_depth, 0.0, 1.0));
    }
    vec4 control = 1.0 + m.c[11] * (depth - 1.0); // reflections, refraction, normals, specular

    float fade = clamp((dist - 8192.0) / (m.c[7].x - 8192.0), 0.0, 1.0);
    fade = fade * (1.0 - w.params.y) + w.params.y;
    vec2 ripple = vec2(0.0);
    if (v_quad > 0.0) {
        ripple = (texture(sampler2D(u_textures[w.slots.w], u_samplers[SAMPLER_ISOTROPIC]), v_flow.xy).zw - 0.5) * m.c[6].w;
    }
    vec3 disp = normalize(vec3(ripple, 0.04));
#ifdef FLOW
    vec2 fn = flow_normal(m) * max(control.z, 0.4);
    vec3 near_n = vec3(fn, sqrt(1.0 - fn.x * fn.x - fn.y * fn.y));
    vec3 far_n = noise(m.noise.x, v_noise01.xy) * 2.0 - 1.0;
    vec3 n = mix(far_n, near_n, fade);
    n = normalize(n);
#else
    vec3 n = vec3(0.0, 0.0, 1.0) + m.c[6].x * (noise(m.noise.x, v_noise01.xy) * 2.0 - vec3(1.0, 1.0, 2.0));
    n += m.c[6].y * (noise(m.noise.y, v_noise01.zw) * 2.0 - 1.0);
    n += m.c[6].z * (noise(m.noise.z, v_noise2) * 2.0 - 1.0);
    n = normalize(n) - vec3(0.0, 0.0, 1.0);
    n = normalize(control.z * n + vec3(0.0, 0.0, 1.0));
#endif
    n = disp + disp.z * (n - disp);

    // Refraction: the view moved along the normal; what it lands on must lie below the surface.
    vec3 moved = v_rel + vec3(control.y * m.c[5].w * n.xy, 0.0);
    vec2 refr_uv = screen_uv(moved);
    float behind = dot(-scene_distance(refr_uv) * V, v_plane.xyz);
    if (v_depth < 0.0 && (1.0 - v_plane.w / behind) * sign(behind) < 0.0) {
        refr_uv = uv;
    }
    vec3 scene = texture(sampler2D(u_textures[w.slots.x], CLAMP), refr_uv).rgb * m.c[7].w;
    float fog = 1.0 - pow(clamp((1.0 - depth.y) * m.c[10].z / m.c[10].w, 0.0, 1.0), m.c[8].w);
    vec3 body = mix(scene, mix(m.c[0].rgb, m.c[1].rgb, depth.y), fog);

    // Reflection: the cube, with the screen's reflections over it.
    vec3 nr = m.c[7].y * n + vec3(0.0, 0.0, 1.0 - m.c[7].y);
    vec3 cube = texture(samplerCube(u_cubes[w.slots.z], CLAMP), reflect(V, nr)).rgb * m.c[7].w;
    vec2 ssr_uv = clamp(uv + n.xy * m.c[13].x, vec2(0.0), vec2(1.0));
    vec4 ssr_blur = texture(sampler2D(u_textures[w.slots2.x], CLAMP), ssr_uv);
    vec4 ssr_raw = texture(sampler2D(u_textures[w.slots2.y], CLAMP), ssr_uv);
    vec4 ssr = mix(ssr_raw, ssr_blur, m.c[12].y);
    vec3 refl = mix(cube, ssr.rgb, clamp(fade * m.c[12].x * ssr.a, 0.0, 1.0));
    refl = mix(m.c[2].rgb, refl, m.c[5].y);

    float fresnel = pow(1.0 - clamp(dot(-V, n), 0.0, 1.0), 5.0);
    fresnel = m.c[3].x + (1.0 - m.c[3].x) * fresnel;
    vec3 color = body + (1.0 + fade * (fresnel * control.x - 1.0)) * (refl - body);

    vec3 R = reflect(V, n);
    float sparkle = pow(clamp(dot(n, vec3(-0.099, -0.099, 0.99)), 0.0, 1.0), m.c[0].w);
    float spec = pow(clamp(dot(R, w.sun_dir_range.xyz), 0.0, 1.0), m.c[5].x);
    color += (sun * spec * m.c[1].w + sun * sparkle * m.c[7].z) * control.w;
#endif
#ifndef UNDER
    color = mix(color, v_fog.rgb, v_fog.a);
#endif
    color = clamp(color * range, 0.0, 1.0);

#ifndef UNDER
    // The game's light pass: specular only, over the surface, off for water seen from below.
    vec3 lights = vec3(0.0);
    for (uint i = 0u; i < uint(w.light_count.x); ++i) {
        vec3 L = w.light_positions[i].xyz - v_rel;
        float d = length(L), r = clamp(d / w.light_positions[i].w, 0.0, 1.0);
        vec3 H = normalize(L / max(d, 1e-3) - V);
        lights += pow(clamp(dot(H, n), 0.0, 1.0), m.c[3].z) * w.light_colors[i].rgb * m.c[3].w * (1.0 - r * r);
    }
    float light_fresnel = m.c[3].x + (1.0 - m.c[3].x) * pow(1.0 - clamp(dot(-V, n), 0.0, 1.0), 5.0);
    color = clamp(color + clamp(lights * light_fresnel * range, 0.0, 1.0), 0.0, 1.0);
    // ISWaterBlend: over the water's last image, reprojected; fully its own state (water_history).
    if (w.params.x > 0.0) {
        vec4 last_clip = w.previous_view_proj * vec4(v_rel * w.camera_z.y, 1.0);
        vec2 last = last_clip.xy / last_clip.w * 0.5 + 0.5;
        vec4 history = texture(sampler2D(u_textures[w.slots3.x], CLAMP), last);
        if (all(greaterThanEqual(last, vec2(0.0))) && all(lessThanEqual(last, vec2(1.0))) && history.a == 1.0) {
            float mask = abs(dot(V, vec3(0.0, 0.0, 1.0)));
            float z = clamp((abs(dot(v_rel, w.camera_forward.xyz)) - w.camera_forward.w) * w.screen.z, 0.1, 0.95);
            color = mix(color, history.rgb, z * (0.95 - 0.85 * mask) * w.params.x);
        }
    }
#endif
    o_color = vec4(color, 1.0);
}
