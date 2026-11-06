// A box decal.
#define DECAL_ALPHA_BLEND 1u
#define DECAL_ALPHA_TEST 2u
#define DECAL_SPECULAR 4u

struct Decal {
    vec4 position;  // xyz camera-relative metres, w the angle test's cosine
    vec4 axes[3];   // xyz the half extents as vectors: width, height, depth; w 1 / their length squared
    vec4 color;     // rgb the DODT's, a the fade
    uvec4 textures; // diffuse, normal (0: the effect shader, unlit), glow (0: none), flags
    uvec4 lights;   // as an instance's
    vec4 params;    // x shininess, y uv scale, zw uv offset
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer DecalRef {
    Decal d[];
};
