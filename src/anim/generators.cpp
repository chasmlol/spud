// Clips, blenders and the other generators. The state machine family and the
// structural nodes live elsewhere. Character scale never appears here: hkbCharacterData.scale is 1 in all 353 characters.

#include "anim/graph.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <optional>
#include <typeinfo>

namespace spud::anim {

namespace {

constexpr float k_epsilon = 1.19209e-7f;
constexpr float k_unset = std::bit_cast<float>(0x7f7fffeeu); // a float slot nothing wrote
constexpr float k_unbound_duration = 0.01333f;
constexpr uint32_t k_event_silent = 1, k_event_sync_point = 2;
constexpr uint32_t k_clip_sync_points = 8; // hkbGeneratorSyncInfo's own limit; the array is sized for more
constexpr size_t k_max_echoes = 2;

// "A ∘ B" of the design: unlike Qs's operator*, scale doesn't reach the translation.
Qs compose(const Qs& a, const Qs& b) { return {a.t + a.r * b.t, a.r * b.r, a.s * b.s}; }

Qs inverse(const Qs& a) {
    const glm::quat q = glm::conjugate(a.r);
    return {q * -a.t, q, 1.0f / a.s};
}

// A blended rotation: the sign that keeps it on the accumulator's side, with a dot of exactly 0 counting as positive.
void add_rotation(glm::quat& sum, const glm::quat& q, float weight) {
    const float w = glm::dot(q, sum) >= 0.0f ? weight : -weight;
    sum.x += w * q.x;
    sum.y += w * q.y;
    sum.z += w * q.z;
    sum.w += w * q.w;
}

glm::quat normalised(const glm::quat& q) {
    const float n = glm::dot(q, q);
    return n > 0.0f ? q / std::sqrt(n) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

void add_transform(Qs& sum, const Qs& v, float weight) {
    sum.t += v.t * weight;
    sum.s += v.s * weight;
    add_rotation(sum.r, v.r, weight);
}

Qs zero_transform() { return {glm::vec3(0.0f), glm::quat(0.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f)}; }

bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

bool event_silent(const Behavior* b, int32_t id) {
    return b && id >= 0 && size_t(id) < b->event_flags.size() && (b->event_flags[size_t(id)] & k_event_silent);
}

// --- Sync ---------------------------------------------------------------------------------------------------------

float period_of(const SyncInfo& s) { return s.base_frequency > 0.0f ? 1.0f / s.base_frequency : 0.0f; }

void zero_sync(SyncInfo& s) {
    s.base_frequency = s.local_time = 0.0f;
    s.playback_speed = 1.0f;
    s.point_count = 0;
}

struct SyncPosition {
    int32_t previous = -1, next = -1;
    float fraction = 0.0f;
};

// "Position on a timeline".
SyncPosition sync_position(const SyncInfo& s, float t) {
    SyncPosition out;
    const uint32_t n = s.point_count;
    if (n == 0) {
        out.fraction = std::clamp(t * s.base_frequency, 0.0f, 1.0f);
        return out;
    }
    const float period = period_of(s);
    if (n == 1 && s.cyclic) {
        out.previous = out.next = s.points[0].id;
        const float x = t < s.points[0].time ? t + period : t;
        out.fraction = std::clamp((x - s.points[0].time) * s.base_frequency, 0.0f, 1.0f);
        return out;
    }
    uint32_t chosen = 0;
    bool none_after = true;
    for (uint32_t i = 0; i < n && none_after; ++i) {
        if (s.points[i].time >= t) {
            chosen = i;
            none_after = false;
        }
    }
    float later = s.points[chosen].time, earlier = 0.0f;
    out.next = s.points[chosen].id;
    if (chosen > 0) {
        out.previous = s.points[chosen - 1].id;
        earlier = s.points[chosen - 1].time;
    } else {
        const SyncInfo::Point& last = s.points[n - 1];
        const bool past = none_after || t > later;
        if (s.cyclic) {
            out.previous = last.id;
            earlier = last.time;
            if (past) {
                later += period;
            } else {
                earlier -= period;
            }
        } else if (past) {
            out.previous = last.id;
            earlier = last.time;
            out.next = -1;
            later = period;
        } else if (later == 0.0f) {
            if (n == 1) {
                return {};
            }
            out.previous = s.points[0].id;
            out.next = s.points[1].id;
            later = s.points[1].time;
        }
    }
    out.fraction = later > earlier ? std::clamp((t - earlier) / (later - earlier), 0.0f, 1.0f) : 0.0f;
    return out;
}

// "Time from a position"; nothing when no point matches.
std::optional<float> time_at(const SyncInfo& s, const SyncPosition& p) {
    const float period = period_of(s);
    auto find = [&](int32_t id) -> int32_t {
        for (uint32_t i = 0; i < s.point_count && id != -1; ++i) {
            if (s.points[i].id == id) {
                return int32_t(i);
            }
        }
        return -1;
    };
    if (p.previous == -1 && p.next == -1) {
        return s.base_frequency > 0.0f ? p.fraction / s.base_frequency : 0.0f;
    }
    const int32_t a = find(p.previous), b = find(p.next);
    if (p.previous == -1 && b >= 0) {
        return s.points[size_t(b)].time * p.fraction;
    }
    if (p.next == -1 && a >= 0) {
        const float t = s.points[size_t(a)].time;
        return (period - t) * p.fraction + t;
    }
    if (a < 0 && b < 0) {
        return std::nullopt;
    }
    float out;
    if (a >= 0 && b >= 0 && p.previous != p.next) {
        const float start = s.points[size_t(a)].time, end = s.points[size_t(b)].time;
        if (b > a) {
            return (end - start) * p.fraction + start;
        }
        out = (end + period - start) * p.fraction + start;
    } else {
        out = period * p.fraction + s.points[size_t(a >= 0 ? a : b)].time;
    }
    if (out + 0.00001f >= period) {
        out = std::clamp(out - period, 0.0f, period);
    }
    return out;
}

const SyncInfo::Point* find_point(const SyncInfo& s, int32_t id) {
    for (uint32_t i = 0; i < s.point_count; ++i) {
        if (s.points[i].id == id) {
            return &s.points[i];
        }
    }
    return nullptr;
}

// "Combining children".
void combine_sync(SyncInfo& own, std::span<const SyncInfo* const> kids, std::span<const float> weights, float threshold,
                  bool first_sync) {
    std::array<int32_t, k_max_sync_points> common{};
    uint32_t n = 0;
    for (uint32_t i = 0; i < kids[0]->point_count && n < common.size(); ++i) {
        const int32_t id = kids[0]->points[i].id;
        if (std::ranges::all_of(kids, [&](const SyncInfo* k) { return find_point(*k, id) != nullptr; })) {
            common[n++] = id;
        }
    }
    const bool from_first = first_sync || n != own.point_count;
    if (from_first) {
        own.cyclic = kids[0]->cyclic;
    }
    const SyncInfo& source = from_first ? *kids[0] : own;
    if (source.base_frequency == 0.0f) {
        zero_sync(own);
        return;
    }
    // The position counts only the common points.
    SyncInfo filtered = source;
    filtered.point_count = 0;
    for (uint32_t i = 0; i < source.point_count; ++i) {
        if (std::find(common.begin(), common.begin() + n, source.points[i].id) != common.begin() + n) {
            filtered.points[filtered.point_count++] = source.points[i];
        }
    }
    const SyncPosition position = sync_position(filtered, source.local_time);
    const float old_time = source.local_time;

    own.point_count = n;
    for (uint32_t i = 0; i < n; ++i) {
        own.points[i].id = common[i];
    }
    if (n > 0 && !(own.cyclic && n == 1)) {
        std::array<float, k_max_sync_points> intervals{};
        float total = 0.0f, tail = 0.0f;
        for (size_t c = 0; c < kids.size(); ++c) {
            const float w = weights[c];
            if (w <= 0.0f) {
                continue;
            }
            const float period = period_of(*kids[c]);
            std::array<float, k_max_sync_points> times{};
            for (uint32_t i = 0; i < n; ++i) {
                times[i] = find_point(*kids[c], common[i])->time;
            }
            for (uint32_t i = n; i-- > 0;) {
                float length;
                if (i == 0 && !own.cyclic) {
                    length = times[0];
                } else {
                    const float start = times[(i + n - 1) % n], end = times[i];
                    length = end > start ? end - start : end - start + period;
                }
                intervals[i] += w * length;
            }
            if (!own.cyclic) {
                tail += w * (period - times[n - 1]);
            }
            total += w;
        }
        if (total <= 0.0f) {
            zero_sync(own);
            return;
        }
        float running = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            running += intervals[i] / total;
            own.points[i].time = running;
        }
        const float period = own.cyclic ? running : running + tail / total;
        own.playback_speed = 1.0f;
        own.base_frequency = period > 0.0f ? 1.0f / period : 0.0f;
    } else {
        float w_sum = 0.0f, p_sum = 0.0f, v_sum = 0.0f;
        for (size_t c = 0; c < kids.size(); ++c) {
            if (weights[c] > 0.0f && kids[c]->base_frequency != 0.0f) {
                w_sum += weights[c];
                p_sum += weights[c] / kids[c]->base_frequency;
                v_sum += weights[c] * kids[c]->playback_speed;
            }
        }
        if (w_sum == 0.0f) {
            zero_sync(own);
            return;
        }
        if (threshold > 0.0f) {
            w_sum = std::max(w_sum, threshold);
        }
        own.playback_speed = v_sum / w_sum;
        own.base_frequency = w_sum / p_sum;
        if (n == 1) {
            own.points[0].time = 0.0f;
        }
    }
    own.local_time = time_at(own, position).value_or(old_time);
    own.mirrored = false;
}

SyncInfo* sync_of(Context& ctx, const Node* child) {
    Record* r = ctx.graph.sync_record(ctx.graph.record_of(child));
    return r ? &r->sync : nullptr;
}

// --- Mirroring ----------------------------------------------------------------------------------------------------

Qs mirror_transform(const MirroredSkeleton& m, const Skeleton& skeleton, int32_t bone, const Qs& v, bool additive) {
    const int32_t parent = skeleton.parents[size_t(bone)];
    const glm::quat q_parent = parent >= 0 ? m.rotations[size_t(parent)] : m.axis;
    const glm::quat q_bone = additive ? q_parent : m.rotations[size_t(bone)];
    return {-(q_parent * v.t), q_parent * v.r * glm::conjugate(q_bone), v.s};
}

// --- hkbClipGenerator ---------------------------------------------------------------------------------------------

struct ClipTrigger final : Bindable {
    float local_time = 0.0f;
    EventProperty event;
    bool relative_to_end = false, acyclic = false, is_annotation = false;

    void describe(Describer& d) override {
        d.member("localTime", local_time);
        d.member("event", event);
        d.member("relativeToEndOfClip", relative_to_end);
        d.member("acyclic", acyclic);
        d.member("isAnnotation", is_annotation);
    }
};

struct ClipTriggerArray final : Bindable {
    std::vector<ClipTrigger> triggers;
    void describe(Describer& d) override { d.objects("triggers", triggers); }
};

class ClipGenerator final : public Generator {
public:
    enum Mode : int8_t { single_play = 0, looping = 1, user_controlled = 2, ping_pong = 3 };
    enum Flags : int8_t {
        flag_sync_half_cycle = 2,
        flag_mirror = 4,
        flag_force_dense = 8,
        flag_dont_convert_annotations = 16,
    };

    std::string animation_name;
    ClipTriggerArray triggers;
    float crop_start = 0.0f, crop_end = 0.0f, start_time = 0.0f, playback_speed = 1.0f, enforced_duration = 0.0f;
    float user_fraction = 0.0f;
    int16_t binding_index = -1;
    int8_t mode = single_play;
    int8_t flags = 0;

    std::unique_ptr<Node> clone() const override { return std::make_unique<ClipGenerator>(*this); }
    void describe(Describer& d) override {
        d.member("animationName", animation_name);
        d.object("triggers", triggers);
        d.member("cropStartAmountLocalTime", crop_start);
        d.member("cropEndAmountLocalTime", crop_end);
        d.member("startTime", start_time);
        d.member("playbackSpeed", playback_speed);
        d.member("enforcedDuration", enforced_duration);
        d.member("userControlledTimeFraction", user_fraction);
        d.member("animationBindingIndex", binding_index);
        d.member("mode", mode);
        d.member("flags", flags);
    }

