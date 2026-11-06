// The vertex position of Skyrim's lighting shaders, shared by the lit pass
// (mesh.vert) and the shadow maps (shadow.vert). Included after common.glsl.

const float k_unit = 0.0142875; // metres per game unit

// The engine's sine (12.4): the argument reduced to [-pi, pi] by rounding a / 2 pi to the nearest integer, then
// r + c3 r^3 + c5 r^5 + c7 r^7.
float engine_sin(float a) {
    float r = a - 6.2831855 * roundEven(a * 0.15915494);
    float r2 = r * r;
    return r + r * r2 * (-0.16652186 + r2 * (0.008199913 + r2 * -0.00016147594));
}

// mode 3: 0.25 (sin pi x + sin 3 pi x + sin 5 pi x + sin 7 pi x), summed (l3 + l2) + (l1 + l0).
float tree_wave(float x) {
    float l0 = 0.25 * engine_sin(3.1415927 * x);
    float l1 = 0.25 * engine_sin(9.424778 * x);
    float l2 = 0.25 * engine_sin(15.707964 * x);
    float l3 = 0.25 * engine_sin(21.99115 * x);
    return (l3 + l2) + (l1 + l0);
}

// The tree's random starting timer in [0, 2 pi): the game draws it from its global generator when the reference's
// 3D is created; here a hash of the reference's FormID, so the phase doesn't depend on load history.
float tree_phase(uint id) {
    uint h = id * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return float(h >> 8) * (6.2831855 / 16777216.0);
}

// Rotates v by the smallest rotation taking unit vector a onto unit vector n (axis a x n).
vec3 turn(vec3 v, vec3 a, vec3 n) {
    vec3 k = cross(a, n);
    float s = length(k), c = dot(a, n);
    if (s < 1e-12) {
        return v;
    }
    k /= s;
    return v * c + cross(k, v) * s + k * dot(k, v) * (1.0 - c);
}

// The game (16.1): the node's world rotation for the camera of this view. W is the node's
// un-billboarded world rotation, P its camera-relative position, s its world scale. Columns are the node's axes.
mat3 billboard_rotation(uint mode, mat3 W, vec3 P, float s) {
    vec3 Rt = pc.frame.camera_right.xyz;
    vec3 U = pc.frame.camera_up.xyz;
    vec3 D = cross(U, Rt); // view direction
    vec3 to_camera = -P;
    if (mode == 0u) {
        vec3 y = W[1] - dot(W[1], D) * D;
        if (length(y) <= 1e-6) {
            return mat3(-Rt, -U, -D);
        }
        vec3 Y = normalize(y), Z = -D;
        return mat3(cross(Y, Z), Y, Z);
    }
    if (mode == 1u) {
        vec3 p = (transpose(W) * to_camera) / s; // the camera in the node's own frame
        float l = length(p.xz);
        if (l < 1e-12) {
            return W;
        }
        return W * mat3(vec3(p.z, 0.0, -p.x) / l, vec3(0.0, 1.0, 0.0), vec3(p.x, 0.0, p.z) / l);
    }
    if (mode == 2u) {
        return mat3(Rt, U, -D);
    }
    if (mode == 3u || mode == 4u) {
        if (dot(to_camera, to_camera) < 0.001 * k_unit * k_unit) {
            return W;
        }
        vec3 n = normalize(to_camera), a = -D;
        vec3 u2 = U, r2 = Rt;
        if (dot(a, n) < 0.999999) {
            u2 = turn(U, a, n);
            r2 = turn(Rt, a, n);
        }
        if (mode == 4u) {
            return mat3(r2, u2, n);
        }
        vec3 y = W[1] - dot(W[1], n) * n;
        if (length(y) <= 1e-6) {
            return mat3(-r2, -u2, n);
        }
        vec3 Y = normalize(y);
        return mat3(cross(Y, n), Y, n);
    }
    if (mode == 5u) {
        vec3 p = to_camera / s;
        float l = length(p.xy);
        if (l < 1e-12) {
            return mat3(1.0);
        }
        return mat3(vec3(-p.y, p.x, 0.0) / l, vec3(p.x, p.y, 0.0) / l, vec3(0.0, 0.0, 1.0)); // determinant -1
    }
    return W;
}


