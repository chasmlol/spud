#version 460
// ISLocalMap (ImageSpace ps 119, effect 98): the local map's scene to the picture the movie shows, grey, with an alpha
// of the brightness and its edges where the fog of war is open, faded out at the rim. The fog
// is the mesh the game draws into the scene's alpha: a grid of vertices per
// cell, read here from the view's list.
#include "post.glsl"
layout(location = 0) out vec4 o_color;

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer LocalMapRef {
    vec4 centre_right; // the camera's x, y; the screen's right
    vec4 up_half;      // the screen's up; the frustum's half size
    vec4 grid;         // the first cell's corner; a cell's size
    uvec4 size;        // cells across, down; vertices along a cell's side
    uint levels[];     // a byte a vertex, 0 to 4 corners seen
};

float vertex_alpha(LocalMapRef m, uint cell, uint x, uint y) {
    const uint i = (cell * m.size.z + y) * m.size.z + x;
    return 1.0 - 0.25 * float((m.levels[i >> 2] >> ((i & 3u) * 8u)) & 0xFFu);
}

float fog(LocalMapRef m, vec2 uv) {
    const vec2 at = m.centre_right.xy + m.centre_right.zw * ((uv.x * 2.0 - 1.0) * m.up_half.z) +
                    m.up_half.xy * ((1.0 - uv.y * 2.0) * m.up_half.w);
    const vec2 cells = (at - m.grid.xy) / m.grid.zw;
    const ivec2 c = ivec2(floor(cells));
    if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, ivec2(m.size.xy)))) {
        return 1.0; // the clear colour's alpha: no mesh here
    }
    const vec2 t = (cells - vec2(c)) * float(m.size.z - 1u);
    const uvec2 v = min(uvec2(t), uvec2(m.size.z - 2u));
    const vec2 f = t - vec2(v);
    const uint cell = uint(c.y) * m.size.x + uint(c.x);
    return mix(mix(vertex_alpha(m, cell, v.x, v.y), vertex_alpha(m, cell, v.x + 1u, v.y), f.x),
               mix(vertex_alpha(m, cell, v.x, v.y + 1u), vertex_alpha(m, cell, v.x + 1u, v.y + 1u), f.x), f.y);
}

vec3 tap(vec2 uv, vec2 texel, float x, float y) { return sample_linear(SRC0, uv + texel * vec2(x, y)).rgb; }

void main() {
    const vec2 uv = target_uv();
    const vec2 texel = 1.0 / target_size();
    const vec3 side = vec3(0.5, 0.25, 1.0), straight = vec3(1.0, 0.5, 2.0);
    const vec3 nw = tap(uv, texel, -1, -1), n = tap(uv, texel, 0, -1), ne = tap(uv, texel, 1, -1);
    const vec3 w = tap(uv, texel, -1, 0), e = tap(uv, texel, 1, 0);
    const vec3 sw = tap(uv, texel, -1, 1), s = tap(uv, texel, 0, 1), se = tap(uv, texel, 1, 1);
    const vec3 gy = (nw + ne - sw - se) * side + (n - s) * straight;
    const vec3 gx = (nw + sw - ne - se) * side + (w - e) * straight;
    const float edge = dot((gy * gy + gx * gx) * 4.0, vec3(1.75));
    const float grey = dot(sample_linear(SRC0, uv).rgb, vec3(0.3, 0.59, 0.11));
    const vec2 p = abs(uv - 0.5) * 2.0;
    const vec2 rim = 1.0 - p * p * p * p * p;
    const float alpha = (1.0 - fog(LocalMapRef(uint64_t(pc.post)), uv)) * (grey + edge) * rim.x * rim.y;
    o_color = vec4(vec3(grey * 1.04), alpha);
}
