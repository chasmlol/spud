#pragma once

// Poses and generator outputs. Lengths are the file's: game
// units, as every value in the graphs is. Only the engine's boundary converts to metres.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace spud::anim {

// hkQsTransform. parent = t + r * (s * local).
struct Qs {
    glm::vec3 t{0.0f};
    glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 s{1.0f};
};

inline Qs operator*(const Qs& a, const Qs& b) {
    return {a.t + a.r * (a.s * b.t), a.r * b.r, a.s * b.s};
}

// hkbGeneratorOutput's live tracks. A track's fraction of 0 means it is absent.
struct Output {
    Qs world_from_model;
    float world_from_model_fraction = 0.0f;
    Qs motion; // extracted motion
    float motion_fraction = 0.0f;
    std::vector<Qs> pose;          // bones, in the animation skeleton's order
    std::vector<uint8_t> present;  // per bone: produced
    bool all_present = false;
    float pose_fraction = 0.0f;
    bool additive = false;         // flag 1 on the pose track
    std::vector<float> floats;     // float slots
    float floats_fraction = 0.0f;

    void resize(size_t bones, size_t float_slots) {
        pose.resize(bones);
        present.resize(bones);
        floats.resize(float_slots);
    }
    // Clearing: every track absent.
    void clear() {
        world_from_model_fraction = motion_fraction = pose_fraction = floats_fraction = 0.0f;
        additive = false;
        all_present = false;
        std::fill(present.begin(), present.end(), uint8_t{0});
    }
    // Copying: absent tracks are skipped.
    void copy_from(const Output& o) {
        world_from_model_fraction = o.world_from_model_fraction;
        if (o.world_from_model_fraction > 0.0f) {
            world_from_model = o.world_from_model;
        }
        motion_fraction = o.motion_fraction;
        if (o.motion_fraction > 0.0f) {
            motion = o.motion;
        }
        pose_fraction = o.pose_fraction;
        if (o.pose_fraction > 0.0f) {
            pose = o.pose;
            present = o.present;
            all_present = o.all_present;
            additive = o.additive;
        }
        floats_fraction = o.floats_fraction;
        if (o.floats_fraction > 0.0f) {
            floats = o.floats;
        }
    }
};

} // namespace spud::anim
