// The modifiers and foot placement. Lengths are game units, as in the
// graphs.

#include "anim/graph.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numbers>

namespace spud::anim {

namespace {

constexpr float k_pi = std::numbers::pi_v<float>;
constexpr float k_epsilon = FLT_EPSILON;
// Beyond this, two unit vectors are treated as parallel by the ground alignment ().
constexpr float k_parallel = 0.99996901f;

glm::vec3 xyz(const glm::vec4& v) {
    return {v.x, v.y, v.z};
}
glm::vec3 xyz(const glm::quat& q) {
    return {q.x, q.y, q.z};
}

// A zero vector stays zero, so the "is it a unit vector" tests below reject it.
glm::vec3 normalized(const glm::vec3& v) {
    const float length_squared = glm::dot(v, v);
    return length_squared > 0.0f ? v / std::sqrt(length_squared) : glm::vec3(0.0f);
}

float angle_of(float cosine) {
    return cosine <= -1.0f ? k_pi : cosine >= 1.0f ? 0.0f : std::acos(cosine);
}

bool is_unit(const glm::vec3& v) {
    return std::fabs(glm::length(v) - 1.0f) < 0.01f;
}

bool differs(const glm::vec3& a, const glm::vec3& b, float tolerance) {
    const glm::vec3 d = glm::abs(a - b);
    return d.x > tolerance || d.y > tolerance || d.z > tolerance;
}

// A gain is defined per 1/30 s frame ().
float gain_fraction(float gain, float dt) {
    return std::clamp(gain * dt * 30.0f, 0.0f, 1.0f);
}

// The specs' A ∘ B: unlike a point's transform, Havok's product leaves scale out of the translation.
Qs compose(const Qs& a, const Qs& b) {
    return {a.t + a.r * b.t, a.r * b.r, a.s * b.s};
}
Qs inverse(const Qs& a) {
    const glm::quat r = glm::conjugate(a.r);
    return {-(r * a.t), r, 1.0f / a.s};
}
glm::vec3 to_world(const Qs& w, const glm::vec3& p) {
    return w.t + w.r * (w.s * p);
}
glm::vec3 to_model(const Qs& w, const glm::vec3& p) {
    return (glm::conjugate(w.r) * (p - w.t)) / w.s;
}

// Havok's approximation. BSDirectAtModifier's angles depend on it, by a few milliradians.
float havok_atan2(float y, float x) {
    const float ay = std::fabs(y);
    const float ax = std::fabs(x);
    const float t = std::min(ay, ax) / (std::max(ay, ax) + k_epsilon);
    float a = t - 0.121079f * t * t - 0.093523f * t * t * t;
    if (ay > ax) {
        a = k_pi / 2.0f - a;
    }
    if (x < 0.0f) {
        a = k_pi - a;
    }
    return y < 0.0f ? -a : a;
}

float wrap_once(float angle) {
    if (angle > k_pi) {
        return angle - 2.0f * k_pi;
    }
    return angle < -k_pi ? angle + 2.0f * k_pi : angle;
}

// The rotation from one unit vector to another, damped by `gain`. Exactly opposite vectors give
// no usable axis and stay put: a leg folded straight back on itself, which the knee limits rule out.
glm::quat shortest_rotation(const glm::vec3& from, const glm::vec3& to, float gain = 1.0f) {
    const float d = glm::dot(from, to);
    const float w_squared = (d * gain + (1.0f - gain) + 1.0f) * 0.5f;
    if (d >= 1.0f || w_squared <= 0.0f) {
        return {1.0f, 0.0f, 0.0f, 0.0f};
    }
    const float w = std::sqrt(w_squared);
    const glm::vec3 v = glm::cross(from, to) * (gain / (2.0f * w));
    return glm::normalize(glm::quat(w, v.x, v.y, v.z));
}

// the shorter way, linear when the two are close.
glm::quat slerp_shorter(const glm::quat& a, const glm::quat& b, float t) {
    float d = glm::dot(a, b);
    const glm::quat end = d < 0.0f ? -b : b;
    d = std::fabs(d);
    if (d > 0.999f) {
        return glm::normalize(a * (1.0f - t) + end * t);
    }
    const float theta = std::acos(d);
    return (a * std::sin((1.0f - t) * theta) + end * std::sin(t * theta)) / std::sin(theta);
}

// Missing bones take the reference pose, as the pose fetch does before any modifier reads them.
void fill_missing(const Skeleton& skeleton, Output& output) {
    if (output.all_present) {
        return;
    }
    for (size_t b = 0; b < output.pose.size(); ++b) {
        if (!output.present[b]) {
            output.pose[b] = b < skeleton.reference.size() ? skeleton.reference[b] : Qs{};
        }
    }
    std::fill(output.present.begin(), output.present.end(), uint8_t{1});
    output.all_present = true;
}

// The pose object ("Pose access"): the track's local transforms, with model space worked out when asked for.
// Writing a bone in model space keeps its descendants' local transforms, so they move with it. Writes go
// straight to the track, so there's nothing to copy back; building one sets every present bit, as the copy
// back would. Skeletons list parents first.
class ModelPose {
  public:
    ModelPose(const Skeleton& skeleton, Output& output)
        : m_skeleton(skeleton), m_pose(output.pose), m_model(output.pose.size()),
          m_valid(output.pose.size(), 0) {
        fill_missing(skeleton, output);
    }

    int32_t size() const { return static_cast<int32_t>(m_pose.size()); }
    const Qs& local(int32_t bone) const { return m_pose[size_t(bone)]; }
    int32_t parent(int32_t bone) const { return m_skeleton.parents[size_t(bone)]; }
    const Qs& model(int32_t bone) {
        if (!m_valid[size_t(bone)]) {
            const int32_t p = parent(bone);
            m_model[size_t(bone)] = p >= 0 ? compose(model(p), local(bone)) : local(bone);
            m_valid[size_t(bone)] = 1;
        }
        return m_model[size_t(bone)];
    }
    void set_local(int32_t bone, const Qs& transform) {
        m_pose[size_t(bone)] = transform;
        invalidate(bone);
    }
    void set_model(int32_t bone, const Qs& transform) {
        const int32_t p = parent(bone);
        set_local(bone, p >= 0 ? compose(inverse(model(p)), transform) : transform);
    }
    void set_model_rotation(int32_t bone, const glm::quat& rotation) {
        Qs transform = model(bone);
        transform.r = rotation;
        set_model(bone, transform);
    }

  private:
    void invalidate(int32_t bone) {
        m_valid[size_t(bone)] = 0;
        for (int32_t b = bone + 1; b < size(); ++b) {
            const int32_t p = parent(b);
            if (p >= 0 && !m_valid[size_t(p)]) {
                m_valid[size_t(b)] = 0;
            }
        }
    }

    const Skeleton& m_skeleton;
    std::vector<Qs>& m_pose;
    std::vector<Qs> m_model;
    std::vector<uint8_t> m_valid;
};

// Track 0 as a modifier reads it. Every generator writes it, so the character's own is only for an empty
// output.
const Qs& track_0(const Context& ctx, const Output& output) {
    return output.world_from_model_fraction > 0.0f ? output.world_from_model : ctx.character.world_from_model;
}

// start first. Leaves `chain` alone when the walk falls off the skeleton.
bool bone_chain(const Skeleton& skeleton, int32_t start, int32_t end, std::vector<int32_t>& chain) {
    if (end < 0 || size_t(end) >= skeleton.size()) {
        return false;
    }
    std::vector<int32_t> walked;
    for (int32_t b = end; b != start && b != 0;) {
        walked.push_back(b);
        b = skeleton.parents[size_t(b)];
        if (b < 0) {
            return false;
        }
    }
    walked.push_back(start);
    chain.assign(walked.rbegin(), walked.rend());
    return true;
}

// A ray in world space. The contract gives a fraction, the character's ray gives the hit point.
bool cast(Character& character, uint32_t filter, const glm::vec3& from, const glm::vec3& to, float& fraction,
          glm::vec3& normal) {
    if (!character.cast_ray) {
        return false;
    }
    character.ray_filter_info = filter;
    glm::vec3 hit{0.0f};
    if (!character.cast_ray(from, to, hit, normal)) {
        return false;
    }
    const glm::vec3 ray = to - from;
    fraction = glm::dot(hit - from, ray) / glm::dot(ray, ray);
    return true;
}

// Helpers the classes hold by value, so that clones get their own.

struct BoneIndexArray final : Bindable {
    std::vector<int32_t> indices;
    void describe(Describer& d) override { d.member("boneIndices", indices); }
};

// Foot placement. Gains are kept in file order: track 11 is the same twelve floats.

enum FootGain : size_t {
    gain_on_off,
    gain_ground_ascending,
    gain_ground_descending,
    gain_foot_planted,
    gain_foot_raised,
    gain_foot_unlock,
    gain_feedback,
    gain_error_up_down_bias,
    gain_align,
    gain_hip_orientation,
    gain_max_knee_difference,
    gain_ankle_orientation,
    foot_gain_count,
};
using FootGains = std::array<float, foot_gain_count>;

struct FootGainsData final : Bindable {
    FootGains gains{};
    void describe(Describer& d) override {
        d.member("onOffGain", gains[gain_on_off]);
        d.member("groundAscendingGain", gains[gain_ground_ascending]);
        d.member("groundDescendingGain", gains[gain_ground_descending]);
        d.member("footPlantedGain", gains[gain_foot_planted]);
        d.member("footRaisedGain", gains[gain_foot_raised]);
        d.member("footUnlockGain", gains[gain_foot_unlock]);
        d.member("worldFromModelFeedbackGain", gains[gain_feedback]);
        d.member("errorUpDownBias", gains[gain_error_up_down_bias]);
        d.member("alignWorldFromModelGain", gains[gain_align]);
        d.member("hipOrientationGain", gains[gain_hip_orientation]);
        d.member("maxKneeAngleDifference", gains[gain_max_knee_difference]);
        d.member("ankleOrientationGain", gains[gain_ankle_orientation]);
    }
};

// What the two forms of foot placement take from their data.
struct FootParams {
    float ray_up = 0.0f;
    float ray_down = 0.0f;
    float original_ground = 0.0f;
    float vertical_offset = 0.0f;
    uint32_t filter = 0;
    float forward_align = 0.0f;
    float sideways_align = 0.0f;
    float sideways_width = 0.0f;
    bool lock_feet = false;
    bool use_character_up = false;
    bool slope_alignment = false; // isQuadrupedNarrow, or ALIGN_MODE_FORWARD
    bool node = false; // hkbFootIkModifier rather than the driver: the differences its section lists
};

// hkaFootPlacementIkSolver.
struct FootSolver {
    // Setup.
    int32_t hip = -1, knee = -1, ankle = -1;
    glm::vec3 knee_axis{0.0f};
    glm::vec3 foot_end{0.0f};
    glm::vec3 world_up{0.0f, 0.0f, 1.0f};
    glm::vec3 model_up{0.0f, 0.0f, 1.0f};
    float original_ground = 0.0f;
    float height_planted = 0.0f, height_raised = 0.0f, height_max = 0.0f, height_min = 0.0f;
    float cos_max_knee = 0.0f, cos_min_knee = 0.0f;
    float ray_up = 0.0f, ray_down = 0.0f;
    bool use_locking = false;
    // State.
    float weight = 0.0f;
    float ground_height = 0.0f;
    glm::vec3 ground_normal{0.0f, 0.0f, 1.0f};
    float displacement = 0.0f;
    bool locked = false;
    glm::vec3 locked_ankle{0.0f};
    glm::vec3 locked_foot_end{0.0f};
    glm::vec3 unlock_offset{0.0f};

    struct Result {
        glm::vec3 normal{0.0f};
        float vertical_error = 0.0f;
        bool hit = false;
    };

