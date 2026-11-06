#version 460
// The box of a decal, its far faces only: a pixel takes it once, wherever the camera stands, inside the box too.
#include "common.glsl"
#include "decal.glsl"

layout(location = 0) flat out uint v_decal;

void main() {
    Decal d = DecalRef(pc.particles).d[gl_InstanceIndex];
    const vec2 corners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    uint face = uint(gl_VertexIndex) / 6u;
    uint axis = face >> 1u;
    float side = (face & 1u) != 0u ? 1.0 : -1.0;
    vec2 c = corners[uint(gl_VertexIndex) % 6u];
    vec3 u = d.axes[(axis + 1u) % 3u].xyz, v = d.axes[(axis + 2u) % 3u].xyz, n = d.axes[axis].xyz * side;
    vec3 middle = d.position.xyz + n;
    v_decal = gl_InstanceIndex;
    if (dot(n, middle) < 0.0) {
        gl_Position = vec4(0.0, 0.0, 0.0, -1.0); // it looks at the camera: out of the clip volume
        return;
    }
    gl_Position = pc.frame.view_proj * vec4(middle + u * c.x + v * c.y, 1.0);
}
