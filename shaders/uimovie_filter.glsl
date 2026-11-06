// Mirrors FilterPush in src/ui/movie_renderer.cpp. Filters follow Flash's definitions (Flash 8 ActionScript 2.0
// Language Reference, flash.filters) with the box blur Ruffle documents for Flash Player (render/wgpu filters/blur.rs).
#extension GL_EXT_scalar_block_layout : require

layout(push_constant, scalar) uniform FilterPush {
    vec4 rect;       // the pass's rectangle in output pixels
    vec2 to_clip;    // 2 / output size
    int mode;        // 0 blur H, 1 blur V, 2 blur alpha H, 3 blur alpha V, 4 shadow combine, 5 colour matrix,
                     // 6 composite (layer over its parent)
    int flags;       // shadow: 1 inner, 2 knockout, 4 composite (draw the object), composite: 8 straight cxform
    vec2 offset;     // input pixel = output pixel + offset (region of the input relative to the output)
    vec2 shadow;     // shadow offset in pixels (distance along the angle)
    vec4 color;      // shadow / glow colour, straight
    float strength;
    float full_size; // box width for blur passes
    int m;           // full-weight taps each side
    float alpha;     // fractional weight of the edge taps
    vec4 mul;        // composite: colour transform
    vec4 add;
    vec4 matrix[5];  // colour matrix rows (4x5, last column / 255 as in Flash) packed: r, g, b, a, offsets
} fp;