    // Of the rays at the ankle and the foot end, the fraction along the ankle's ray of the higher ground.
    bool cast_rays(Character& character, uint32_t filter, const glm::vec3& ankle_ws,
                   const glm::vec3& foot_end_ws, float& fraction, glm::vec3& normal) const {
        const glm::vec3 lift = world_up * ray_up;
        const glm::vec3 drop = world_up * ray_down;
        float first = 0.0f;
        glm::vec3 first_normal{0.0f};
        const bool first_hit = cast(character, filter, ankle_ws + lift, ankle_ws - drop, first, first_normal);
        float second = 0.0f;
        glm::vec3 second_normal{0.0f};
        bool second_hit = false;
        if (differs(foot_end_ws, ankle_ws, 0.001f)) {
            const glm::vec3 from = foot_end_ws + lift;
            const glm::vec3 to = foot_end_ws - drop;
            float raw = 0.0f;
            glm::vec3 n{0.0f};
            if (cast(character, filter, from, to, raw, n) && glm::dot(world_up, n) > 0.0f) {
                const glm::vec3 point = from + (to - from) * raw;
                second = glm::dot(ankle_ws + lift - point, n) / (glm::dot(world_up, n) * (ray_up + ray_down));
                second_normal = n;
                second_hit = true;
            }
        }
        if (!first_hit && !second_hit) {
            return false;
        }
        const bool use_first = first_hit && (!second_hit || first <= second);
        fraction = use_first ? first : second;
        normal = use_first ? first_normal : second_normal;
        return true;
    }

    // hkaTwoJointsIkSolver, enforcing position only, with every gain equal to `gain`.
    void solve_leg(ModelPose& pose, const glm::vec3& target, float max_angle_change, float gain) const {
        const glm::vec3 h = pose.model(hip).t;
        const glm::vec3 k = pose.model(knee).t;
        const glm::vec3 e = pose.model(ankle).t;
        const glm::quat knee_rotation = pose.model(knee).r;
        const glm::vec3 axis = normalized(knee_rotation * knee_axis);
        const glm::vec3 upper = axis * glm::dot(k - h, axis);
        const glm::vec3 lower = axis * glm::dot(e - k, axis);
        const glm::vec3 u = (k - h) - upper;
        const glm::vec3 v = (e - k) - lower;
        const glm::vec3 w = upper - lower;
        const float l1 = glm::length(u);
        const float l2 = glm::length(v);
        if (l1 <= 0.0f || l2 <= 0.0f) {
            return;
        }
        const glm::vec3 to_target = target - h;
        const float cosine =
            (glm::dot(to_target, to_target) - glm::dot(w, w) - l1 * l1 - l2 * l2) / (-2.0f * l1 * l2);
        const float wanted = angle_of(std::clamp(cosine, cos_max_knee, cos_min_knee));
        const float current = angle_of(glm::dot(normalized(-u), normalized(v)));
        float delta = wanted - current;
        if (max_angle_change > 0.0f) {
            const float limit = glm::radians(max_angle_change);
            delta = std::clamp(delta, -limit, limit);
        }
        delta *= gain;
        pose.set_model_rotation(knee, glm::normalize(glm::angleAxis(delta, axis) * knee_rotation));

        const glm::vec3 end = pose.model(ankle).t;
        const glm::quat q = shortest_rotation(normalized(end - h), normalized(target - h), gain);
        pose.set_model_rotation(hip, glm::normalize(q * pose.model(hip).r));
    }

    // doFootPlacement.
    Result place(Character& character, ModelPose& pose, const Qs& w, const Qs& original, bool on,
                 const FootGains& gains, float max_angle_change, uint32_t filter) {
        const glm::vec3& up = world_up;
        const glm::vec3 up_ms = glm::conjugate(w.r) * up;
        const float animated_height = glm::dot(model_up, original.t);
        glm::vec3 ankle_ms = pose.model(ankle).t;
        glm::vec3 ankle_ws = to_world(w, ankle_ms);
        const auto foot_end_ws = [&](const glm::vec3& position) {
            const Qs& transform = pose.model(ankle);
            return to_world(w, position + transform.r * (foot_end * transform.s));
        };

        Result result;
        result.normal = ground_normal;
        const bool was_locked = locked;
        float fraction = 0.0f;
        glm::vec3 hit_normal{0.0f};
        bool hit = on;
        if (hit) {
            if (use_locking) {
                locked = false;
                if (animated_height < height_planted) {
                    if (was_locked) {
                        ankle_ws = locked_ankle;
                        ankle_ms = to_model(w, ankle_ws);
                    } else {
                        locked_ankle = ankle_ws;
                        locked_foot_end = foot_end_ws(ankle_ms);
                    }
                    locked = true;
                }
            }
            const glm::vec3 end = locked ? locked_foot_end : foot_end_ws(ankle_ms);
            hit = cast_rays(character, filter, ankle_ws, end, fraction, hit_normal);
        }

        float weight_target = 0.0f;
        float smoothed_height = 0.0f;
        glm::vec3 target{0.0f};
        if (hit) {
            const glm::vec3 ground = ankle_ws - up * (fraction * (ray_up + ray_down) - ray_up);
            if (locked) {
                locked_ankle = ground;
                ankle_ws = ground;
                ankle_ms = to_model(w, ankle_ws);
                locked_foot_end = foot_end_ws(ankle_ms);
            }
            const float height = glm::dot(up, ground);
            result.vertical_error = height - (glm::dot(up, w.t) + original_ground);

            glm::vec3 horizontal = ground - up * height;
            const float planted = std::clamp(
                1.0f - (animated_height - height_planted) / (height_raised - height_planted), 0.0f, 1.0f);
            const float foot_gain =
                (1.0f - planted) * gains[gain_foot_raised] + planted * gains[gain_foot_planted];
            smoothed_height = height;
            if (weight != 0.0f) {
                const float rise =
                    height >= ground_height ? gains[gain_ground_ascending] : gains[gain_ground_descending];
                smoothed_height = ground_height + rise * foot_gain * (height - ground_height);
            }

            if (use_locking) {
                if (locked) {
                    unlock_offset = glm::vec3(0.0f);
                } else {
                    if (was_locked) {
                        const glm::vec3 d = locked_ankle - horizontal;
                        unlock_offset = d - up * glm::dot(up, d);
                    }
                    horizontal += unlock_offset;
                    unlock_offset *= 1.0f - gains[gain_foot_unlock];
                }
            }

            target =
                to_model(w, up * smoothed_height + horizontal) + up_ms * (animated_height - original_ground);
            const float target_height = glm::dot(model_up, target);
            if (target_height > height_max) {
                target -= model_up * (target_height - height_max);
            }
            weight_target = 1.0f;
            result.hit = true;
            result.normal = hit_normal;
            if (target_height < height_min) {
                // Too low to reach: this counts as a miss after all.
                result.hit = false;
                result.vertical_error = 0.0f;
                result.normal = ground_normal;
                target = ankle_ms + up_ms * displacement;
                smoothed_height = glm::dot(up, ankle_ws) + displacement - animated_height;
                weight_target = 0.0f;
            }
            displacement = glm::dot(model_up, target - ankle_ms);
        } else {
            target = ankle_ms + up_ms * displacement;
            smoothed_height = displacement + glm::dot(up, ankle_ws) - animated_height;
            locked = false;
        }

        weight += (weight_target - weight) * gains[gain_on_off];
        if (weight < 0.01f) {
            weight = 0.0f;
        }
        if (weight <= 0.0f) {
            result.vertical_error = 0.0f;
        } else {
            solve_leg(pose, target, max_angle_change, weight);
            const glm::quat q = shortest_rotation(up_ms, glm::conjugate(w.r) * result.normal);
            pose.set_model_rotation(ankle,
                                    glm::normalize(original.r * (1.0f - weight) + (q * original.r) * weight));
        }
        ground_height = smoothed_height;
        ground_normal = result.normal;
        return result;
    }
};

// One leg of a running foot placement.
struct FootLeg {
    FootSolver solver;
    glm::vec3 ground_position{0.0f};
    glm::vec3 ground_normal{0.0f};
    float vertical_error = 0.0f;
    bool hit_something = false;
    bool is_planted_ms = false;
    float planted_height = 0.0f;  // footPlantedAnkleHeightMS, unscaled
    float max_ankle_angle = 0.0f; // radians
    Qs original;
    bool original_latched = false;
    // The game keeps this in the shared hkbFootIkDriverInfoLeg. Kept per character, as the design advises until
    // probed.
    glm::quat prev_ankle_rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// A leg's data in the file: both hkbFootIkDriverInfoLeg and hkbFootIkModifierLeg have these.
struct FootLegData : Bindable {
    glm::vec4 knee_axis{0.0f};
    glm::vec4 foot_end{0.0f};
    float planted_height = 0.0f;
    float raised_height = 0.0f;
    float max_height = 0.0f;
    float min_height = 0.0f;
    float max_knee_degrees = 0.0f;
    float min_knee_degrees = 0.0f;
    float max_ankle_degrees = 0.0f;
    int16_t hip = -1;
    int16_t knee = -1;
    int16_t ankle = -1;

    void describe(Describer& d) override {
        d.member("kneeAxisLS", knee_axis);
        d.member("footEndLS", foot_end);
        d.member("footPlantedAnkleHeightMS", planted_height);
        d.member("footRaisedAnkleHeightMS", raised_height);
        d.member("maxAnkleHeightMS", max_height);
        d.member("minAnkleHeightMS", min_height);
        d.member("maxKneeAngleDegrees", max_knee_degrees);
        d.member("minKneeAngleDegrees", min_knee_degrees);
        d.member("maxAnkleAngleDegrees", max_ankle_degrees);
        d.member("hipIndex", hip);
        d.member("kneeIndex", knee);
        d.member("ankleIndex", ankle);
    }