// Where a vertex lands, camera-relative, with the transforms the lit pass keeps using.
struct MeshXform {
    mat3 sub;       // the submesh (shape) transform: v * sub = S * v
    vec3 sub_t;
    mat3 ins;       // the instance's rows
    vec3 ins_t;     // its translation, camera-relative
    vec3 shape_pos; // the vertex in shape space after the leaf animation
    vec3 mesh_pos;
    vec3 rel;
};

MeshXform mesh_transform(Material m, Instance inst, uint type, vec4 in_pos, vec4 in_normal, vec4 in_color) {
    mat3 sub = mat3(m.xform[0].xyz, m.xform[1].xyz, m.xform[2].xyz); // columns = rows: v * sub = S * v
    vec3 sub_t = vec3(m.xform[0].w, m.xform[1].w, m.xform[2].w);
    mat3 ins = mat3(inst.r0.xyz, inst.r1.xyz, inst.r2.xyz);
    vec3 ins_t = vec3(inst.r0.w, inst.r1.w, inst.r2.w) - pc.frame.camera_pos.xyz;
    if ((m.flags & MAT_BILLBOARD) != 0u) {
        // Under a billboard the submesh transform is below the node; the node's frame is re-oriented for this view
        // and expressed back in mesh space, so everything after this treats it like any other shape.
        mat3 B = transpose(mat3(m.billboard[0].xyz, m.billboard[1].xyz, m.billboard[2].xyz)); // mesh <- node
        vec3 bt = vec3(m.billboard[0].w, m.billboard[1].w, m.billboard[2].w);
        mat3 I = transpose(ins); // world <- mesh
        mat3 Wf = I * B;
        float s = length(Wf[0]);
        float s_ins = length(I[0]);
        mat3 M = billboard_rotation(m.billboard_mode & 7u, Wf / s, I * bt + ins_t, s);
        mat3 L = transpose(I / s_ins) * M * (s / s_ins); // mesh <- node, re-oriented
        sub_t = bt + L * sub_t;
        sub = transpose(L * transpose(sub));
    }

    vec3 shape_pos = in_pos.xyz;
    if (is_type(type, TYPE_TREE)) {
        // Leaves (12.5, 12.6): TreeParams and WindTimers as the game writes them; the vertex
        // moves along its model-space normal. Model-space values are in game units.
        vec4 tp = vec4(0.0, 3.0, 1.0, 1.0);
        float wx = 0.0;
        if ((inst.flags & INSTANCE_LEAF_ANIM) != 0u) {
            float t = tree_phase(inst.form_id) + pc.frame.camera_pos.w;
            float amplitude = tree_wave(pc.frame.tree.w * t) * inst.leaf_amplitude;
            float dist = length(vec3(inst.r0.w, inst.r1.w, inst.r2.w) - pc.frame.camera_pos.xyz);
            float f = 1.0 - (dist - pc.frame.tree.x) / (pc.frame.tree.y - pc.frame.tree.x);
            tp = vec4(0.0, pc.frame.tree.z, min(max(f * amplitude, 0.0), amplitude), inst.leaf_frequency);
            wx = 6.0 * t;
        }
        float k = tp.y * tp.w;
        vec3 p_units = in_pos.xyz / k_unit;
        vec2 ph = fract(k * wx * vec2(0.1, 0.25) + (p_units.x + p_units.y + p_units.z) + 0.5) * 2.0 - 1.0;
        vec2 w = ph * ph * (3.0 - 2.0 * abs(ph));
        float a = (m.technique & TECH_VC) != 0u ? in_color.a : 1.0;
        shape_pos += (in_normal.xyz * 2.0 - 1.0) * ((w.x + 0.1 * w.y) * a * tp.z * k_unit);
    }
    vec3 mesh_pos = shape_pos * sub + sub_t;
    MeshXform x;
    x.sub = sub;
    x.sub_t = sub_t;
    x.ins = ins;
    x.ins_t = ins_t;
    x.shape_pos = shape_pos;
    x.mesh_pos = mesh_pos;
    x.rel = mesh_pos * ins + ins_t;
    return x;
}
