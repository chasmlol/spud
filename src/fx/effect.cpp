#include "fx/effect.hpp"

#include "core/error.hpp"
#include "core/units.hpp"
#include "fx/random.hpp"
#include "fx/tables.hpp"

#include <glm/ext/quaternion_common.hpp>
#include <glm/ext/quaternion_exponential.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>

// Every behaviour here, which cites the the game functions it
// was read from. Arithmetic is kept in the game's order and in Skyrim units so that, given the same random
// numbers, the floats come out as the game's; output is converted to metres at the end.

namespace spud::fx {

namespace {

constexpr float k_neg_max = -3.402823466e+38f; // -FLT_MAX: "no value / never" throughout Gamebryo
constexpr float k_pos_max = 3.402823466e+38f;
constexpr uint32_t k_none = ose::k_none;
constexpr float k_unit_m = units::k_skyrim_unit_m;
// The NiBlendTransformInterpolators a NiMultiTargetTransformController makes for its targets; not a file type.
constexpr uint32_t k_blend_transform = 7;

// Controller flag bits (NiTimeController).
constexpr uint16_t k_app_init = 0x01;
constexpr uint16_t k_cycle_mask = 0x06;
constexpr uint16_t k_active = 0x08;
constexpr uint16_t k_reverse = 0x10;
constexpr uint16_t k_manager_controlled = 0x20;
constexpr uint16_t k_compute_scaled = 0x40;
constexpr uint16_t k_force_update = 0x80;

struct Xf {
    glm::mat3 rot{1.0f};
    glm::vec3 t{0.0f};
    float s = 1.0f;
};

Xf compose(const Xf& p, const Xf& l) {
    return {p.rot * l.rot, p.t + p.s * (p.rot * l.t), p.s * l.s};
}

Xf inverse(const Xf& x) {
    Xf r;
    r.rot = glm::transpose(x.rot);
    r.s = 1.0f / x.s;
    r.t = -(r.s * (r.rot * x.t));
    return r;
}

// The game's row-by-column form: (R p) * s + t.
glm::vec3 apply(const Xf& x, const glm::vec3& p) {
    return (x.rot * p) * x.s + x.t;
}

void unitize(glm::vec3& v) {
    float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len <= 0.000001f) {
        v = glm::vec3(0.0f);
    } else {
        float inv = 1.0f / len;
        v = glm::vec3(v.x * inv, v.y * inv, v.z * inv);
    }
}

float unitize_length(glm::vec3& v) {
    float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len <= 0.000001f) {
        v = glm::vec3(0.0f);
        return 0.0f;
    }
    float inv = 1.0f / len;
    v = glm::vec3(v.x * inv, v.y * inv, v.z * inv);
    return len;
}

uint64_t mix(uint64_t h, const void* data, size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

template <typename T> uint64_t mix_vec(uint64_t h, const std::vector<T>& v) {
    return v.empty() ? h : mix(h, v.data(), v.size() * sizeof(T));
}

struct ParticleInfo {
    glm::vec3 velocity{0.0f};
    float age = 0.0f;
    float life_span = 0.0f;
    float last_update = 0.0f;
    uint16_t generation = 0;
    uint16_t code = 0;
    int16_t subtex_counter = -1; // info
    uint16_t subtex_frames = 0;  // info
};

struct SystemState {
    const ose::System* def = nullptr;
    uint16_t max = 0;
    uint16_t active = 0;
    uint16_t added = 0;
    uint16_t added_base = 0;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec4> colors;
    std::vector<float> radii;
    std::vector<float> sizes;
    std::vector<glm::vec4> rotations;
    std::vector<float> angles;
    std::vector<glm::vec3> axes;
    std::vector<uint8_t> tex;
    std::vector<float> speeds;
    std::vector<ParticleInfo> info;
    float emit_scale = 1.0f;      // psys
    float prev_emit_scale = 1.0f; // psys
    float size_scale = 1.0f;      // psys
    float last_time = k_neg_max;  // psys
    bool reset = false;           // psys
    glm::vec3 bound_center{0.0f}; // psys
    float bound_radius = 0.0f;
    std::vector<uint32_t> mods;
    Xf world;
    std::vector<Particle> out;
};

struct ModifierState {
    const ose::Modifier* def = nullptr;
    uint32_t system = 0;
    bool active = true;
    float carry = 0.5f;        // emitter (constructor value)
    float radius_scale = 1.0f; // emitter (constructor value, not streamed)
    int16_t bound_groups = -1; // bound update
    uint16_t bound_current = 0;
    std::vector<glm::vec4> sub_bounds;
};

struct BlendItem {
    uint32_t interp = k_none;
    float weight = 0.0f;
    float normalized = 0.0f;
    int8_t priority = 0;
    float ease = 0.0f;
    float time = k_neg_max;
};

struct InterpState {
    const ose::Interpolator* def = nullptr;
    float last = k_neg_max;
    float fvalue = k_neg_max;
    uint8_t bvalue = 2;
    uint32_t key_index = 0;
    uint32_t prev_index = 0; // timeline
    uint8_t last_raw = 0;    // timeline
    // NiBlendInterpolator
    uint8_t flags = 0;
    std::vector<BlendItem> items;
    uint8_t count = 0;
    uint8_t single = 0x80;
    int8_t high = -128;
    int8_t next_high = -128;
    uint32_t single_interp = k_none;
    float single_time = k_neg_max;
    float high_sum = k_neg_max;
    float next_sum = k_neg_max;
    float high_ease = k_neg_max;
    // NiTransformInterpolator: the pose cache ( translation, rotation w x y z, scale) and the
    // key indices it resumes from ( translation, rotation, scale; the XYZ rotation axes keep
    // theirs in the NiEulerRotKey).
    glm::vec3 tvalue{k_neg_max};
    glm::vec4 qvalue{k_neg_max};
    float svalue = k_neg_max;
    uint32_t pos_index = 0;
    uint32_t rot_index = 0;
    uint32_t scale_index = 0;
    uint32_t axis_index[3] = {0, 0, 0};
};

// NiQuatTransform: translation, rotation (w, x, y, z), scale; -FLT_MAX components hold no value.
struct QuatTransform {
    glm::vec3 t{k_neg_max};
    glm::vec4 q{k_neg_max};
    float s = k_neg_max;
};

struct CtlrState {
    const ose::Controller* def = nullptr;
    uint16_t flags = 0;
    float lo = 0.0f;
    float hi = 0.0f;
    float start = k_neg_max;
    float last = k_neg_max;
    float acc = 0.0f;
    float scaled = k_neg_max;
    // NiPSysEmitterCtlr
    uint32_t last_rate_interp = k_none;
    float last_seq_time = k_neg_max;
    float carry = 0.0f;
    uint8_t last_active = 0;
    std::vector<float> ages;
};

enum class SeqState : uint32_t {
    inactive = 0,
    animating = 1,
    ease_in = 2,
    ease_out = 3,
    trans_source = 4,
    trans_dest = 5,
};

struct SeqBlock {
    uint32_t blend = k_none; // the controller's blend interpolator
    uint32_t interp = k_none;
    uint8_t blend_index = 0;
    uint8_t priority = 0;
};

struct SeqStateData {
    const ose::Sequence* def = nullptr;
    SeqState state = SeqState::inactive;
    float weight = 1.0f;
    float offset = k_neg_max;
    float start = k_neg_max;
    float end = k_neg_max;
    float last = k_neg_max;
    float acc = 0.0f;
    float last_scaled = 0.0f;
    float posed = k_neg_max;  // SetTimePosition's key time, held
    std::vector<SeqBlock> blocks;
};

} // namespace

struct EffectInstance::State {
    std::shared_ptr<const Effect> effect;
    const ose::Effect& e;
    CrtRandom crt;
    BsRandom bs;
    std::vector<Xf> local;
    std::vector<Xf> world;
    Xf root_override;
    bool has_root_override = false;
    std::vector<SystemState> systems;
    std::vector<ModifierState> modifiers;
    std::vector<InterpState> interps;
    std::vector<CtlrState> ctlrs;
    std::vector<SeqStateData> seqs;
    std::vector<uint32_t> active_seqs; // manager, in activation order
    std::vector<MaterialState> materials;
    // NiMultiTargetTransformController: one NiBlendTransformInterpolator per target, made at load time (they are
    // not in the file); ctlr_blends[c][k] is the interpolator index of controller c's k-th target.
    std::deque<ose::Interpolator> runtime_interps;
    std::vector<std::vector<uint32_t>> ctlr_blends;
    float lod_distance = 0.0f;
    float density = 1.0f; // g_ParticleDensityScale

    explicit State(std::shared_ptr<const Effect> eff) : effect(std::move(eff)), e(effect->data) {}

    // ---- Transforms.

    void compute_world() {
        world.assign(e.nodes.size(), Xf{});
        std::vector<uint8_t> done(e.nodes.size(), 0);
        for (size_t i = 0; i < e.nodes.size(); ++i) {
            resolve_world(static_cast<uint32_t>(i), done, 0);
        }
        // a world-space system keeps only the scale.
        for (auto& s : systems) {
            const auto& n = e.nodes[s.def->node];
            if (s.def->flags & ose::system_world_space) {
                Xf w;
                float parent_scale = n.parent != k_none ? world[n.parent].s : 1.0f;
                w.s = parent_scale * local[s.def->node].s;
                s.world = w;
            } else {
                s.world = world[s.def->node];
            }
        }
    }

    void resolve_world(uint32_t i, std::vector<uint8_t>& done, int depth) {
        if (done[i] || depth > 256) {
            return;
        }
        const auto& n = e.nodes[i];
        Xf l = (i == e.root && has_root_override) ? root_override : local[i];
        if (n.parent != k_none) {
            resolve_world(n.parent, done, depth + 1);
            world[i] = compose(world[n.parent], l);
        } else {
            world[i] = l;
        }
        done[i] = 1;
    }

    // M = inverse(system world) * object world (the game and the field
    // modifiers).
    Xf to_system(const SystemState& s, uint32_t node) const {
        Xf obj = node != k_none ? world[node] : Xf{};
        return compose(inverse(s.world), obj);
    }

    // ---- NiPSysData.

    uint16_t add_particle(SystemState& s) {
        if (s.added) {
            uint32_t base = s.added_base;
            if (base + s.added < s.max) {
                return static_cast<uint16_t>(base + s.added++);
            }
            return 0xFFFF;
        }
        s.added_base = s.active;
        if (s.added_base >= s.max) {
            return 0xFFFF;
        }
        s.added = 1;
        return s.added_base;
    }

    void remove_particle(SystemState& s, uint16_t i) {
        if (s.active == 0) {
            return;
        }
        uint16_t last = static_cast<uint16_t>(s.active - 1);
        if (i != last) {
            s.positions[i] = s.positions[last];
            if (!s.colors.empty()) {
                s.colors[i] = s.colors[last];
            }
            s.radii[i] = s.radii[last];
            s.sizes[i] = s.sizes[last];
            if (!s.rotations.empty()) {
                s.rotations[i] = s.rotations[last];
            }
            if (!s.angles.empty()) {
                s.angles[i] = s.angles[last];
            }
            if (!s.axes.empty()) {
                s.axes[i] = s.axes[last];
            }
            if (!s.tex.empty()) {
                s.tex[i] = s.tex[last];
            }
            s.info[i] = s.info[last];
            if (!s.speeds.empty()) {
                s.speeds[i] = s.speeds[last];
            }
        }
        s.positions[last] = glm::vec3(0.0f);
        --s.active;
    }

    void resolve_added(SystemState& s) {
        uint32_t v1 = s.active;
        uint32_t base = s.added_base;
        if (v1 >= base) {
            v1 += s.added;
            s.active = static_cast<uint16_t>(v1);
        } else {
            s.active = static_cast<uint16_t>(base + s.added);
            uint32_t limit = base;
            while (s.active > base) {
                remove_particle(s, static_cast<uint16_t>(v1));
                limit = s.added_base;
                ++v1;
                if (v1 >= limit) {
                    break;
                }
            }
            if (v1 >= limit) {
                v1 = s.active;
            } else {
                s.active = static_cast<uint16_t>(v1);
            }
        }
        s.added_base = static_cast<uint16_t>(v1);
        s.added = 0;
    }

    // ---- Keys and interpolators.