    bool in_range(int32_t bones) const {
        return hip >= 0 && hip < bones && knee >= 0 && knee < bones && ankle >= 0 && ankle < bones;
    }
};

// `scale` applies to heights and rays, not to originalGroundHeightMS.
FootLeg make_leg(const FootLegData& data, const FootParams& params, const Project& project, float scale) {
    FootLeg leg;
    FootSolver& s = leg.solver;
    s.hip = data.hip;
    s.knee = data.knee;
    s.ankle = data.ankle;
    s.knee_axis = xyz(data.knee_axis);
    s.foot_end = xyz(data.foot_end) * scale;
    s.world_up = project.world_up();
    s.model_up = project.model_up();
    s.original_ground = params.original_ground;
    s.height_planted = data.planted_height * scale;
    s.height_raised = data.raised_height * scale;
    s.height_max = data.max_height * scale;
    s.height_min = data.min_height * scale;
    s.cos_max_knee = std::cos(glm::radians(data.max_knee_degrees));
    s.cos_min_knee = std::cos(glm::radians(data.min_knee_degrees));
    s.ray_up = params.ray_up * scale;
    s.ray_down = params.ray_down * scale;
    s.use_locking = params.lock_feet;
    s.ground_normal = s.world_up;
    leg.ground_normal = s.world_up;
    leg.planted_height = data.planted_height;
    leg.max_ankle_angle = glm::radians(data.max_ankle_degrees);
    return leg;
}

// `v` keeps only `fraction` of its turn away from `from`.
void keep_turn(float fraction, const glm::vec3& from, glm::vec3& v) {
    if (fraction >= 1.0f) {
        return;
    }
    const glm::quat q = shortest_rotation(from, v);
    const float angle = 2.0f * angle_of(std::fabs(q.w));
    if (angle <= 0.0f) {
        v = from;
        return;
    }
    const glm::vec3 axis = normalized(xyz(q)) * (q.w < 0.0f ? -1.0f : 1.0f);
    v = normalized(glm::angleAxis(angle * fraction, axis) * from);
}

glm::vec3 mean_of(std::span<const glm::vec3> points) {
    glm::vec3 sum{0.0f};
    for (const glm::vec3& p : points) {
        sum += p;
    }
    return sum / float(points.size());
}

// the regression line through `points` of height along `y_axis` over distance along `x_axis`, as a
// direction.
bool fit_line(std::span<const glm::vec3> points, const glm::vec3& x_axis, const glm::vec3& y_axis,
              glm::vec3& out) {
    const glm::vec3 centre = mean_of(points);
    float sum_x = 0.0f, sum_y = 0.0f, sum_xy = 0.0f, sum_xx = 0.0f;
    for (const glm::vec3& p : points) {
        const float x = glm::dot(x_axis, p - centre);
        const float y = glm::dot(y_axis, p - centre);
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }
    const float n = float(points.size());
    const float det = n * sum_xx - sum_x * sum_x;
    if (det == 0.0f) {
        return false;
    }
    out = normalized(x_axis + y_axis * ((n * sum_xy - sum_x * sum_y) / det));
    if (glm::dot(out, x_axis) < 0.0f) {
        out = -out;
    }
    return true;
}

// the least-squares plane of height along `up`, as its normal.
bool fit_plane(std::span<const glm::vec3> points, const glm::vec3& up, glm::vec3& out) {
    glm::vec3 pick{0.0f};
    int axis = up.x > up.y ? 1 : 0;
    if (up.x > up.z) {
        axis = 2;
    }
    pick[axis] = 1.0f;
    const glm::vec3 e1 = normalized(glm::cross(pick, up));
    const glm::vec3 e2 = normalized(glm::cross(up, e1));

    const glm::vec3 centre = mean_of(points);
    glm::mat3 m(0.0f);
    glm::vec3 rhs{0.0f};
    for (const glm::vec3& p : points) {
        const glm::vec3 d = p - centre;
        const glm::vec3 row{glm::dot(d, e2), glm::dot(d, e1), 1.0f};
        const float z = glm::dot(d, up);
        m += glm::outerProduct(row, row);
        rhs += row * z;
    }
    if (glm::determinant(m) == 0.0f) {
        return false;
    }
    const glm::vec3 solution = glm::inverse(m) * rhs;
    const glm::vec3 along_e2 = normalized(e2 + up * solution.x);
    const glm::vec3 along_e1 = normalized(e1 + up * solution.y);
    out = normalized(glm::cross(along_e2, along_e1));
    if (glm::dot(out, up) < 0.0f) {
        out = -out;
    }
    return true;
}

// scales the tilt of `normal` about model right and forward by the two align fractions.
glm::vec3 align_fractions(const Character& character, const FootParams& params, const Qs& w,
                          glm::vec3 normal) {
    if (params.forward_align >= 1.0f && params.sideways_align >= 1.0f) {
        return normal;
    }
    const Project& project = *character.project;
    const glm::vec3 up_now = w.r * project.model_up();
    const glm::vec3 current = normalized(up_now);
    glm::quat rotation = w.r;
    if (differs(normal, current, 0.001f)) {
        const glm::quat q = shortest_rotation(current, normal);
        if (2.0f * angle_of(std::fabs(q.w)) > 0.0f) {
            rotation = q * w.r;
        }
    }
    glm::vec3 right = rotation * project.model_right();
    glm::vec3 forward = rotation * project.model_forward();

    const glm::vec3& world_up = project.world_up();
    const glm::vec3 right_now = normalized(w.r * project.model_right());
    if (std::fabs(glm::dot(world_up, right_now)) > k_parallel) {
        return up_now;
    }
    const glm::vec3 level_forward = normalized(glm::cross(world_up, right_now));
    keep_turn(params.forward_align, level_forward, forward);
    keep_turn(params.sideways_align, glm::cross(level_forward, world_up), right);
    if (std::fabs(std::fabs(glm::dot(forward, right)) - 1.0f) >= 0.001f) {
        return normalized(glm::cross(right, forward));
    }
    return normalized(up_now);
}

// and ALIGN_MODE_FORWARD: a slope along forward, from the feet projected onto the character's
// middle plane, and a slope across from two more rays at the sides or, without a sample width, from the
// feet's normals.
glm::vec3 slope_normal(Character& character, const FootParams& params, std::span<const FootLeg> legs,
                       const Qs& w, const glm::vec3& up) {
    const Project& project = *character.project;
    const glm::vec3& world_up = project.world_up();
    const glm::vec3 fallback = w.r * project.model_up();
    glm::vec3 right = normalized(w.r * project.model_right());

    std::vector<glm::vec3> points;
    points.reserve(legs.size());
    for (const FootLeg& leg : legs) {
        points.push_back(leg.ground_position - right * glm::dot(right, leg.ground_position));
    }
    glm::vec3 forward{0.0f};
    if (!fit_line(points, normalized(glm::cross(up, right)), up, forward)) {
        forward = w.r * project.model_forward();
    }
    if (std::fabs(glm::dot(right, world_up)) > k_parallel) {
        return fallback;
    }
    const glm::vec3 level_forward = normalized(glm::cross(world_up, right));
    const glm::vec3 level_right = glm::cross(level_forward, world_up);

    if (params.sideways_width != 0.0f) {
        glm::vec3 hits[2]{};
        bool hit[2]{};
        glm::vec3 side = level_right * params.sideways_width;
        for (int i = 0; i < 2; ++i) {
            const glm::vec3 from = w.t + side + world_up * params.ray_up;
            const glm::vec3 to = w.t + side - world_up * params.ray_down;
            float fraction = 0.0f;
            glm::vec3 normal{0.0f};
            hit[i] = cast(character, params.filter, from, to, fraction, normal);
            hits[i] = from + (to - from) * fraction;
            side = -side;
        }
        if (hit[0] || hit[1]) {
            right = normalized((hit[0] ? hits[0] : w.t) - (hit[1] ? hits[1] : w.t));
            keep_turn(params.sideways_align, level_right, right);
        }
    } else {
        glm::vec3 sum{0.0f};
        for (const FootLeg& leg : legs) {
            sum += leg.ground_normal;
        }
        const glm::vec3 normal = normalized(sum);
        if (std::fabs(glm::dot(normal, forward)) > k_parallel) {
            return fallback;
        }
        right = normalized(glm::cross(forward, normal));
        keep_turn(params.sideways_align, level_right, right);
    }

    keep_turn(params.forward_align, level_forward, forward);
    if (std::fabs(glm::dot(right, forward)) > k_parallel) {
        return normalized(fallback);
    }
    return normalized(glm::cross(right, forward));
}

// The ground's normal under the feet: for the driver, for the node.
glm::vec3 ground_normal_of(Character& character, const FootParams& params, std::span<const FootLeg> legs,
                           const Qs& w, const glm::vec3& up) {
    if (!params.node && legs.size() == 2) {
        const glm::vec3 normal = normalized(legs[0].ground_normal + legs[1].ground_normal);
        return align_fractions(character, params, w, normal);
    }
    if (params.slope_alignment) {
        return slope_normal(character, params, legs, w, up);
    }
    std::vector<glm::vec3> points;
    points.reserve(legs.size());
    for (const FootLeg& leg : legs) {
        points.push_back(leg.ground_position);
    }
    glm::vec3 normal{0.0f};
    if (!fit_plane(points, up, normal)) {
        return w.r * character.project->model_up();
    }
    return params.node ? normal : align_fractions(character, params, w, normal);
}

// The driver's sequence, which hkbFootIkModifier shares but for the differences `params.node` selects.
struct FootPlacement {
    std::vector<FootLeg> legs;
    glm::vec3 error_out_translation{0.0f};
    glm::quat align_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float feedback = 0.0f;
    float error_out = 0.0f;
    float prev_on_fraction = 0.0f;
    bool is_set_up = false;
    bool ground_position_valid = false;

    float combined_error(float bias) const {
        float highest = -FLT_MAX;
        float lowest = FLT_MAX;
        for (const FootLeg& leg : legs) {
            highest = std::max(highest, leg.vertical_error);
            lowest = std::min(lowest, leg.vertical_error);
        }
        return legs.empty() ? 0.0f : (1.0f - bias) * highest + bias * lowest;
    }

    void align_with_ground(Character& character, const FootParams& params, Qs& w, const glm::vec3& up,
                           float gain) {
        const glm::vec3 normal = ground_normal_of(character, params, legs, w, up);
        const glm::vec3 current = normalized(w.r * character.project->model_up());
        if (!differs(normal, current, 0.001f)) {
            return;
        }
        const glm::quat q = shortest_rotation(current, normal);
        const float angle = 2.0f * angle_of(std::fabs(q.w));
        if (angle > 0.0f) {
            const glm::vec3 axis = normalized(xyz(q)) * (q.w < 0.0f ? -1.0f : 1.0f);
            align_rotation = glm::angleAxis(angle * gain, axis);
            w.r = align_rotation * w.r;
        }
    }

    // turns each hip by `gain` of the way from the character's up to the wanted up, about the
    // hip's own axes.
    void orient_hips(const Project& project, ModelPose& pose, const Qs& w, const glm::vec3& up, float gain) {
        const glm::vec3 current = normalized(w.r * project.model_up());
        if (!differs(current, up, 0.001f)) {
            return;
        }
        const glm::quat q = shortest_rotation(current, up);
        if (glm::length(xyz(q)) <= k_epsilon) {
            return;
        }
        const glm::vec3 axis = normalized(xyz(q)) * (q.w < 0.0f ? -1.0f : 1.0f);
        const float angle = 2.0f * angle_of(std::fabs(q.w)) * gain;
        for (const FootLeg& leg : legs) {
            const glm::quat rotation = pose.model(leg.solver.hip).r;
            const glm::vec3 local_axis = glm::conjugate(w.r * rotation) * axis;
            pose.set_model_rotation(leg.solver.hip, rotation * glm::angleAxis(angle, local_axis));
        }
    }

    // Steps 8 to 20 of "The driver, per frame". `gains` have been through fraction already.
    void run(Character& character, Output& output, ModelPose& pose, const FootGains& gains, bool on,
             const FootParams& params, bool first_frame) {
        const Project& project = *character.project;
        const Skeleton& skeleton = *character.skeleton;
        const glm::vec3& model_up = project.model_up();
        Qs& w = output.world_from_model;
        align_rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const auto current_up = [&] {
            return params.use_character_up ? glm::vec3(w.r * model_up) : project.world_up();
        };
        glm::vec3 up = current_up();

        Qs root = pose.local(0);
        root.t += (glm::conjugate(w.r) * up) * params.vertical_offset;
        pose.set_local(0, root);
        for (FootLeg& leg : legs) {
            leg.solver.height_max += params.vertical_offset;
            leg.solver.height_min += params.vertical_offset;
            leg.solver.original_ground += params.vertical_offset;
        }

        if (params.node) {
            error_out = combined_error(gains[gain_error_up_down_bias]);
        }
        if (gains[gain_feedback] > 0.0f) {
            error_out_translation = up * (params.node ? error_out * gains[gain_feedback] : feedback);
            w.t += error_out_translation;
            character.foot_ik_feedback = error_out_translation;
            character.foot_ik_on = on;
        }

        if (legs.size() > (params.node ? 2u : 1u) && ground_position_valid) {
            if (gains[gain_align] > 0.0f) {
                align_with_ground(character, params, w, up, gains[gain_align]);
            }
            up = current_up();
        }

        for (FootLeg& leg : legs) {
            if (!leg.original_latched) {
                leg.original = pose.model(leg.solver.ankle);
            }
            leg.original_latched = false;
        }

        if (gains[gain_hip_orientation] > 0.0f) {
            orient_hips(project, pose, w, up, gains[gain_hip_orientation]);
        }

        const float max_angle_change = params.node ? 0.0f : gains[gain_max_knee_difference];
        for (FootLeg& leg : legs) {
            leg.solver.world_up = up;
            const FootSolver::Result r = leg.solver.place(character, pose, w, leg.original, on, gains,
                                                          max_angle_change, params.filter);
            leg.ground_normal = r.normal;
            leg.vertical_error = r.vertical_error + (r.hit ? params.vertical_offset : 0.0f);
            leg.hit_something = r.hit;
            leg.is_planted_ms = glm::dot(model_up, leg.original.t) < leg.planted_height;
            if (gains[gain_align] > 0.0f) {
                const glm::vec3 ankle = to_world(w, pose.model(leg.solver.ankle).t);
                const float height = glm::dot(up, w.t) + leg.vertical_error;
                leg.ground_position = ankle + up * (height - glm::dot(up, ankle));
                ground_position_valid = true;
            }
        }

        for (FootLeg& leg : legs) {
            const int32_t ankle = leg.solver.ankle;
            Qs local = pose.local(ankle);
            if (!first_frame) {
                const glm::quat smoothed =
                    slerp_shorter(leg.prev_ankle_rotation, local.r, gains[gain_ankle_orientation]);
                // In the driver, smoothing only touches large jumps.
                if (params.node || differs(xyz(smoothed), xyz(leg.prev_ankle_rotation), 0.75f)) {
                    local.r = smoothed;
                }
            }
            const glm::quat& reference = skeleton.reference[size_t(ankle)].r;
            const glm::quat q = glm::conjugate(reference) * local.r;
            const glm::vec3 vector = xyz(q);
            if (2.0f * angle_of(std::fabs(q.w)) > leg.max_ankle_angle && glm::length(vector) > k_epsilon) {
                const glm::vec3 axis = normalized(vector) * (q.w < 0.0f ? -1.0f : 1.0f);
                local.r = reference * glm::angleAxis(leg.max_ankle_angle, axis);
            }
            pose.set_local(ankle, local);
            leg.prev_ankle_rotation = local.r;
        }

        for (FootLeg& leg : legs) {
            leg.solver.height_max -= params.vertical_offset;
            leg.solver.height_min -= params.vertical_offset;
            leg.solver.original_ground -= params.vertical_offset;
        }

        if (!params.node) {
            feedback = combined_error(gains[gain_error_up_down_bias]) * gains[gain_feedback];
        }
    }
};

// hkbFootIkDriverInfo.
struct FootIkInfo final : Bindable {
    std::vector<FootLegData> legs;
    float raycast_up = 0.0f;
    float raycast_down = 0.0f;
    float original_ground = 0.0f;
    float vertical_offset = 0.0f;
    uint32_t collision_filter = 0;
    float forward_align = 0.0f;
    float sideways_align = 0.0f;
    float sideways_width = 0.0f;
    bool lock_feet = false;
    bool use_character_up = false;
    bool quadruped_narrow = false;

