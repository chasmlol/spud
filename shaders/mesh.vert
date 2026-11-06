#version 460
// Skyrim's lighting (and effect) vertex shader inputs and outputs.
// The game lights in model space; this lights in world space (camera-relative), which is the same maths for
// rotations and uniform scales. Inputs are the game's own packed bytes (.osm v2).
#include "common.glsl"
out float gl_ClipDistance[1];

layout(location = 0) in vec4 in_pos;     // xyz shape space (metres), w = bitangent x
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_normal;  // unorm8: xyz normal, w = bitangent y
layout(location = 3) in vec4 in_tangent; // unorm8: xyz tangent, w = bitangent z
layout(location = 4) in vec4 in_color;

layout(location = 0) out vec3 v_rel;   // camera-relative position
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec3 v_b;     // world-space frame, not normalised (interpolated like the game's)
layout(location = 3) out vec3 v_t;
layout(location = 4) out vec3 v_n;
layout(location = 5) out vec4 v_color;
layout(location = 6) out vec4 v_fog;   // rgb fog colour, a fog amount
layout(location = 7) out vec3 v_view_shape; // eye - position in the shape's own space (parallax)
layout(location = 8) flat out uint v_material;
layout(location = 9) flat out float v_scale; // the object's world scale
layout(location = 10) out float v_falloff;   // effect shader: view-angle falloff, per vertex like the game's
layout(location = 11) out float v_view_depth; // effect shader: view depth (metres) for the soft fade
layout(location = 12) flat out uvec4 v_lights;  // the object's point-light list
layout(location = 13) out vec2 v_uvp;           // projected UV: (world y, -world x) in game units (11.4)
layout(location = 14) flat out float v_fade;    // the reference's LOD fade metric (2.2)
layout(location = 15) out vec2 v_refraction;    // refraction: depth divisor k, model-space N.V (13.4)
layout(location = 16) out vec3 v_eye_normal;    // eyes: from the eyeball centre to the vertex (15.3), not normalised
layout(location = 17) flat out uint v_form;       // the reference's FormID (debug view "form")
layout(location = 18) flat out vec4 v_emittance;   // the reference's emittance colour, w = 1 when it has one
layout(location = 19) flat out uint v_draw;       // the draw instance: mesh.frag reads its instance and material

#include "mesh_transform.glsl"

