// What the world map's land and objects share.
#ifndef WORLD_MAP_GLSL
#define WORLD_MAP_GLSL

const float k_world_map_overlay_scale = 3.5e-5; // fMapMenuOverlayScale:MapMenu
const float k_world_map_snow_scale = 4.5e-5;    // fMapMenuOverlaySnowScale:MapMenu
const float k_world_map_normal_mix = 1.0 - 1.1; // 1 - fMapMenuOverlayNormalStrength:MapMenu

// WorldMapOverlayParameters: the world position in game units.
vec3 world_map_position(vec3 rel) { return (rel + pc.frame.camera_pos.xyz) * (1.0 / 0.0142875); }

vec3 world_map_weights(vec3 N) {
    vec3 a = (abs(N) - 0.2) * 7.0;
    vec3 w = max(a * a * a, vec3(0.01));
    return w / (w.x + w.y + w.z);
}

vec3 world_map_triplanar(uint slot, vec3 p, vec3 w) {
    return w.x * tex(slot, SAMPLER_WRAP, p.yz).rgb + w.y * tex(slot, SAMPLER_WRAP, p.xz).rgb +
           w.z * tex(slot, SAMPLER_WRAP, p.xy).rgb;
}

vec3 world_map_remap(vec3 c, float s, float amount) {
    const vec3 b = vec3(0.078, 0.098, 0.465) + vec3(0.27, 0.281, 0.441) * s;
    vec3 p;
    p.rg = max(c.rg, 2.0 * b.rg);
    p.b = b.b > 0.5 ? max(c.b, 2.0 * b.b - 1.0) : min(c.b, 2.0 * b.b);
    return mix(c, p, amount);
}

#endif