    void describe(Describer& d) override {
        d.objects("legs", legs);
        d.member("raycastDistanceUp", raycast_up);
        d.member("raycastDistanceDown", raycast_down);
        d.member("originalGroundHeightMS", original_ground);
        d.member("verticalOffset", vertical_offset);
        d.member("collisionFilterInfo", collision_filter);
        d.member("forwardAlignFraction", forward_align);
        d.member("sidewaysAlignFraction", sideways_align);
        d.member("sidewaysSampleWidth", sideways_width);
        d.member("lockFeetWhenPlanted", lock_feet);
        d.member("useCharacterUpVector", use_character_up);
        d.member("isQuadrupedNarrow", quadruped_narrow);
    }
};

} // namespace

// hkbFootIkDriver: the real foot placement. Its gains arrive from hkbFootIkControlsModifier.
class FootIkDriver {
  public:
    explicit FootIkDriver(const FootIkInfo& info) : m_info(info) {
        m_params.ray_up = info.raycast_up;
        m_params.ray_down = info.raycast_down;
        m_params.original_ground = info.original_ground;
        m_params.vertical_offset = info.vertical_offset;
        m_params.filter = info.collision_filter;
        m_params.forward_align = info.forward_align;
        m_params.sideways_align = info.sideways_align;
        m_params.sideways_width = info.sideways_width;
        m_params.lock_feet = info.lock_feet;
        m_params.use_character_up = info.use_character_up;
        m_params.slope_alignment = info.quadruped_narrow;
    }

    const std::vector<FootLeg>& legs() const { return m_feet.legs; }

    void run(Character& character, Output& output, float dt) {
        if (!character.skeleton || m_info.legs.empty() || output.pose.empty()) {
            return;
        }
        const Skeleton& skeleton = *character.skeleton;
        for (const FootLegData& leg : m_info.legs) {
            if (!leg.in_range(static_cast<int32_t>(output.pose.size()))) {
                return;
            }
        }
        const float on_fraction = character.foot_ik_fraction;
        if (on_fraction <= 0.0f) {
            m_feet = FootPlacement{}; // switching off drops all state at once
            return;
        }
        if (!character.cast_ray) {
            return;
        }

        FootGains gains = character.foot_ik_gains;
        for (size_t i = 0; i < gains.size(); ++i) {
            if (i != gain_error_up_down_bias && i != gain_max_knee_difference) {
                gains[i] = gain_fraction(gains[i], dt);
            }
        }
        // A blend-out turns IK off at once; the weight then fades by onOffGain.
        const bool on = on_fraction - m_feet.prev_on_fraction >= 0.0f;
        m_feet.prev_on_fraction = on_fraction;

        // Feet are solved at full size, so the track's scale moves into the pose until the end.
        fill_missing(skeleton, output);
        const glm::vec3 scale = output.world_from_model.s;
        const bool scaled = scale != glm::vec3(1.0f) && scale.x != 0.0f && scale.y != 0.0f && scale.z != 0.0f;
        if (scaled) {
            output.world_from_model.s = glm::vec3(1.0f);
            for (Qs& bone : output.pose) {
                bone.t *= scale;
            }
            output.pose[0].s *= scale;
        }

        const bool first_frame = !m_feet.is_set_up;
        if (first_frame) {
            const float size = character.world_from_model.s.x;
            m_feet.legs.clear();
            for (const FootLegData& leg : m_info.legs) {
                m_feet.legs.push_back(make_leg(leg, m_params, *character.project, size));
            }
            m_feet.is_set_up = true;
        }

        ModelPose pose(skeleton, output);
        m_feet.run(character, output, pose, gains, on, m_params, first_frame);

        if (scaled) {
            output.pose[0].s /= scale;
            for (Qs& bone : output.pose) {
                bone.t /= scale;
            }
            output.world_from_model.s = scale;
        }
    }

  private:
    const FootIkInfo& m_info;
    FootParams m_params;
    FootPlacement m_feet;
};

namespace {

// The driver of a character whose project has one, made when first needed.
FootIkDriver* foot_ik_driver(Character& character) {
    if (!character.foot_ik && character.project) {
        if (const auto* info = static_cast<const FootIkInfo*>(character.project->foot_ik_info())) {
            character.foot_ik = std::make_shared<FootIkDriver>(*info);
        }
    }
    return character.foot_ik.get();
}

class IsActive final : public Modifier {
  public:
    std::array<bool, 5> active{};
    std::array<bool, 5> invert{};

    std::unique_ptr<Node> clone() const override { return std::make_unique<IsActive>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        static constexpr const char* k_active[] = {"bIsActive0", "bIsActive1", "bIsActive2", "bIsActive3",
                                                   "bIsActive4"};
        static constexpr const char* k_invert[] = {"bInvertActive0", "bInvertActive1", "bInvertActive2",
                                                   "bInvertActive3", "bInvertActive4"};
        for (size_t i = 0; i < active.size(); ++i) {
            d.member(k_active[i], active[i], true);
            d.member(k_invert[i], invert[i]);
        }
    }
    void activate(Context&) override {
        for (size_t i = 0; i < active.size(); ++i) {
            active[i] = !invert[i];
        }
    }
    void deactivate(Context&) override { active = invert; }
};

struct LookAtBone final : Bindable {
    int16_t index = -1;
    glm::vec4 forward{0.0f};
    float limit_degrees = 0.0f;
    float on_gain = 0.0f;
    float off_gain = 0.0f;
    bool enabled = false;
    glm::vec3 current_forward{
        0.0f}; // last frame's forward, relative to the bone, so it follows the animation

    void describe(Describer& d) override {
        d.member("index", index);
        d.member("fwdAxisLS", forward);
        d.member("limitAngleDegrees", limit_degrees);
        d.member("onGain", on_gain);
        d.member("offGain", off_gain);
        d.member("enabled", enabled);
    }
};

class LookAt final : public Modifier {
  public:
    bool look_at_target = false;
    std::vector<LookAtBone> bones;
    std::vector<LookAtBone> eye_bones;
    float limit_degrees = 0.0f;
    float limit_threshold_degrees = 0.0f;
    bool continue_outside_limit = false;
    float on_gain = 0.0f;
    float off_gain = 0.0f;
    bool use_bone_gains = false;
    glm::vec4 target_location{0.0f};
    bool target_outside_limits = false;
    EventProperty target_out_of_limit_event;
    bool look_at_camera = false;
    float camera_x = 0.0f;
    float camera_y = 0.0f;
    float camera_z = 0.0f;

    std::unique_ptr<Node> clone() const override { return std::make_unique<LookAt>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("lookAtTarget", look_at_target);
        d.objects("bones", bones);
        d.objects("eyeBones", eye_bones);
        d.member("limitAngleDegrees", limit_degrees);
        d.member("limitAngleThresholdDegrees", limit_threshold_degrees);
        d.member("continueLookOutsideOfLimit", continue_outside_limit);
        d.member("onGain", on_gain);
        d.member("offGain", off_gain);
        d.member("useBoneGains", use_bone_gains);
        d.member("targetLocation", target_location);
        d.member("targetOutsideLimits", target_outside_limits, true);
        d.member("targetOutOfLimitEvent", target_out_of_limit_event);
        d.member("lookAtCamera", look_at_camera);
        d.member("lookAtCameraX", camera_x);
        d.member("lookAtCameraY", camera_y);
        d.member("lookAtCameraZ", camera_z);
    }

    void activate(Context& ctx) override {
        m_time_step = 0.0f;
        target_outside_limits = false;
        m_bones_valid = ctx.character.skeleton != nullptr;
        const auto check = [&](LookAtBone& bone) {
            m_bones_valid =
                m_bones_valid && bone.index < static_cast<int32_t>(ctx.character.skeleton->size());
            bone.current_forward = xyz(bone.forward);
        };
        std::for_each(bones.begin(), bones.end(), check);
        std::for_each(eye_bones.begin(), eye_bones.end(), check);
    }
    void update(Context&, float timestep) override { m_time_step = timestep; }
    void deactivate(Context&) override { target_outside_limits = false; }

    void modify(Context& ctx, Output& output) override {
        if (!m_bones_valid || bones.empty() || output.pose.empty() || !ctx.character.skeleton) {
            return;
        }
        const int32_t n = static_cast<int32_t>(output.pose.size());
        const LookAtBone& head = bones.back();
        if (head.index < 0 || head.index >= n) {
            return;
        }
        ModelPose pose(*ctx.character.skeleton, output);

        // The target.
        const glm::vec3 target =
            look_at_camera ? glm::vec3(camera_x, camera_y, camera_z) : xyz(target_location);
        float angle = 0.0f;
        glm::vec3 target_ms{0.0f};
        glm::vec3 look_point{0.0f};
        if (!look_at_target) {
            target_outside_limits = false;
        } else {
            const Qs& head_ms = pose.model(head.index);
            const glm::vec3 forward = normalized(head_ms.r * xyz(head.forward));
            target_ms = to_model(track_0(ctx, output), target);
            const glm::vec3 to_target = target_ms - head_ms.t;
            const float distance = glm::length(to_target);
            const glm::vec3 direction = normalized(to_target);
            angle = angle_of(glm::dot(forward, direction));

            const float limit = glm::radians(limit_degrees);
            if (angle <= limit) {
                target_outside_limits = false;
            } else if (angle > limit + glm::radians(limit_threshold_degrees)) {
                if (!target_outside_limits) {
                    ctx.graph.raise(ctx, target_out_of_limit_event);
                }
                target_outside_limits = true;
            }

            if (target_outside_limits && !continue_outside_limit) {
                angle = 0.0f;
            } else {
                look_point = target_ms;
                if (target_outside_limits) {
                    // Pulled back to the edge of the cone in the horizontal plane. The Z here is hard-coded,
                    // not model up.
                    const glm::vec3 z{0.0f, 0.0f, 1.0f};
                    const glm::vec3 side = normalized(glm::cross(forward, z));
                    const float turn = glm::dot(side, direction) >= 0.0f ? -limit : limit;
                    const glm::vec3 level = normalized({forward.x, forward.y, 0.0f});
                    look_point = head_ms.t + (glm::angleAxis(turn, z) * level) * distance;
                    angle = limit;
                }
            }
        }

        // Apply: the angle is shared out along the chain.
        int32_t count = 0;
        int32_t last_enabled = -1;
        for (size_t i = 0; i < bones.size(); ++i) {
            if (bones[i].enabled && bones[i].index < n) {
                ++count;
                last_enabled = static_cast<int32_t>(i);
            }
        }
        float share = count > 0 ? angle / float(count) : 0.0f;
        float remaining = angle;
        int32_t remaining_bones = count;
        const bool looking = look_at_target && (!target_outside_limits || continue_outside_limit);
        for (size_t i = 0; i < bones.size(); ++i) {
            LookAtBone& bone = bones[i];
            if (bone.index >= n) {
                continue;
            }
            const float bone_limit = glm::radians(bone.limit_degrees);
            float cap = bone_limit;
            if (static_cast<int32_t>(i) != last_enabled) {
                cap = std::min(bone_limit, share);
                if (bone.enabled) {
                    remaining -= cap;
                    --remaining_bones;
                    share = remaining_bones > 0 ? remaining / float(remaining_bones) : 0.0f;
                }
            }
            bone_step(pose, bone, cap, look_point, looking);
        }
        for (LookAtBone& eye : eye_bones) {
            if (eye.index < n) {
                bone_step(pose, eye, glm::radians(eye.limit_degrees), target_ms, looking);
            }
        }
    }