    float eval_float_keys(const ose::KeyGroup& g, float t, uint32_t& index) const {
        const auto* keys = e.float_keys.data() + g.first;
        uint32_t n = g.count;
        if (n == 1 || t == k_neg_max) {
            return keys[0].value;
        }
        uint32_t last = n - 1;
        uint32_t i = keys[std::min(index, last)].time <= t ? std::min(index, last) : 0;
        uint32_t j = i + 1;
        while (j <= last && t > keys[j].time) {
            ++i;
            ++j;
        }
        if (j >= n) {
            index = i;
            return keys[i].value;
        }
        const auto& a = keys[i];
        const auto& b = keys[j];
        float u = (t - a.time) / (b.time - a.time);
        float v;
        if (g.key_type == 2) {
            // > the game(u, p0, out tangent of a, p1, in tangent of b).
            float p0 = a.value, m0 = a.backward, p1 = b.value, m1 = b.forward;
            float d = p1 - p0;
            v = (((((m0 + m1) - (d + d)) * u) + ((d * 3.0f) - ((m0 + m0) + m1))) * u + m0) * u + p0;
        } else if (g.key_type == 5) {
            v = u >= 1.0f ? b.value : a.value;
        } else {
            // Linear (also used for TBC, see the design): (1 - u) * a + u * b.
            v = (1.0f - u) * a.value + u * b.value;
        }
        index = i;
        return v;
    }

    // The game: the same search as the float keys; u = 1 when the two keys share a time.
    // The game: the segment that holds t, then by key type: step, slerp along the short way, or
    // for quadratic and TBC keys squad through each key's intermediate. Results stay in (w, x, y, z) order.
    glm::vec4 eval_quat_keys(const ose::TransformInterp& ti, float t) const {
        const auto* keys = e.quat_keys.data() + ti.rotation_first;
        const uint32_t n = ti.rotation_count;
        auto value = [&](uint32_t i) {
            const float* v = keys[i].value;
            return glm::quat(v[0], v[1], v[2], v[3]);
        };
        auto out = [](const glm::quat& q) { return glm::vec4(q.w, q.x, q.y, q.z); };
        if (n == 1 || t == k_neg_max || t <= keys[0].time) {
            return out(value(0));
        }
        if (t >= keys[n - 1].time) {
            return out(value(n - 1));
        }
        uint32_t i = 0;
        while (i + 2 < n && t >= keys[i + 1].time) {
            ++i;
        }
        const float span = keys[i + 1].time - keys[i].time;
        const float u = span > 0.0f ? (t - keys[i].time) / span : 0.0f;
        const glm::quat q0 = value(i);
        glm::quat q1 = value(i + 1);
        if (glm::dot(q0, q1) < 0.0f) {
            q1 = -q1;
        }
        if (ti.rotation_type == 5) {
            return out(q0);
        }
        if (ti.rotation_type == 1 || n == 2) {
            return out(glm::normalize(glm::slerp(q0, q1, u)));
        }
        // at the ends the key itself.
        auto middle = [&](uint32_t k) {
            const glm::quat q = value(k);
            if (k == 0 || k + 1 == n) {
                return q;
            }
            glm::quat before = value(k - 1), after = value(k + 1);
            before = glm::dot(q, before) < 0.0f ? -before : before;
            after = glm::dot(q, after) < 0.0f ? -after : after;
            const glm::quat inverse = glm::conjugate(q);
            return glm::exp((glm::log(after * inverse) + glm::log(before * inverse)) * -0.25f) * q;
        };
        const glm::quat outer = glm::slerp(q0, q1, u), inner = glm::slerp(middle(i), middle(i + 1), u);
        return out(glm::normalize(glm::slerp(outer, inner, 2.0f * u * (1.0f - u))));
    }

    glm::vec3 eval_vec3_keys(const ose::TransformInterp& ti, float t, uint32_t& index) const {
        const auto* keys = e.vec3_keys.data() + ti.translation_first;
        const uint32_t n = ti.translation_count;
        auto value = [](const ose::Vec3Key& k) { return glm::vec3(k.value[0], k.value[1], k.value[2]); };
        if (n == 1 || t == k_neg_max) {
            return value(keys[0]);
        }
        uint32_t i = keys[std::min(index, n - 1)].time <= t ? std::min(index, n - 1) : 0;
        uint32_t j = i + 1;
        while (j < n && t > keys[j].time) {
            ++i;
            ++j;
        }
        const uint32_t jj = j < n ? j : n - 1;
        const auto& a = keys[i];
        const auto& b = keys[jj];
        const float u = a.time == b.time ? 1.0f : (t - a.time) / (b.time - a.time);
        index = i;
        // The game: u b + (1 - u) a per component. Other key types are reported by the
        // importer and evaluated the same way.
        const float w = 1.0f - u;
        return {u * b.value[0] + w * a.value[0], u * b.value[1] + w * a.value[1], u * b.value[2] + w * a.value[2]};
    }

    // The game.
    bool update_transform(uint32_t i, float t, QuatTransform& out) {
        auto& st = interps[i];
        if (static_cast<uint32_t>(st.def->type) == k_blend_transform) {
            // NiBlendTransformInterpolator, manager controlled: with one item, its interpolator at the item's time.
            if (st.count == 1 && st.single_interp != k_none && st.single_time != k_neg_max) {
                return update_transform(st.single_interp, st.single_time, out);
            }
            if (st.count > 1) {
                // NiBlendTransformInterpolator: each channel the weighted mean of the sequences that give one, the
                // rotations on one side of the sphere (FireSprayFX01's movers, mSprayLong against mSprayShort).
                compute_normalized_weights(st);
                glm::vec3 t(0.0f);
                glm::vec4 q(0.0f);
                float scale = 0.0f, tw = 0.0f, qw = 0.0f, sw = 0.0f;
                glm::vec4 first(0.0f);
                for (size_t k = 0; k < st.items.size(); ++k) {
                    const BlendItem item = st.items[k];
                    QuatTransform x;
                    if (item.interp == k_none || !(item.normalized > 0.0f) || item.time == k_neg_max ||
                        !update_transform(item.interp, item.time, x)) {
                        continue;
                    }
                    if (x.t.x != k_neg_max) {
                        t += glm::vec3(x.t.x, x.t.y, x.t.z) * item.normalized;
                        tw += item.normalized;
                    }
                    if (x.q.y != k_neg_max) {
                        glm::vec4 v(x.q.x, x.q.y, x.q.z, x.q.w);
                        if (qw == 0.0f) {
                            first = v;
                        } else if (glm::dot(first, v) < 0.0f) {
                            v = -v;
                        }
                        q += v * item.normalized;
                        qw += item.normalized;
                    }
                    if (x.s != k_neg_max) {
                        scale += x.s * item.normalized;
                        sw += item.normalized;
                    }
                }
                if (tw <= 0.0f && qw <= 0.0f && sw <= 0.0f) {
                    return false;
                }
                if (tw > 0.0f) {
                    t /= tw;
                    out.t = {t.x, t.y, t.z};
                }
                if (qw > 0.0f) {
                    q = glm::normalize(q);
                    out.q.x = q.x;
                    out.q.y = q.y;
                    out.q.z = q.z;
                    out.q.w = q.w;
                }
                if (sw > 0.0f) {
                    out.s = scale / sw;
                }
                return true;
            }
            return false;
        }
        if (st.def->type != ose::InterpolatorType::transform) {
            return false;
        }
        auto valid = [&] { return st.svalue != k_neg_max || st.qvalue.y != k_neg_max || st.tvalue.x != k_neg_max; };
        if (t != st.last) {
            const auto& ti = e.transforms[st.def->key_group];
            if (ti.translation_count) {
                st.tvalue = eval_vec3_keys(ti, t, st.pos_index);
            }
            if (ti.rotation_type == 4) {
                // NiEulerRotKey: each axis's float keys at t, then the quaternion of the half angles,
                // q = qz qy qx (rotation about X first, then Y, then Z).
                float a[3];
                for (int k = 0; k < 3; ++k) {
                    const auto& g = e.key_groups[ti.rotation_first + uint32_t(k)];
                    a[k] = g.count ? eval_float_keys(g, t, st.axis_index[k]) : 0.0f;
                }
                const float sx = std::sin(a[0] * 0.5f), cx = std::cos(a[0] * 0.5f);
                const float sy = std::sin(a[1] * 0.5f), cy = std::cos(a[1] * 0.5f);
                const float sz = std::sin(a[2] * 0.5f), cz = std::cos(a[2] * 0.5f);
                st.qvalue.x = cz * cy * cx + sz * sy * sx;
                st.qvalue.y = cz * cy * sx - sz * sy * cx;
                st.qvalue.w = sz * cy * cx - cz * sy * sx;
                st.qvalue.z = cz * sy * cx + sz * cy * sx;
            } else if (ti.rotation_count) {
                st.qvalue = eval_quat_keys(ti, t);
            }
            if (ti.scale_group != k_none && e.key_groups[ti.scale_group].count) {
                st.svalue = eval_float_keys(e.key_groups[ti.scale_group], t, st.scale_index);
            }
            if (!valid()) {
                return false;
            }
            st.last = t;
        } else if (!valid()) {
            return false;
        }
        out.t = st.tvalue;
        out.q = st.qvalue;
        out.s = st.svalue;
        return true;
    }

    // The game: when active, each target with flag 0x2 takes the pose
    // of its blend interpolator: translation, the rotation matrix of the quaternion, |scale|.
    bool update_transform_ctlr(size_t index) {
        const auto& c = ctlrs[index];
        if (!(c.flags & k_active)) {
            return false;
        }
        bool changed = false;
        for (uint32_t k = 0; k < c.def->target_count; ++k) {
            const uint32_t node = e.controller_targets[c.def->first_target + k];
            if (node == k_none || !(e.nodes[node].flags & 0x2)) {
                continue;
            }
            QuatTransform x;
            if (!update_transform(ctlr_blends[index][k], k_neg_max, x)) {
                continue;
            }
            auto& l = local[node];
            if (x.t.x != k_neg_max) {
                l.t = x.t;
                changed = true;
            }
            if (x.q.y != k_neg_max) {
                const float w = x.q.x, qx = x.q.y, qy = x.q.z, qz = x.q.w;
                const float x2 = qx * (qx + qx), y2 = qy * (qy + qy), z2 = qz * (qz + qz);
                const float wx = w * (qx + qx), wy = w * (qy + qy), wz = w * (qz + qz);
                const float xy = qx * (qy + qy), xz = qx * (qz + qz), yz = qy * (qz + qz);
                // Row-major entries as the game writes them, stored column-major here.
                const float m[3][3] = {{1.0f - (z2 + y2), xy - wz, xz + wy},
                                       {xy + wz, 1.0f - (z2 + x2), yz - wx},
                                       {xz - wy, yz + wx, 1.0f - (y2 + x2)}};
                for (int r = 0; r < 3; ++r) {
                    for (int col = 0; col < 3; ++col) {
                        l.rot[col][r] = m[r][col];
                    }
                }
                changed = true;
            }
            if (x.s != k_neg_max) {
                l.s = std::fabs(x.s);
                changed = true;
            }
        }
        return changed;
    }

    uint8_t eval_bool_keys(const ose::KeyGroup& g, float t, uint32_t& index) const {
        const auto* keys = e.bool_keys.data() + g.first;
        uint32_t n = g.count;
        if (n == 1 || t == k_neg_max) {
            return keys[0].value;
        }
        uint32_t start = keys[std::min(index, n - 1)].time <= t ? std::min(index, n - 1) : 0;
        uint32_t i = start;
        uint32_t j = i + 1;
        float ti = keys[i].time;
        while (j < n && t > keys[j].time) {
            ti = keys[j].time;
            ++i;
            ++j;
        }
        uint32_t jj = j < n ? j : n - 1;
        float tj = keys[jj].time;
        float u = tj == ti ? 1.0f : (t - ti) / (tj - ti);
        uint8_t v;
        if (g.key_type == 5 || g.key_type == 1 || g.key_type == 2 || g.key_type == 3) {
            // The game; the other bool key types share the step behaviour in practice
            // (see the design).
            v = u >= 1.0f ? keys[jj].value : keys[i].value;
        } else {
            v = keys[i].value;
        }
        index = i;
        return v;
    }

    bool interp_is_constant(uint32_t i) const {
        const auto& s = interps[i];
        switch (s.def->type) {
        case ose::InterpolatorType::float_value:
            return s.def->key_group == k_none && s.fvalue != k_neg_max;
        case ose::InterpolatorType::bool_value:
        case ose::InterpolatorType::bool_timeline:
            return s.def->key_group == k_none && s.bvalue != 2;
        default:
            return false;
        }
    }