    // The project holds every clip, so the streaming states of `userData` never arise: an activation always binds.
    void activate(Context& ctx) override {
        m_clip = ctx.character.project->clip(animation_name);
        m_triggers.clear();
        for (const ClipTrigger& t : triggers.triggers) {
            m_triggers.push_back({t.local_time, t.event, t.relative_to_end, t.acyclic});
        }
        if (m_clip && !(flags & flag_dont_convert_annotations) && ctx.behavior) {
            const float cropped = cropped_duration();
            for (const BindingTrigger& b : ctx.character.project->binding_triggers(*m_clip)) {
                const float t = b.time - crop_start;
                if (t < 0.0f || t > cropped) {
                    continue;
                }
                // The game logs an event the behaviour lacks and skips it; here that would be a line per activation.
                const int32_t id = ctx.behavior->event_internal(b.event);
                if (id >= 0) {
                    m_triggers.push_back({t, {id, b.payload}, false, false});
                }
            }
        }
        m_skeleton_bones = ctx.character.skeleton->size();
        count_tracks();
        reset();
    }

    void deactivate(Context&) override {
        m_ignore_start_time = false;
        m_echo_count = 0;
        m_triggers.clear();
        m_clip = nullptr;
        m_local_bones.clear();
    }

    void update(Context& ctx, float dt) override {
        if (std::abs(playback_speed) < 0.001f) {
            playback_speed = playback_speed > 0.0f ? 0.001f : -0.001f; // so zero plays backwards, very slowly
        }
        m_control_time = m_local_time;
        if (m_at_end && mode != single_play) {
            reset();
        }
        const bool user = mode == user_controlled;
        if (user) {
            user_fraction = std::clamp(user_fraction, 0.0f, 1.0f);
        }
        if (m_at_end) {
            const float s = speed();
            m_at_end = m_local_time > crop_start ? s >= 0.0f : s <= 0.0f;
        }
        // With atEnd set the time step only feeds extracted motion, and none of that exists here: see generate.
        if (!m_at_end) {
            const Step step = time_step(dt);
            m_local_time = m_control_time = step.next;
            m_at_end = step.at_end;
            fire_triggers(ctx, step);
            if (!user && (step.loops & 1)) {
                m_ping_pong_backward = !m_ping_pong_backward;
            }
        }
        if (!user) {
            const float advance = dt * speed();
            fire_acyclic(ctx, m_time, m_time + advance);
            m_time += advance;
            for (size_t i = 0; i < m_echo_count;) {
                m_echoes[i].weight += dt * m_echoes[i].dwdt;
                if (m_echoes[i].weight <= 0.0f) {
                    std::copy(m_echoes.begin() + i + 1, m_echoes.begin() + m_echo_count, m_echoes.begin() + i);
                    --m_echo_count;
                } else {
                    ++i;
                }
            }
        }
        m_previous_fraction = user_fraction;
    }

    // Track 1 is identity, track 0 the character's worldFromModel: a clip's motion comes from one of two places. The
    // text caches feed the game's side through Graph::active_clips, and the 13 clips with an extractedMotion object
    // don't carry it in .osn. timeOffset has no live path and doesn't change the sampled pose either way.
    void generate(Context& ctx, std::span<const Output* const>, Output& out, float) override {
        m_control_time = m_local_time;
        out.world_from_model = ctx.character.world_from_model;
        out.world_from_model_fraction = 1.0f;
        out.motion = Qs{};
        out.motion_fraction = 1.0f;
        if (out.pose.empty()) {
            return;
        }
        const bool additive = is_additive();
        if (!out.floats.empty()) {
            std::fill(out.floats.begin(), out.floats.end(), additive ? 0.0f : k_unset);
            out.floats_fraction = 1.0f;
        }
        std::fill(out.present.begin(), out.present.end(), uint8_t{0});
        out.all_present = false;
        sample(ctx, out, additive);
        if ((flags & flag_force_dense) && !out.all_present) {
            const Skeleton& skeleton = *ctx.character.skeleton;
            for (size_t b = 0; b < out.pose.size(); ++b) {
                if (!out.present[b]) {
                    out.pose[b] = additive ? Qs{} : skeleton.reference[b];
                }
            }
            std::fill(out.present.begin(), out.present.end(), uint8_t{1});
            out.all_present = true;
        }
        out.pose_fraction = 1.0f;
        out.additive = additive;
    }

    void update_sync(Context&) override {
        if (!record) {
            return;
        }
        SyncInfo& s = record->sync;
        s.point_count = 0;
        s.playback_speed = 1.0f;
        s.cyclic = mode == looping;
        const float v = speed();
        if (v == 0.0f) {
            s.base_frequency = 0.0f;
        } else {
            const float inv = 1.0f / std::abs(v);
            const float cropped = cropped_duration();
            float period = cropped * inv;
            float sync = ((m_local_time - crop_start) - start_offset()) * inv;
            if (sync < 0.0f) {
                sync += period;
            }
            if (doubled_ping_pong()) {
                period *= 2.0f;
            }
            if (v < 0.0f) {
                sync = period - sync;
            }
            s.local_time = std::min(sync, period);
            s.base_frequency = period > 0.0f ? 1.0f / period : 0.0f;
            const Behavior* b = record->behavior;
            for (const Trigger& t : m_triggers) {
                if (s.point_count >= k_clip_sync_points) {
                    break;
                }
                const size_t id = size_t(t.event.id);
                if (b && t.event.id >= 0 && id < b->event_flags.size() && (b->event_flags[id] & k_event_sync_point)) {
                    s.points[s.point_count++] = {b->event_to_external[id], effective(t, cropped) / std::abs(playback_speed)};
                }
            }
        }
        s.additive = is_additive();
        // The mirrored id table's writer was never found, and an empty table leaves ids alone.
        s.mirrored = (flags & flag_mirror) != 0;
    }

    void set_local_time(Context&, float sync_time) override {
        crop_control();
        float u = std::abs(speed()) * sync_time + start_offset();
        m_ping_pong_backward = false;
        const bool doubled = doubled_ping_pong();
        const float period = doubled ? cropped_duration() * 2.0f : cropped_duration();
        if (u > period && period > 0.0f) {
            u = std::max(u - std::floor(u / period) * period, 0.0f);
        }
        if (doubled && u > period * 0.5f) {
            m_ping_pong_backward = true;
            u = period - u;
        }
        if (playback_speed < 0.0f) {
            u = period - u;
        }
        m_local_time = m_control_time = u + crop_start;
        m_ignore_start_time = true;
    }

    void start_echo(Context&, float duration) override {
        if (mode == user_controlled) {
            return;
        }
        float offset = m_local_time - (start_offset() + crop_start);
        if (offset < 0.0f) {
            offset += m_clip ? m_clip->data.duration : 0.0f;
        }
        m_ignore_start_time = false;
        reset();
        if (duration == 0.0f) {
            m_echo_count = 0;
            return;
        }
        const float cropped = cropped_duration();
        float sum = 0.0f, smallest = 1.0f;
        size_t smallest_at = m_echo_count;
        for (size_t i = 0; i < m_echo_count; ++i) {
            Echo& e = m_echoes[i];
            e.offset += offset;
            if (mode == looping && e.offset > cropped) {
                e.offset -= cropped;
            }
            sum += e.weight;
            if (e.weight < smallest) {
                smallest = e.weight;
                smallest_at = i;
            }
        }
        size_t slot = m_echo_count;
        if (m_echo_count < k_max_echoes) {
            ++m_echo_count;
        } else {
            if (smallest >= 1.0f - sum || smallest_at == m_echo_count) {
                return;
            }
            slot = smallest_at;
        }
        m_echoes[slot] = {offset, 1.0f - sum, -1.0f / duration};
    }

    bool clip_state(ActiveClip& c) const override {
        c.name = &name;
        c.playback_speed = playback_speed;
        c.local_time = m_control_time;
        c.mirrored = (flags & flag_mirror) != 0;
        return true;
    }

    // For BSSynchronizedClipGenerator, which drives its clip itself.
    bool bound() const { return m_clip != nullptr; }
    const Clip* bound_clip() const { return m_clip; }
    float local_time() const { return m_local_time; }
    float animation_duration() const { return m_clip ? m_clip->data.duration : k_unbound_duration; }
    // What the clip raises goes here instead of to the queue while set.
    void capture_events(std::vector<EventProperty>* to) { m_capture = to; }
    // The local binding: a bone per track, and no float map or blend hint.
    void use_local_binding(std::vector<int32_t> bones) {
        m_local_bones = std::move(bones);
        count_tracks();
    }

private:
    struct Trigger {
        float local_time;
        EventProperty event;
        bool relative_to_end, acyclic;
    };
    struct Echo {
        float offset = 0.0f, weight = 0.0f, dwdt = 0.0f;
    };
    struct Step {
        float prev = 0.0f, next = 0.0f;
        int32_t loops = 0;
        bool at_end = false;
    };

    float cropped_duration() const { return std::max(animation_duration() - (crop_start + crop_end), 0.0f); }
    bool doubled_ping_pong() const { return mode == ping_pong && !(flags & flag_sync_half_cycle); }
    bool is_additive() const { return m_clip && m_local_bones.empty() && m_clip->data.additive; }

    // Effective speed S.
    float speed() const {
        const float cropped = cropped_duration();
        float s = playback_speed;
        if (cropped > 0.0f && enforced_duration > 0.0f) {
            s *= cropped / enforced_duration;
        }
        return mode == ping_pong && m_ping_pong_backward ? -s : s;
    }
    // Start offset T0.
    float start_offset() const {
        const float cropped = cropped_duration();
        const float t = enforced_duration > 0.0f ? start_time * (cropped / enforced_duration) : start_time;
        return std::min(t, cropped);
    }
    // Trigger span L.
    float span() const { return enforced_duration > 0.0f ? enforced_duration : cropped_duration(); }

    static float effective(const Trigger& t, float length) {
        if (t.acyclic) {
            return t.local_time;
        }
        return std::clamp(t.relative_to_end ? length + t.local_time : t.local_time, 0.0f, length);
    }

    // The highest track that reaches a bone: numBonesPerLod is empty in vanilla, so no LOD limits it.
    void count_tracks() {
        m_track_count = 0;
        if (!m_clip) {
            return;
        }
        for (uint32_t i = 0; i < m_clip->data.transform_tracks; ++i) {
            const int32_t bone = track_bone(i);
            if (bone >= 0 && size_t(bone) < m_skeleton_bones) {
                m_track_count = i + 1;
            }
        }
    }
    int32_t track_bone(uint32_t track) const {
        if (m_local_bones.empty()) {
            return m_clip->track_bone(track);
        }
        return track < m_local_bones.size() ? m_local_bones[track] : -1;
    }

    void crop_control() {
        m_control_time = std::max(m_control_time, crop_start);
        if (m_clip && m_control_time >= m_clip->data.duration - crop_end) {
            m_control_time = m_clip->data.duration - crop_end;
        }
    }

    void reset() {
        crop_control();
        const float cropped = cropped_duration();
        if (mode == user_controlled) {
            m_local_time = cropped * user_fraction + crop_start;
        } else {
            if (!m_ignore_start_time) {
                m_local_time = start_offset() + crop_start;
            }
            float t = m_local_time - crop_start;
            if (playback_speed >= 0.0f) {
                t = t == cropped ? 0.0f : t;
            } else {
                t = t == 0.0f ? cropped : t;
            }
            m_local_time = t + crop_start;
        }
        m_control_time = m_local_time;
        m_at_end = m_ping_pong_backward = false;
        m_time = 0.0f;
        m_previous_fraction = user_fraction;
        const float length = span();
        std::ranges::stable_sort(m_triggers, {}, [length](const Trigger& t) { return effective(t, length); });
    }

    // "The control step".
    float control_step(float dt, float stored_speed, int32_t& loops) const {
        loops = 0;
        const float period = animation_duration() - crop_end - crop_start;
        if (period <= 0.0f) {
            return m_control_time;
        }
        float x = (m_control_time - crop_start) + dt * stored_speed;
        if (x > period) {
            const int32_t n = int32_t(x / period);
            if (n >= 2) {
                x = period;
                loops = 1;
            } else {
                loops = n;
                x -= float(n) * period;
            }
        }
        if (x < 0.0f) {
            const int32_t n = int32_t(x / period) - 1;
            if (-n >= 3) {
                x = 0.0f;
                loops = 2; // positive, as the game has it
            } else {
                loops = n;
                x -= float(n) * period;
            }
        }
        return x + crop_start;
    }