  private:
    void bone_step(ModelPose& pose, LookAtBone& bone, float cap, const glm::vec3& point, bool looking) const {
        if (bone.index == -1) {
            return;
        }
        const Qs transform = pose.model(bone.index);
        const glm::vec3 animated = normalized(transform.r * xyz(bone.forward));
        const glm::vec3 previous = normalized(transform.r * bone.current_forward);

        glm::vec3 goal = animated;
        if (bone.enabled && looking) {
            const glm::vec3 direction = normalized(point - transform.t);
            const glm::vec3 axis = normalized(glm::cross(animated, direction));
            if (is_unit(axis)) {
                const float turn = std::max(std::min(angle_of(glm::dot(animated, direction)), cap), 0.0f);
                goal = normalized(glm::angleAxis(turn, axis) * animated);
            }
        }

        const float gain =
            looking ? (use_bone_gains ? bone.on_gain : on_gain) : (use_bone_gains ? bone.off_gain : off_gain);
        const float f = gain_fraction(gain, m_time_step);
        const glm::vec3 blended = normalized((1.0f - f) * previous + f * goal);
        bone.current_forward = normalized(glm::conjugate(transform.r) * blended);

        const float theta = angle_of(glm::dot(blended, animated));
        if (theta >= 0.001f) {
            const glm::vec3 axis = normalized(glm::cross(animated, blended));
            if (is_unit(axis)) {
                pose.set_model_rotation(bone.index, glm::angleAxis(theta, axis) * transform.r);
            }
        }
    }

    float m_time_step = 0.0f;
    bool m_bones_valid = false;
};

class EventOnDeactivate final : public Modifier {
  public:
    EventProperty event;

    std::unique_ptr<Node> clone() const override { return std::make_unique<EventOnDeactivate>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("event", event);
    }
    void deactivate(Context& ctx) override { ctx.graph.raise(ctx, event); }
};

class EventEveryNEvents final : public Modifier {
  public:
    EventProperty event_to_check_for;
    EventProperty event_to_send;
    int8_t number_before_send = 0;
    int8_t minimum_before_send = 0;
    bool randomize = false;

    // The count is set when the file's object is built, from the file's value, and clones copy it.
    std::unique_ptr<Node> clone() const override {
        auto copy = std::make_unique<EventEveryNEvents>(*this);
        copy->m_calculated = number_before_send;
        return copy;
    }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("eventToCheckFor", event_to_check_for);
        d.member("eventToSend", event_to_send);
        d.member("numberOfEventsBeforeSend", number_before_send);
        d.member("minimumNumberOfEventsBeforeSend", minimum_before_send);
        d.member("randomizeNumberOfEvents", randomize);
    }
    void activate(Context& ctx) override {
        if (m_calculated <= 0) {
            m_calculated = 1;
        }
        m_seen = 0;
        if (randomize) {
            roll(ctx);
        }
    }
    void handle_event(Context& ctx, const Event& e) override {
        if (e.id != event_to_check_for.id) {
            return;
        }
        m_seen = (m_seen + 1) % std::max(m_calculated, 1);
        if (m_seen == 0) {
            ctx.graph.raise(ctx, event_to_send);
            if (randomize) {
                roll(ctx);
            }
        }
    }

  private:
    // the graph's own generator, a plain LCG.
    void roll(Context& ctx) {
        uint32_t& state = ctx.character.random_state;
        state = 1664525u * state + 1013904223u;
        const int32_t range = number_before_send - minimum_before_send + 1;
        if (range > 0) {
            m_calculated = minimum_before_send + static_cast<int32_t>((state >> 13) % uint32_t(range));
        }
    }

    int32_t m_calculated = 0;
    int32_t m_seen = 0;
};

class Timer final : public Modifier {
  public:
    float alarm_time = 0.0f;
    EventProperty alarm_event;

    std::unique_ptr<Node> clone() const override { return std::make_unique<Timer>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("alarmTimeSeconds", alarm_time);
        d.member("alarmEvent", alarm_event);
    }
    void activate(Context&) override { m_elapsed = 0.0f; }
    void update(Context& ctx, float timestep) override {
        const float next = m_elapsed + timestep;
        if (m_elapsed <= alarm_time && alarm_time < next) {
            ctx.graph.raise(ctx, alarm_event);
        }
        m_elapsed = next;
    }

  private:
    float m_elapsed = 0.0f;
};

class BsTimer final : public Modifier {
  public:
    float alarm_time = 0.0f;
    EventProperty alarm_event;
    bool reset_alarm = false;

    std::unique_ptr<Node> clone() const override { return std::make_unique<BsTimer>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("alarmTimeSeconds", alarm_time);
        d.member("alarmEvent", alarm_event);
        d.member("resetAlarm", reset_alarm);
    }
    void activate(Context&) override { m_elapsed = 0.0f; }
    void update(Context& ctx, float timestep) override {
        float next = m_elapsed + timestep;
        if (m_elapsed <= alarm_time && alarm_time < next) {
            ctx.graph.raise(ctx, alarm_event);
            if (reset_alarm) {
                next -= alarm_time; // unlike the hkb timer, even with no event to send
            }
        }
        m_elapsed = next;
    }

  private:
    float m_elapsed = 0.0f;
};

class Damping final : public Modifier {
  public:
    float k_p = 0.0f;
    float k_i = 0.0f;
    float k_d = 0.0f;
    bool enable_scalar = false;
    bool enable_vector = false;
    float raw_value = 0.0f;
    float damped_value = 0.0f;
    glm::vec4 raw_vector{0.0f};
    glm::vec4 damped_vector{0.0f};
    glm::vec4 vector_error_sum{0.0f};
    glm::vec4 vector_previous_error{0.0f};
    float error_sum = 0.0f;
    float previous_error = 0.0f;

    std::unique_ptr<Node> clone() const override { return std::make_unique<Damping>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("kP", k_p);
        d.member("kI", k_i);
        d.member("kD", k_d);
        d.member("enableScalarDamping", enable_scalar);
        d.member("enableVectorDamping", enable_vector);
        d.member("rawValue", raw_value);
        d.member("dampedValue", damped_value, true);
        d.member("rawVector", raw_vector);
        d.member("dampedVector", damped_vector, true);
        d.member("vecErrorSum", vector_error_sum);
        d.member("vecPreviousError", vector_previous_error, true);
        d.member("errorSum", error_sum, true);
        d.member("previousError", previous_error, true);
    }
    void activate(Context&) override {
        damped_value = 0.0f;
        damped_vector = glm::vec4(0.0f);
        error_sum = previous_error = 0.0f;
        vector_error_sum = vector_previous_error = glm::vec4(0.0f);
    }
    // There's no time step here: the PID runs per generate.
    void modify(Context&, Output&) override {
        if (enable_scalar) {
            const float e = raw_value - damped_value;
            error_sum += e;
            damped_value += k_p * e + k_i * error_sum + k_d * (e - previous_error);
            previous_error = e;
        }
        if (enable_vector) {
            const glm::vec4 e = raw_vector - damped_vector;
            vector_error_sum += e;
            damped_vector += k_p * e + k_i * vector_error_sum + k_d * (e - vector_previous_error);
            vector_previous_error = e;
        }
    }
};

class SpeedSampler final : public Modifier {
  public:
    int32_t state = 0;
    float direction = 0.0f;
    float goal_speed = 0.0f;
    float speed_out = 0.0f;

    std::unique_ptr<Node> clone() const override { return std::make_unique<SpeedSampler>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("state", state);
        d.member("direction", direction);
        d.member("goalSpeed", goal_speed);
        d.member("speedOut", speed_out, true);
    }
    void update(Context& ctx, float) override {
        speed_out = ctx.character.sample_speed ? ctx.character.sample_speed(state, direction, goal_speed)
                                               : goal_speed;
    }
};

class InterpValue final : public Modifier {
  public:
    float source = 0.0f;
    float target = 0.0f;
    float result = 0.0f;
    float gain = 0.0f;

    std::unique_ptr<Node> clone() const override { return std::make_unique<InterpValue>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("source", source);
        d.member("target", target);
        d.member("result", result, true);
        d.member("gain", gain);
    }
    // `source` isn't advanced here: the data binds it and `result` to one variable to get smoothing.
    void update(Context&, float timestep) override {
        result = source + gain_fraction(gain, timestep) * (target - source);
    }
};

// The one node that calls into its children: they aren't in the active list (the active query reports none),
// and they run for one instant, so there's nothing for the root's loops to do. Vanilla's are all unbound
// hkbEvaluateExpressionModifiers; no variable copy happens here, and the child's `modify` never
// runs.
class ModifyOnce final : public Modifier {
  public:
    Node* on_activate = nullptr;
    Node* on_deactivate = nullptr;

    std::unique_ptr<Node> clone() const override {
        auto copy = std::make_unique<ModifyOnce>(*this);
        copy->m_is_clone = true;
        return copy;
    }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.node("pOnActivateModifier", on_activate);
        d.node("pOnDeactivateModifier", on_deactivate);
    }
    void children(uint32_t flags, std::vector<Child>& out) override {
        if (!m_is_clone && !(flags & (children_active_only | children_generators_only))) {
            out.push_back({on_activate});
            out.push_back({on_deactivate});
        }
    }
    void activate(Context& ctx) override {
        if (enable && on_activate) {
            run_once(ctx, *on_activate);
        }
    }
    void deactivate(Context& ctx) override {
        if (on_deactivate) {
            run_once(ctx, *on_deactivate);
        }
    }

  private:
    static void run_once(Context& ctx, const Node& child) {
        const std::unique_ptr<Node> instant = child.clone();
        instant->activate(ctx);
        instant->update(ctx, 0.0f);
        instant->deactivate(ctx);
    }

    bool m_is_clone = false;
};

struct FootIkControlData final : Bindable {
    FootGainsData gains;
    void describe(Describer& d) override { d.object("gains", gains); }
};

struct FootIkControlsLeg final : Bindable {
    glm::vec4 ground_position{0.0f};
    EventProperty ungrounded_event;
    float vertical_error = 0.0f;
    bool hit_something = false;
    bool is_planted_ms = false;

    void describe(Describer& d) override {
        d.member("groundPosition", ground_position, true);
        d.member("ungroundedEvent", ungrounded_event);
        d.member("verticalError", vertical_error, true);
        d.member("hitSomething", hit_something, true);
        d.member("isPlantedMS", is_planted_ms, true);
    }
};

class FootIkControls final : public Modifier {
  public:
    FootIkControlData control_data;
    std::vector<FootIkControlsLeg> legs;
    // Declared because the data binds them; nothing in this build writes them (Open questions).
    glm::vec4 error_out_translation{0.0f};
    glm::quat align_with_ground_rotation{1.0f, 0.0f, 0.0f, 0.0f};

    std::unique_ptr<Node> clone() const override { return std::make_unique<FootIkControls>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.object("controlData", control_data);
        d.objects("legs", legs);
        d.member("errorOutTranslation", error_out_translation, true);
        d.member("alignWithGroundRotation", align_with_ground_rotation, true);
    }
    void activate(Context& ctx) override {
        if (foot_ik_driver(ctx.character)) {
            copy_legs(ctx);
        }
    }
    void update(Context& ctx, float) override {
        if (foot_ik_driver(ctx.character)) {
            copy_legs(ctx);
        }
        for (const FootIkControlsLeg& leg : legs) {
            if (leg.is_planted_ms && !leg.hit_something) {
                ctx.graph.raise(ctx, leg.ungrounded_event);
            }
        }
    }
    void modify(Context& ctx, Output&) override {
        ctx.character.foot_ik_gains = control_data.gains.gains;
        ctx.character.foot_ik_fraction = 1.0f;
        if (foot_ik_driver(ctx.character)) {
            copy_legs(ctx);
        }
    }

  private:
    // Past the driver's legs the fields read zero.
    void copy_legs(Context& ctx) {
        const std::vector<FootLeg>& driver = ctx.character.foot_ik->legs();
        for (size_t i = 0; i < legs.size(); ++i) {
            FootIkControlsLeg& leg = legs[i];
            const bool has = i < driver.size();
            leg.ground_position = has ? glm::vec4(driver[i].ground_position, 0.0f) : glm::vec4(0.0f);
            leg.vertical_error = has ? driver[i].vertical_error : 0.0f;
            leg.hit_something = has && driver[i].hit_something;
            leg.is_planted_ms = has && driver[i].is_planted_ms;
        }
    }
};

struct FootIkModifierLeg final : FootLegData {
    Qs original_ankle;
    EventProperty ungrounded_event;
    float vertical_error = 0.0f;
    bool hit_something = false;
    bool is_planted_ms = false;
    bool is_original_ankle_set = false;