    bool update_float(uint32_t i, float t, float& out) {
        auto& s = interps[i];
        using T = ose::InterpolatorType;
        if (s.def->type == T::float_value) {
            if (t == s.last) {
                out = s.fvalue;
                return s.fvalue != k_neg_max;
            }
            if (s.def->key_group != k_none && e.key_groups[s.def->key_group].count) {
                s.fvalue = eval_float_keys(e.key_groups[s.def->key_group], t, s.key_index);
            }
            if (s.fvalue == k_neg_max) {
                return false;
            }
            out = s.fvalue;
            s.last = t;
            return true;
        }
        if (s.def->type != T::blend_float) {
            return false;
        }
        bool result = false;
        float time = t;
        if (s.count == 1) {
            if (s.flags & 1) {
                time = s.single_time;
            }
            if (time != k_neg_max) {
                float v;
                if (s.single_interp != k_none && update_float(s.single_interp, time, v)) {
                    s.fvalue = v;
                    out = v;
                    result = true;
                } else {
                    s.fvalue = k_neg_max;
                    out = k_neg_max;
                }
            }
        } else if (s.count) {
            compute_normalized_weights(s);
            float total = 1.0f;
            bool any = false;
            s.fvalue = 0.0f;
            for (auto& item : s.items) {
                if (item.interp == k_none || !(item.normalized > 0.0f)) {
                    continue;
                }
                float it = (s.flags & 1) ? item.time : t;
                float v;
                if (it == k_neg_max || !update_float(item.interp, it, v)) {
                    total -= item.normalized;
                } else {
                    any = true;
                    s.fvalue = item.normalized * v + s.fvalue;
                }
            }
            s.fvalue = s.fvalue / total;
            if (any) {
                out = s.fvalue;
                result = true;
            } else {
                out = k_neg_max;
                s.fvalue = k_neg_max;
            }
        }
        s.last = t;
        return result;
    }

    bool update_bool(uint32_t i, float t, uint8_t& out) {
        auto& s = interps[i];
        using T = ose::InterpolatorType;
        if (s.def->type == T::bool_value || s.def->type == T::bool_timeline) {
            if (t == s.last) {
                if (s.bvalue != 2) {
                    out = s.bvalue != 0;
                    return true;
                }
                out = 0;
                return false;
            }
            uint8_t v = s.bvalue;
            if (s.def->key_group != k_none && e.key_groups[s.def->key_group].count) {
                const auto& g = e.key_groups[s.def->key_group];
                if (s.def->type == T::bool_value) {
                    v = eval_bool_keys(g, t, s.key_index) != 0;
                } else {
                    v = timeline_value(s, g, t);
                }
                s.bvalue = v;
            }
            if (v == 2) {
                out = 0;
                return false;
            }
            out = v != 0;
            s.last = t;
            return true;
        }
        if (s.def->type != T::blend_bool) {
            return false;
        }
        bool result = false;
        float time = t;
        if (s.count == 1) {
            if (s.flags & 1) {
                time = s.single_time;
            }
            if (time != k_neg_max) {
                uint8_t v = 0;
                if (s.single_interp != k_none && update_bool(s.single_interp, time, v)) {
                    s.bvalue = v;
                    out = v;
                    result = true;
                } else {
                    out = 0;
                    s.bvalue = 2;
                }
            }
        } else if (s.count) {
            compute_normalized_weights(s);
            float total = 1.0f;
            float acc = 0.0f;
            bool any = false;
            for (auto& item : s.items) {
                if (item.interp == k_none || !(item.normalized > 0.0f)) {
                    continue;
                }
                float it = (s.flags & 1) ? item.time : t;
                if (it == k_neg_max) {
                    total -= item.normalized;
                    continue;
                }
                uint8_t v = 0;
                if (update_bool(item.interp, it, v)) {
                    any = true;
                    acc = acc + static_cast<float>(v) * item.normalized;
                } else {
                    total -= item.normalized;
                }
            }
            if (any) {
                result = true;
                s.bvalue = (acc / total) >= 0.5f ? 1 : 0;
                out = s.bvalue;
            } else {
                out = 0;
                s.bvalue = 2;
            }
        }
        s.last = t;
        return result;
    }

    // a toggle that happened between the previous and the current
    // key index is reported even if the value is back where it was.
    uint8_t timeline_value(InterpState& s, const ose::KeyGroup& g, float t) {
        const auto* keys = e.bool_keys.data() + g.first;
        uint32_t n = g.count;
        uint32_t prev = s.key_index;
        s.prev_index = prev;
        uint8_t raw = eval_bool_keys(g, t, s.key_index);
        uint32_t cur = s.key_index;
        uint8_t result = raw;
        if (raw == s.last_raw) {
            uint8_t opposite = raw == 0 ? 1 : 0;
            uint32_t k = prev + 1;
            bool found = false;
            if (prev != cur && k > cur) {
                // The index wrapped: the keys after the previous index, then those before the current one.
                for (; k < n && !found; ++k) {
                    found = keys[k].value == opposite;
                }
                for (uint32_t j = 0; j < cur && !found; ++j) {
                    found = keys[j].value == opposite;
                }
            } else if (k < cur) {
                for (; k < n && k < cur && !found; ++k) {
                    found = keys[k].value == opposite;
                }
            }
            if (found) {
                result = opposite;
            }
        }
        s.last_raw = raw;
        return result;
    }

    void compute_normalized_weights(InterpState& s) {
        if (!(s.flags & 4)) {
            return;
        }
        s.flags = static_cast<uint8_t>(s.flags & ~4);
        if (s.count == 1) {
            if (s.single < s.items.size()) {
                s.items[s.single].normalized = 1.0f;
            }
            return;
        }
        if (s.count == 2) {
            normalized_weights_for_2(s);
            return;
        }
        if (s.high_sum == k_neg_max) {
            s.high_sum = 0.0f;
            s.next_sum = 0.0f;
            s.high_ease = 0.0f;
            for (const auto& item : s.items) {
                if (item.interp == k_none) {
                    continue;
                }
                float w = item.weight * item.ease;
                if (item.priority == s.high) {
                    s.high_sum = w + s.high_sum;
                    if (item.ease > s.high_ease) {
                        s.high_ease = item.ease;
                    }
                } else if (item.priority == s.next_high) {
                    s.next_sum = w + s.next_sum;
                }
            }
        }
        float ease = s.high_ease;
        float total = (1.0f - ease) * s.next_sum + ease * s.high_sum;
        float inv = total <= 0.0f ? 0.0f : 1.0f / total;
        for (auto& item : s.items) {
            if (item.interp == k_none) {
                continue;
            }
            if (item.priority == s.high) {
                item.normalized = item.weight * s.high_ease * item.ease * inv;
            } else if (item.priority == s.next_high) {
                item.normalized = (1.0f - ease) * item.weight * item.ease * inv;
            } else {
                item.normalized = 0.0f;
            }
        }
        float sum = 1.0f;
        if (s.def->weight_threshold > 0.0f) {
            sum = 0.0f;
            for (auto& item : s.items) {
                if (item.interp == k_none || item.normalized == 0.0f) {
                    continue;
                }
                if (item.normalized < s.def->weight_threshold) {
                    item.normalized = 0.0f;
                }
                sum = sum + item.normalized;
            }
        }
        if (sum != 1.0f) {
            float inv_sum = sum <= 0.0f ? 0.0f : 1.0f / sum;
            for (auto& item : s.items) {
                if (item.normalized != 0.0f) {
                    item.normalized = item.normalized * inv_sum;
                }
            }
        }
        if (s.flags & 2) {
            float best = -1.0f;
            int best_index = -1;
            for (size_t k = 0; k < s.items.size(); ++k) {
                float w = s.items[k].normalized;
                s.items[k].normalized = 0.0f;
                if (w > best) {
                    best = w;
                    best_index = static_cast<int>(k);
                }
            }
            if (best_index >= 0) {
                s.items[static_cast<size_t>(best_index)].normalized = 1.0f;
            }
        }
    }

    void normalized_weights_for_2(InterpState& s) {
        BlendItem* a = nullptr;
        BlendItem* b = nullptr;
        for (auto& item : s.items) {
            if (item.interp != k_none) {
                if (a) {
                    b = &item;
                    break;
                }
                a = &item;
            }
        }
        if (!a || !b) {
            return;
        }
        float ea = a->ease;
        float eb = b->ease;
        float wa = ea * a->weight;
        float wb = eb * b->weight;
        if (wa == 0.0f && wb == 0.0f) {
            a->normalized = 0.0f;
            b->normalized = 0.0f;
            return;
        }
        float nb;
        if (a->priority <= b->priority) {
            if (a->priority >= b->priority) {
                float inv = 1.0f / (wb + wa);
                nb = inv * wb;
                a->normalized = inv * wa;
            } else {
                if (eb == 1.0f) {
                    a->normalized = 0.0f;
                    b->normalized = 1.0f;
                    return;
                }
                float x = (1.0f - eb) * wa;
                float inv = 1.0f / ((eb * wb) + x);
                a->normalized = x * inv;
                nb = (wb * b->ease) * inv;
            }
        } else {
            if (ea == 1.0f) {
                a->normalized = 1.0f;
                b->normalized = 0.0f;
                return;
            }
            float x = (1.0f - ea) * wb;
            float inv = 1.0f / (x + (ea * wa));
            nb = x * inv;
            a->normalized = (ea * wa) * inv;
        }
        b->normalized = nb;
        if (s.flags & 2) {
            if (nb > a->normalized) {
                a->normalized = 0.0f;
                b->normalized = 1.0f;
            } else {
                a->normalized = 1.0f;
                b->normalized = 0.0f;
            }
            return;
        }
        float threshold = s.def->weight_threshold;
        if (threshold > 0.0f) {
            bool dropped = false;
            if (threshold > a->normalized) {
                a->normalized = 0.0f;
                dropped = true;
            }
            if (threshold > b->normalized || dropped) {
                b->normalized = 1.0f;
            }
        }
    }

    int highest_weighted(const InterpState& s) const {
        if (s.count == 1) {
            return s.single;
        }
        int result = -1;
        float best = 0.0f;
        for (size_t k = 0; k < s.items.size(); ++k) {
            if (s.items[k].interp != k_none && s.items[k].normalized > best) {
                result = static_cast<int>(k);
                best = s.items[k].normalized;
            }
        }
        return result;
    }

    uint8_t add_interp_info(InterpState& s, uint32_t interp, float weight, int8_t priority, float ease) {
        size_t i = 0;
        while (i < s.items.size() && s.items[i].interp != k_none) {
            ++i;
        }
        if (i == s.items.size()) {
            s.items.resize(s.items.size() + 1); // the game grows the array by one (slot 60)
        }
        auto& item = s.items[i];
        item.interp = interp;
        item.weight = weight;
        item.normalized = 0.0f;
        item.priority = priority;
        item.ease = ease;
        item.time = k_neg_max;
        if (priority > s.next_high) {
            if (priority > s.high) {
                s.next_high = s.high;
                s.high = priority;
            } else if (priority != s.high) {
                s.next_high = priority;
            }
        }
        if (++s.count == 1) {
            s.single = static_cast<uint8_t>(i);
            s.single_interp = interp;
        } else {
            s.single_time = k_neg_max;
            s.single = 0xFF;
            s.single_interp = k_none;
        }
        s.flags |= 4;
        s.high_sum = s.next_sum = s.high_ease = k_neg_max;
        return static_cast<uint8_t>(i);
    }

    void remove_interp_info(InterpState& s, uint8_t index) {
        if (index >= s.items.size()) {
            return;
        }
        auto& item = s.items[index];
        int8_t priority = item.priority;
        item.interp = k_none;
        if (priority == s.high || priority == s.next_high) {
            s.high = -128;
            s.next_high = -128;
            for (const auto& it : s.items) {
                if (it.interp == k_none) {
                    continue;
                }
                if (it.priority > s.next_high) {
                    if (it.priority > s.high) {
                        s.next_high = s.high;
                        s.high = it.priority;
                    } else if (it.priority < s.high) {
                        s.next_high = it.priority;
                    }
                }
            }
        }
        --s.count;
        if (s.count == 1) {
            if (s.items.size() == 2) {
                s.single = index == 0 ? 1 : 0;
            } else {
                for (size_t k = 0; k < s.items.size(); ++k) {
                    if (s.items[k].interp != k_none) {
                        s.single = static_cast<uint8_t>(k);
                        break;
                    }
                }
            }
            s.single_interp = s.items[s.single].interp;
            s.single_time = s.items[s.single].time;
        } else if (s.count == 0) {
            s.high = -128;
            s.single = 0xFF;
            s.next_high = -128;
            s.single_interp = k_none;
            s.single_time = k_neg_max;
        }
        s.flags |= 4;
        s.high_sum = s.next_sum = s.high_ease = k_neg_max;
    }

    // ---- Controller timing.

    float compute_scaled_time(CtlrState& c, float t) const {
        if (c.start == k_neg_max) {
            c.start = t;
        }
        float delta;
        float acc;
        if (c.last == k_neg_max) {
            delta = (c.flags & k_app_init) ? 0.0f : t;
            acc = 0.0f;
        } else {
            delta = t - c.last;
            acc = c.acc;
        }
        delta = delta * c.def->frequency;
        c.last = t;
        acc = delta + acc;
        c.acc = acc;
        float s = acc + c.def->phase;
        float lo = c.lo;
        float hi = c.hi;
        uint32_t cycle = (c.flags >> 1) & 3;
        if (hi != k_neg_max && lo != k_pos_max) {
            if (cycle == 0) {
                float span = hi - lo;
                if (span != 0.0f) {
                    float r = std::fmod(s - lo, span);
                    s = r + lo;
                    if (r + lo < lo) {
                        s = s + span;
                    }
                } else {
                    s = lo;
                }
            } else if (cycle == 1) {
                float span = hi - lo;
                if (span != 0.0f) {
                    float d = span + span;
                    float r = std::fmod(s, d);
                    if (r < 0.0f) {
                        r = r + d;
                    }
                    s = r <= span ? lo + r : lo + (d - r);
                } else {
                    s = lo;
                }
            }
        }
        float out = s <= hi ? std::max(lo, s) : hi;
        if (c.flags & k_reverse) {
            out = hi - (out - lo);
        }
        return out;
    }