    Step time_step(float dt) const {
        Step s;
        const float cropped = cropped_duration();
        if (mode == user_controlled) {
            s.prev = cropped * m_previous_fraction + crop_start;
            s.next = cropped * user_fraction + crop_start;
            return s;
        }
        const float v = speed();
        s.prev = m_local_time;
        s.next = m_clip ? control_step(dt, v, s.loops) : m_control_time;
        if (mode == ping_pong && (s.loops & 1)) {
            s.next = (cropped - s.next) + 2.0f * crop_start;
        } else if (mode == single_play) {
            const float y = s.prev + dt * v;
            if (y >= cropped + crop_start) {
                s = {s.prev, cropped + crop_start, 0, true};
            } else if (y <= crop_start) {
                s = {s.prev, crop_start, 0, true};
            }
        }
        return s;
    }

    void fire(Context& ctx, const Trigger& t) {
        if (m_capture) {
            m_capture->push_back(t.event);
        } else {
            ctx.graph.raise(ctx, t.event);
        }
    }

    void window(Context& ctx, float a, float b, bool inclusive) {
        float lo = std::min(a, b) - crop_start, hi = std::max(a, b) - crop_start;
        const float cropped = cropped_duration();
        if (enforced_duration > 0.0f && cropped > 0.0f) {
            lo *= enforced_duration / cropped;
            hi *= enforced_duration / cropped;
        }
        const float length = span();
        for (const Trigger& t : m_triggers) {
            if (t.acyclic || event_silent(ctx.behavior, t.event.id)) {
                continue;
            }
            const float time = effective(t, length);
            if (lo <= time && (inclusive ? hi >= time : hi > time)) {
                fire(ctx, t);
            }
        }
    }

    // The update passes. The bounds D and 0 have no crop added though the check subtracts it, so on a cropped clip they
    // fall short by the crop at the wrap: kept as the game has it.
    void fire_triggers(Context& ctx, const Step& s) {
        if (m_triggers.empty()) {
            return;
        }
        if (s.loops == 0) {
            window(ctx, s.prev, s.next, m_at_end);
            return;
        }
        const bool saved = m_ping_pong_backward;
        const float cropped = cropped_duration();
        auto end = [&] { return speed() > 0.0f ? cropped : 0.0f; };
        auto start = [&] { return speed() > 0.0f ? 0.0f : cropped; };
        auto flip = [&] {
            if (mode == ping_pong) {
                m_ping_pong_backward = !m_ping_pong_backward;
            }
        };
        window(ctx, s.prev, end(), true);
        flip();
        for (int32_t i = 1; i < s.loops; ++i) {
            window(ctx, start(), end(), true);
            flip();
        }
        window(ctx, start(), s.next, m_at_end);
        m_ping_pong_backward = saved;
    }

    void fire_acyclic(Context& ctx, float from, float to) {
        for (const Trigger& t : m_triggers) {
            if (t.acyclic && !event_silent(ctx.behavior, t.event.id) && from <= t.local_time && t.local_time < to) {
                fire(ctx, t);
            }
        }
    }

    size_t float_slot(uint32_t track) const {
        const auto& slots = m_clip->data.float_slots;
        return m_local_bones.empty() && track < slots.size() ? size_t(uint16_t(slots[track])) : size_t(track);
    }

    float echo_time(float offset) const {
        int32_t loops = 0;
        const float t = control_step(offset, 1.0f, loops);
        return mode == single_play && loops != 0 ? m_clip->data.duration + crop_start : t;
    }

    void sample(Context& ctx, Output& out, bool additive) {
        if (!m_clip) {
            return;
        }
        m_tracks.resize(m_track_count);
        m_floats.resize(m_clip->data.float_tracks);
        if (m_echo_count == 0) {
            m_clip->sample(m_local_time, m_tracks, m_floats);
            for (uint32_t i = 0; i < m_floats.size(); ++i) {
                if (const size_t slot = float_slot(i); slot < out.floats.size()) {
                    out.floats[slot] = m_floats[i];
                }
            }
        } else {
            m_sum.assign(m_track_count, zero_transform());
            auto add = [&](float time, float weight) {
                m_clip->sample(time, m_tracks, m_floats);
                for (uint32_t i = 0; i < m_track_count; ++i) {
                    add_transform(m_sum[i], m_tracks[i], weight);
                }
                for (uint32_t i = 0; i < m_floats.size(); ++i) {
                    if (const size_t slot = float_slot(i); slot < out.floats.size()) {
                        float& f = out.floats[slot];
                        f = (f == k_unset ? 0.0f : f) + weight * m_floats[i];
                    }
                }
            };
            float echoed = 0.0f;
            for (size_t i = 0; i < m_echo_count; ++i) {
                add(echo_time(m_echoes[i].offset), m_echoes[i].weight);
                echoed += m_echoes[i].weight;
            }
            add(echo_time(0.0f), 1.0f - echoed);
            for (uint32_t i = 0; i < m_track_count; ++i) {
                Qs& q = m_sum[i];
                q.r = glm::dot(q.r, q.r) < k_epsilon ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f) : normalised(q.r);
                q.s = glm::dot(q.s, q.s) < k_epsilon ? glm::vec3(1.0f) : q.s;
                m_tracks[i] = q;
            }
        }

        const Skeleton& skeleton = *ctx.character.skeleton;
        const MirroredSkeleton* mirror = nullptr;
        if ((flags & flag_mirror) && !ctx.character.project->mirrored_skeleton().rotations.empty()) {
            mirror = &ctx.character.project->mirrored_skeleton();
        }
        const auto bones = int32_t(out.pose.size());
        int32_t written = 0;
        for (uint32_t i = 0; i < m_track_count; ++i) {
            int32_t bone = track_bone(i);
            Qs v = m_tracks[i];
            if (mirror && bone >= 0 && size_t(bone) < mirror->bone_pairs.size()) {
                bone = mirror->bone_pairs[size_t(bone)];
                if (bone >= 0 && size_t(bone) < mirror->rotations.size()) {
                    v = mirror_transform(*mirror, skeleton, bone, v, additive);
                }
            }
            if (bone >= 0 && bone < bones) {
                out.pose[size_t(bone)] = v;
                out.present[size_t(bone)] = 1;
                ++written;
            }
        }
        out.all_present = written == bones;
    }

    const Clip* m_clip = nullptr;
    size_t m_skeleton_bones = 0;
    std::vector<Trigger> m_triggers; // the file's, then the annotations'; `triggers` stays as the file has it
    std::vector<int32_t> m_local_bones;
    uint32_t m_track_count = 0;
    float m_local_time = 0.0f;
    float m_control_time = 0.0f;
    float m_time = 0.0f; // played, in scaled seconds, unwrapped: only acyclic triggers read it
    float m_previous_fraction = 0.0f;
    bool m_at_end = false;
    bool m_ping_pong_backward = false;
    bool m_ignore_start_time = false;
    std::array<Echo, k_max_echoes> m_echoes{};
    size_t m_echo_count = 0;
    std::vector<EventProperty>* m_capture = nullptr;
    std::vector<Qs> m_tracks, m_sum;
    std::vector<float> m_floats;
};

// --- hkbBlenderGenerator ------------------------------------------------------------------------------------------

float bone_weight(const BoneWeightArray* weights, size_t bone) { return weights ? weights->weight(bone) : 1.0f; }

// "Blend bones". `total` is the weight each bone received; returns whether the child was dense.
bool blend_bones(Output& out, const Output& child, float w, const BoneWeightArray* weights, std::vector<float>& total) {
    if (child.pose_fraction <= 0.0f) {
        return false;
    }
    const bool dense = child.all_present;
    for (size_t b = 0; b < out.pose.size(); ++b) {
        if (!dense && !child.present[b]) {
            continue;
        }
        const float v = w * bone_weight(weights, b);
        total[b] += v;
        add_transform(out.pose[b], child.pose[b], v);
        if (!dense) {
            out.present[b] = 1;
        }
    }
    return dense;
}

// "Add bones": the additive transform applies on the parent side, and the source is the clip's stored delta.
bool add_bones(Output& out, const Output& child, float w, const BoneWeightArray* weights) {
    if (child.pose_fraction <= 0.0f) {
        return false;
    }
    const bool dense = child.all_present;
    for (size_t b = 0; b < out.pose.size(); ++b) {
        if (!dense && !child.present[b]) {
            continue;
        }
        const float v = w * bone_weight(weights, b);
        const Qs& s = child.pose[b];
        Qs& p = out.pose[b];
        const Qs r = compose(s, p);
        p.s = (1.0f - v) * p.s + v * (s.s * p.s);
        p.t = (1.0f - v) * p.t + v * r.t;
        glm::quat q = p.r * (1.0f - v);
        add_rotation(q, r.r, v);
        p.r = normalised(q);
        if (!dense) {
            out.present[b] = 1;
        }
    }
    return dense;
}

// "Subtract bones", dead in vanilla (subtractLastChild is false in all 2009), and only outlined by the design.
void subtract_bones(Output& out, const Output& child, float w, const BoneWeightArray* weights) {
    if (child.pose_fraction <= 0.0f) {
        return;
    }
    for (size_t b = 0; b < out.pose.size(); ++b) {
        if (!child.all_present && !child.present[b]) {
            continue;
        }
        const float v = w * bone_weight(weights, b);
        Qs x = zero_transform();
        add_transform(x, out.pose[b], 1.0f - v);
        add_transform(x, child.pose[b], v);
        x.r = normalised(x.r);
        out.pose[b] = compose(out.pose[b], inverse(x));
    }
}

// The float track of "Other tracks". `total` is the track's running weight.
void blend_floats(Output& out, const Output& child, float w, float& total) {
    if (child.floats_fraction <= 0.0f) {
        return;
    }
    out.floats_fraction += child.floats_fraction * w;
    total += w;
    const size_t n = std::min(out.floats.size(), child.floats.size());
    for (size_t i = 0; i < n; ++i) {
        const float src = child.floats[i], dst = out.floats[i];
        const float b = dst != k_unset ? 1.0f : 0.0f;
        const float a = src != k_unset && !(child.additive && src == 0.0f && b == 0.0f) ? 1.0f : 0.0f;
        out.floats[i] = ((1.0f - b) * total + b * w) * a * src + ((1.0f - a) * w + 1.0f) * b * dst +
                        (1.0f - a) * (1.0f - b) * k_unset;
    }
}

void normalise_floats(Output& out, float total) {
    if (total <= 0.0f) {
        return;
    }
    for (float& f : out.floats) {
        if (f != k_unset) {
            f /= total;
        }
    }
}

class BlenderGenerator : public Generator {
public:
    enum Flags : int16_t {
        flag_sync = 1,
        flag_smooth_weights = 4,
        flag_dont_deactivate = 8,
        flag_parametric = 16,
        flag_cyclic = 32,
        flag_force_dense = 64,
    };

    struct ChildEntry final : Bindable {
        Node* generator = nullptr;
        BoneWeightArray bone_weights;
        bool has_bone_weights = false;
        float weight = 1.0f;
        float world_from_model_weight = 1.0f;

        void describe(Describer& d) override {
            d.node("generator", generator);
            d.object("boneWeights", bone_weights, &has_bone_weights);
            d.member("weight", weight);
            d.member("worldFromModelWeight", world_from_model_weight);
        }
    };

    float threshold = 0.0f; // referencePoseWeightThreshold
    float blend_parameter = 0.0f;
    float min_cyclic_parameter = 0.0f, max_cyclic_parameter = 0.0f;
    int16_t sync_master = -1; // indexOfSyncMasterChild
    int16_t flags = 0;
    bool subtract_last_child = false;
    std::vector<ChildEntry> entries;

    std::unique_ptr<Node> clone() const override { return std::make_unique<BlenderGenerator>(*this); }
    void describe(Describer& d) override {
        d.member("referencePoseWeightThreshold", threshold);
        d.member("blendParameter", blend_parameter);
        d.member("minCyclicBlendParameter", min_cyclic_parameter);
        d.member("maxCyclicBlendParameter", max_cyclic_parameter);
        d.member("indexOfSyncMasterChild", sync_master);
        d.member("flags", flags);
        d.member("subtractLastChild", subtract_last_child);
        d.objects("children", entries);
    }
    // The root doesn't recurse into what is listed here, so a child's weights go in beside it.
    void collect_bindables(std::vector<Bindable*>& out) override {
        for (ChildEntry& e : entries) {
            out.push_back(&e);
            out.push_back(&e.bone_weights);
        }
    }

