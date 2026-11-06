#version 460
// A skinned decal's triangles: its vertices are the actor's, as skin.comp poses them, with the decal's UVs.
// pc.particles the decal's slot, pc.pass.xy the vertex arena's address, pc.pass.z the actor's instance.
#extension GL_EXT_buffer_reference_uvec2 : require
#include "common.glsl"
out float gl_ClipDistance[1];

struct PosedVertex {
    vec3 position;
    float bitangent_x;
    vec2 uv;
    uint normal;
    uint tangent;
    uint color;
};
struct DecalVertex {
    uint vertex;
    vec2 uv;
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer PosedVertices { PosedVertex v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer DecalSlot {
    uvec4 draw;
    DecalVertex v[];
};

layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_fog;

void main() {
    DecalVertex d = DecalSlot(pc.particles).v[gl_VertexIndex];
    vec3 position = PosedVertices(pc.pass.xy).v[d.vertex].position;
    Instance inst = pc.instances.v[pc.pass.z];
    mat3 ins = mat3(inst.r0.xyz, inst.r1.xyz, inst.r2.xyz);
    vec3 rel = position * ins + vec3(inst.r0.w, inst.r1.w, inst.r2.w) - pc.frame.camera_pos.xyz;
    v_uv = d.uv;
    vec4 clip = pc.frame.fog_view_proj * vec4(rel, 1.0);
    vec4 fp = pc.frame.fog_param;
    v_fog = min(pow(clamp(length(clip.xyz) * fp.y - fp.x, 0.0, 1.0), fp.z), fp.w);
    gl_Position = pc.frame.view_proj * vec4(rel, 1.0);
    gl_ClipDistance[0] = dot(pc.frame.clip_plane, vec4(rel, 1.0));
}
