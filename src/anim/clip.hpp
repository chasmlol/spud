#pragma once

// Sampling a clip's splines, and the animation skeleton.

#include "anim/pose.hpp"
#include "formats/osk.hpp"
#include "formats/osn.hpp"

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::anim {

// An animation skeleton: bones in the order clips and graphs number them. Lengths in game units.
struct Skeleton {
    std::vector<std::string> names;
    std::vector<int16_t> parents;
    std::vector<Qs> reference;
    std::vector<uint8_t> lock_translation;
    std::unordered_map<std::string, int32_t> by_name; // lowercase

    static Skeleton from(const osk::Skeleton& s);
    int32_t bone(std::string_view name) const; // -1 when absent
    size_t size() const { return names.size(); }
};

// A clip generator's root motion. Keys ascend; time 0 is
// implicitly identity.
struct ClipMotion {
    struct TranslationKey {
        float time;
        glm::vec3 translation; // units
    };
    struct RotationKey {
        float time;
        glm::quat rotation;
    };
    std::vector<TranslationKey> translation;
    std::vector<RotationKey> rotation;

    glm::vec3 translation_at(float time) const;
    glm::quat rotation_at(float time) const;
};

struct Clip {
    osn::Clip data;
    std::string name;
    // The transform tracks at `time` (seconds, clamped to the clip) into `out`, one entry per track, in the source's
    // units. `floats` receives the float tracks when given.
    void sample(float time, std::span<Qs> out, std::span<float> floats = {}) const;
    int32_t track_bone(uint32_t track) const {
        return track < data.track_bones.size() ? data.track_bones[track] : static_cast<int32_t>(track);
    }
};

} // namespace spud::anim