    void activate(Context& ctx) override {
        m_begin = m_end = 0;
        m_end_weight = 0.0f;
        m_init_sync = true;
        m_do_subtractive = false;
        m_active.assign(entries.size(), 0);
        m_sync_next_frame.assign(entries.size(), 0);
        update_activity(ctx);
    }

    void update(Context& ctx, float dt) override {
        if (record) {
            SyncInfo& s = record->sync;
            s.local_time += dt;
            if (s.base_frequency != 0.0f) {
                const float period = 1.0f / s.base_frequency;
                if (s.local_time > period) {
                    s.local_time -= std::floor(s.local_time / period) * period;
                }
            }
        }
        update_activity(ctx);
    }

    void deactivate(Context&) override {
        m_sorted.clear();
        m_active.clear();
        m_listed.clear();
    }

    void children(uint32_t child_flags, std::vector<Child>& out) override {
        const bool active_only = (child_flags & children_active_only) != 0;
        if (active_only) {
            m_listed.clear();
        }
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].generator || (active_only && !(i < m_active.size() && m_active[i]))) {
                continue;
            }
            const bool sync = (active_only && (flags & flag_sync)) || int32_t(i) == sync_master;
            out.push_back({entries[i].generator, false, false, sync, false});
            if (active_only) {
                m_listed.push_back(uint32_t(i));
            }
        }
    }

    void child_weight(size_t index, float& blend, float& motion) const override {
        if (index < m_listed.size()) {
            const uint32_t i = m_listed[index];
            blend = weight(i);
            motion = blend * entries[i].world_from_model_weight;
        }
    }

    void update_sync(Context& ctx) override {
        if (!record) {
            return;
        }
        SyncInfo& own = record->sync;
        m_sync_kids.clear();
        m_sync_weights.clear();
        uint32_t additive = 0;
        bool last_additive = false;
        for (const uint32_t i : m_listed) {
            const SyncInfo* s = sync_of(ctx, entries[i].generator);
            if (!s) {
                continue;
            }
            additive += s->additive;
            last_additive = s->additive;
            float w;
            if (sync_master >= 0) {
                w = int32_t(i) == sync_master ? 1.0f : 0.0f;
            } else if (s->additive || (subtract_last_child && i + 1 == entries.size())) {
                w = 0.0f;
            } else {
                w = weight(i);
            }
            m_sync_kids.push_back(s);
            m_sync_weights.push_back(w);
        }
        const auto active = uint32_t(m_sync_kids.size());
        if (flags & flag_sync) {
            if (active > 0) {
                combine_sync(own, m_sync_kids, m_sync_weights, threshold, m_init_sync);
                m_init_sync = false;
                join_new_children(ctx, own);
            } else {
                zero_sync(own);
            }
        } else if (const SyncInfo* master = sync_master >= 0 && size_t(sync_master) < entries.size()
                                                ? sync_of(ctx, entries[size_t(sync_master)].generator)
                                                : nullptr) {
            own = *master;
            own.mirrored = false;
        } else {
            zero_sync(own);
        }
        const bool last_listed = !m_listed.empty() && m_listed.back() + 1 == entries.size();
        m_do_subtractive = subtract_last_child && last_listed && !last_additive && active - additive > 1;
        own.additive = m_do_subtractive || additive == active;
    }

    void generate(Context& ctx, std::span<const Output* const> kids, Output& out, float) override {
        const size_t count = std::min(kids.size(), m_listed.size());
        if (count == 1 && threshold == 0.0f) {
            generate_single(ctx, *kids[0], entries[m_listed[0]], out);
            return;
        }
        const size_t bones = out.pose.size();
        const size_t last = count - 1;
        auto pass_of = [&](size_t k) { return m_do_subtractive && k == last ? 2 : kids[k]->additive ? 1 : 0; };
        // Not vacuous: no active children isn't all additive, so that case gives the reference pose.
        bool all_additive = bones > 0 && count > 0;
        for (size_t k = 0; k < count; ++k) {
            all_additive = all_additive && pass_of(k) == 1;
        }
        const Qs start = all_additive ? Qs{} : zero_transform();
        Qs world_from_model = start, motion = start;
        std::fill(out.pose.begin(), out.pose.end(), start);
        std::fill(out.present.begin(), out.present.end(), uint8_t{0});
        std::fill(out.floats.begin(), out.floats.end(), k_unset);
        out.all_present = false;
        out.world_from_model_fraction = out.motion_fraction = out.pose_fraction = 1.0f;
        out.floats_fraction = 0.0f;
        m_total.assign(bones, 0.0f);
        float child_total = 0.0f, motion_total = 0.0f, float_total = 0.0f;
        bool any_dense = false;

        auto bones_of = [&](size_t k) { return &entries[m_listed[k]].bone_weights; };
        auto finalise = [&] {
            const bool complete = std::ranges::all_of(out.present, [](uint8_t p) { return p != 0; });
            if (any_dense || (flags & flag_force_dense) || count == 0 || complete) {
                std::fill(out.present.begin(), out.present.end(), uint8_t{1});
                out.all_present = true;
            }
            if (!all_additive) {
                const Skeleton& skeleton = *ctx.character.skeleton;
                for (size_t b = 0; b < bones; ++b) {
                    if (!out.present[b]) {
                        out.pose[b] = skeleton.reference[b];
                    } else if (m_total[b] <= threshold) {
                        const float w = fill_weight(m_total[b]);
                        add_transform(out.pose[b], skeleton.reference[b], w);
                        m_total[b] += w;
                    }
                }
                if (motion_total <= threshold) {
                    const float w = fill_weight(motion_total);
                    add_transform(world_from_model, ctx.character.world_from_model, w);
                    add_transform(motion, Qs{}, w);
                    motion_total += w;
                }
            }
            if (motion_total > 0.0f) {
                world_from_model = {world_from_model.t / motion_total, normalised(world_from_model.r),
                                    world_from_model.s / motion_total};
                motion = {motion.t / motion_total, normalised(motion.r), motion.s / motion_total};
            }
            for (size_t b = 0; b < bones; ++b) {
                if (out.present[b] && m_total[b] > 0.0f) {
                    Qs& p = out.pose[b];
                    p = {p.t / m_total[b], normalised(p.r), p.s / m_total[b]};
                }
            }
            normalise_floats(out, float_total);
            if (child_total > 0.0f) {
                out.floats_fraction /= child_total;
            }
        };

        for (size_t k = 0; k < count; ++k) {
            if (pass_of(k) != 0) {
                continue;
            }
            const ChildEntry& e = entries[m_listed[k]];
            const float w = weight(m_listed[k]);
            if (w > k_epsilon) {
                child_total += w;
                any_dense = blend_bones(out, *kids[k], w, bones_of(k), m_total) || any_dense;
                const float m = w * e.world_from_model_weight;
                add_transform(world_from_model, kids[k]->world_from_model, m);
                add_transform(motion, kids[k]->motion, m);
                motion_total += m;
            }
            blend_floats(out, *kids[k], w, float_total);
        }
        if (!all_additive) {
            finalise();
        }
        for (size_t k = 0; k < count; ++k) {
            const float w = weight(m_listed[k]);
            if (pass_of(k) == 1 && w > k_epsilon) {
                any_dense = add_bones(out, *kids[k], w, bones_of(k)) || any_dense;
                blend_floats(out, *kids[k], w, float_total);
            }
        }
        if (all_additive) {
            finalise();
        }
        if (m_do_subtractive && count > 0) {
            subtract_bones(out, *kids[last], weight(m_listed[last]), bones_of(last));
            world_from_model = motion = Qs{};
            blend_floats(out, *kids[last], weight(m_listed[last]), float_total);
        }
        out.world_from_model = world_from_model;
        out.motion = motion;
        if ((flags & flag_force_dense) && all_additive && !out.all_present) {
            for (size_t b = 0; b < bones; ++b) {
                if (!out.present[b]) {
                    out.pose[b] = Qs{};
                }
            }
            std::fill(out.present.begin(), out.present.end(), uint8_t{1});
            out.all_present = true;
        }
        out.additive = all_additive || m_do_subtractive;
    }

protected:
    // The child's weight for blending, w(i). Never normalised here: that happens per bone at the end.
    float weight(size_t i) const {
        if (flags & flag_parametric) {
            if (int32_t(i) == m_end) {
                return m_end_weight;
            }
            return int32_t(i) == m_begin ? 1.0f - m_end_weight : 0.0f;
        }
        const float w = std::clamp(entries[i].weight, 0.0f, 1.0f);
        return (flags & flag_smooth_weights) ? w * w * (3.0f - 2.0f * w) : w;
    }

    const std::vector<uint32_t>& listed() const { return m_listed; }

private:
    float fill_weight(float total) const { return threshold == 0.0f && total == 0.0f ? 1.0f : threshold - total; }

    // The children's `weight` fields are anchors on a parameter line, here.
    void compute_parametric_interval() {
        const size_t n = entries.size();
        if (n == 0) {
            return;
        }
        m_sorted.resize(n);
        for (size_t i = 0; i < n; ++i) {
            m_sorted[i] = uint32_t(i);
        }
        std::ranges::stable_sort(m_sorted, {}, [this](uint32_t i) { return entries[i].weight; });
        auto anchor = [this](size_t k) { return entries[m_sorted[k]].weight; };
        const bool cyclic = (flags & flag_cyclic) != 0;
        const float lo = cyclic ? min_cyclic_parameter : anchor(0);
        const float hi = cyclic ? max_cyclic_parameter : anchor(n - 1);
        float x = blend_parameter;
        if (x <= lo) {
            x = lo;
        } else if (x >= hi) {
            x = cyclic ? lo : hi;
        }
        size_t k = 0;
        while (k < n && anchor(k) <= x) {
            ++k;
        }
        float t = 0.0f;
        if (k > 0 && k < n) {
            t = (x - anchor(k - 1)) / (anchor(k) - anchor(k - 1));
        } else if (k == 0) {
            t = 1.0f - (anchor(0) - x) / ((hi - anchor(n - 1)) + (anchor(0) - lo));
        } else if (!cyclic) {
            k = n - 1;
            t = 1.0f;
        } else {
            k = 0;
            t = (x - anchor(n - 1)) / ((anchor(0) - lo) + (hi - anchor(n - 1)));
        }
        m_end = int32_t(m_sorted[k]);
        m_begin = int32_t(k > 0 ? m_sorted[k - 1] : m_sorted[n - 1]);
        m_end_weight = t;
    }

    void update_activity(Context& ctx) {
        if (flags & flag_parametric) {
            compute_parametric_interval();
        }
        bool changed = false;
        for (size_t i = 0; i < entries.size(); ++i) {
            bool active;
            if (flags & flag_dont_deactivate) {
                active = true;
            } else if (int32_t(i) == sync_master) {
                active = true;
                m_sync_next_frame[i] = 0;
            } else {
                active = weight(i) != 0.0f;
                if (active && !m_active[i]) {
                    m_sync_next_frame[i] = 1;
                }
            }
            changed = changed || active != (m_active[i] != 0);
            m_active[i] = active;
        }
        if (changed) {
            ctx.graph.request_update_active_nodes();
        }
    }

    void join_new_children(Context& ctx, const SyncInfo& own) {
        for (const uint32_t i : m_listed) {
            const SyncInfo* s = sync_of(ctx, entries[i].generator);
            if (!m_sync_next_frame[i] || !s || s->base_frequency == 0.0f || own.base_frequency == 0.0f) {
                continue;
            }
            if (const auto t = time_at(*s, sync_position(own, own.local_time))) {
                ctx.graph.join(ctx, entries[i].generator, *t);
            }
            m_sync_next_frame[i] = 0;
        }
    }

    // One active child, threshold 0: the child's own weight isn't used, so a lone child at 0.5 plays in full.
    void generate_single(Context& ctx, const Output& child, const ChildEntry& e, Output& out) {
        out.copy_from(child);
        if (e.world_from_model_weight == 0.0f) {
            out.world_from_model = ctx.character.world_from_model;
            out.motion = Qs{};
            out.world_from_model_fraction = out.motion_fraction = 1.0f;
        }
        if (out.pose_fraction <= 0.0f) {
            return;
        }
        if (!e.bone_weights.weights.empty()) {
            if (out.all_present) {
                std::fill(out.present.begin(), out.present.end(), uint8_t{1});
            }
            bool all = true;
            for (size_t b = 0; b < out.pose.size(); ++b) {
                if (e.bone_weights.weight(b) == 0.0f) {
                    out.present[b] = 0;
                }
                all = all && out.present[b];
            }
            out.all_present = all;
        }
        if ((flags & flag_force_dense) && !out.all_present) {
            for (size_t b = 0; b < out.pose.size(); ++b) {
                if (!out.present[b]) {
                    out.pose[b] = Qs{};
                }
            }
            std::fill(out.present.begin(), out.present.end(), uint8_t{1});
            out.all_present = true;
        }
    }

    int32_t m_begin = 0, m_end = 0;
    float m_end_weight = 0.0f;
    bool m_init_sync = true;
    bool m_do_subtractive = false;
    std::vector<uint32_t> m_sorted;
    std::vector<uint8_t> m_active, m_sync_next_frame;
    std::vector<uint32_t> m_listed; // the entries children() last reported: what generate's outputs pair with
    std::vector<const SyncInfo*> m_sync_kids;
    std::vector<float> m_sync_weights;
    std::vector<float> m_total;
};

