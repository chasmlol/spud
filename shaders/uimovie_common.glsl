// Shared by the movie shaders. Mirrors MovieDraw and MoviePush in src/ui/movie_renderer.cpp.
// Colours stay in gamma space (UNORM targets) and blend premultiplied, as Flash and Scaleform do.

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

struct MovieDraw {
    vec4 xform;     // a, b, c, d: local -> target pixels
    vec4 xform_t;   // tx, ty, vertex mode (0 mesh, 1 rect), paint (0 solid, 1 linear, 2 radial, 3 focal, 4 bitmap)
    vec4 mul;       // colour transform, straight colours: c * mul + add
    vec4 add;
    vec4 color;     // solid paint, straight RGBA
    vec4 fill;      // a, b, c, d: local -> gradient square [-1, 1] or bitmap UV
    vec4 fill_t;    // tx, ty, spread (0 pad, 1 reflect, 2 repeat), focal point
    vec4 rect;      // rect mode: x0, y0, x1, y1 in local space
    vec4 inner;     // scale-9: the shape's placement in the clip, a, b, c, d
    vec4 inner_t;   // tx, ty, the clip's on-screen scale x, y
    vec4 s9_bounds; // the clip's content bounds and grid, clip space
    vec4 s9_grid;
    uint64_t vertices;
    uint64_t stops;
    uint stop_count;
    uint sampler_index; // bitmap paint: 0 clamp, 1 repeat, +2 smooth
    uint flags;     // 1 scale-9, 2 gradient in linear RGB, 4 bitmap with straight alpha
    uint pad;
};

struct GradientStop {
    float ratio;
    uint color; // RGBA8, R in the low byte
};

layout(buffer_reference, scalar) readonly buffer MovieDraws { MovieDraw d[]; };
layout(buffer_reference, scalar) readonly buffer MovieVertices { vec2 v[]; };
layout(buffer_reference, scalar) readonly buffer MovieStops { GradientStop s[]; };

layout(push_constant, scalar) uniform MoviePush {
    MovieDraws draws;
    vec2 to_clip; // 2 / target size
    uint draw;
    uint pad;
} pc;
