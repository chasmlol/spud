// Continuous-LOD land: the heightfield and the far land's colour.

#define LAND_STEP 0.1143 // metres: the game's land heights are whole multiples of 8 units

float land_height(ivec2 s) {
    const FarLand fl = pc.frame.far_land;
    const uint i = uint(s.y) * fl.maps.w + uint(s.x);
    const uint word = TerrainHeightRef(fl.heights).w[i >> 1];
    const uint v = (i & 1u) != 0u ? word >> 16 : word & 0xFFFFu;
    return fl.samples.w + float(v) * LAND_STEP;
}

// What a vertex between its parent's collapses to: the parent's surface there, every parent quad split south-west to
// north-east.
float parent_height(ivec2 s, bool odd_column, bool odd_row, int step) {
    const ivec2 a = ivec2(odd_column ? step : 0, odd_row ? step : 0);
    return (land_height(s - a) + land_height(s + a)) * 0.5;
}

// The LOD landscape's albedo: the macro colour times the game's noise factor.
vec3 far_albedo(vec2 world) {
    const FarLand fl = pc.frame.far_land;
    const vec3 l = tex(fl.maps.x, SAMPLER_WRAP, world * fl.map.xy + fl.map.zw).rgb;
    // The game's noise repeats 3 times over a 4-cell LOD tile.
    const float noise = tex(fl.maps.z, SAMPLER_WRAP, world * (pc.frame.params.y * 0.75)).r;
    const float g = clamp((0.55 * (l.r + l.g + l.b) - 0.4) * (5.0 / 3.0), 0.0, 1.0);
    const float s = 0.8 * g * g * (3.0 - 2.0 * g);
    return l * ((noise + s * (0.37 - noise)) * (5.0 / 6.0) + 0.37);
}

vec3 far_normal(vec2 world) {
    const FarLand fl = pc.frame.far_land;
    const vec2 n = tex(fl.maps.y, SAMPLER_WRAP, world * fl.map.xy + fl.map.zw).rg * 2.0 - 1.0;
    return vec3(n, sqrt(max(1.0 - dot(n, n), 0.0)));
}