    void describe(Describer& d) override {
        FootLegData::describe(d);
        d.member("originalAnkleTransformMS", original_ankle);
        d.member("ungroundedEvent", ungrounded_event);
        d.member("verticalError", vertical_error, true);
        d.member("hitSomething", hit_something, true);
        d.member("isPlantedMS", is_planted_ms, true);
        d.member("isOriginalAnkleTransformMSSet", is_original_ankle_set);
    }
};

// The same pipeline as the driver, as a node. Two vanilla objects.
class FootIk final : public Modifier {
  public:
    FootGainsData gains;
    std::vector<FootIkModifierLeg> legs;
    float raycast_up = 0.0f;
    float raycast_down = 0.0f;
    float original_ground = 0.0f;
    float error_out = 0.0f;
    glm::vec4 error_out_translation{0.0f};
    glm::quat align_with_ground_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float vertical_offset = 0.0f;
    uint32_t collision_filter = 0;
    float forward_align = 0.0f;
    float sideways_align = 0.0f;
    float sideways_width = 0.0f;
    bool use_track_data = false;
    bool lock_feet = false;
    bool use_character_up = false;
    int8_t align_mode = 0;

    std::unique_ptr<Node> clone() const override { return std::make_unique<FootIk>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.object("gains", gains);
        d.objects("legs", legs);
        d.member("raycastDistanceUp", raycast_up);
        d.member("raycastDistanceDown", raycast_down);
        d.member("originalGroundHeightMS", original_ground);
        d.member("errorOut", error_out, true);
        d.member("errorOutTranslation", error_out_translation, true);
        d.member("alignWithGroundRotation", align_with_ground_rotation, true);
        d.member("verticalOffset", vertical_offset);
        d.member("collisionFilterInfo", collision_filter);
        d.member("forwardAlignFraction", forward_align);
        d.member("sidewaysAlignFraction", sideways_align);
        d.member("sidewaysSampleWidth", sideways_width);
        d.member("useTrackData", use_track_data);
        d.member("lockFeetWhenPlanted", lock_feet);
        d.member("useCharacterUpVector", use_character_up);
        d.member("alignMode", align_mode);
    }

    void activate(Context&) override {
        m_feet = FootPlacement{};
        for (FootIkModifierLeg& leg : legs) {
            leg.vertical_error = 0.0f;
            leg.hit_something = false;
            leg.is_planted_ms = false;
        }
        error_out = 0.0f;
        vertical_offset = 0.0f;
        m_time_step = 0.0f;
    }
    void update(Context& ctx, float timestep) override {
        m_time_step = timestep;
        for (const FootIkModifierLeg& leg : legs) {
            if (leg.is_planted_ms && !leg.hit_something) {
                ctx.graph.raise(ctx, leg.ungrounded_event);
            }
        }
    }
    void deactivate(Context&) override { m_feet = FootPlacement{}; }

    void modify(Context& ctx, Output& output) override {
        Character& character = ctx.character;
        align_with_ground_rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        if (legs.empty() || !character.cast_ray || !character.skeleton || !character.project ||
            output.pose.empty()) {
            return;
        }
        for (const FootIkModifierLeg& leg : legs) {
            if (!leg.in_range(static_cast<int32_t>(output.pose.size()))) {
                return;
            }
        }

        const bool track = use_track_data && character.foot_ik_fraction > 0.0f;
        FootGains values = track ? character.foot_ik_gains : gains.gains;
        for (size_t i = 0; i < values.size(); ++i) {
            // Unlike the driver, a gain of exactly 1 means "at once" here.
            if (i != gain_error_up_down_bias && i != gain_max_knee_difference && values[i] != 1.0f) {
                values[i] = gain_fraction(values[i], m_time_step);
            }
        }
        const float on_fraction = !use_track_data ? 1.0f : track ? character.foot_ik_fraction : 0.0f;
        const bool on = on_fraction >= 1.0f || (on_fraction > 0.0f && on_fraction > m_feet.prev_on_fraction);
        m_feet.prev_on_fraction = on_fraction;

        FootParams params;
        params.ray_up = raycast_up;
        params.ray_down = raycast_down;
        params.original_ground = original_ground;
        params.vertical_offset = vertical_offset;
        params.filter = collision_filter;
        params.forward_align = forward_align;
        params.sideways_align = sideways_align;
        params.sideways_width = sideways_width;
        params.lock_feet = lock_feet;
        params.use_character_up = use_character_up;
        params.slope_alignment = align_mode == 1;
        params.node = true;

        const bool first_frame = !m_feet.is_set_up;
        if (first_frame) {
            m_feet.legs.clear();
            for (const FootIkModifierLeg& leg : legs) {
                m_feet.legs.push_back(make_leg(leg, params, *character.project, 1.0f));
            }
            m_feet.is_set_up = true;
        }
        for (size_t i = 0; i < legs.size(); ++i) {
            m_feet.legs[i].vertical_error = legs[i].vertical_error;
            m_feet.legs[i].original = legs[i].original_ankle;
            m_feet.legs[i].original_latched = legs[i].is_original_ankle_set;
        }

        ModelPose pose(*character.skeleton, output);
        m_feet.run(character, output, pose, values, on, params, first_frame);

        error_out = m_feet.error_out;
        error_out_translation = glm::vec4(m_feet.error_out_translation, 0.0f);
        align_with_ground_rotation = m_feet.align_rotation;
        for (size_t i = 0; i < legs.size(); ++i) {
            const FootLeg& leg = m_feet.legs[i];
            legs[i].original_ankle = leg.original;
            legs[i].is_original_ankle_set = false;
            legs[i].vertical_error = leg.vertical_error;
            legs[i].hit_something = leg.hit_something;
            legs[i].is_planted_ms = leg.is_planted_ms;
        }
    }

  private:
    FootPlacement m_feet;
    float m_time_step = 0.0f;
};

class EventOnFalseToTrue final : public Modifier {
  public:
    struct Slot {
        bool enable_event = false;
        bool variable_to_test = false;
        EventProperty event;
        bool activated_last_frame = false;
    };
    std::array<Slot, 3> slots;

    std::unique_ptr<Node> clone() const override { return std::make_unique<EventOnFalseToTrue>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("bEnableEvent1", slots[0].enable_event);
        d.member("bVariableToTest1", slots[0].variable_to_test);
        d.member("EventToSend1", slots[0].event);
        d.member("bEnableEvent2", slots[1].enable_event);
        d.member("bVariableToTest2", slots[1].variable_to_test);
        d.member("EventToSend2", slots[1].event);
        d.member("bEnableEvent3", slots[2].enable_event);
        d.member("bVariableToTest3", slots[2].variable_to_test);
        d.member("EventToSend3", slots[2].event);
    }
    void update(Context& ctx, float) override {
        for (Slot& slot : slots) {
            const bool now = slot.enable_event && slot.variable_to_test;
            if (now && !slot.activated_last_frame) {
                ctx.graph.raise(ctx, slot.event);
            }
            slot.activated_last_frame = now;
        }
    }
};

class DirectAt final : public Modifier {
  public:
    bool direct_at_target = false;
    int16_t source_bone = -1;
    int16_t start_bone = -1;
    int16_t end_bone = -1;
    float limit_heading_degrees = 0.0f;
    float limit_pitch_degrees = 0.0f;
    float offset_heading_degrees = 0.0f;
    float offset_pitch_degrees = 0.0f;
    float on_gain = 0.0f;
    float off_gain = 0.0f;
    glm::vec4 target_location{0.0f};
    uint32_t user_info = 0;
    bool direct_at_camera = false;
    float camera_x = 0.0f;
    float camera_y = 0.0f;
    float camera_z = 0.0f;
    bool active = false;
    float current_heading = 0.0f;
    float current_pitch = 0.0f;

    std::unique_ptr<Node> clone() const override { return std::make_unique<DirectAt>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("directAtTarget", direct_at_target);
        d.member("sourceBoneIndex", source_bone);
        d.member("startBoneIndex", start_bone);
        d.member("endBoneIndex", end_bone);
        d.member("limitHeadingDegrees", limit_heading_degrees);
        d.member("limitPitchDegrees", limit_pitch_degrees);
        d.member("offsetHeadingDegrees", offset_heading_degrees);
        d.member("offsetPitchDegrees", offset_pitch_degrees);
        d.member("onGain", on_gain);
        d.member("offGain", off_gain);
        d.member("targetLocation", target_location);
        d.member("userInfo", user_info);
        d.member("directAtCamera", direct_at_camera);
        d.member("directAtCameraX", camera_x);
        d.member("directAtCameraY", camera_y);
        d.member("directAtCameraZ", camera_z);
        d.member("active", active, true);
        d.member("currentHeadingOffset", current_heading, true);
        d.member("currentPitchOffset", current_pitch, true);
    }

    void activate(Context&) override {
        active = enable;
        current_heading = current_pitch = 0.0f;
    }
    void deactivate(Context&) override {
        active = false;
        current_heading = current_pitch = 0.0f;
        m_time_step = 0.0f;
    }
    void update(Context& ctx, float timestep) override {
        Character& character = ctx.character;
        m_has_target = direct_at_target;
        m_time_step = timestep;
        if (character.direct_at_target) {
            character.direct_at_target(user_info, m_has_target, m_target);
        } else if (user_info == 1) {
            // The actor's head-tracking target, which the game passes as the look target.
            m_has_target = character.has_look_target;
            m_target = character.look_target;
        }
        if (character.skeleton) {
            bone_chain(*character.skeleton, start_bone, end_bone, m_chain);
        }
    }

    void modify(Context& ctx, Output& output) override {
        const Character& character = ctx.character;
        const int32_t n = static_cast<int32_t>(output.pose.size());
        if (!character.skeleton || !character.project || start_bone < 0 || end_bone < 0 || start_bone >= n ||
            end_bone >= n) {
            return;
        }
        ModelPose pose(*character.skeleton, output);
        const Qs& c = character.world_from_model;
        const glm::vec3& up = character.project->world_up();

        float wanted_heading = 0.0f;
        float wanted_pitch = 0.0f;
        const bool on_target = direct_at_target && m_has_target;
        if (on_target) {
            const glm::vec3 source = source_bone >= 0 && source_bone < n
                                         ? to_world(track_0(ctx, output), pose.model(source_bone).t)
                                         : c.t;
            const glm::vec3 direction = normalized(m_target - source);
            const glm::vec3 forward = c.r * glm::vec3(0.0f, 1.0f, 0.0f);
            const float heading =
                -wrap_once(havok_atan2(direction.x, direction.y) - havok_atan2(forward.x, forward.y));
            // Both use the direction's horizontal length.
            const float h = std::sqrt(direction.x * direction.x + direction.y * direction.y);
            const float pitch = wrap_once(havok_atan2(direction.z, h) - havok_atan2(forward.z, h));
            const float limit_heading = glm::radians(limit_heading_degrees);
            const float limit_pitch = glm::radians(limit_pitch_degrees);
            wanted_heading =
                std::clamp(heading, -limit_heading, limit_heading) + glm::radians(offset_heading_degrees);
            wanted_pitch = std::clamp(pitch, -limit_pitch, limit_pitch) + glm::radians(offset_pitch_degrees);
        }
        const float f = gain_fraction(on_target ? on_gain : off_gain, m_time_step);
        current_heading += (wanted_heading - current_heading) * f;
        current_pitch += (wanted_pitch - current_pitch) * f;
        if (!on_target) {
            current_heading = std::fabs(current_heading) < 0.01f ? 0.0f : current_heading;
            current_pitch = std::fabs(current_pitch) < 0.01f ? 0.0f : current_pitch;
        }

        if ((current_heading == 0.0f && current_pitch == 0.0f) || m_chain.empty()) {
            return;
        }
        const float count = float(m_chain.size());
        const glm::vec3 right = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), up);
        const int32_t first_parent = pose.parent(m_chain.front());
        glm::quat parent = first_parent >= 0 ? pose.model(first_parent).r : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        for (int32_t bone : m_chain) {
            const glm::quat to_parent = glm::conjugate(parent);
            const glm::quat q1 = glm::angleAxis(current_heading / count, normalized(to_parent * up));
            const glm::quat q2 = glm::angleAxis(current_pitch / count, normalized(to_parent * right));
            Qs local = pose.local(bone);
            local.r = (q1 * q2) * local.r;
            pose.set_local(bone, local);
            parent = parent * local.r;
        }
    }

  private:
    float m_time_step = 0.0f;
    bool m_has_target = false;
    glm::vec3 m_target{0.0f}; // directAtTargetLocation, from the game
    std::vector<int32_t> m_chain;
};