    // The "should this controller update" prologue shared by the game and
    // Returns false if nothing is to be done.
    bool interp_ctlr_prologue(CtlrState& c, float t) {
        if (c.flags & k_manager_controlled) {
            c.scaled = k_neg_max;
            return true;
        }
        if (c.flags & k_active) {
            if (t != c.last || (c.flags & k_force_update)) {
                if (c.flags & k_compute_scaled) {
                    float s = compute_scaled_time(c, t);
                    if (s != c.scaled || (c.flags & k_force_update)) {
                        c.scaled = s;
                        c.flags = static_cast<uint16_t>(c.flags & ~k_force_update);
                        return true;
                    }
                } else {
                    c.flags = static_cast<uint16_t>(c.flags & ~k_force_update);
                    return true;
                }
            }
        }
        return false; // AlwaysUpdate() is false for every interpolator type here
    }

    void update_modifier_active(CtlrState& c, float t) {
        if (!interp_ctlr_prologue(c, t) || c.def->interpolator == k_none) {
            return;
        }
        uint8_t v = 0;
        if (update_bool(c.def->interpolator, c.scaled, v) && c.def->modifier != k_none) {
            modifiers[c.def->modifier].active = v != 0;
        }
    }

    void update_effect_float(CtlrState& c, float t) {
        if (!interp_ctlr_prologue(c, t) || c.def->interpolator == k_none) {
            return;
        }
        float v;
        if (!update_float(c.def->interpolator, c.scaled, v) ||
            c.def->target_kind != ose::TargetKind::material) {
            return;
        }
        auto& m = materials[c.def->target];
        switch (c.def->variable) {
        case 0:
            m.base_color_scale = v;
            break;
        case 1:
            m.falloff[0] = std::cos(v * 0.017453292f);
            break;
        case 2:
            m.falloff[1] = std::cos(v * 0.017453292f);
            break;
        case 3:
            m.falloff[2] = v;
            break;
        case 4:
            m.falloff[3] = v;
            break;
        case 5:
            m.base_color.a = std::max(v, 0.0f);
            break;
        case 6:
            m.uv_offset.x = v;
            break;
        case 7:
            m.uv_scale.x = v;
            break;
        case 8:
            m.uv_offset.y = v;
            break;
        case 9:
            m.uv_scale.y = v;
            break;
        default:
            break;
        }
    }

    void update_update_ctlr(CtlrState& c, float t) {
        c.flags = static_cast<uint16_t>((c.flags & ~0x02) | 0x04); // forced to CLAMP
        c.lo = k_neg_max;
        c.hi = k_pos_max;
        if (c.def->target_kind != ose::TargetKind::system) {
            return;
        }
        auto& sys = systems[c.def->target];
        // The first NiPSysEmitterCtlr on the same system: a gap longer than its emitter's life span is
        // skipped when that controller loops.
        const CtlrState* emitter_ctlr = nullptr;
        for (const auto& other : ctlrs) {
            if (other.def->type == ose::ControllerType::emitter &&
                other.def->target_kind == ose::TargetKind::system && other.def->target == c.def->target) {
                emitter_ctlr = &other;
                break;
            }
        }
        float last = c.last;
        if (last != k_neg_max && emitter_ctlr && (emitter_ctlr->flags & k_cycle_mask) == 0 &&
            emitter_ctlr->def->modifier != k_none) {
            float life = modifiers[emitter_ctlr->def->modifier].def->f[12];
            if (t - last > life) {
                c.last = t;
                last = t;
            }
        }
        bool skip = true;
        if ((c.flags & k_active) && (last != t || (c.flags & k_force_update))) {
            if (c.flags & k_compute_scaled) {
                float s = compute_scaled_time(c, t);
                if (s != c.scaled || (c.flags & k_force_update)) {
                    c.scaled = s;
                    skip = false;
                }
            } else {
                skip = false;
            }
            if (!skip) {
                c.flags = static_cast<uint16_t>(c.flags & ~k_force_update);
            }
        }
        c.lo = 0.0f;
        c.hi = 0.0f;
        if (!skip) {
            update_system(sys, c.scaled);
        }
    }

    void update_emitter_ctlr(CtlrState& c, float t) {
        if (c.last > t) {
            c.last_seq_time = k_neg_max;
        }
        if (c.def->target_kind != ose::TargetKind::system || c.def->modifier == k_none) {
            return;
        }
        auto& sys = systems[c.def->target];
        auto& emitter = modifiers[c.def->modifier];
        if (emitter.def->type != ose::ModifierType::mesh_emitter && emitter.def->type != ose::ModifierType::volume_emitter) {
            return;
        }
        if (c.flags & k_manager_controlled) {
            c.scaled = k_neg_max;
        }
        float v6 = c.last;
        if (v6 != k_neg_max && (c.flags & k_cycle_mask) == 0 && t - v6 > emitter.def->f[12]) {
            c.last = t;
            v6 = t;
        }
        if (!(c.flags & k_active) || (v6 == t && !(c.flags & k_force_update))) {
            return;
        }
        if (c.flags & k_compute_scaled) {
            float s = compute_scaled_time(c, t);
            if (s == c.scaled && !(c.flags & k_force_update)) {
                return;
            }
            c.scaled = s;
        }
        c.flags = static_cast<uint16_t>(c.flags & ~k_force_update);
        uint32_t vis = c.def->interpolator2;
        uint32_t rate = c.def->interpolator;
        if (vis == k_none || rate == k_none) {
            return;
        }
        float vis_time = k_neg_max;
        uint32_t vis_interp = resolve_dominant(vis, vis_time);
        float rate_time = k_neg_max;
        uint32_t rate_interp = resolve_dominant(rate, rate_time);
        if (vis_interp == k_none || rate_interp == k_none) {
            c.last_rate_interp = k_none;
            return;
        }
        float time = c.scaled;
        if (vis_time != k_neg_max || rate_time != k_neg_max) {
            time = rate_time;
        }
        if ((c.flags & k_manager_controlled) && c.last_rate_interp != rate_interp) {
            c.last_seq_time = time;
            uint8_t b = 0;
            update_bool(vis_interp, time, b);
            c.last_active = b;
            c.last_rate_interp = rate_interp;
            return;
        }
        c.last_rate_interp = rate_interp;
        const auto& vis_def = *interps[vis_interp].def;
        bool keyed = vis_def.key_group != k_none && e.key_groups[vis_def.key_group].count != 0;
        if (!keyed) {
            if (!interp_is_constant(vis_interp)) {
                return;
            }
            uint8_t on = 0;
            update_bool(vis_interp, 0.0f, on);
            if (!on) {
                return;
            }
            float last = c.last_seq_time;
            c.last_seq_time = time;
            float base = sys.last_time;
            if (interp_is_constant(rate_interp)) {
                float dt = time - last;
                if (last > time) {
                    last = 0.0f;
                    dt = time;
                }
                float r;
                if (update_float(rate_interp, 0.0f, r)) {
                    emit_in_interval(c, sys, emitter, dt + base, time, last, 0.0f, time, r);
                }
            } else {
                const auto& g = e.key_groups[interps[rate_interp].def->key_group];
                float first_key = 0.0f;
                float last_key = 0.0f;
                if (interps[rate_interp].def->key_group != k_none && g.count) {
                    first_key = e.float_keys[g.first].time;
                    last_key = e.float_keys[g.first + g.count - 1].time;
                }
                float wrap = 0.0f;
                if (last > time) {
                    wrap = last_key - last;
                    float r;
                    if (update_float(rate_interp, wrap * 0.5f + last, r)) {
                        emit_in_interval(c, sys, emitter, wrap + base, last_key, last, first_key, last_key,
                                         r);
                    }
                    last = first_key;
                }
                float d = time - last;
                float r;
                if (update_float(rate_interp, d * 0.5f + last, r)) {
                    emit_in_interval(c, sys, emitter, d + base + wrap, time, last, first_key, time, r);
                }
            }
            return;
        }
        emit_timeline(c, sys, emitter, vis_interp, rate_interp, time);
    }

    // The interpolator a controller's (possibly blended) interpolator currently follows, and the sequence
    // time of that blend item (-FLT_MAX for a plain interpolator).
    uint32_t resolve_dominant(uint32_t i, float& time) {
        auto& s = interps[i];
        using T = ose::InterpolatorType;
        if (s.def->type != T::blend_float && s.def->type != T::blend_bool) {
            return i;
        }
        compute_normalized_weights(s);
        int idx = highest_weighted(s);
        if (idx < 0 || idx == 0xFF || static_cast<size_t>(idx) >= s.items.size()) {
            return k_none;
        }
        if (s.count == 1 && idx == s.single) {
            time = s.single_time;
            return s.single_interp;
        }
        time = s.items[static_cast<size_t>(idx)].time;
        return s.items[static_cast<size_t>(idx)].interp;
    }

    // The keyed-visibility branch of the game: walks the on/off intervals between the
    // previous and the current time (at most 20) and emits for each "on" stretch.
    void emit_timeline(CtlrState& c, SystemState& sys, ModifierState& emitter, uint32_t vis_interp,
                       uint32_t rate_interp, float time) {
        const auto& g = e.key_groups[interps[vis_interp].def->key_group];
        const auto* keys = e.bool_keys.data() + g.first;
        uint32_t n = g.count;
        uint32_t scratch = 0;
        uint8_t now_on = eval_bool_keys(g, time, scratch);
        float v64 = c.last_seq_time;
        c.last_seq_time = time;
        if (v64 == k_neg_max) {
            c.last_active = now_on;
            return;
        }
        uint8_t state = c.last_active;
        c.last_active = now_on;
        for (int iter = 0; iter < 20; ++iter) {
            bool wrapped = v64 > time;
            uint32_t v67;
            if (state) {
                v67 = n;
                while (v67 > 0) {
                    const auto& k = keys[v67 - 1];
                    if (v64 >= k.time && !k.value) {
                        break;
                    }
                    --v67;
                }
            } else {
                v67 = 0;
                while (v67 < n) {
                    const auto& k = keys[v67];
                    if (k.time > v64 && (wrapped || k.time <= time) && k.value) {
                        break;
                    }
                    ++v67;
                }
                if (v67 == n) {
                    if (!wrapped) {
                        return;
                    }
                    v67 = 0;
                    while (v67 < n && !(time > keys[v67].time && keys[v67].value)) {
                        ++v67;
                    }
                    if (v67 == n) {
                        return;
                    }
                }
            }
            uint32_t v94 = v67;
            while (v94 < n && keys[v94].value) {
                ++v94;
            }
            uint32_t v95 = v94 != n ? v94 : v94 - 1;
            if (v67 >= n) {
                v67 = n - 1;
            }
            float on = keys[v67].time;
            float off = keys[v95].time;
            float t1;
            float t0;
            float sample;
            if (wrapped) {
                if (off > v64) {
                    t0 = v64;
                    t1 = keys[n - 1].time;
                } else {
                    t0 = keys[0].time;
                    off = std::min(off, time);
                    t1 = time;
                }
                sample = (off - on) * 0.5f + on;
            } else {
                t0 = v64;
                if (off >= time) {
                    t1 = time;
                    sample = time;
                } else {
                    t1 = off;
                    sample = (off - on) * 0.5f + on;
                }
            }
            float r;
            if (update_float(rate_interp, sample, r)) {
                float emit_time = sys.last_time + (wrapped ? c.hi - c.lo : 0.0f) + time - v64;
                emit_in_interval(c, sys, emitter, emit_time, t1, t0, on, off, r);
            }
            if (time > keys[v95].time) {
                v64 = t1;
                state = eval_bool_keys(g, t1, scratch);
            } else {
                return;
            }
        }
    }

