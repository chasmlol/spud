#version 460
// A rectangle in target pixels, for filter passes and layer composites (push constants in uimovie_filter.glsl).
#include "uimovie_filter.glsl"

void main() {
    const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    const vec2 p = mix(fp.rect.xy, fp.rect.zw, corners[gl_VertexIndex % 6]);
    gl_Position = vec4(p * fp.to_clip - 1.0, 0.0, 1.0);
}