class Twist final : public Modifier {
  public:
    glm::vec4 axis_of_rotation{0.0f};
    float twist_angle = 0.0f;
    int16_t start_bone = -1;
    int16_t end_bone = -1;
    int8_t set_angle_method = 0;
    int8_t axis_coordinates = 0;
    bool is_additive = false;

    std::unique_ptr<Node> clone() const override { return std::make_unique<Twist>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("axisOfRotation", axis_of_rotation);
        d.member("twistAngle", twist_angle);
        d.member("startBoneIndex", start_bone);
        d.member("endBoneIndex", end_bone);
        d.member("setAngleMethod", set_angle_method);
        d.member("rotationAxisCoordinates", axis_coordinates);
        d.member("isAdditive", is_additive);
    }

    void activate(Context& ctx) override {
        const Skeleton* skeleton = ctx.character.skeleton;
        if (!skeleton || start_bone < 0 || end_bone < 0 || size_t(start_bone) >= skeleton->size()) {
            return;
        }
        bone_chain(*skeleton, start_bone, end_bone, m_chain);
        m_parents.clear();
        for (int32_t b = skeleton->parents[size_t(start_bone)]; b >= 0; b = skeleton->parents[size_t(b)]) {
            m_parents.push_back(b);
        }
    }
    void deactivate(Context&) override {
        m_chain.clear();
        m_parents.clear();
    }

    void modify(Context& ctx, Output& output) override {
        const int32_t n = static_cast<int32_t>(output.pose.size());
        const bool in_range = (start_bone >= 0 && start_bone < n) || (end_bone >= 0 && end_bone < n);
        if (!ctx.character.skeleton || output.pose.empty() || m_chain.empty() || !in_range) {
            return;
        }
        fill_missing(*ctx.character.skeleton, output);

        constexpr int8_t k_ramped = 1;
        constexpr int8_t k_model_coordinates = 0;
        const bool model_axis = axis_coordinates == k_model_coordinates;
        glm::quat parent{1.0f, 0.0f, 0.0f, 0.0f};
        if (model_axis) {
            for (auto b = m_parents.rbegin(); b != m_parents.rend(); ++b) {
                if (*b < n) {
                    parent = parent * output.pose[size_t(*b)].r;
                }
            }
        }
        const float count = float(m_chain.size());
        float step = glm::radians(twist_angle) / count;
        if (set_angle_method == k_ramped) {
            step *= 2.0f / count;
        }
        for (size_t i = 0; i < m_chain.size(); ++i) {
            if (set_angle_method == k_ramped) {
                step /= float(i) + 0.5f;
            }
            const int32_t bone = m_chain[i];
            if (bone < 0 || bone >= n) {
                continue;
            }
            glm::vec3 axis = xyz(axis_of_rotation);
            if (model_axis) {
                axis = normalized(glm::conjugate(parent) * axis);
            }
            const glm::quat q = glm::angleAxis(step, axis);
            glm::quat& rotation = output.pose[size_t(bone)].r;
            rotation = is_additive ? q * rotation : q;
            if (model_axis) {
                parent = parent * rotation;
            }
        }
    }

  private:
    std::vector<int32_t> m_chain;
    std::vector<int32_t> m_parents; // from the start bone's parent to the root
};

class RotateCharacter final : public Modifier {
  public:
    float degrees_per_second = 0.0f;
    float speed_multiplier = 0.0f;
    glm::vec4 axis_of_rotation{0.0f};

    std::unique_ptr<Node> clone() const override { return std::make_unique<RotateCharacter>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("degreesPerSecond", degrees_per_second);
        d.member("speedMultiplier", speed_multiplier);
        d.member("axisOfRotation", axis_of_rotation);
    }
    void activate(Context&) override { m_angle = 0.0f; }
    void update(Context&, float timestep) override {
        m_angle += glm::radians(degrees_per_second) * speed_multiplier * timestep;
    }
    void modify(Context& ctx, Output& output) override {
        const Qs& c = ctx.character.world_from_model;
        // What the track holds relative to the character survives the turn.
        const Qs motion = compose(inverse(c), track_0(ctx, output));
        glm::vec3 axis = xyz(axis_of_rotation);
        if (!differs(axis, glm::vec3(0.0f), 0.001f) && ctx.character.project) {
            axis = ctx.character.project->model_up();
        }
        Qs turned = c;
        turned.r = c.r * glm::angleAxis(m_angle, axis);
        Qs result = compose(turned, motion);
        result.r = glm::normalize(result.r);
        output.world_from_model = result;
        m_angle = 0.0f;
    }

  private:
    float m_angle = 0.0f;
};

class Tweener final : public Modifier {
  public:
    bool tween_position = false;
    bool tween_rotation = false;
    bool use_tween_duration = false;
    float tween_duration = 0.0f;
    glm::vec4 target_position{0.0f};
    glm::quat target_rotation{1.0f, 0.0f, 0.0f, 0.0f};

    std::unique_ptr<Node> clone() const override { return std::make_unique<Tweener>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("tweenPosition", tween_position);
        d.member("tweenRotation", tween_rotation);
        d.member("useTweenDuration", use_tween_duration);
        d.member("tweenDuration", tween_duration);
        d.member("targetPosition", target_position);
        d.member("targetRotation", target_rotation);
    }
    void activate(Context& ctx) override {
        m_start = ctx.character.world_from_model;
        m_time = 0.0f;
        m_duration = 0.0f;
    }
    void update(Context&, float timestep) override { m_time += timestep; }
    void modify_sync(Context&, SyncInfo& sync) override {
        if (use_tween_duration) {
            m_duration = tween_duration;
        } else if (sync.base_frequency != 0.0f) {
            m_duration = 1.0f / sync.base_frequency;
        }
    }
    void modify(Context&, Output& output) override {
        float t = 0.0f;
        if (m_duration != 0.0f && m_time / m_duration >= 0.0f) {
            t = std::min(m_time / m_duration, 1.0f);
        }
        if (tween_position) {
            output.world_from_model.t = (1.0f - t) * m_start.t + t * xyz(target_position);
        }
        if (tween_rotation) {
            const glm::quat end =
                glm::dot(m_start.r, target_rotation) < 0.0f ? -target_rotation : target_rotation;
            output.world_from_model.r = glm::normalize(m_start.r * (1.0f - t) + end * t);
        }
    }

  private:
    Qs m_start;
    float m_time = 0.0f;
    float m_duration = 0.0f;
};

struct EventRangeData final : Bindable {
    float upper_bound = 0.0f;
    EventProperty event;
    int8_t event_mode = 0;

    void describe(Describer& d) override {
        d.member("upperBound", upper_bound);
        d.member("event", event);
        d.member("eventMode", event_mode);
    }
};

struct EventRangeDataArray final : Bindable {
    std::vector<EventRangeData> event_data;
    void describe(Describer& d) override { d.objects("eventData", event_data); }
};

class EventsFromRange final : public Modifier {
  public:
    float input_value = 0.0f;
    float lower_bound = 0.0f;
    EventRangeDataArray event_ranges;

    std::unique_ptr<Node> clone() const override { return std::make_unique<EventsFromRange>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("inputValue", input_value);
        d.member("lowerBound", lower_bound);
        d.object("eventRanges", event_ranges);
    }
    void activate(Context&) override { m_was_active.assign(event_ranges.event_data.size(), 0); }
    void update(Context& ctx, float) override {
        constexpr int8_t k_send_on_enter = 0;
        constexpr int8_t k_send_when_in_range = 1;
        bool searching = lower_bound <= input_value; // ranges stack, so at most one matches
        for (size_t i = 0; i < event_ranges.event_data.size() && i < m_was_active.size(); ++i) {
            const EventRangeData& range = event_ranges.event_data[i];
            if (range.upper_bound < input_value || !searching) {
                m_was_active[i] = 0;
                continue;
            }
            searching = false;
            if (range.event_mode == k_send_when_in_range ||
                (range.event_mode == k_send_on_enter && !m_was_active[i])) {
                ctx.graph.raise(ctx, range.event);
            }
            m_was_active[i] = 1;
        }
    }

  private:
    std::vector<uint8_t> m_was_active;
};

struct IStateData final : Bindable {
    Node* state_machine = nullptr;
    int32_t state_id = 0;
    int32_t state_to_set_as = 0;

    void describe(Describer& d) override {
        d.node("pStateMachine", state_machine);
        d.member("StateID", state_id);
        d.member("iStateToSetAs", state_to_set_as);
    }
};

// Inert: nothing in the game attaches its listener, so iStateVar keeps its bound value.
class IStateManager final : public Modifier {
  public:
    int32_t state_var = 0;
    std::vector<IStateData> state_data;

    std::unique_ptr<Node> clone() const override { return std::make_unique<IStateManager>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("iStateVar", state_var, true);
        d.objects("stateData", state_data);
    }
};

class GetTimeStep final : public Modifier {
  public:
    float time_step = 0.0f;

    std::unique_ptr<Node> clone() const override { return std::make_unique<GetTimeStep>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("timeStep", time_step, true);
    }
    void update(Context&, float timestep) override { time_step = timestep; }
};

class PassByTargetTrigger final : public Modifier {
  public:
    glm::vec4 target_position{0.0f};
    float radius = 0.0f;
    glm::vec4 movement_direction{0.0f};
    EventProperty trigger_event;

    std::unique_ptr<Node> clone() const override { return std::make_unique<PassByTargetTrigger>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("targetPosition", target_position);
        d.member("radius", radius);
        d.member("movementDirection", movement_direction);
        d.member("triggerEvent", trigger_event);
    }
    // True at the start, which stops a firing on the first frame.
    void activate(Context&) override { m_target_passed = true; }
    void modify(Context& ctx, Output&) override {
        const Character& character = ctx.character;
        glm::vec3 target{0.0f};
        glm::vec3 movement{0.0f};
        if (!character.pass_by_target || !character.pass_by_target(target, movement)) {
            m_target_passed = false;
            return;
        }
        target_position = glm::vec4(target, 0.0f);
        const glm::vec3 to_target = target - character.world_from_model.t;
        if (glm::length(to_target) >= radius) {
            m_target_passed = false;
            return;
        }
        const glm::vec3 direction = normalized(movement);
        movement_direction = glm::vec4(direction, 0.0f);
        const bool passed = glm::dot(normalized(to_target), direction) < 0.0f;
        if (passed && !m_target_passed) {
            ctx.graph.raise(ctx, trigger_event);
        }
        m_target_passed = passed;
    }

  private:
    bool m_target_passed = false;
};

class DecomposeVector final : public Modifier {
  public:
    glm::vec4 vector{0.0f};
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    std::unique_ptr<Node> clone() const override { return std::make_unique<DecomposeVector>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("vector", vector);
        d.member("x", x, true);
        d.member("y", y, true);
        d.member("z", z, true);
        d.member("w", w, true);
    }
    void update(Context&, float) override {
        x = vector.x;
        y = vector.y;
        z = vector.z;
        w = vector.w;
    }
};

class LimbIk final : public Modifier {
  public:
    float limit_degrees = 0.0f;
    int16_t start_bone = -1;
    int16_t end_bone = -1;
    float gain = 0.0f;
    float bone_radius = 0.0f;
    float cast_offset = 0.0f;

    std::unique_ptr<Node> clone() const override { return std::make_unique<LimbIk>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("limitAngleDegrees", limit_degrees);
        d.member("startBoneIndex", start_bone);
        d.member("endBoneIndex", end_bone);
        d.member("gain", gain);
        d.member("boneRadius", bone_radius);
        d.member("castOffset", cast_offset);
    }
    void activate(Context&) override { m_current_angle = 0.0f; }
    void update(Context&, float timestep) override { m_time_step = timestep; }
    void deactivate(Context&) override { m_time_step = 0.0f; }