    void emit_in_interval(CtlrState& c, SystemState& sys, ModifierState& emitter, float emit_time, float t1,
                          float t0, float on, float off, float rate) {
        if (!(rate > 0.0f && on < off && t1 > on && t0 < off)) {
            return;
        }
        if (t1 > off) {
            t1 = off;
        }
        float d1 = t1 - on;
        float d0 = t0 >= on ? t0 - on : 0.0f;
        float e_scale = sys.emit_scale;
        float p_scale = sys.prev_emit_scale;
        float r1 = e_scale * rate;
        float n = std::max(r1 * d1 - (p_scale * rate) * d0, 0.0f);
        float whole = std::floor(n);
        float carry = (n - whole) + c.carry;
        c.carry = carry;
        if (carry >= 1.0f) {
            if (p_scale > 0.0f && e_scale > 0.0f) {
                whole = whole + 1.0f;
            }
            c.carry = carry - 1.0f;
        }
        auto count = static_cast<uint16_t>(static_cast<int>(whole));
        count = std::min<uint16_t>(count, 15); // word_1420CF408
        c.ages.clear();
        for (uint16_t i = 0; i < count; ++i) {
            c.ages.push_back(static_cast<float>(i) * (1.0f / r1));
        }
        emit_particles(sys, emitter, emit_time, count, c.ages.data());
    }

    // ---- Emission.

    void emit_particles(SystemState& sys, ModifierState& em, float time, int count, const float* ages) {
        const float* f = em.def->f;
        float v9 = static_cast<float>(count) * density + em.carry;
        int n = static_cast<int>(v9);
        em.carry = v9 - static_cast<float>(n);
        const auto& tables = math_tables();
        for (int i = 0; i < n; ++i) {
            float age = ages[i];
            float life = (crt.unit() - 0.5f) * f[13] + f[12];
            if (life <= 0.0f) {
                life = 0.000001f;
            }
            if (!(age <= life)) {
                continue;
            }
            uint16_t idx = add_particle(sys);
            if (idx == 0xFFFF) {
                return;
            }
            auto& info = sys.info[idx];
            float speed = (crt.unit() - 0.5f) * f[1] + f[0];
            float decl = crt.symmetric() * f[3] + f[2];
            float planar = crt.symmetric() * f[5] + f[4];
            uint32_t di = angle_index(decl);
            uint32_t pi = angle_index(planar);
            info.velocity = glm::vec3((tables.sin[di] * tables.cos[pi]) * speed,
                                      (tables.sin[di] * tables.sin[pi]) * speed, speed * tables.cos[di]);
            info.generation = 0;
            info.subtex_counter = -1;
            info.age = age;
            info.life_span = life;
            initial_position_and_velocity(sys, em, sys.positions[idx], info);
            if (!sys.colors.empty()) {
                sys.colors[idx] = glm::vec4(f[6], f[7], f[8], f[9]);
            }
            sys.radii[idx] = ((crt.symmetric() * f[11] + f[10]) * em.radius_scale) * sys.size_scale;
            sys.sizes[idx] = 1.0f;
            if (!sys.tex.empty()) {
                sys.tex[idx] = static_cast<uint8_t>(crt.next() % static_cast<int>(sys.def->subtexture_count));
            }
            info.last_update = time - info.age;
            initialize_particle(sys, idx);
        }
    }

    // NiPSysVolumeEmitter: a point of the volume (the slot of each emitter: sphere, box
    // cylinder), then both through the emitter object's place in the system.
    void volume_position_and_velocity(SystemState& sys, ModifierState& em, glm::vec3& pos, ParticleInfo& info) {
        const float* f = em.def->f;
        const auto& tables = math_tables();
        const auto index = [&] { return static_cast<uint32_t>(static_cast<int>(static_cast<float>(crt.next()) * 0.015625477f)) & 0x1FFu; };
        switch (em.def->u0) {
        case 0: {
            const float r = (static_cast<float>(crt.next()) * f[14]) * 0.000030518509f;
            const uint32_t a = index(), b = index();
            const float s = r * tables.sin[b];
            pos = glm::vec3(s * tables.cos[a], s * tables.sin[a], tables.cos[b] * r);
            break;
        }
        case 1: {
            const float z = crt.unit(), y = crt.unit(), x = crt.unit();
            pos = glm::vec3((x - 0.5f) * f[14], (y - 0.5f) * f[15], (z - 0.5f) * f[16]);
            break;
        }
        default: {
            const float r = (static_cast<float>(crt.next()) * f[14]) * 0.000030518509f;
            const uint32_t a = index();
            pos = glm::vec3(r * tables.cos[a], tables.sin[a] * r, 0.0f);
            pos.z = (crt.unit() - 0.5f) * f[15];
            break;
        }
        }
        const Xf m = to_system(sys, em.def->ref);
        pos = apply(m, pos);
        info.velocity = m.rot * info.velocity;
    }

    // EmitFromFace and TransformToSystem.
    void initial_position_and_velocity(SystemState& sys, ModifierState& em, glm::vec3& pos,
                                       ParticleInfo& info) {
        if (em.def->type == ose::ModifierType::volume_emitter) {
            volume_position_and_velocity(sys, em, pos, info);
            return;
        }
        uint32_t count = em.def->count;
        if (count == 0) {
            return;
        }
        auto mesh_index = static_cast<uint32_t>(static_cast<int>(crt.unit() * static_cast<float>(count)));
        if (mesh_index == count) {
            mesh_index = count - 1;
        }
        if (mesh_index >= count) {
            return;
        }
        uint32_t emission = em.def->u1;
        if (emission != 1 && emission != 3) {
            return;
        }
        const auto& mesh = e.emitter_meshes[em.def->first + mesh_index];
        if (mesh.triangle_count == 0) {
            return;
        }
        const auto* vp = e.mesh_positions.data() + mesh.first_vertex;
        const auto* vn = e.mesh_normals.data() + mesh.first_vertex;
        const auto& tri =
            e.mesh_triangles[mesh.first_triangle + static_cast<uint32_t>(crt.next()) % mesh.triangle_count];
        glm::vec3 p0(vp[tri.v[0]].x, vp[tri.v[0]].y, vp[tri.v[0]].z);
        glm::vec3 p1(vp[tri.v[1]].x, vp[tri.v[1]].y, vp[tri.v[1]].z);
        glm::vec3 p2(vp[tri.v[2]].x, vp[tri.v[2]].y, vp[tri.v[2]].z);
        pos.x = ((p0.x + p1.x) + p2.x) * 0.33333334f;
        pos.z = ((p0.z + p1.z) + p2.z) * 0.33333334f;
        pos.y = ((p0.y + p1.y) + p2.y) * 0.33333334f;
        glm::vec3& v = info.velocity;
        if (em.def->u0 == 0) {
            const auto& n0 = vn[tri.v[0]];
            const auto& n1 = vn[tri.v[1]];
            const auto& n2 = vn[tri.v[2]];
            float sx = (n0.x + n1.x) + n2.x;
            float sy = (n1.y + n0.y) + n2.y;
            float sz = (n1.z + n0.z) + n2.z;
            float mx = sx * 0.33333334f;
            float mz = sz * 0.33333334f;
            float sq = ((sy * sy) + (sx * sx) + (sz * sz)) * 0.11111112f;
            float inv = sq == 0.0f ? 0.0f : 1.0f / fast_sqrt(sq);
            float speed = std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
            v = glm::vec3((mx * inv) * speed, ((sy * 0.33333334f) * inv) * speed, (mz * inv) * speed);
        }
        if (emission == 3) {
            // As observed: the point lies on the edge p1-p2 (the game drops the sqrt(r1) factor of the
            // p1 - p0 term of uniform triangle sampling).
            glm::vec3 e1 = p1 - p0;
            glm::vec3 e2 = (p2 - p0) - e1;
            float r1 = crt.unit();
            uint32_t yi = 0x5F3759DFu - (std::bit_cast<uint32_t>(r1) >> 1);
            float y = std::bit_cast<float>(yi);
            float r2 = crt.unit();
            float sqrt_r1 = ((1.5f - ((r1 * 0.5f) * y) * y) * y) * r1;
            float k = r2 * sqrt_r1;
            pos = glm::vec3((k * e2.x + p0.x) + e1.x, (k * e2.y + p0.y) + e1.y, (k * e2.z + p0.z) + e1.z);
        }
        // TransformToSystem.
        Xf m = to_system(sys, mesh.node);
        pos = apply(m, pos);
        const float* f = em.def->f;
        switch (em.def->u0) {
        case 0:
            v = m.rot * v;
            break;
        case 1: {
            v.x = crt.symmetric();
            v.y = crt.symmetric();
            v.z = crt.symmetric();
            unitize(v);
            float speed = (crt.unit() - 0.5f) * f[1] + f[0];
            v = glm::vec3(speed * v.x, speed * v.y, speed * v.z);
            break;
        }
        case 2: {
            glm::vec3 axis = m.rot * glm::vec3(f[14], f[15], f[16]);
            float speed = (crt.unit() - 0.5f) * f[1] + f[0];
            unitize(axis);
            v = glm::vec3(axis.x * speed, axis.y * speed, axis.z * speed);
            break;
        }
        default:
            break;
        }
    }

    // every modifier's Initialize (only the rotation modifier has one).
    void initialize_particle(SystemState& sys, uint16_t idx) {
        const auto& tables = math_tables();
        for (auto mi : sys.mods) {
            auto& m = modifiers[mi];
            if (m.def->type != ose::ModifierType::rotation) {
                continue;
            }
            const float* f = m.def->f;
            if (!sys.axes.empty()) {
                if (m.def->flag1) {
                    float c = tables.cos[static_cast<uint32_t>(static_cast<int>(
                                             static_cast<float>(crt.next()) * 0.0078127384f)) &
                                         0x1FF];
                    float s = std::sqrt(1.0f - c * c);
                    uint32_t k = static_cast<uint32_t>(
                                     static_cast<int>(static_cast<float>(crt.next()) * 0.015625477f)) &
                                 0x1FF;
                    sys.axes[idx] = glm::vec3(s * tables.sin[k], tables.cos[k] * s, c);
                } else {
                    sys.axes[idx] = glm::vec3(f[4], f[5], f[6]);
                }
            }
            if (!sys.angles.empty()) {
                sys.angles[idx] = crt.symmetric() * f[3] + f[2];
                float speed = crt.symmetric() * f[1] + f[0];
                if (m.def->flag0 && crt.unit() <= 0.5f) {
                    speed = -speed;
                }
                if (!sys.speeds.empty()) {
                    sys.speeds[idx] = speed;
                }
            }
        }
    }

    // ---- The particle system update.

    void update_system(SystemState& s, float t) {
        resolve_added(s);
        if (s.reset || s.last_time > t) {
            s.active = 0;
            s.added = 0;
            s.last_time = k_neg_max;
            s.bound_center = glm::vec3(0.0f);
            s.bound_radius = 0.0f;
            s.reset = false;
        }
        if (s.last_time == k_neg_max) {
            s.last_time = t;
        }
        s.bound_center = glm::vec3(0.0f);
        s.bound_radius = 0.0f;
        for (auto mi : s.mods) {
            auto& m = modifiers[mi];
            if (m.active && !update_modifier(s, m, t)) {
                break;
            }
        }
        s.last_time = t;
    }

    bool update_modifier(SystemState& s, ModifierState& m, float t) {
        using T = ose::ModifierType;
        switch (m.def->type) {
        case T::age_death:
            age_death(s, m, t);
            return true;
        case T::lod:
            lod(s, m);
            return true;
        case T::mesh_emitter:
        case T::volume_emitter:
            return s.active != 0;
        case T::spawn:
            return true;
        case T::simple_color:
            simple_color(s, m);
            return true;
        case T::rotation:
            rotation(s, t);
            return true;
        case T::subtex:
            subtex(s, m);
            return true;
        case T::scale:
            scale(s, m);
            return true;
        case T::gravity:
            gravity(s, m, t);
            return true;
        case T::drag:
            drag(s, m, t);
            return true;
        case T::position:
            position(s, t);
            return true;
        case T::bound_update:
            bound_update(s, m);
            return true;
        }
        return true;
    }

    // a stub in this build that only draws random numbers.
    void spawn_particles(ModifierState& spawn, const ParticleInfo& info) {
        if (info.generation < static_cast<uint16_t>(spawn.def->u0)) {
            float u = crt.unit();
            if (u <= density * spawn.def->f[0]) {
                crt.next();
            }
        }
    }

    void age_death(SystemState& s, ModifierState& m, float t) {
        ModifierState* spawn = (m.def->flag0 && m.def->ref != k_none) ? &modifiers[m.def->ref] : nullptr;
        for (uint16_t i = s.active; i-- > 0;) {
            auto& info = s.info[i];
            if (spawn) {
                float age = (t - info.last_update) + info.age;
                info.age = age;
                if (age > info.life_span) {
                    spawn_particles(*spawn, info);
                    remove_particle(s, i);
                }
            } else {
                float age = std::max(t - info.last_update, 0.0f) + info.age;
                bool keep = age <= info.life_span;
                info.age = age;
                if (!keep) {
                    remove_particle(s, i);
                }
            }
        }
    }