// --- hkbPoseMatchingGenerator -------------------------------------------------------------------------------------

// Runs a generator once outside the graph: activate, generate, deactivate. Nothing below is in the active list, so what
// a modifier below would do is lost, which is fine for a first pose.
void run_once(Context& ctx, const Node* node, Output& out) {
    std::unique_ptr<Node> clone = node->clone();
    auto* generator = dynamic_cast<Generator*>(clone.get());
    if (!generator) {
        return;
    }
    generator->activate(ctx);
    std::vector<Child> kids;
    generator->children(children_active_only | children_generators_only, kids);
    std::vector<Output> outputs(kids.size());
    std::vector<const Output*> pointers;
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i].node) {
            outputs[i].resize(out.pose.size(), out.floats.size());
            outputs[i].clear();
            run_once(ctx, kids[i].node, outputs[i]);
            pointers.push_back(&outputs[i]);
        }
    }
    generator->generate(ctx, pointers, out, 0.0f);
    generator->deactivate(ctx);
}

class PoseMatchingGenerator final : public BlenderGenerator {
public:
    glm::quat world_from_model_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float blend_speed = 0.0f, min_speed_to_switch = 0.0f;
    float min_switch_time_no_error = 0.0f, min_switch_time_full_error = 0.0f;
    int32_t start_playing_event = -1, start_matching_event = -1;
    int16_t root_bone = -1, other_bone = -1, another_bone = -1, pelvis_bone = -1;
    int8_t mode = mode_match;

    std::unique_ptr<Node> clone() const override { return std::make_unique<PoseMatchingGenerator>(*this); }
    void describe(Describer& d) override {
        BlenderGenerator::describe(d);
        d.member("worldFromModelRotation", world_from_model_rotation);
        d.member("blendSpeed", blend_speed);
        d.member("minSpeedToSwitch", min_speed_to_switch);
        d.member("minSwitchTimeNoError", min_switch_time_no_error);
        d.member("minSwitchTimeFullError", min_switch_time_full_error);
        d.member("startPlayingEventId", start_playing_event);
        d.member("startMatchingEventId", start_matching_event);
        d.member("rootBoneIndex", root_bone);
        d.member("otherBoneIndex", other_bone);
        d.member("anotherBoneIndex", another_bone);
        d.member("pelvisIndex", pelvis_bone);
        d.member("mode", mode);
    }

    void activate(Context& ctx) override {
        m_error = 1.0f;
        m_current = m_best = -1;
        m_time_since_better = 0.0f;
        mode = mode_match;
        if (m_signatures.empty() && bones_valid(ctx)) {
            build_signatures(ctx);
        }
        for (ChildEntry& e : entries) {
            e.weight = 0.0f;
        }
        match(ctx, 0.0f);
        BlenderGenerator::activate(ctx);
    }

    void update(Context& ctx, float dt) override {
        match(ctx, dt);
        for (size_t i = 0; i < entries.size(); ++i) {
            float& w = entries[i].weight;
            w = int32_t(i) == m_current ? std::min(w + dt * blend_speed, 1.0f) : std::max(w - dt * blend_speed, 0.0f);
        }
        BlenderGenerator::update(ctx, dt);
    }

    void handle_event(Context&, const Event& e) override {
        if (e.id == start_playing_event) {
            mode = mode_play;
        } else if (e.id == start_matching_event) {
            mode = mode_match;
            m_reset_current = true;
        }
    }

    void deactivate(Context& ctx) override {
        m_signatures.clear();
        BlenderGenerator::deactivate(ctx);
    }

    // While matching, the poses hold still.
    void update_sync(Context& ctx) override {
        if (mode == mode_play) {
            BlenderGenerator::update_sync(ctx);
            return;
        }
        if (record) {
            zero_sync(record->sync);
        }
        for (const uint32_t i : listed()) {
            if (SyncInfo* s = sync_of(ctx, entries[i].generator)) {
                s->base_frequency = s->local_time = 0.0f;
            }
        }
        if (m_reset_current && m_current >= 0 && size_t(m_current) < entries.size()) {
            ctx.graph.join(ctx, entries[size_t(m_current)].generator, 0.0f);
        }
        m_reset_current = false;
    }

private:
    static constexpr int8_t mode_match = 0, mode_play = 1;

    bool bones_valid(const Context& ctx) const {
        const RagdollPoses* ragdoll = ctx.character.ragdoll;
        if (!ragdoll) {
            return false;
        }
        const auto n = int32_t(ragdoll->bone_count());
        auto valid = [n](int32_t b) { return b >= 0 && b < n; };
        return valid(root_bone) && valid(other_bone) && valid(another_bone) && root_bone != other_bone &&
               root_bone != another_bone && other_bone != another_bone;
    }

    // Any frame built the same way cancels in the dot of two signatures, so F's column order doesn't reach the result.
    glm::quat signature(const Context& ctx, const std::vector<glm::vec3>& positions) const {
        const glm::vec3 root = positions[size_t(root_bone)];
        const glm::vec3 a = glm::normalize(positions[size_t(other_bone)] - root);
        const glm::vec3 b = glm::normalize(positions[size_t(another_bone)] - root);
        const glm::vec3 c = glm::normalize(glm::cross(b, a));
        const glm::vec3 x = glm::normalize(glm::cross(c, a));
        const glm::quat q = glm::quat_cast(glm::mat3(x, glm::cross(a, x), a));

        const glm::vec3 up(ctx.character.project->model_up());
        int largest = 0;
        for (int i = 1; i < 3; ++i) {
            if (std::abs(up[i]) > std::abs(up[largest])) {
                largest = i;
            }
        }
        glm::vec3 e(0.0f);
        e[(largest + 1) % 3] = 1.0f;
        const glm::vec3 d = glm::normalize(glm::cross(q * e, up));
        const glm::quat frame = glm::quat_cast(glm::mat3(d, glm::cross(up, d), up));
        return glm::conjugate(frame) * q;
    }

    void build_signatures(Context& ctx) {
        const Skeleton& skeleton = *ctx.character.skeleton;
        std::vector<glm::vec3> positions;
        std::vector<Qs> model(skeleton.size());
        for (const ChildEntry& e : entries) {
            Output out;
            out.resize(skeleton.size(), 0);
            out.clear();
            if (e.generator) {
                run_once(ctx, e.generator, out);
            }
            for (size_t b = 0; b < skeleton.size(); ++b) {
                const Qs local = out.all_present || out.present[b] ? out.pose[b] : skeleton.reference[b];
                const int16_t parent = skeleton.parents[b];
                model[b] = parent >= 0 ? compose(model[size_t(parent)], local) : local;
            }
            ctx.character.ragdoll->map_pose(model, positions);
            m_signatures.push_back(signature(ctx, positions));
        }
    }

    void match(Context& ctx, float dt) {
        if (mode != mode_match || m_signatures.empty() || !ctx.character.ragdoll) {
            return;
        }
        const RagdollPoses& ragdoll = *ctx.character.ragdoll;
        std::vector<glm::vec3> positions;
        ragdoll.current_pose({glm::vec3(0.0f), world_from_model_rotation, glm::vec3(1.0f)}, positions);
        const glm::quat q = signature(ctx, positions);
        int32_t best = -1;
        float error = FLT_MAX;
        for (size_t i = 0; i < m_signatures.size(); ++i) {
            const float e = 1.0f - std::abs(glm::dot(q, m_signatures[i]));
            if (e < error) {
                error = e;
                best = int32_t(i);
            }
        }
        m_error = error;
        if (m_current == -1) {
            m_current = m_best = best;
            for (size_t i = 0; i < entries.size(); ++i) {
                entries[i].weight = int32_t(i) == best ? 1.0f : 0.0f;
            }
            return;
        }
        m_time_since_better = best == m_best ? m_time_since_better + dt : 0.0f;
        m_best = best;
        const float wait = (1.0f - error) * min_switch_time_no_error + error * min_switch_time_full_error;
        if (best != m_current && m_time_since_better > wait && ragdoll.body_speed(pelvis_bone) >= min_speed_to_switch) {
            m_current = best;
            m_time_since_better = 0.0f;
        }
    }

    float m_error = 1.0f;
    int32_t m_current = -1, m_best = -1;
    float m_time_since_better = 0.0f;
    bool m_reset_current = false;
    std::vector<glm::quat> m_signatures;
};

// --- hkbReferencePoseGenerator ------------------------------------------------------------------------------------

class ReferencePoseGenerator final : public Generator {
public:
    std::unique_ptr<Node> clone() const override { return std::make_unique<ReferencePoseGenerator>(*this); }
    void describe(Describer&) override {}
    void generate(Context& ctx, std::span<const Output* const>, Output& out, float) override {
        if (!out.pose.empty()) {
            out.pose = ctx.character.skeleton->reference;
            std::fill(out.present.begin(), out.present.end(), uint8_t{1});
            out.all_present = true;
            out.pose_fraction = 1.0f;
        }
        out.world_from_model = ctx.character.world_from_model;
        out.motion = Qs{};
        out.world_from_model_fraction = out.motion_fraction = 1.0f;
    }
};

// --- BSiStateTaggingGenerator -------------------------------------------------------------------------------------

class StateTaggingGenerator final : public Generator {
public:
    Node* default_generator = nullptr;
    int32_t state_to_set_as = 0, priority = 0;

    std::unique_ptr<Node> clone() const override { return std::make_unique<StateTaggingGenerator>(*this); }
    void describe(Describer& d) override {
        d.node("pDefaultGenerator", default_generator);
        d.member("iStateToSetAs", state_to_set_as);
        d.member("iPriority", priority);
    }
    void children(uint32_t, std::vector<Child>& out) override { out.push_back({default_generator, false, false, true, false}); }
    void generate(Context&, std::span<const Output* const> kids, Output& out, float) override {
        if (!kids.empty()) {
            out.copy_from(*kids[0]);
        }
    }
    void update_sync(Context& ctx) override { ctx.graph.copy_sync(*this, default_generator); }
};

// --- BSBoneSwitchGenerator ----------------------------------------------------------------------------------------

class BoneSwitchGenerator final : public Generator {
public:
    struct BoneData final : Bindable {
        Node* generator = nullptr;
        BoneWeightArray bone_weight;
        bool has_bone_weight = false;

        void describe(Describer& d) override {
            d.node("pGenerator", generator);
            d.object("spBoneWeight", bone_weight, &has_bone_weight);
        }
    };

    Node* default_generator = nullptr;
    std::vector<BoneData> entries;