void main() {
    uvec2 di = pc.draw_instances.v[gl_InstanceIndex];
    Instance inst = pc.instances.v[di.x];
    v_form = inst.form_id;
    v_draw = gl_InstanceIndex;
    v_emittance = (inst.flags & INSTANCE_EMITTANCE) != 0u
                      ? vec4(vec3((uvec3(inst.flags) >> uvec3(8u, 16u, 24u)) & 0xFFu) / 255.0, 1.0)
                      : vec4(0.0);
    Material m = load_material(di.y);
    // The pipeline's specialisation (common.glsl): features its materials never use compile out.
    m.technique &= k_spec_technique | 0xFF000000u;
    m.flags &= k_spec_flags;
    const uint type = m.technique >> 24;

    // world = instance * submesh. Rows hold [R * s | t].
    const MeshXform x = mesh_transform(m, inst, type, in_pos, in_normal, in_color);
    const mat3 sub = x.sub;
    const vec3 sub_t = x.sub_t;
    const mat3 ins = x.ins;
    const vec3 ins_t = x.ins_t;
    const vec3 shape_pos = x.shape_pos;
    const vec3 mesh_pos = x.mesh_pos;
    const vec3 rel = x.rel;

    vec3 n = in_normal.xyz * 2.0 - 1.0;
    vec3 t = in_tangent.xyz * 2.0 - 1.0;
    vec3 b = vec3(in_pos.w, in_normal.w * 2.0 - 1.0, in_tangent.w * 2.0 - 1.0);
    const bool skinned = m.extra.y != 0.0; // posed by skin.comp: the frame holds the skin's axes for model-space normals
    if ((m.technique & TECH_MSN) != 0u && !skinned) {
        // Model-space normal maps: pass the shape's axes instead of the vertex frame.
        v_b = (vec3(1, 0, 0) * sub) * ins;
        v_t = (vec3(0, 1, 0) * sub) * ins;
        v_n = (vec3(0, 0, 1) * sub) * ins;
    } else {
        v_n = (n * sub) * ins;
        v_t = (t * sub) * ins;
        v_b = (b * sub) * ins;
    }

    v_rel = rel;
    v_eye_normal = v_n;
    if (is_type(type, TYPE_EYE) && !skinned) {
        // The eye's centre: the game picks LeftEyeCenter or RightEyeCenter by each vertex's eye-data value, which
        // .osm doesn't keep; every vertex of the vanilla eye meshes lies nearer its own eye's centre (15.6).
        vec3 centre = distance(shape_pos, m.layer.xyz) <= distance(shape_pos, m.tint.xyz) ? m.layer.xyz : m.tint.xyz;
        vec3 centre_rel = (centre * sub + sub_t) * ins + ins_t;
        v_eye_normal = rel - centre_rel;
    }
    v_uvp = vec2(0.0);
    if ((m.technique & TECH_PROJECTED_UV) != 0u) {
        // TextureProj applied to the absolute world position in game units: the fixed top-down basis for the single
        // pass, the MATO's projection for the multi pass (11.4.1).
        vec3 world = (mesh_pos * ins + vec3(inst.r0.w, inst.r1.w, inst.r2.w)) * (1.0 / 0.0142875);
        v_uvp = vec2(dot(world, m.projection[0].xyz), dot(world, m.projection[1].xyz));
    }
    // Controllers replace UV values one frame late (16.5): load_material has applied them.
    vec4 uv_transform = m.uv; // offset u, v, scale u, v
    v_uv = in_uv * uv_transform.zw + uv_transform.xy;
    v_color = (m.technique & TECH_VC) != 0u ? in_color : vec4(1.0);
    v_material = di.y;
    v_lights = inst.lights;
    v_scale = length(inst.r0.xyz);

    // The view vector in the shape's own space: inverse(ins * sub) applied to (eye - position). Only its direction
    // is used, so transposes stand in for the inverses (rotations with uniform scale): M * w is M^T applied to w.
    v_view_shape = sub * (ins * (-rel));

    // Fog (5.2): amount from the length of Skyrim's clip-space position.
    vec4 clip = pc.frame.fog_view_proj * vec4(rel, 1.0);
    vec4 fp = pc.frame.fog_param;
    float f = min(pow(clamp(length(clip.xyz) * fp.y - fp.x, 0.0, 1.0), fp.z), fp.w);
    v_fog = vec4(mix(pc.frame.fog_near_color.rgb, pc.frame.fog_far_color.rgb, f), f);

    gl_Position = pc.frame.view_proj * vec4(rel, 1.0);
    gl_ClipDistance[0] = dot(pc.frame.clip_plane, vec4(rel, 1.0));
    v_view_depth = gl_Position.w;

    // The LOD fade metric of the reference's fade node: its bound centre's distance to the camera (2.2).
    v_fade = lod_fade_metric(inst.sphere.xyz, pc.frame.camera_pos.xyz, inst.flags, pc.frame.lod_fade,
                             pc.frame.lod_fade_types);
    v_refraction = vec2(1.0);
    if ((m.flags & MAT_REFRACTION) != 0u) {
        // Utility vs 0x61B (13.4.1): k from the game projection's clip z (near 15, far 353840 units), and the
        // model-space normal (not normalised) against the direction to the camera.
        float z_view = gl_Position.w / k_unit;
        float z_clip = 353840.0 / (353840.0 - 15.0) * (z_view - 15.0);
        float scale = length((vec3(1.0, 0.0, 0.0) * sub) * ins);
        v_refraction = vec2(max(1.0, 0.8 + z_clip * 0.0013333333), dot(v_n, normalize(-rel)) / scale);
    }

    v_falloff = 1.0;
    if ((m.flags & MAT_EFFECT) != 0u) {
        if ((m.effect_flags & EFFECT_MEMBRANE) != 0u) {
            v_uvp = in_uv; // the mesh's own: its normal map and the alpha of its diffuse
            if ((m.technique & EFX_PROJECTED) != 0u) {
                // Effect vs 0x28043: wrapped round the camera, and up the height of the fade node's bound. Only the
                // second axis of TexcoordOffsetMembrane is read.
                float low = inst.sphere.z - inst.sphere.w;
                v_uv = vec2(abs(atan(rel.x, rel.y)) * 0.31830987 * m.uv.w + m.uv.y,
                            (rel.z + pc.frame.camera_pos.z - low) / (2.0 * inst.sphere.w));
            }
        }
        if ((m.technique & EFX_FALLOFF) != 0u) {
            // |N.V| between the start and stop cosines, smoothstepped.
            vec3 N = normalize(v_n);
            vec3 V = normalize(-rel);
            float t = clamp((abs(dot(N, V)) - m.falloff.x) / (m.falloff.y - m.falloff.x), 0.0, 1.0);
            v_falloff = m.falloff.z + t * t * (3.0 - 2.0 * t) * (m.falloff.w - m.falloff.z);
        }
        if ((m.technique & EFX_FAR_PLANE) != 0u || (inst.flags & (1u << 5)) != 0u) {
            gl_Position.z = 0.0; // drawn at the far plane (reverse-Z): a weather's sky model too
        }
    }
}