    void lod(SystemState& s, const ModifierState& m) {
        const float* f = m.def->f;
        float emit = 1.0f;
        float size = 1.0f;
        float d = lod_distance;
        if (d > f[0]) {
            emit = f[2];
            size = f[3];
            if (d <= f[1]) {
                float k = (d - f[0]) / (f[1] - f[0]);
                emit = (emit - 1.0f) * k + 1.0f;
                size = (size - 1.0f) * k + 1.0f;
            }
        }
        float old = s.emit_scale;
        s.emit_scale = emit;
        s.prev_emit_scale = old;
        s.size_scale = size;
    }

    void simple_color(SystemState& s, const ModifierState& m) {
        if (s.colors.empty()) {
            return;
        }
        const float* f = m.def->f;
        const float* c0 = f + 6;
        const float* c1 = f + 10;
        const float* c2 = f + 14;
        float span01 = f[3] - f[2];
        float span12 = f[5] - f[4];
        for (uint16_t i = 0; i < s.active; ++i) {
            float r = s.info[i].age / s.info[i].life_span;
            auto& col = s.colors[i];
            if (span01 == 0.0f || r >= f[3]) {
                if (span12 == 0.0f || r <= f[4]) {
                    col.r = c1[0];
                    col.g = c1[1];
                    col.b = c1[2];
                } else if (r <= f[5]) {
                    float k = (r - f[4]) / span12;
                    col.r = (c2[0] - c1[0]) * k + c1[0];
                    col.g = (c2[1] - c1[1]) * k + c1[1];
                    col.b = (c2[2] - c1[2]) * k + c1[2];
                } else {
                    col.r = c2[0];
                    col.g = c2[1];
                    col.b = c2[2];
                }
            } else if (r >= f[2]) {
                float k = (r - f[2]) / span01;
                col.r = (c1[0] - c0[0]) * k + c0[0];
                col.g = (c1[1] - c0[1]) * k + c0[1];
                col.b = (c1[2] - c0[2]) * k + c0[2];
            } else {
                col.r = c0[0];
                col.g = c0[1];
                col.b = c0[2];
            }
            float fade_out = f[1];
            if (fade_out == 0.0f || r <= fade_out) {
                float fade_in = f[0];
                if (fade_in == 0.0f || r >= fade_in) {
                    col.a = c1[3];
                } else {
                    col.a = (c1[3] - c0[3]) * (r / fade_in) + c0[3];
                }
            } else {
                col.a = ((r - fade_out) / (1.0f - fade_out)) * (c2[3] - c1[3]) + c1[3];
            }
        }
    }

    void rotation(SystemState& s, float t) {
        if (s.angles.empty() || s.speeds.empty()) {
            return;
        }
        for (uint16_t i = 0; i < s.active; ++i) {
            float a = std::max(t - s.info[i].last_update, 0.0f) * s.speeds[i] + s.angles[i];
            s.angles[i] = a;
            if (a <= 31.415928f) {
                while (s.angles[i] > 6.2831855f) {
                    s.angles[i] = s.angles[i] - 6.2831855f;
                }
            } else {
                s.angles[i] = 0.0f;
            }
        }
    }

    void subtex(SystemState& s, const ModifierState& m) {
        if (s.tex.empty()) {
            return;
        }
        const float* f = m.def->f;
        for (uint16_t i = 0; i < s.active; ++i) {
            auto& info = s.info[i];
            int16_t counter = info.subtex_counter;
            uint16_t frames;
            if (counter == -1) {
                float u = static_cast<float>(static_cast<uint64_t>(bs.next(0xFFFFFFFFu))) *
                          std::bit_cast<float>(0x2F800000u);
                frames = static_cast<uint16_t>(static_cast<int>(u * f[6] + f[5]));
                info.subtex_frames = frames;
            } else {
                frames = info.subtex_frames;
            }
            int current = static_cast<int>((info.age / info.life_span) * static_cast<float>(frames));
            if (current > counter) {
                info.subtex_counter = static_cast<int16_t>(current);
                if (counter == -1) {
                    s.tex[i] = static_cast<uint8_t>(static_cast<int>(bs.range(0.0f, f[1]) + f[0]));
                } else if (static_cast<float>(s.tex[i] + 1) <= f[2]) {
                    ++s.tex[i];
                } else {
                    s.tex[i] = static_cast<uint8_t>(static_cast<int>(bs.range(0.0f, f[4]) + f[3]));
                }
            }
        }
    }

    // The game with the game.
    void scale(SystemState& s, const ModifierState& m) {
        uint32_t n = m.def->count;
        if (n == 0) {
            return;
        }
        const float* v = e.floats.data() + m.def->first;
        for (uint16_t i = 0; i < s.active; ++i) {
            float x = std::min(std::max(s.info[i].age / s.info[i].life_span, 0.0f), 1.0f);
            uint32_t last = n - 1;
            float out;
            if (x == 1.0f) {
                out = v[last];
            } else {
                float f = static_cast<float>(last) * x;
                int k = static_cast<int>(f);
                float fl = static_cast<float>(k);
                if (f - fl < 0.0f) {
                    fl = fl - 1.0f;
                }
                float a = v[static_cast<uint32_t>(k)];
                out = (v[static_cast<uint32_t>(k + 1)] - a) * (f - fl) + a;
            }
            s.sizes[i] = out;
        }
    }

    void gravity(SystemState& s, const ModifierState& m, float t) {
        if (s.active == 0 || m.def->ref == k_none) {
            return;
        }
        const float* f = m.def->f;
        Xf x = to_system(s, m.def->ref);
        glm::vec3 axis(f[0], f[1], f[2]);
        glm::vec3 dir;
        if (m.def->flag0) {
            dir = local[m.def->ref].rot * axis; // world aligned: the object's local rotation
        } else {
            dir = x.rot * axis;
        }
        float len = std::sqrt((dir.y * dir.y + dir.x * dir.x) + dir.z * dir.z);
        if (len <= 0.000001f) {
            dir = glm::vec3(0.0f);
        } else {
            dir = glm::vec3(dir.x * (1.0f / len), dir.y * (1.0f / len), dir.z * (1.0f / len));
        }
        float strength = f[4] * 1.6f;
        float decay = f[3];
        float turbulence = f[5];
        const glm::vec3 origin = x.t;
        for (uint16_t i = 0; i < s.active; ++i) {
            auto& info = s.info[i];
            const glm::vec3& p = s.positions[i];
            glm::vec3 rnd(0.0f);
            float turb = turbulence * f[6] * 500.0f;
            if (m.def->u0 == 1) {
                // Spherical.
                if (decay == 0.0f) {
                    glm::vec3 d = origin - p;
                    unitize(d);
                    if (turbulence != 0.0f) {
                        rnd.x = crt.symmetric() * turb;
                        rnd.y = crt.symmetric() * turb;
                        rnd.z = crt.symmetric() * turb;
                        float dt = std::max(t - info.last_update, 0.0f);
                        info.velocity.x = ((d.x * strength) + rnd.x) * dt + info.velocity.x;
                        info.velocity.y = ((d.y * strength) + rnd.y) * dt + info.velocity.y;
                        info.velocity.z = ((d.z * strength) + rnd.z) * dt + info.velocity.z;
                    } else {
                        float k = std::max(t - info.last_update, 0.0f) * strength;
                        info.velocity = glm::vec3(d.x * k + info.velocity.x, d.y * k + info.velocity.y,
                                                  d.z * k + info.velocity.z);
                    }
                } else if (turbulence == 0.0f) {
                    glm::vec3 d = origin - p;
                    float dist = unitize_length(d);
                    float k = (std::exp(-(dist * decay)) * strength) * std::max(t - info.last_update, 0.0f);
                    info.velocity = glm::vec3(d.x * k + info.velocity.x, d.y * k + info.velocity.y,
                                              d.z * k + info.velocity.z);
                } else {
                    rnd.x = crt.symmetric() * turb;
                    rnd.y = crt.symmetric() * turb;
                    rnd.z = crt.symmetric() * turb;
                    glm::vec3 d = origin - p;
                    float dist = unitize_length(d);
                    float k = std::exp(-(dist * decay)) * strength;
                    float dt = std::max(t - info.last_update, 0.0f);
                    // As observed, this branch accelerates along the planar axis, not towards the object.
                    info.velocity.y = ((dir.y * k) + rnd.y) * dt + info.velocity.y;
                    info.velocity.x = ((dir.x * k) + rnd.x) * dt + info.velocity.x;
                    info.velocity.z = ((dir.z * k) + rnd.z) * dt + info.velocity.z;
                }
            } else if (m.def->u0 == 0) {
                // Planar.
                if (decay == 0.0f) {
                    if (turbulence != 0.0f) {
                        rnd.x = crt.symmetric() * turb;
                        rnd.y = crt.symmetric() * turb;
                        rnd.z = crt.symmetric() * turb;
                        float dt = std::max(t - info.last_update, 0.0f);
                        info.velocity.x = ((dir.x * strength) + rnd.x) * dt + info.velocity.x;
                        info.velocity.y = ((dir.y * strength) + rnd.y) * dt + info.velocity.y;
                        info.velocity.z = ((dir.z * strength) + rnd.z) * dt + info.velocity.z;
                    } else {
                        float k = std::max(t - info.last_update, 0.0f) * strength;
                        info.velocity.y = dir.y * k + info.velocity.y;
                        info.velocity.x = dir.x * k + info.velocity.x;
                        info.velocity.z = dir.z * k + info.velocity.z;
                    }
                } else {
                    if (turbulence != 0.0f) {
                        rnd.x = crt.symmetric() * turb;
                        rnd.y = crt.symmetric() * turb;
                        rnd.z = crt.symmetric() * turb;
                    }
                    float proj =
                        ((origin.y - p.y) * dir.y + (origin.x - p.x) * dir.x) + (origin.z - p.z) * dir.z;
                    float ex = proj * decay;
                    if (proj >= 0.0f) {
                        ex = -ex;
                    }
                    float k = std::exp(ex) * strength;
                    float dt = std::max(t - info.last_update, 0.0f);
                    if (turbulence != 0.0f) {
                        info.velocity.y = ((dir.y * k) + rnd.y) * dt + info.velocity.y;
                        info.velocity.x = ((dir.x * k) + rnd.x) * dt + info.velocity.x;
                        info.velocity.z = ((dir.z * k) + rnd.z) * dt + info.velocity.z;
                    } else {
                        float kk = k * dt;
                        info.velocity.y = dir.y * kk + info.velocity.y;
                        info.velocity.x = dir.x * kk + info.velocity.x;
                        info.velocity.z = dir.z * kk + info.velocity.z;
                    }
                }
            }
        }
    }

    void drag(SystemState& s, const ModifierState& m, float t) {
        const float* f = m.def->f;
        if (f[3] <= 0.0f || s.active == 0 || m.def->ref == k_none) {
            return;
        }
        Xf x = to_system(s, m.def->ref);
        glm::vec3 dir = x.rot * glm::vec3(f[0], f[1], f[2]);
        float len = std::sqrt((dir.y * dir.y + dir.x * dir.x) + dir.z * dir.z);
        if (len <= 0.000001f) {
            dir = glm::vec3(0.0f);
        } else {
            dir = glm::vec3(dir.x * (1.0f / len), dir.y * (1.0f / len), dir.z * (1.0f / len));
        }
        float span = f[5] - f[4];
        float inv_len2 = -1.0f / ((dir.y * dir.y + dir.x * dir.x) + dir.z * dir.z);
        for (uint16_t i = 0; i < s.active; ++i) {
            const glm::vec3 rel = s.positions[i] - x.t;
            auto& info = s.info[i];
            float dt = std::max(t - info.last_update, 0.0f);
            float dist = std::sqrt((rel.y * rel.y + rel.x * rel.x) + rel.z * rel.z);
            float k;
            if (dist <= f[4]) {
                k = f[3];
            } else if (dist < f[5]) {
                k = (1.0f - (dist - f[4]) / span) * f[3];
            } else {
                continue;
            }
            float amount = (dt * k) * 30.000029f;
            float proj = ((dir.y * info.velocity.y) + (info.velocity.x * dir.x) + (dir.z * info.velocity.z)) *
                         inv_len2;
            if (amount <= 1.0f) {
                proj = proj * amount;
            }
            info.velocity.x = proj * dir.x + info.velocity.x;
            info.velocity.y = dir.y * proj + info.velocity.y;
            info.velocity.z = dir.z * proj + info.velocity.z;
        }
    }

    void position(SystemState& s, float t) {
        resolve_added(s);
        for (uint16_t i = 0; i < s.active; ++i) {
            auto& info = s.info[i];
            float dt = std::max(t - info.last_update, 0.0f);
            glm::vec3& p = s.positions[i];
            float x = dt * info.velocity.x + p.x;
            float z = info.velocity.z * dt + p.z;
            p.y = info.velocity.y * dt + p.y;
            p.x = x;
            p.z = z;
            info.last_update = t;
        }
    }