    std::unique_ptr<Node> clone() const override { return std::make_unique<BoneSwitchGenerator>(*this); }
    void describe(Describer& d) override {
        d.node("pDefaultGenerator", default_generator);
        d.objects("ChildrenA", entries);
    }
    void collect_bindables(std::vector<Bindable*>& out) override {
        for (BoneData& e : entries) {
            out.push_back(&e);
            out.push_back(&e.bone_weight);
        }
    }
    void children(uint32_t, std::vector<Child>& out) override {
        if (default_generator) {
            out.push_back({default_generator, false, false, false, false});
        }
        for (const BoneData& e : entries) {
            if (e.generator) {
                out.push_back({e.generator, false, false, false, false});
            }
        }
    }
    // The weight is a mask: any positive weight switches the bone whole, and later children overwrite earlier ones.
    void generate(Context&, std::span<const Output* const> kids, Output& out, float) override {
        size_t next = 0;
        if (default_generator && next < kids.size()) {
            out.copy_from(*kids[next++]);
        }
        const size_t bones = out.pose.size();
        bool copied = false;
        for (const BoneData& e : entries) {
            if (!e.generator || next >= kids.size()) {
                continue;
            }
            const Output& child = *kids[next++];
            if (child.pose_fraction <= 0.0f || e.bone_weight.weights.size() < bones) {
                continue;
            }
            if (out.all_present) {
                std::fill(out.present.begin(), out.present.end(), uint8_t{1});
            }
            for (size_t b = 0; b < bones; ++b) {
                if (e.bone_weight.weights[b] > 0.0f && (child.all_present || child.present[b])) {
                    out.pose[b].t = child.pose[b].t;
                    out.pose[b].r = child.pose[b].r;
                    out.present[b] = 1;
                    copied = true;
                }
            }
        }
        if (copied && !out.all_present) {
            out.all_present = std::ranges::all_of(out.present, [](uint8_t p) { return p != 0; });
        }
    }
};

// --- BSCyclicBlendTransitionGenerator -----------------------------------------------------------------------------

// The effect the node creates (an hkbBlendingTransitionEffect with FLAG_SYNC, END_MODE_NONE and the zero curve, which is
// smooth) lives here as a few fields: it isn't a node of the graph, and only its update, sync and blend are used.
class CyclicBlendTransitionGenerator final : public Generator {
public:
    enum Mode : int8_t { mode_inactive = -1, mode_default = 0, mode_frozen = 1, mode_blending = 2, mode_waiting = 3 };

    Node* blender = nullptr; // pBlenderGenerator
    EventProperty freeze_event, cross_blend_event;
    float blend_parameter = 0.0f;
    float transition_duration = 0.0f;
    int8_t blend_curve = 0; // nothing reads it

    std::unique_ptr<Node> clone() const override { return std::make_unique<CyclicBlendTransitionGenerator>(*this); }
    void describe(Describer& d) override {
        d.node("pBlenderGenerator", blender);
        d.member("EventToFreezeBlendValue", freeze_event);
        d.member("EventToCrossBlend", cross_blend_event);
        d.member("fBlendParameter", blend_parameter);
        d.member("fTransitionDuration", transition_duration);
        d.member("eBlendCurve", blend_curve);
    }

    void activate(Context&) override {
        m_mode = mode_default;
        // Exact class: a pose matcher is a blender too, but only a plain one is cloned. The root treats the copy as a
        // template of its own, so its children stay the live blender's: a clip of both has two parents and runs once.
        if (!m_transition && blender && typeid(*blender) == typeid(BlenderGenerator)) {
            m_transition = std::shared_ptr<Node>(blender->clone());
        }
        m_parameter_pending = true; // the live blender has no clone until the root collects it
    }

    void deactivate(Context&) override { m_mode = mode_inactive; } // the copy stays: old records still name it

    void update(Context& ctx, float dt) override {
        if (m_mode == mode_frozen) {
            return;
        }
        if (BlenderGenerator* live = live_blender(ctx)) {
            live->blend_parameter = blend_parameter;
        }
        if (m_mode == mode_waiting) {
            m_mode = mode_blending;
            ctx.graph.request_update_active_nodes();
        } else if (m_mode == mode_blending) {
            if (record) {
                SyncInfo& s = record->sync;
                s.local_time += dt;
                if (s.base_frequency != 0.0f && s.local_time > 1.0f / s.base_frequency) {
                    const float period = 1.0f / s.base_frequency;
                    s.local_time -= std::floor(s.local_time / period) * period;
                }
            }
            m_elapsed += dt;
            m_remaining -= dt;
            if (m_elapsed > 0.0f && m_remaining <= 0.0f) {
                m_mode = mode_default;
                ctx.graph.request_update_active_nodes();
            }
        }
    }

    void handle_event(Context& ctx, const Event& e) override {
        if (m_mode == mode_default) {
            if (e.id == freeze_event.id) {
                m_mode = mode_frozen;
            }
            return;
        }
        if (e.id != cross_blend_event.id) {
            return;
        }
        const BlenderGenerator* live = live_blender(ctx);
        const float parameter = live ? live->blend_parameter : blend_parameter;
        if (!m_transition) {
            return;
        }
        // The copy, and the root's clone of it if the crossfade is running.
        static_cast<BlenderGenerator&>(*m_transition).blend_parameter = parameter;
        if (auto* running = static_cast<BlenderGenerator*>(ctx.graph.clone_of(m_transition.get()))) {
            running->blend_parameter = parameter;
        }
        if (transition_duration <= k_epsilon) {
            return;
        }
        if (m_mode == mode_blending) {
            const float elapsed = (1.0f - m_elapsed / transition_duration) * m_duration;
            m_elapsed = elapsed;
            m_remaining = m_duration - elapsed;
        } else {
            m_mode = mode_waiting;
            m_duration = transition_duration;
            m_elapsed = 0.0f;
            m_remaining = m_duration;
        }
    }

    void children(uint32_t, std::vector<Child>& out) override {
        const bool blending = m_mode == mode_blending && m_transition;
        out.push_back({blender, false, false, blending, false});
        if (blending) {
            out.push_back({m_transition.get(), false, false, true, false});
        }
    }

    void child_weight(size_t index, float& blend, float& motion) const override {
        if (m_mode == mode_blending) {
            const float w = curve_weight();
            blend = motion = index == 0 ? w : 1.0f - w;
        }
    }

    void generate(Context&, std::span<const Output* const> kids, Output& out, float) override {
        if (m_mode == mode_blending && kids.size() >= 2) {
            crossfade(*kids[1], *kids[0], curve_weight(), out);
        } else if (!kids.empty()) {
            out.copy_from(*kids[0]);
        }
    }

    void update_sync(Context& ctx) override {
        if (m_parameter_pending) {
            if (BlenderGenerator* live = live_blender(ctx)) {
                live->blend_parameter = blend_parameter;
                m_parameter_pending = false;
            }
        }
        if (m_mode != mode_blending || !record) {
            ctx.graph.copy_sync(*this, blender);
            return;
        }
        SyncInfo& own = record->sync;
        const SyncInfo* to = sync_of(ctx, blender);
        const SyncInfo* from = sync_of(ctx, m_transition.get());
        if (to && from && to->base_frequency != 0.0f && from->base_frequency != 0.0f) {
            const float total = m_elapsed + m_remaining;
            const float w = total > 0.0f ? std::min(m_elapsed / total, 1.0f) : 0.0f;
            const std::array<const SyncInfo*, 2> kids{from, to};
            const std::array<float, 2> weights{1.0f - w, w};
            combine_sync(own, kids, weights, 0.0f, m_elapsed == 0.0f);
            if (m_elapsed == 0.0f) {
                if (const auto t = time_at(*to, sync_position(own, own.local_time))) {
                    ctx.graph.join(ctx, blender, *t);
                }
            }
        } else {
            zero_sync(own);
        }
        m_remaining = m_duration - m_elapsed;
    }

private:
    BlenderGenerator* live_blender(Context& ctx) const {
        return dynamic_cast<BlenderGenerator*>(ctx.graph.clone_of(blender));
    }

    float curve_weight() const {
        const float total = m_elapsed + m_remaining;
        const float t = total > 0.0f ? std::min(std::min(m_elapsed, total) / total, 1.0f) : 0.0f;
        return std::clamp(3.0f * t * t - 2.0f * t * t * t, 0.0f, 1.0f);
    }

    // (1 - w) from + w to, weighted per bone so that a bone only one side wrote keeps that side's value.
    void crossfade(const Output& from, const Output& to, float w, Output& out) {
        const size_t bones = out.pose.size();
        Qs world_from_model = zero_transform(), motion = zero_transform();
        add_transform(world_from_model, from.world_from_model, 1.0f - w);
        add_transform(world_from_model, to.world_from_model, w);
        add_transform(motion, from.motion, 1.0f - w);
        add_transform(motion, to.motion, w);
        world_from_model.r = normalised(world_from_model.r);
        motion.r = normalised(motion.r);
        out.world_from_model = world_from_model;
        out.motion = motion;
        out.world_from_model_fraction = out.motion_fraction = out.pose_fraction = 1.0f;

        std::fill(out.pose.begin(), out.pose.end(), zero_transform());
        std::fill(out.present.begin(), out.present.end(), uint8_t{0});
        m_total.assign(bones, 0.0f);
        const bool dense_from = blend_bones(out, from, 1.0f - w, nullptr, m_total);
        const bool dense_to = blend_bones(out, to, w, nullptr, m_total);
        out.all_present = dense_from || dense_to;
        if (out.all_present) {
            std::fill(out.present.begin(), out.present.end(), uint8_t{1});
        }
        for (size_t b = 0; b < bones; ++b) {
            Qs& p = out.pose[b];
            if (m_total[b] > 0.0f) {
                p = {p.t / m_total[b], normalised(p.r), p.s / m_total[b]};
            } else if (out.present[b]) {
                p = to.all_present || to.present[b] ? to.pose[b] : from.pose[b];
            }
        }
        std::fill(out.floats.begin(), out.floats.end(), k_unset);
        out.floats_fraction = 0.0f;
        float float_total = 0.0f;
        blend_floats(out, from, 1.0f - w, float_total);
        blend_floats(out, to, w, float_total);
        normalise_floats(out, float_total);
    }

    int8_t m_mode = mode_inactive;
    std::shared_ptr<Node> m_transition; // pTransitionBlenderGenerator
    bool m_parameter_pending = false;
    float m_duration = 0.0f, m_elapsed = 0.0f, m_remaining = 0.0f;
    std::vector<float> m_total;
};

// --- BSOffsetAnimationGenerator -----------------------------------------------------------------------------------

class OffsetAnimationGenerator final : public Generator {
public:
    Node* default_generator = nullptr;
    Node* offset_clip = nullptr; // only read: never activated here
    float offset_variable = 0.0f, range_start = 0.0f, range_end = 0.0f;

    std::unique_ptr<Node> clone() const override { return std::make_unique<OffsetAnimationGenerator>(*this); }
    void describe(Describer& d) override {
        d.node("pDefaultGenerator", default_generator);
        d.node("pOffsetClipGenerator", offset_clip);
        d.member("fOffsetVariable", offset_variable);
        d.member("fOffsetRangeStart", range_start);
        d.member("fOffsetRangeEnd", range_end);
    }

    void activate(Context& ctx) override { refresh(ctx); }
    void update(Context& ctx, float) override { refresh(ctx); }
    void deactivate(Context&) override {
        m_offsets.clear();
        m_bones.clear();
        m_valid = false;
        m_current_frame = -1;
    }

    void children(uint32_t flags, std::vector<Child>& out) override {
        out.push_back({default_generator, false, false, false, false});
        if (offset_clip && !(flags & children_active_only)) {
            out.push_back({offset_clip, false, false, false, false});
        }
    }

    void generate(Context&, std::span<const Output* const> kids, Output& out, float) override {
        if (!kids.empty()) {
            out.copy_from(*kids[0]);
        }
        if (m_zero || !m_valid) {
            return;
        }
        for (size_t i = 0; i < m_offsets.size(); ++i) {
            const int32_t b = m_bones[i];
            if (b >= 0 && size_t(b) < out.pose.size() && (out.all_present || out.present[size_t(b)])) {
                out.pose[size_t(b)] = compose(m_offsets[i], out.pose[size_t(b)]);
            }
        }
    }

    void update_sync(Context& ctx) override { ctx.graph.copy_sync(*this, default_generator); }

private:
    static bool is_identity(const Qs& q) {
        constexpr float k = 0.001f;
        auto near = [](float a, float b) { return std::abs(a - b) <= k; };
        for (int i = 0; i < 3; ++i) {
            if (!near(q.t[i], 0.0f) || !near(q.s[i], 1.0f)) {
                return false;
            }
        }
        return near(q.r.x, 0.0f) && near(q.r.y, 0.0f) && near(q.r.z, 0.0f) && near(q.r.w, 1.0f);
    }

