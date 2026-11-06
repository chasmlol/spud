#version 460
// A tree impostor: one camera-facing quad; the three frames of the octahedral atlas nearest
// the view, each with the quad's corner seen in that frame. PushConstants::terrain_cells holds the placements and
// terrain_vertices the impostor table for this draw.
#include "common.glsl"
#include "impostor.glsl"
out float gl_ClipDistance[1];

layout(location = 0) out vec2 v_local[3];    // the corner in each frame, 0..1
layout(location = 3) flat out vec2 v_origin[3]; // each frame's corner in the atlas
layout(location = 6) flat out vec3 v_weights;
layout(location = 7) flat out vec4 v_rotation;
layout(location = 8) flat out uvec3 v_maps;  // colour, normal, frames
layout(location = 9) out vec3 v_rel;
layout(location = 10) out vec4 v_fog;

vec3 rotate(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

vec3 frame_direction(vec2 g, float last) {
    const vec2 p = g / last * 2.0 - 1.0;
    const float x = (p.x + p.y) * 0.5, y = (p.x - p.y) * 0.5;
    return normalize(vec3(x, y, 1.0 - abs(x) - abs(y)));
}

void frame_basis(vec3 d, out vec3 right, out vec3 up) {
    right = normalize(cross(abs(d.z) > 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0), d));
    up = cross(d, right);
}

void main() {
    const Tree t = TreeRef(pc.terrain_cells).t[gl_InstanceIndex];
    const Impostor imp = ImpostorRef(pc.terrain_vertices).i[t.impostor];
    FrameRef f = pc.frame;
    const vec3 centre = t.position + rotate(t.rotation, imp.centre * t.scale);
    const float radius = imp.radius * t.scale;
    const vec3 to_camera = normalize(f.camera_pos.xyz - centre);
    vec3 right, up;
    frame_basis(to_camera, right, up);
    // A first vertex of 6 marks a loaded cell's run: the real tree stands near, the two overlapping by a metre at the swap.
    if (gl_VertexIndex >= 6 && distance(f.camera_pos.xyz, centre) < f.lod_fade.w - 1.0) {
        gl_Position = vec4(0.0);
        gl_ClipDistance[0] = 0.0;
        return;
    }
    const uint v = uint(gl_VertexIndex) % 6u;
    vec2 corner = vec2((v == 1u || v == 2u || v == 4u) ? 1.0 : -1.0, (v == 2u || v == 4u || v == 5u) ? 1.0 : -1.0);

    // The view in the model's frame. A tree is narrow from the side: the quad only needs its full width from above.
    const vec4 inverse = vec4(-t.rotation.xyz, t.rotation.w);
    vec3 d = rotate(inverse, to_camera);
    d.z = max(d.z, 0.0);
    d = normalize(d);
    corner.x *= mix(imp.lateral, 1.0, d.z);
    const vec3 offset = right * corner.x + up * corner.y;
    const vec3 local = rotate(inverse, offset);
    const vec3 o = d / (abs(d.x) + abs(d.y) + abs(d.z));
    const float last = float(imp.frames - 1u);
    const vec2 grid = (vec2(o.x + o.y, o.x - o.y) * 0.5 + 0.5) * last;
    const vec2 cell = min(floor(grid), vec2(last - 1.0));
    const vec2 w = grid - cell;
    vec2 frames[3];
    if (w.x + w.y < 1.0) {
        frames = vec2[3](cell, cell + vec2(1.0, 0.0), cell + vec2(0.0, 1.0));
        v_weights = vec3(1.0 - w.x - w.y, w.x, w.y);
    } else {
        frames = vec2[3](cell + vec2(1.0), cell + vec2(1.0, 0.0), cell + vec2(0.0, 1.0));
        v_weights = vec3(w.x + w.y - 1.0, 1.0 - w.y, 1.0 - w.x);
    }
    // Too small for the frames' blend to show: the nearest alone costs a third of the sampling.
    const float pixels = radius * f.lod_fade.z * 0.5 / max(length(f.camera_pos.xyz - centre), 1.0);
    if (pixels < 24.0) {
        const int best = v_weights.x >= v_weights.y && v_weights.x >= v_weights.z ? 0 : v_weights.y >= v_weights.z ? 1 : 2;
        v_weights = vec3(best == 0 ? 1.0 : 0.0, best == 1 ? 1.0 : 0.0, best == 2 ? 1.0 : 0.0);
    }
    for (int k = 0; k < 3; ++k) {
        vec3 fr, fu;
        frame_basis(frame_direction(frames[k], last), fr, fu);
        v_local[k] = vec2(dot(local, fr), dot(local, fu)) * 0.5 + 0.5;
        v_origin[k] = frames[k];
    }
    v_rotation = t.rotation;
    v_maps = uvec3(imp.colour, imp.normal, imp.frames);

    const vec3 rel = centre + offset * radius - f.camera_pos.xyz;
    v_rel = rel;
    vec4 clip = f.fog_view_proj * vec4(rel, 1.0);
    vec4 fp = f.fog_param;
    float fog = min(pow(clamp(length(clip.xyz) * fp.y - fp.x, 0.0, 1.0), fp.z), fp.w);
    v_fog = vec4(mix(f.fog_near_color.rgb, f.fog_far_color.rgb, fog), fog);
    gl_Position = f.view_proj * vec4(rel, 1.0);
    gl_ClipDistance[0] = dot(f.clip_plane, vec4(rel, 1.0));
}