    void bound_update(SystemState& s, ModifierState& m) {
        uint16_t n = s.active;
        glm::vec4 b(0.0f);
        if (n == 0) {
            m.bound_current = 0;
        } else {
            if (m.bound_groups == -1) {
                set_update_skip(m, static_cast<int16_t>(s.max / 50 + 1));
            }
            int16_t groups_set = m.bound_groups;
            glm::vec3 lo = s.positions[0];
            glm::vec3 hi = s.positions[0];
            int stride = 1;
            int groups = n / 50 + 1;
            if (groups_set < groups) {
                groups = groups_set;
            }
            if (groups > 1) {
                stride = groups;
            }
            uint32_t current = m.bound_current;
            for (uint32_t k = current; k < n; k += static_cast<uint32_t>(stride)) {
                float r = s.sizes[k] * s.radii[k];
                const auto& p = s.positions[k];
                lo.x = std::min(p.x - r, lo.x);
                hi.x = std::max(p.x + r, hi.x);
                lo.y = std::min(p.y - r, lo.y);
                hi.y = std::max(p.y + r, hi.y);
                lo.z = std::min(p.z - r, lo.z);
                hi.z = std::max(p.z + r, hi.z);
            }
            if (current >= m.sub_bounds.size()) {
                m.sub_bounds.resize(current + 1, glm::vec4(0.0f));
            }
            glm::vec4& sb = m.sub_bounds[current];
            sb.z = (hi.z + lo.z) * 0.5f;
            sb.x = (hi.x + lo.x) * 0.5f;
            sb.y = (hi.y + lo.y) * 0.5f;
            sb.w = std::sqrt(((hi.y - lo.y) * (hi.y - lo.y) + (hi.x - lo.x) * (hi.x - lo.x)) +
                             (hi.z - lo.z) * (hi.z - lo.z)) *
                   0.5f;
            for (int k = stride; k < m.bound_groups; ++k) {
                if (static_cast<size_t>(k) < m.sub_bounds.size()) {
                    m.sub_bounds[static_cast<size_t>(k)] = glm::vec4(0.0f);
                }
            }
            b = m.sub_bounds[current];
            for (int k = 1; k < stride; ++k) {
                if (static_cast<size_t>(k) < m.sub_bounds.size() &&
                    m.sub_bounds[static_cast<size_t>(k)].w != 0.0f) {
                    merge_bound(b, m.sub_bounds[static_cast<size_t>(k)]);
                }
            }
            if (b.w == 0.0f) {
                b.w = s.radii[0] * s.sizes[0];
            }
            if (++m.bound_current >= static_cast<uint16_t>(stride)) {
                m.bound_current = 0;
            }
        }
        s.bound_center = glm::vec3(b);
        s.bound_radius = b.w;
    }

    static void set_update_skip(ModifierState& m, int16_t skip) {
        m.bound_groups = skip;
        m.sub_bounds.assign(static_cast<size_t>(std::max<int>(skip, 1)), glm::vec4(0.0f));
    }

    static void merge_bound(glm::vec4& a, const glm::vec4& b) {
        float ra = a.w;
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        float d2 = (dx * dx + dy * dy) + dz * dz;
        float rb = b.w;
        float dr = rb - ra;
        float dr2 = dr * dr;
        if (dr < 0.0f) {
            if (dr2 >= d2) {
                return;
            }
        } else if (!(dr2 < d2)) {
            a = b;
            return;
        }
        float d = std::sqrt(d2);
        if (d > 0.000001f) {
            float k = (d - dr) * (0.5f / d);
            a.x = k * dx + b.x;
            a.y = k * dy + b.y;
            a.z = k * dz + b.z;
        }
        a.w = ((rb + d) + ra) * 0.5f;
    }

    // ---- Sequences (NiControllerSequence).

    void seq_set_state(uint32_t i, SeqState st) {
        auto& q = seqs[i];
        if (q.state == SeqState::inactive && st != SeqState::inactive) {
            if (std::ranges::find(active_seqs, i) == active_seqs.end()) {
                active_seqs.push_back(i);
            }
        }
        q.state = st;
        if (st == SeqState::animating) {
            q.start = 0.0f;
        }
    }

    bool seq_activate(uint32_t i, uint8_t priority, bool start_over, float weight, float ease_in) {
        auto& q = seqs[i];
        if (q.state != SeqState::inactive) {
            return false;
        }
        q.weight = weight;
        for (auto& b : q.blocks) {
            if (b.interp == k_none || b.blend == k_none) {
                continue;
            }
            uint8_t p = b.priority != 0xFF ? b.priority : priority;
            b.blend_index = add_interp_info(interps[b.blend], b.interp, weight, static_cast<int8_t>(p), 1.0f);
        }
        if (ease_in <= 0.0f) {
            seq_set_state(i, SeqState::animating);
        } else {
            seq_set_state(i, SeqState::ease_in);
            q.end = ease_in;
            q.start = k_neg_max;
        }
        if (start_over) {
            q.offset = k_neg_max;
        }
        q.last = k_neg_max;
        q.last_scaled = 0.0f;
        q.posed = k_neg_max;
        return true;
    }

    bool seq_deactivate(uint32_t i, float ease_out) {
        auto& q = seqs[i];
        if (q.state == SeqState::inactive) {
            return false;
        }
        if (ease_out <= 0.0f) {
            if (q.last != k_neg_max) {
                q.offset = ((q.acc / q.def->frequency) - q.last) + q.offset;
            }
            seq_set_state(i, SeqState::inactive);
            for (auto& b : q.blocks) {
                if (b.interp != k_none && b.blend != k_none) {
                    remove_interp_info(interps[b.blend], b.blend_index);
                }
            }
        } else {
            seq_set_state(i, SeqState::ease_out);
            q.end = ease_out;
            q.start = k_neg_max;
        }
        return true;
    }

    float seq_scaled_time(SeqStateData& q, float t) const {
        if (q.posed != k_neg_max) {
            q.acc = q.posed;
            q.last = t;
            q.last_scaled = std::clamp(q.posed, q.def->start_time, q.def->stop_time);
            return q.last_scaled;
        }
        float acc = q.acc;
        float x = t;
        if (q.last == k_neg_max) {
            acc = 0.0f;
        } else {
            x = t - q.last;
        }
        acc = acc + x * q.def->frequency;
        float r = acc;
        float begin = q.def->start_time;
        float end = q.def->stop_time;
        if (q.def->cycle_type == 0) {
            float span = end - begin;
            r = begin;
            if (span != 0.0f) {
                float m = acc - begin;
                r = m;
                if (m != span) {
                    r = std::fmod(m, span) + begin;
                }
                if (r < begin) {
                    r = r + span;
                }
            }
        }
        float out = r > end ? end : std::max(begin, r);
        q.acc = acc;
        q.last = t;
        q.last_scaled = out;
        return out;
    }

    void seq_update(uint32_t i, float t) {
        auto& q = seqs[i];
        if (q.state == SeqState::inactive) {
            return;
        }
        if (q.offset == k_neg_max) {
            q.offset = -t;
        }
        if (q.start == k_neg_max) {
            q.start = t;
            q.end = t + q.end;
        }
        float ease = 1.0f;
        float trans = 1.0f;
        float tt = t;
        switch (q.state) {
        case SeqState::ease_in:
            if (tt < q.end) {
                tt = std::max(q.start, tt);
                ease = (tt - q.start) / (q.end - q.start);
            } else {
                seq_set_state(i, SeqState::animating);
            }
            break;
        case SeqState::ease_out:
            if (tt < q.end) {
                tt = std::max(q.start, tt);
                ease = (q.end - tt) / (q.end - q.start);
            } else {
                seq_deactivate(i, 0.0f);
                return;
            }
            break;
        case SeqState::trans_source:
            if (tt >= q.end) {
                seq_deactivate(i, 0.0f);
                return;
            }
            tt = std::max(q.start, tt);
            trans = (q.end - tt) / (q.end - q.start);
            break;
        case SeqState::trans_dest:
            if (tt >= q.end) {
                seq_set_state(i, SeqState::animating);
            } else {
                tt = std::max(q.start, tt);
                trans = (tt - q.start) / (q.end - q.start);
            }
            break;
        default:
            break;
        }
        float scaled = seq_scaled_time(q, tt + q.offset);
        float w = trans * q.weight;
        for (auto& b : q.blocks) {
            if (b.blend == k_none || b.interp == k_none) {
                continue;
            }
            auto& blend = interps[b.blend];
            if (blend.count == 1 && b.blend_index == blend.single) {
                blend.single_time = scaled;
            } else if (b.blend_index < blend.items.size()) {
                auto& item = blend.items[b.blend_index];
                item.time = scaled;
                item.weight = w;
                item.ease = ease;
                blend.flags |= 4;
                blend.high_sum = blend.next_sum = blend.high_ease = k_neg_max;
            }
        }
    }

    // ---- Output.

    void build_output() {
        for (auto& s : systems) {
            s.out.resize(s.active);
            float world_scale = s.world.s;
            for (uint16_t i = 0; i < s.active; ++i) {
                auto& p = s.out[i];
                const auto& info = s.info[i];
                // Through the system's world transform even in world space, which keeps the scale (compute_world): positions
                // are held divided by it, so a reference scaled 2 away from the origin would otherwise spray from half way there.
                p.position = apply(s.world, s.positions[i]) * k_unit_m;
                p.velocity = (s.world.rot * info.velocity) * s.world.s * k_unit_m;
                p.radius = s.radii[i] * world_scale * k_unit_m;
                p.size = s.sizes[i];
                p.color = s.colors.empty() ? glm::vec4(1.0f) : s.colors[i];
                p.rotation = s.angles.empty() ? 0.0f : s.angles[i];
                p.rotation_axis = s.axes.empty() ? glm::vec3(0.0f) : s.axes[i];
                p.frame = s.tex.empty() ? 0 : s.tex[i];
                if (s.def->subtexture_count && p.frame < s.def->subtexture_count) {
                    const auto& r = e.subtextures[s.def->first_subtexture + p.frame];
                    p.uv_rect = glm::vec4(r[0], r[1], r[2], r[3]);
                } else {
                    p.uv_rect = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
                }
                p.age = info.age;
                p.life_span = info.life_span;
            }
        }
    }
};

std::shared_ptr<const Effect> load_effect(std::span<const std::byte> ose_file) {
    return make_effect(ose::read(ose_file));
}

std::shared_ptr<const Effect> make_effect(ose::Effect data) {
    auto e = std::make_shared<Effect>();
    e->data = std::move(data);
    return e;
}