    void refresh(Context& ctx) {
        const float range = range_end - range_start;
        // fOffsetRangeStart isn't subtracted from the variable, as the game has it.
        const float p = range > k_epsilon ? std::clamp(offset_variable / range, 0.0f, 1.0f) : 0.0f;
        if (m_valid && std::abs(p - m_percentage) <= k_epsilon) {
            return;
        }
        m_percentage = p;
        const auto* generator = dynamic_cast<const ClipGenerator*>(offset_clip);
        const Clip* clip = generator ? ctx.character.project->clip(generator->animation_name) : nullptr;
        if (!clip || !clip->data.additive) {
            m_zero = true;
            if (clip) {
                log::error("{}: offset clip {} isn't additive", name, clip->name);
            }
            return;
        }
        const float duration = clip->data.duration;
        const auto frames = int32_t(clip->data.frames);
        if (frames < 2 || duration <= 0.0f) {
            m_zero = true;
            return;
        }
        // The frame range skips the first frame, which is the additive base.
        const float frame_time = duration / float(frames - 1);
        const float time = (duration - frame_time) * p + frame_time;
        const int32_t frame = std::min(int32_t((time / duration) * float(frames - 1)), frames - 2);
        if (frame == m_current_frame) {
            return;
        }
        m_current_frame = frame;
        const uint32_t tracks = clip->data.transform_tracks;
        m_offsets.resize(tracks);
        m_bones.resize(tracks);
        m_valid = true;
        clip->sample(time, m_offsets);
        m_zero = true;
        for (uint32_t i = 0; i < tracks; ++i) {
            m_bones[i] = is_identity(m_offsets[i]) ? -1 : clip->track_bone(i);
            m_zero = m_zero && m_bones[i] < 0;
        }
    }

    std::vector<Qs> m_offsets;      // BoneOffsetA
    std::vector<int32_t> m_bones;   // BoneIndexA
    float m_percentage = 0.0f;
    int32_t m_current_frame = -1;
    bool m_valid = false;
    bool m_zero = false;
};

// --- BSSynchronizedClipGenerator ----------------------------------------------------------------------------------

// hkbDefaultSynchronizedScene: everyone always there, one float of time.
class DefaultSyncScene final : public SyncScene {
public:
    bool all_in_scene() const override { return true; }
    bool all_at_marks() const override { return true; }
    float time() const override { return m_time; }
    void set_time(float time) override { m_time = time; }
    void join(SyncMarks&) override {}
    void leave(SyncMarks&) override {}
    void left_early(SyncMarks&) override {}

private:
    float m_time = 0.0f;
};

class SynchronizedAnimationInstance final : public SyncScene {
public:
    SynchronizedAnimationInstance(uint32_t expected, std::function<void()> on_all_left, std::function<void()> on_left_early)
        : m_expected(expected), m_on_all_left(std::move(on_all_left)), m_on_left_early(std::move(on_left_early)) {}

    bool all_in_scene() const override {
        std::lock_guard lock(m_mutex); // each side's graph runs on its own worker
        return m_joined.size() == m_expected;
    }
    bool all_at_marks() const override {
        std::lock_guard lock(m_mutex);
        return std::ranges::all_of(m_joined, [](const SyncMarks* m) { return m->at_mark; });
    }
    float time() const override {
        std::lock_guard lock(m_mutex);
        return m_time;
    }
    // Only the first call of each round counts, so that all share one value.
    void set_time(float time) override {
        std::lock_guard lock(m_mutex);
        if (m_round == 0) {
            m_time = time;
        }
        m_round = m_round + 1 >= m_joined.size() ? 0 : m_round + 1;
    }
    void join(SyncMarks& marks) override {
        std::lock_guard lock(m_mutex);
        m_joined.push_back(&marks);
        if (m_joined.size() == m_expected) {
            resolve_marks();
        }
    }
    void leave(SyncMarks& marks) override {
        std::lock_guard lock(m_mutex);
        if (std::erase(m_joined, &marks) && m_joined.empty() && m_on_all_left) {
            m_on_all_left();
        }
    }
    void abandon(const void* owner) override {
        std::lock_guard lock(m_mutex);
        if (std::erase_if(m_joined, [owner](const SyncMarks* m) { return m->owner == owner; }) && m_joined.empty() &&
            m_on_all_left) {
            m_on_all_left();
        }
    }
    void left_early(SyncMarks&) override {
        if (m_on_left_early) {
            m_on_left_early();
        }
    }

private:
    // Followers move to where the clip puts them relative to the lead's start. The lead stays where it started.
    void resolve_marks() {
        if (m_joined.empty()) {
            return;
        }
        auto it = std::ranges::find_if(m_joined, [](const SyncMarks* m) { return m->lead; });
        SyncMarks& lead = **(it != m_joined.end() ? it : m_joined.begin());
        lead.lead = true;
        const Qs base = compose(lead.start_ws, inverse(lead.start_ms));
        for (SyncMarks* m : m_joined) {
            if (m != &lead) {
                const glm::vec3 scale = m->end_ws.s;
                m->end_ws = compose(base, m->start_ms);
                m->end_ws.s = scale;
            }
        }
    }

    uint32_t m_expected;
    std::function<void()> m_on_all_left, m_on_left_early;
    mutable std::mutex m_mutex;
    std::vector<SyncMarks*> m_joined;
    float m_time = 0.0f;
    size_t m_round = 0;
};

// Its clip is listed only outside the active set, so the root never reaches it: this node drives it, as the game does.
class SynchronizedClipGenerator final : public Generator {
public:
    Node* clip_template = nullptr; // pClipGenerator
    std::string prefix;            // SyncAnimPrefix
    bool ignore_mark_placement = false; // no code reads it
    float get_to_mark_time = 0.0f, mark_error_threshold = 0.0f;
    bool lead_character = false, reorient_support_char = false, apply_motion_from_root = false;
    int16_t binding_index = -1; // sAnimationBindingIndex

    std::unique_ptr<Node> clone() const override { return std::make_unique<SynchronizedClipGenerator>(*this); }
    void describe(Describer& d) override {
        d.node("pClipGenerator", clip_template);
        d.member("SyncAnimPrefix", prefix);
        d.member("bSyncClipIgnoreMarkPlacement", ignore_mark_placement);
        d.member("fGetToMarkTime", get_to_mark_time);
        d.member("fMarkErrorThreshold", mark_error_threshold);
        d.member("bLeadCharacter", lead_character);
        d.member("bReorientSupportChar", reorient_support_char);
        d.member("bApplyMotionFromRoot", apply_motion_from_root);
        d.member("sAnimationBindingIndex", binding_index);
    }

    void activate(Context& ctx) override {
        if (!m_clip) {
            if (const auto* c = dynamic_cast<const ClipGenerator*>(clip_template)) {
                m_clip = std::make_shared<ClipGenerator>(*c);
            }
        }
        build_event_map(ctx);
        Qs start_mark;
        if (m_clip) {
            m_clip->activate(ctx);
            if (const Clip* clip = m_clip->bound_clip()) {
                const auto& tracks = ctx.character.project->track_names(*clip);
                if (binding_index == -1) {
                    bind_locally(ctx, tracks);
                }
                start_mark = find_start_mark(*clip, tracks);
            }
        }
        m_scene = ctx.character.sync_scene ? ctx.character.sync_scene : std::make_shared<DefaultSyncScene>();
        m_marks.lead = lead_character;
        m_marks.owner = &ctx.character;
        m_marks.at_mark = false;
        m_marks.start_ms = start_mark;
        m_marks.start_ws = m_marks.end_ws = ctx.character.world_from_model;
        m_lerp = get_to_mark_time == 0.0f ? 1.0f : 0.0f;
        m_scene->join(m_marks);
    }

    void update(Context& ctx, float dt) override {
        m_all_at_marks = m_scene->all_at_marks();
        m_all_in_scene = m_scene->all_in_scene();
        // The clip's time doesn't run until everyone is in place.
        if (m_clip && m_clip->bound() && m_all_at_marks) {
            m_raised.clear();
            m_clip->capture_events(&m_raised);
            m_clip->update(ctx, dt);
            m_clip->capture_events(nullptr);
            for (const EventProperty& e : m_raised) {
                if (auto it = m_event_map.find(e.id); it != m_event_map.end()) {
                    ctx.graph.raise(ctx, it->second, e.payload.get());
                }
            }
        }
        if (m_lerp < 1.0f) {
            m_lerp = std::min(m_lerp + dt / get_to_mark_time, 1.0f);
        }
        if (!m_marks.at_mark && m_lerp == 1.0f) {
            m_marks.at_mark = at_mark(ctx.character.world_from_model);
        }
    }

    void generate(Context& ctx, std::span<const Output* const> kids, Output& out, float time_offset) override {
        if (m_scene && m_all_in_scene && !m_marks.at_mark) {
            generate_clip(ctx, kids, out, time_offset);
            out.world_from_model = mark_interpolation();
            out.world_from_model_fraction = 1.0f;
            return;
        }
        if (m_scene && m_all_at_marks) {
            m_scene->set_time(time_offset);
            time_offset = m_scene->time();
        }
        generate_clip(ctx, kids, out, time_offset);
        if (apply_motion_from_root && !out.pose.empty()) {
            const auto& parents = ctx.character.skeleton->parents;
            const auto root = std::ranges::find(parents, int16_t{-1});
            if (root != parents.end()) {
                Qs& bone = out.pose[size_t(root - parents.begin())];
                out.world_from_model = compose(m_marks.end_ws, bone);
                out.world_from_model_fraction = 1.0f;
                bone = Qs{};
            }
        }
    }

    void deactivate(Context& ctx) override {
        if (m_clip && m_scene && m_clip->local_time() < 0.2f * m_clip->animation_duration()) {
            m_scene->left_early(m_marks);
        }
        if (m_clip) {
            m_clip->deactivate(ctx);
        }
        if (m_scene) {
            m_scene->leave(m_marks);
        }
        m_scene.reset();
    }

    void children(uint32_t flags, std::vector<Child>& out) override {
        if (!(flags & children_active_only)) {
            out.push_back({clip_template, false, false, false, false});
        }
    }

    void update_sync(Context&) override {
        if (record) {
            zero_sync(record->sync);
        }
    }

    bool clip_state(ActiveClip& c) const override { return m_clip && m_clip->clip_state(c); }

private:
    std::string_view event_prefix() const { return prefix.empty() ? std::string_view("NPC") : std::string_view(prefix); }

    // `NPCSoundPlay` raises `SoundPlay` on the lead's side, and `2_SoundPlay` the same on the other.
    void build_event_map(Context& ctx) {
        m_event_map.clear();
        if (!ctx.behavior) {
            return;
        }
        const std::string_view lead = event_prefix();
        const auto& names = ctx.behavior->event_names;
        for (size_t i = 0; i < names.size(); ++i) {
            const std::string_view event = names[i];
            if (iequals(event, "PairEnd")) {
                m_event_map[int32_t(i)] = int32_t(i);
            } else if (event.size() > lead.size() && iequals(event.substr(0, lead.size()), lead)) {
                if (const int32_t mapped = ctx.behavior->event_id(event.substr(lead.size())); mapped >= 0) {
                    m_event_map[int32_t(i)] = mapped;
                }
            }
        }
    }

    // One clip file holds every participant: a track named for this side's bone drives it, and any other is dropped.
    void bind_locally(Context& ctx, const std::vector<std::string>& tracks) {
        const Skeleton& skeleton = *ctx.character.skeleton;
        const Clip& clip = *m_clip->bound_clip();
        const size_t size = std::max({clip.data.track_bones.size(), tracks.size(), size_t(clip.data.transform_tracks)});
        std::vector<int32_t> bones(size, int32_t(skeleton.size()) + 1);
        bool matched = false;
        for (size_t b = 0; b < skeleton.size(); ++b) {
            const std::string wanted = prefix + skeleton.names[b];
            const auto it = std::ranges::find_if(tracks, [&](const std::string& t) { return iequals(t, wanted); });
            if (it != tracks.end()) {
                bones[size_t(it - tracks.begin())] = int32_t(b);
                matched = true;
            }
        }
        if (matched) {
            m_clip->use_local_binding(std::move(bones));
        }
    }

    Qs find_start_mark(const Clip& clip, const std::vector<std::string>& tracks) const {
        const std::string_view lead = event_prefix();
        const auto it = std::ranges::find_if(tracks, [&](const std::string& t) { return iequals(t, lead); });
        if (it == tracks.end() || size_t(it - tracks.begin()) >= clip.data.transform_tracks) {
            return {};
        }
        std::vector<Qs> sampled(clip.data.transform_tracks);
        clip.sample(0.0f, sampled);
        return sampled[size_t(it - tracks.begin())];
    }