    void modify(Context& ctx, Output& output) override {
        Character& character = ctx.character;
        const int32_t n = static_cast<int32_t>(output.pose.size());
        if (!character.skeleton || !character.project || !character.shapes_in_box || !character.cast_ray ||
            start_bone < 0 || end_bone < start_bone || end_bone >= n) {
            return;
        }
        ModelPose pose(*character.skeleton, output);
        const Qs& w = track_0(ctx, output);
        const glm::vec3& up = character.project->world_up();

        constexpr float k_scale = 69.991249f; // the utility's,
        constexpr uint32_t k_collision_layer = 43;
        const float offset = k_scale * cast_offset;
        const float span = k_scale * bone_radius + offset;

        // The bones are the consecutive index range, not a parent chain.
        struct Cast {
            glm::vec3 position, top, bottom;
        };
        std::vector<Cast> casts;
        glm::vec3 lo{FLT_MAX};
        glm::vec3 hi{-FLT_MAX};
        for (int32_t b = start_bone; b <= end_bone; ++b) {
            const glm::vec3 position = to_world(w, pose.model(b).t);
            const glm::vec3 top = position + glm::vec3(0.0f, 0.0f, offset);
            const glm::vec3 bottom = top - up * span;
            casts.push_back({position, top, bottom});
            lo = glm::min(lo, glm::min(top, bottom));
            hi = glm::max(hi, glm::max(top, bottom));
        }
        if (!character.shapes_in_box(lo, hi)) {
            return;
        }

        float best = -FLT_MAX;
        bool kept = false;
        glm::vec3 kept_bottom{0.0f};
        glm::vec3 kept_hit{0.0f};
        for (const Cast& c : casts) {
            float fraction = 0.0f;
            glm::vec3 normal{0.0f};
            if (cast(character, k_collision_layer, c.top, c.bottom, fraction, normal)) {
                const float height = c.top.z - fraction * span;
                if (height - c.bottom.z > best) {
                    best = height - c.bottom.z;
                    kept = true;
                    kept_bottom = c.bottom;
                    kept_hit = {c.position.x, c.position.y, height};
                }
            }
        }

        float wanted = 0.0f;
        if (kept) {
            const glm::vec3 origin = pose.model(start_bone).t;
            const glm::vec3 a = normalized(to_model(w, kept_bottom) - origin);
            const glm::vec3 b = normalized(to_model(w, kept_hit) - origin);
            wanted = std::min(glm::radians(limit_degrees), angle_of(glm::dot(a, b)));
        }
        if (wanted == 0.0f && m_current_angle == 0.0f) {
            return;
        }
        m_current_angle += (wanted - m_current_angle) * gain_fraction(gain, m_time_step);
        if (wanted == 0.0f && std::fabs(m_current_angle) < 0.01f) {
            m_current_angle = 0.0f;
        }

        const int32_t parent = pose.parent(start_bone);
        const glm::quat to_parent =
            parent >= 0 ? glm::conjugate(pose.model(parent).r) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        Qs local = pose.local(start_bone);
        local.r =
            glm::angleAxis(m_current_angle, normalized(to_parent * glm::vec3(-1.0f, 0.0f, 0.0f))) * local.r;
        pose.set_local(start_bone, local);
    }

  private:
    float m_current_angle = 0.0f;
    float m_time_step = 0.0f;
};

class TransformVector final : public Modifier {
  public:
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 translation{0.0f};
    glm::vec4 vector_in{0.0f};
    glm::vec4 vector_out{0.0f};
    bool rotate_only = false;
    bool inverse_transform = false;
    bool compute_on_activate = false;
    bool compute_on_modify = false;

    std::unique_ptr<Node> clone() const override { return std::make_unique<TransformVector>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("rotation", rotation);
        d.member("translation", translation);
        d.member("vectorIn", vector_in);
        d.member("vectorOut", vector_out, true);
        d.member("rotateOnly", rotate_only);
        d.member("inverse", inverse_transform);
        d.member("computeOnActivate", compute_on_activate);
        d.member("computeOnModify", compute_on_modify);
    }
    void activate(Context&) override {
        if (compute_on_activate) {
            compute();
        } else {
            vector_out = glm::vec4(0.0f);
        }
    }
    void modify(Context&, Output&) override {
        if (compute_on_modify) {
            compute();
        }
    }

  private:
    void compute() {
        const glm::vec3 v = xyz(vector_in);
        const glm::quat inverted = glm::conjugate(rotation);
        glm::vec3 out{0.0f};
        if (rotate_only) {
            out = (inverse_transform ? inverted : rotation) * v;
        } else {
            out = inverse_transform ? inverted * (v - xyz(translation)) : rotation * v + xyz(translation);
        }
        vector_out = glm::vec4(out, 0.0f);
    }
};

// The ragdoll's modifiers load and declare their members so that bindings resolve. The ragdoll workstream
// gives them their behaviour (the design's contracts for the ragdoll driver).

struct PoweredRagdollControlData final : Bindable {
    float max_force = 0.0f;
    float tau = 0.0f;
    float damping = 0.0f;
    float proportional_recovery_velocity = 0.0f;
    float constant_recovery_velocity = 0.0f;

    void describe(Describer& d) override {
        d.member("maxForce", max_force);
        d.member("tau", tau);
        d.member("damping", damping);
        d.member("proportionalRecoveryVelocity", proportional_recovery_velocity);
        d.member("constantRecoveryVelocity", constant_recovery_velocity);
    }
};

struct WorldFromModelModeData final : Bindable {
    int16_t pose_matching_bone_0 = -1;
    int16_t pose_matching_bone_1 = -1;
    int16_t pose_matching_bone_2 = -1;
    int8_t mode = 0;

    void describe(Describer& d) override {
        d.member("poseMatchingBone0", pose_matching_bone_0);
        d.member("poseMatchingBone1", pose_matching_bone_1);
        d.member("poseMatchingBone2", pose_matching_bone_2);
        d.member("mode", mode);
    }
};

class PoweredRagdollControls final : public Modifier {
  public:
    PoweredRagdollControlData control_data;
    BoneIndexArray bones;
    WorldFromModelModeData world_from_model_mode_data;
    BoneWeightArray bone_weights;

    std::unique_ptr<Node> clone() const override { return std::make_unique<PoweredRagdollControls>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.object("controlData", control_data);
        d.object("bones", bones);
        d.object("worldFromModelModeData", world_from_model_mode_data);
        d.object("boneWeights", bone_weights);
    }
    void collect_bindables(std::vector<Bindable*>& out) override {
        out.push_back(&bones);
        out.push_back(&bone_weights);
    }
};

struct KeyframeInfo final : Bindable {
    glm::vec4 keyframed_position{0.0f};
    glm::quat keyframed_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    int16_t bone_index = -1;
    bool is_valid = false;

    void describe(Describer& d) override {
        d.member("keyframedPosition", keyframed_position);
        d.member("keyframedRotation", keyframed_rotation);
        d.member("boneIndex", bone_index);
        d.member("isValid", is_valid);
    }
};

class KeyframeBones final : public Modifier {
  public:
    std::vector<KeyframeInfo> keyframe_info;
    BoneIndexArray keyframed_bones_list;

    std::unique_ptr<Node> clone() const override { return std::make_unique<KeyframeBones>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.objects("keyframeInfo", keyframe_info);
        d.object("keyframedBonesList", keyframed_bones_list);
    }
    void collect_bindables(std::vector<Bindable*>& out) override { out.push_back(&keyframed_bones_list); }
};

struct KeyFrameHierarchyControlData final : Bindable {
    float hierarchy_gain = 0.0f;
    float velocity_damping = 0.0f;
    float acceleration_gain = 0.0f;
    float velocity_gain = 0.0f;
    float position_gain = 0.0f;
    float position_max_linear_velocity = 0.0f;
    float position_max_angular_velocity = 0.0f;
    float snap_gain = 0.0f;
    float snap_max_linear_velocity = 0.0f;
    float snap_max_angular_velocity = 0.0f;
    float snap_max_linear_distance = 0.0f;
    float snap_max_angular_distance = 0.0f;

    void describe(Describer& d) override {
        d.member("hierarchyGain", hierarchy_gain);
        d.member("velocityDamping", velocity_damping);
        d.member("accelerationGain", acceleration_gain);
        d.member("velocityGain", velocity_gain);
        d.member("positionGain", position_gain);
        d.member("positionMaxLinearVelocity", position_max_linear_velocity);
        d.member("positionMaxAngularVelocity", position_max_angular_velocity);
        d.member("snapGain", snap_gain);
        d.member("snapMaxLinearVelocity", snap_max_linear_velocity);
        d.member("snapMaxAngularVelocity", snap_max_angular_velocity);
        d.member("snapMaxLinearDistance", snap_max_linear_distance);
        d.member("snapMaxAngularDistance", snap_max_angular_distance);
    }
};

struct RigidBodyRagdollControlData final : Bindable {
    KeyFrameHierarchyControlData key_frame_hierarchy_control_data;
    float duration_to_blend = 0.0f;

    void describe(Describer& d) override {
        d.object("keyFrameHierarchyControlData", key_frame_hierarchy_control_data);
        d.member("durationToBlend", duration_to_blend);
    }
};

class RigidBodyRagdollControls final : public Modifier {
  public:
    RigidBodyRagdollControlData control_data;
    BoneIndexArray bones;

    std::unique_ptr<Node> clone() const override { return std::make_unique<RigidBodyRagdollControls>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.object("controlData", control_data);
        d.object("bones", bones);
    }
    void collect_bindables(std::vector<Bindable*>& out) override { out.push_back(&bones); }
};

class RagdollContactListener final : public Modifier {
  public:
    EventProperty contact_event;
    BoneIndexArray bones;

    std::unique_ptr<Node> clone() const override { return std::make_unique<RagdollContactListener>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("contactEvent", contact_event);
        d.object("bones", bones);
    }
    void collect_bindables(std::vector<Bindable*>& out) override { out.push_back(&bones); }
    void update(Context& ctx, float) override {
        if (ctx.character.ragdoll_touching && ctx.character.ragdoll_touching(bones.indices)) {
            ctx.graph.raise(ctx, contact_event);
        }
    }
};

class GetUp final : public Modifier {
  public:
    glm::vec4 ground_normal{0.0f};
    float duration = 0.0f;
    float align_with_ground_duration = 0.0f;
    int16_t root_bone = -1;
    int16_t other_bone = -1;
    int16_t another_bone = -1;

    std::unique_ptr<Node> clone() const override { return std::make_unique<GetUp>(*this); }
    void describe(Describer& d) override {
        Modifier::describe(d);
        d.member("groundNormal", ground_normal);
        d.member("duration", duration);
        d.member("alignWithGroundDuration", align_with_ground_duration);
        d.member("rootBoneIndex", root_bone);
        d.member("otherBoneIndex", other_bone);
        d.member("anotherBoneIndex", another_bone);
    }
};

} // namespace

std::unique_ptr<Bindable> make_foot_ik_info() {
    return std::make_unique<FootIkInfo>();
}

void run_foot_ik_driver(Character& character, Output& output, float dt) {
    if (FootIkDriver* driver = foot_ik_driver(character)) {
        driver->run(character, output, dt);
    }
    character.foot_ik_fraction = 0.0f; // the track is cleared with the output each frame
}

void register_modifier_nodes() {
    register_node<IsActive>("BSIsActiveModifier");
    register_node<LookAt>("BSLookAtModifier");
    register_node<EventOnDeactivate>("BSEventOnDeactivateModifier");
    register_node<PoweredRagdollControls>("hkbPoweredRagdollControlsModifier");
    register_node<KeyframeBones>("hkbKeyframeBonesModifier");
    register_node<EventEveryNEvents>("BSEventEveryNEventsModifier");
    register_node<Timer>("hkbTimerModifier");
    register_node<RigidBodyRagdollControls>("hkbRigidBodyRagdollControlsModifier");
    register_node<RagdollContactListener>("BSRagdollContactListenerModifier");
    register_node<GetUp>("hkbGetUpModifier");
    register_node<Damping>("hkbDampingModifier");
    register_node<SpeedSampler>("BSSpeedSamplerModifier");
    register_node<InterpValue>("BSInterpValueModifier");
    register_node<ModifyOnce>("BSModifyOnceModifier");
    register_node<FootIkControls>("hkbFootIkControlsModifier");
    register_node<EventOnFalseToTrue>("BSEventOnFalseToTrueModifier");
    register_node<DirectAt>("BSDirectAtModifier");
    register_node<Twist>("hkbTwistModifier");
    register_node<RotateCharacter>("hkbRotateCharacterModifier");
    register_node<Tweener>("BSTweenerModifier");
    register_node<EventsFromRange>("hkbEventsFromRangeModifier");
    register_node<IStateManager>("BSIStateManagerModifier");
    register_node<FootIk>("hkbFootIkModifier");
    register_node<GetTimeStep>("BSGetTimeStepModifier");
    register_node<PassByTargetTrigger>("BSPassByTargetTriggerModifier");
    register_node<DecomposeVector>("BSDecomposeVectorModifier");
    register_node<BsTimer>("BSTimerModifier");
    register_node<LimbIk>("BSLimbIKModifier");
    register_node<TransformVector>("hkbTransformVectorModifier");
}

} // namespace spud::anim
