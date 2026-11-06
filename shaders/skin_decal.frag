#version 460
// A skinned decal: always the effect shader on the texture set's diffuse, clamped to its edge, blending DESTCOLOR, INVSRCALPHA
// As effect.frag with that pair: fog goes to white, the colour is premultiplied. The fade
// is the material's alpha, a factor twice over (10.5.2). pc.mode the texture, pc.pad the fade.
#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_fog;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 base = texture(sampler2D(u_textures[nonuniformEXT(pc.mode)], u_samplers[0]), v_uv);
    float fade = uintBitsToFloat(pc.pad);
    float a = base.a * fade * fade;
    o_color = vec4(mix(base.rgb, vec3(1.0), clamp(1.5 * v_fog, 0.0, 1.0)) * a, a);
}