    bool at_mark(const Qs& world_from_model) const {
        const float k = mark_error_threshold;
        const Qs& end = m_marks.end_ws;
        auto near = [k](float a, float b) { return std::abs(a - b) <= k; };
        for (int i = 0; i < 3; ++i) {
            if (!near(end.t[i], world_from_model.t[i])) {
                return false;
            }
        }
        if (!reorient_support_char) {
            return true;
        }
        for (int i = 0; i < 3; ++i) {
            if (!near(end.s[i], world_from_model.s[i])) {
                return false;
            }
        }
        // Ours comes back through the actor's matrix, which can flip q to -q: the same turn.
        const glm::quat& a = end.r;
        const glm::quat b = glm::dot(end.r, world_from_model.r) < 0.0f ? -world_from_model.r : world_from_model.r;
        return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z) && near(a.w, b.w);
    }

    Qs mark_interpolation() const {
        const Qs& start = m_marks.start_ws;
        const Qs& end = m_marks.end_ws;
        if (!reorient_support_char) {
            return {end.t, start.r, start.s};
        }
        Qs q = zero_transform();
        add_transform(q, start, 1.0f - m_lerp);
        add_transform(q, end, m_lerp);
        q.r = normalised(q.r);
        return q;
    }

    void generate_clip(Context& ctx, std::span<const Output* const> kids, Output& out, float time_offset) {
        if (m_clip && m_clip->bound()) {
            m_clip->generate(ctx, kids, out, time_offset);
            return;
        }
        out.pose = ctx.character.skeleton->reference;
        std::fill(out.present.begin(), out.present.end(), uint8_t{1});
        out.all_present = true;
        out.pose_fraction = 1.0f;
    }

    std::shared_ptr<ClipGenerator> m_clip; // this clone's own
    std::shared_ptr<SyncScene> m_scene;
    SyncMarks m_marks;
    float m_lerp = 0.0f;
    bool m_all_at_marks = false, m_all_in_scene = false;
    std::unordered_map<int32_t, int32_t> m_event_map;
    std::vector<EventProperty> m_raised;
};

// --- BGSGamebryoSequenceGenerator ---------------------------------------------------------------------------------

// Plays a .kf sequence on the object's scene graph through the character's hook, so it produces no pose.
class GamebryoSequenceGenerator final : public Generator {
public:
    enum BlendMode : int8_t { bmf_none = 0, bmf_percent = 1, bmf_one_minus_percent = 2 };

    std::string sequence;
    int8_t blend_mode = bmf_none;
    float percent = 1.0f;

    std::unique_ptr<Node> clone() const override { return std::make_unique<GamebryoSequenceGenerator>(*this); }
    void describe(Describer& d) override {
        d.member("pSequence", sequence);
        d.member("eBlendModeFunction", blend_mode);
        d.member("fPercent", percent);
    }

    void activate(Context&) override {
        m_time = 0.0f;
        m_delayed_activate = true;
    }

    void update(Context& ctx, float dt) override {
        m_time += dt;
        Character& c = ctx.character;
        if (m_delayed_activate) {
            m_delayed_activate = false;
            start(ctx);
        }
        if (!m_started || !c.sequence_update) {
            return;
        }
        const float length = c.sequence_update(sequence, m_time, weight());
        if (length <= 0.0f) {
            return;
        }
        float previous = m_time - dt, now = m_time;
        if (m_looping) {
            previous = std::fmod(previous, length);
            now = std::fmod(now, length);
        }
        for (const SequenceEvent& e : m_events) {
            const bool hit = previous < now ? previous <= e.time && e.time < now
                                            : previous > now && (e.time >= previous || e.time < now);
            if (hit) {
                ctx.graph.raise(ctx, e.id);
            }
        }
    }

    void deactivate(Context& ctx) override {
        m_time = -FLT_MAX;
        m_delayed_activate = false;
        m_events.clear();
        if (m_started && ctx.character.sequence_stop) {
            ctx.character.sequence_stop(sequence);
        }
        m_started = false;
    }

    void generate(Context& ctx, std::span<const Output* const>, Output& out, float) override {
        out.world_from_model = ctx.character.world_from_model;
        out.world_from_model_fraction = 1.0f;
        std::fill(out.present.begin(), out.present.end(), uint8_t{0});
        out.all_present = false;
    }

    void set_local_time(Context&, float time) override { m_time = time; }

private:
    struct SequenceEvent {
        float time;
        int32_t id;
    };

    void start(Context& ctx) {
        if (!ctx.character.sequence_start) {
            return;
        }
        std::vector<SequenceKey> keys;
        m_started = ctx.character.sequence_start(sequence, m_time, m_looping, keys);
        for (const SequenceKey& k : keys) {
            if (const int32_t id = ctx.behavior ? ctx.behavior->event_id(k.name) : -1; id >= 0) {
                m_events.push_back({k.time, id});
            }
        }
    }

    float weight() const {
        switch (blend_mode) {
        case bmf_percent:
            return std::clamp(percent, 0.0f, 1.0f);
        case bmf_one_minus_percent:
            return std::clamp(1.0f - percent, 0.0f, 1.0f);
        default:
            return 1.0f;
        }
    }

    float m_time = 0.0f;
    bool m_delayed_activate = false, m_started = false, m_looping = false;
    std::vector<SequenceEvent> m_events;
};

} // namespace

std::vector<BindingTrigger> bind_annotations(const Project& project, const Clip& clip) {
    std::vector<BindingTrigger> out;
    for (const osn::Annotation& a : clip.data.annotations) {
        if (const int32_t id = project.external_event(a.text); id >= 0) {
            out.push_back({a.time, id, nullptr});
            continue;
        }
        const size_t dot = a.text.find('.');
        if (dot == std::string::npos) {
            continue;
        }
        if (const int32_t id = project.external_event(std::string_view(a.text).substr(0, dot)); id >= 0) {
            out.push_back({a.time, id, std::make_shared<const std::string>(a.text.substr(dot + 1))});
        }
    }
    return out;
}

MirroredSkeleton make_mirrored_skeleton(const Skeleton& skeleton, const glm::vec4& mirror_axis,
                                        std::vector<int16_t> bone_pairs) {
    // The rotation group of a cube, in the game's table order, so clean skeletons get exact mirrors.
    constexpr float h = 0.70710678f;
    constexpr std::array<std::array<float, 4>, 24> cube{{
        {1, 0, 0, 0},      {0, 1, 0, 0},       {0, 0, 1, 0},       {0, 0, 0, 1},
        {h, h, 0, 0},      {h, -h, 0, 0},      {0, h, h, 0},       {0, h, -h, 0},
        {0, 0, h, h},      {0, 0, h, -h},      {h, 0, h, 0},       {h, 0, -h, 0},
        {0, h, 0, h},      {0, h, 0, -h},      {h, 0, 0, h},       {h, 0, 0, -h},
        {.5f, .5f, .5f, .5f},   {.5f, .5f, .5f, -.5f},   {.5f, .5f, -.5f, .5f},   {.5f, .5f, -.5f, -.5f},
        {.5f, -.5f, .5f, .5f},  {.5f, -.5f, .5f, -.5f},  {.5f, -.5f, -.5f, .5f},  {.5f, -.5f, -.5f, -.5f},
    }};
    constexpr float tolerance = 0.1f;

    MirroredSkeleton out;
    out.axis = glm::quat(mirror_axis.w, mirror_axis.x, mirror_axis.y, mirror_axis.z);
    const size_t bones = skeleton.size();
    std::vector<glm::quat> model(bones);
    for (size_t b = 0; b < bones; ++b) {
        const int16_t parent = skeleton.parents[b];
        model[b] = parent >= 0 ? model[size_t(parent)] * skeleton.reference[b].r : skeleton.reference[b].r;
    }
    out.rotations.resize(bones);
    for (size_t b = 0; b < bones; ++b) {
        const int16_t pair = b < bone_pairs.size() ? bone_pairs[b] : int16_t(b);
        const glm::quat paired = pair >= 0 && size_t(pair) < bones ? model[size_t(pair)] : model[b];
        glm::quat q = normalised(glm::conjugate(model[b]) * out.axis * paired);
        for (const auto& c : cube) {
            const glm::quat candidate(c[3], c[0], c[1], c[2]);
            if (std::abs(glm::dot(q, candidate)) >= 1.0f - tolerance) {
                q = candidate;
                break;
            }
        }
        out.rotations[b] = q;
    }
    out.bone_pairs = std::move(bone_pairs);
    return out;
}

std::shared_ptr<SyncScene> make_sync_scene(uint32_t participants, std::function<void()> on_all_left,
                                           std::function<void()> on_left_early) {
    return std::make_shared<SynchronizedAnimationInstance>(participants, std::move(on_all_left), std::move(on_left_early));
}

// "Joining". The record's sync time moves with the node: the next timestep measures from it.
void Graph::join(Context& ctx, const Node* template_node, float time) {
    auto it = m_index.find(template_node);
    if (it == m_index.end()) {
        return;
    }
    Behavior* saved = ctx.behavior;
    auto place = [&](auto& self, uint32_t index, float t) -> void {
        Record& r = *m_records[index];
        if (!r.generator) {
            return;
        }
        ctx.behavior = r.behavior;
        static_cast<Generator*>(r.clone.get())->set_local_time(ctx, t);
        Record* own = sync_record(&r);
        own->sync.local_time = t;
        for (uint32_t k = 0; k < r.child_count; ++k) {
            const int32_t c = m_children[r.first_child + k];
            if (c < 0) {
                continue;
            }
            Record& child = *m_records[size_t(c)];
            if (!child.has_sync_parent || child.sync_parent != int32_t(index)) {
                continue;
            }
            const SyncInfo& parent_sync = own->sync;
            const Record* child_sync = sync_record(&child);
            float child_time = t;
            if (parent_sync.base_frequency != 0.0f && child_sync->sync.base_frequency != 0.0f) {
                child_time = time_at(child_sync->sync, sync_position(parent_sync, t)).value_or(t);
            }
            self(self, uint32_t(c), child_time);
        }
    };
    place(place, it->second, time);
    ctx.behavior = saved;
}

// The game with the game: down from the root, each node visited once, by the first
// path that reaches it.
void Graph::active_clips(std::vector<ActiveClip>& out) {
    out.clear();
    if (m_records.empty()) {
        return;
    }
    std::vector<uint8_t> seen(m_records.size(), 0);
    auto walk = [&](auto& self, size_t index, float blend, float motion) -> void {
        if (seen[index]) {
            return;
        }
        seen[index] = 1;
        Record& r = *m_records[index];
        if (!r.generator) {
            return;
        }
        const auto* generator = static_cast<const Generator*>(r.clone.get());
        if (ActiveClip clip; generator->clip_state(clip)) {
            clip.blend_weight = blend;
            clip.motion_weight = motion;
            out.push_back(clip);
            return;
        }
        for (uint32_t k = 0; k < r.child_count; ++k) {
            const int32_t c = m_children[r.first_child + k];
            if (c >= 0) {
                float child_blend = 1.0f, child_motion = 1.0f;
                generator->child_weight(k, child_blend, child_motion);
                self(self, size_t(c), blend * child_blend, motion * child_motion);
            }
        }
    };
    walk(walk, m_records.size() - 1, 1.0f, 1.0f);
}

void register_generator_nodes() {
    register_node<ClipGenerator>("hkbClipGenerator");
    register_node<BlenderGenerator>("hkbBlenderGenerator");
    register_node<PoseMatchingGenerator>("hkbPoseMatchingGenerator");
    register_node<ReferencePoseGenerator>("hkbReferencePoseGenerator");
    register_node<BoneSwitchGenerator>("BSBoneSwitchGenerator");
    register_node<CyclicBlendTransitionGenerator>("BSCyclicBlendTransitionGenerator");
    register_node<StateTaggingGenerator>("BSiStateTaggingGenerator");
    register_node<SynchronizedClipGenerator>("BSSynchronizedClipGenerator");
    register_node<OffsetAnimationGenerator>("BSOffsetAnimationGenerator");
    register_node<GamebryoSequenceGenerator>("BGSGamebryoSequenceGenerator");
}

} // namespace spud::anim