EffectInstance::EffectInstance(std::shared_ptr<const Effect> effect, uint32_t seed)
    : m_state(std::make_unique<State>(std::move(effect))) {
    auto& st = *m_state;
    const auto& e = st.e;
    st.bs.seed(seed, st.crt);
    st.local.resize(e.nodes.size());
    for (size_t i = 0; i < e.nodes.size(); ++i) {
        const auto& n = e.nodes[i];
        std::memcpy(&st.local[i].rot, n.rotation, sizeof n.rotation);
        st.local[i].t = glm::vec3(n.translation[0], n.translation[1], n.translation[2]);
        st.local[i].s = n.scale;
    }
    st.systems.resize(e.systems.size());
    for (size_t i = 0; i < e.systems.size(); ++i) {
        auto& s = st.systems[i];
        s.def = &e.systems[i];
        s.max = s.def->max_particles;
        size_t n = s.max;
        uint16_t f = s.def->flags;
        s.positions.assign(n, glm::vec3(0.0f));
        if (f & ose::system_colors) {
            s.colors.assign(n, glm::vec4(0.0f));
        }
        s.radii.assign(n, 1.0f); // always allocated, 1.0
        s.sizes.assign(n, 1.0f);
        if (f & ose::system_rotations) {
            s.rotations.assign(n, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        }
        if (f & ose::system_rotation_angles) {
            s.angles.assign(n, 0.0f);
        }
        if (f & ose::system_rotation_axes) {
            s.axes.assign(n, glm::vec3(0.0f));
        }
        if (f & ose::system_texture_indices) {
            s.tex.assign(n, 0);
        }
        if (f & ose::system_rotation_speeds) {
            s.speeds.assign(n, 0.0f);
        }
        s.info.assign(n, ParticleInfo{});
        for (uint32_t k = 0; k < s.def->modifier_count; ++k) {
            s.mods.push_back(s.def->first_modifier + k);
        }
    }
    st.modifiers.resize(e.modifiers.size());
    for (size_t i = 0; i < e.modifiers.size(); ++i) {
        auto& m = st.modifiers[i];
        m.def = &e.modifiers[i];
        m.system = m.def->system;
        m.active = m.def->active != 0;
        if (m.def->type == ose::ModifierType::bound_update) {
            State::set_update_skip(m, static_cast<int16_t>(m.def->u0));
        }
    }
    st.interps.resize(e.interpolators.size());
    for (size_t i = 0; i < e.interpolators.size(); ++i) {
        auto& s = st.interps[i];
        s.def = &e.interpolators[i];
        s.fvalue = s.def->float_value;
        s.bvalue = s.def->bool_value;
        s.flags = s.def->blend_flags;
        s.items.resize(s.def->array_size);
        if (s.def->type == ose::InterpolatorType::transform) {
            const auto& t = e.transforms[s.def->key_group];
            s.tvalue = glm::vec3(t.translation[0], t.translation[1], t.translation[2]);
            s.qvalue = glm::vec4(t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]);
            s.svalue = t.scale;
        }
    }
    st.ctlrs.resize(e.controllers.size());
    for (size_t i = 0; i < e.controllers.size(); ++i) {
        auto& c = st.ctlrs[i];
        c.def = &e.controllers[i];
        c.flags = c.def->flags;
        c.lo = c.def->start_time;
        c.hi = c.def->stop_time;
    }
    // One manager-controlled blend transform interpolator per target of each NiMultiTargetTransformController.
    st.ctlr_blends.resize(e.controllers.size());
    for (size_t i = 0; i < e.controllers.size(); ++i) {
        const auto& c = e.controllers[i];
        if (c.type != ose::ControllerType::transform) {
            continue;
        }
        for (uint32_t k = 0; k < c.target_count; ++k) {
            ose::Interpolator def{};
            def.type = static_cast<ose::InterpolatorType>(k_blend_transform);
            def.float_value = k_neg_max;
            def.bool_value = 2;
            def.blend_flags = 1; // manager controlled
            def.key_group = k_none;
            st.runtime_interps.push_back(def);
            InterpState is;
            is.def = &st.runtime_interps.back();
            is.flags = def.blend_flags;
            st.ctlr_blends[i].push_back(static_cast<uint32_t>(st.interps.size()));
            st.interps.push_back(std::move(is));
        }
    }
    st.seqs.resize(e.sequences.size());
    for (size_t i = 0; i < e.sequences.size(); ++i) {
        auto& q = st.seqs[i];
        q.def = &e.sequences[i];
        q.weight = q.def->weight;
        for (uint32_t k = 0; k < q.def->block_count; ++k) {
            const auto& b = e.blocks[q.def->first_block + k];
            SeqBlock sb;
            sb.interp = b.interpolator;
            sb.priority = b.priority;
            if (b.controller != k_none) {
                const auto& c = e.controllers[b.controller];
                // The block drives the controller's interpolator that its interpolator ID names; the emitter
                // controller has two ("BirthRate" and "EmitterActive").
                std::string_view id = e.string(b.interpolator_id);
                if (c.type == ose::ControllerType::transform) {
                    // The target whose node the block's ID tag names.
                    for (uint32_t t = 0; t < c.target_count; ++t) {
                        const uint32_t n = e.controller_targets[c.first_target + t];
                        if (n != k_none && e.string(e.nodes[n].name) == e.string(b.node_name)) {
                            sb.blend = st.ctlr_blends[b.controller][t];
                        }
                    }
                    q.blocks.push_back(sb);
                    continue;
                }
                if (c.type == ose::ControllerType::emitter && id == "EmitterActive") {
                    sb.blend = c.interpolator2;
                } else {
                    sb.blend = c.interpolator;
                }
                if (sb.blend != k_none) {
                    auto t = e.interpolators[sb.blend].type;
                    if (t != ose::InterpolatorType::blend_float && t != ose::InterpolatorType::blend_bool) {
                        sb.blend = k_none;
                    }
                }
            }
            q.blocks.push_back(sb);
        }
    }
    st.materials.resize(e.materials.size());
    for (size_t i = 0; i < e.materials.size(); ++i) {
        auto& m = st.materials[i];
        const auto& src = e.materials[i];
        m.source = &src;
        m.base_color = glm::vec4(src.base_color[0], src.base_color[1], src.base_color[2], src.base_color[3]);
        m.base_color_scale = src.base_color_scale;
        m.falloff = glm::vec4(src.falloff[0], src.falloff[1], src.falloff[2], src.falloff[3]);
        m.uv_offset = glm::vec2(src.uv_offset[0], src.uv_offset[1]);
        m.uv_scale = glm::vec2(src.uv_scale[0], src.uv_scale[1]);
    }
    st.compute_world();
}

EffectInstance::~EffectInstance() = default;
EffectInstance::EffectInstance(EffectInstance&&) noexcept = default;
EffectInstance& EffectInstance::operator=(EffectInstance&&) noexcept = default;

const ose::Effect& EffectInstance::data() const {
    return m_state->e;
}

void EffectInstance::set_root_transform(const glm::mat3& rotation, const glm::vec3& translation_m,
                                        float scale) {
    auto& st = *m_state;
    st.root_override.rot = rotation;
    st.root_override.t = translation_m / k_unit_m;
    st.root_override.s = scale;
    st.has_root_override = true;
    st.compute_world();
}

void EffectInstance::set_lod_distance(float value) {
    m_state->lod_distance = value;
}

float EffectInstance::lod_distance(const glm::vec3& camera_m, const glm::vec3& bound_center_m,
                                   float lod_adjust, float fade_mult) {
    glm::vec3 d = (bound_center_m - camera_m) / k_unit_m;
    float dist = std::sqrt((d.y * d.y + d.x * d.x) + d.z * d.z);
    float factor = fade_mult <= 0.0f ? 0.2f : lod_adjust / fade_mult;
    return dist * factor * 0.00013888889f;
}

void EffectInstance::set_density_scale(float scale) {
    m_state->density = scale;
}

namespace {

uint32_t find_sequence(const ose::Effect& e, std::string_view name) {
    for (size_t i = 0; i < e.sequences.size(); ++i) {
        if (e.string(e.sequences[i].name) == name) {
            return static_cast<uint32_t>(i);
        }
    }
    return k_none;
}

} // namespace

bool EffectInstance::activate_sequence(std::string_view name, float weight, float ease_in, uint8_t priority) {
    auto i = find_sequence(m_state->e, name);
    return i != k_none && m_state->seq_activate(i, priority, true, weight, ease_in);
}

bool EffectInstance::deactivate_sequence(std::string_view name, float ease_out) {
    auto i = find_sequence(m_state->e, name);
    return i != k_none && m_state->seq_deactivate(i, ease_out);
}

void EffectInstance::pose_sequence(std::string_view name, float key_time) {
    if (const auto i = find_sequence(m_state->e, name); i != k_none) {
        m_state->seqs[i].posed = key_time;
    }
}

bool EffectInstance::sequence_range(std::string_view name, float& begin, float& end) const {
    const auto i = find_sequence(m_state->e, name);
    if (i == k_none) {
        return false;
    }
    begin = m_state->e.sequences[i].start_time;
    end = m_state->e.sequences[i].stop_time;
    return true;
}

void EffectInstance::set_sequence_weight(std::string_view name, float weight) {
    auto i = find_sequence(m_state->e, name);
    if (i != k_none) {
        m_state->seqs[i].weight = std::max(weight, 0.0f);
    }
}

bool EffectInstance::sequence_active(std::string_view name) const {
    auto i = find_sequence(m_state->e, name);
    return i != k_none && m_state->seqs[i].state != SeqState::inactive;
}

float EffectInstance::sequence_time(std::string_view name) const {
    auto i = find_sequence(m_state->e, name);
    if (i == k_none || m_state->seqs[i].state == SeqState::inactive) {
        return -1.0f;
    }
    return m_state->seqs[i].last_scaled;
}

float EffectInstance::sequence_duration(std::string_view name) const {
    auto i = find_sequence(m_state->e, name);
    if (i == k_none) {
        return -1.0f;
    }
    const auto& s = m_state->e.sequences[i];
    return (s.stop_time - s.start_time) / s.frequency;
}

std::vector<std::string> EffectInstance::sequence_names() const {
    std::vector<std::string> out;
    for (const auto& s : m_state->e.sequences) {
        out.emplace_back(m_state->e.string(s.name));
    }
    return out;
}

void EffectInstance::update(float time) {
    auto& st = *m_state;
    m_time = time;
    // The game (the root's first controller), then every controller in scene order.
    for (size_t k = 0; k < st.active_seqs.size();) {
        uint32_t i = st.active_seqs[k];
        st.seq_update(i, time);
        if (st.seqs[i].state == SeqState::inactive) {
            st.active_seqs.erase(st.active_seqs.begin() + static_cast<std::ptrdiff_t>(k));
        } else {
            ++k;
        }
    }
    for (size_t ci = 0; ci < st.ctlrs.size(); ++ci) {
        auto& c = st.ctlrs[ci];
        switch (c.def->type) {
        case ose::ControllerType::transform:
            if (st.update_transform_ctlr(ci)) {
                st.compute_world();
            }
            break;
        case ose::ControllerType::modifier_active:
            st.update_modifier_active(c, time);
            break;
        case ose::ControllerType::emitter:
            st.update_emitter_ctlr(c, time);
            break;
        case ose::ControllerType::update:
            st.update_update_ctlr(c, time);
            break;
        case ose::ControllerType::effect_float:
            st.update_effect_float(c, time);
            break;
        default:
            break;
        }
    }
    st.build_output();
}

uint32_t EffectInstance::find_node(std::string_view name) const {
    const auto& e = m_state->e;
    for (size_t i = 0; i < e.nodes.size(); ++i) {
        if (e.string(e.nodes[i].name) == name) {
            return static_cast<uint32_t>(i);
        }
    }
    return k_none;
}

glm::mat3 EffectInstance::node_world_rotation(uint32_t node) const {
    return m_state->world.at(node).rot;
}

glm::vec3 EffectInstance::node_world_translation_units(uint32_t node) const {
    return m_state->world.at(node).t;
}

float EffectInstance::node_world_scale(uint32_t node) const {
    return m_state->world.at(node).s;
}

size_t EffectInstance::system_count() const {
    return m_state->systems.size();
}

std::span<const Particle> EffectInstance::particles(size_t system) const {
    return m_state->systems.at(system).out;
}

size_t EffectInstance::particle_count(size_t system) const {
    return m_state->systems.at(system).active;
}

Bound EffectInstance::bound(size_t system) const {
    const auto& s = m_state->systems.at(system);
    Bound b;
    b.center = apply(s.world, s.bound_center) * k_unit_m;
    b.radius = s.bound_radius * s.world.s * k_unit_m;
    return b;
}

const MaterialState& EffectInstance::system_material(size_t system) const {
    return m_state->materials.at(m_state->e.systems.at(system).material);
}

size_t EffectInstance::material_count() const {
    return m_state->materials.size();
}

const MaterialState& EffectInstance::material(size_t index) const {
    return m_state->materials.at(index);
}

uint64_t EffectInstance::state_hash() const {
    const auto& st = *m_state;
    uint64_t h = 1469598103934665603ull;
    uint32_t crt = st.crt.state();
    h = mix(h, &crt, sizeof crt);
    uint64_t bs = st.bs.hash();
    h = mix(h, &bs, sizeof bs);
    for (const auto& s : st.systems) {
        h = mix(h, &s.active, sizeof s.active);
        h = mix(h, &s.added, sizeof s.added);
        h = mix(h, &s.last_time, sizeof s.last_time);
        h = mix(h, &s.emit_scale, sizeof s.emit_scale);
        h = mix(h, &s.bound_center, sizeof s.bound_center);
        h = mix(h, &s.bound_radius, sizeof s.bound_radius);
        h = mix_vec(h, s.positions);
        h = mix_vec(h, s.colors);
        h = mix_vec(h, s.radii);
        h = mix_vec(h, s.sizes);
        h = mix_vec(h, s.angles);
        h = mix_vec(h, s.axes);
        h = mix_vec(h, s.tex);
        h = mix_vec(h, s.speeds);
        for (const auto& info : s.info) {
            h = mix(h, &info.velocity, sizeof info.velocity);
            h = mix(h, &info.age, sizeof info.age);
            h = mix(h, &info.life_span, sizeof info.life_span);
            h = mix(h, &info.last_update, sizeof info.last_update);
            h = mix(h, &info.subtex_counter, sizeof info.subtex_counter);
            h = mix(h, &info.subtex_frames, sizeof info.subtex_frames);
        }
    }
    for (const auto& m : st.modifiers) {
        h = mix(h, &m.active, sizeof m.active);
        h = mix(h, &m.carry, sizeof m.carry);
    }
    for (const auto& c : st.ctlrs) {
        h = mix(h, &c.scaled, sizeof c.scaled);
        h = mix(h, &c.carry, sizeof c.carry);
        h = mix(h, &c.last_seq_time, sizeof c.last_seq_time);
    }
    for (const auto& m : st.materials) {
        h = mix(h, &m.base_color, sizeof m.base_color);
        h = mix(h, &m.uv_offset, sizeof m.uv_offset);
    }
    return h;
}

} // namespace spud::fx
