#pragma once

// A character's ragdoll, from its skeleton file: rigid bodies, constraints, and the mappers between the ragdoll's bones
// and the animation skeleton's. Lengths are game units.

#include "anim/pose.hpp"
#include "formats/osb.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace spud::anim {

struct RagdollBody {
    std::string name; // its node in the skeleton NIF
    float radius = 0.0f;
    glm::vec3 a{0.0f}, b{0.0f}; // the capsule's ends in the body
    float mass = 1.0f;
    glm::vec3 inverse_inertia{0.0f};
    float friction = 0.0f;
    float restitution = 0.0f;
    float linear_damping = 0.0f;
    float angular_damping = 0.0f;
    float gravity_factor = 1.0f;
    float max_linear_velocity = 0.0f;  // m/s, from the skeleton NIF
    float max_angular_velocity = 0.0f; // radians/s
    uint32_t collision_filter = 0;
    float force_scale = 1.0f; // of a hit's velocity, by body part
    Qs rest; // in the skeleton file's rest pose, model space
};

struct RagdollConstraint {
    int32_t body_a = -1, body_b = -1;
    bool hinge = false; // hkpLimitedHingeConstraintData, else hkpRagdollConstraintData
    // The constraint's frame in each body: columns are its axes (twist, plane, cone; a hinge's axis first).
    glm::mat3 axes_a{1.0f}, axes_b{1.0f};
    glm::vec3 pivot_a{0.0f}, pivot_b{0.0f};
    float twist_min = 0.0f, twist_max = 0.0f; // radians
    float cone_max = 0.0f;
    float plane_min = 0.0f, plane_max = 0.0f;
    float hinge_min = 0.0f, hinge_max = 0.0f;
    float friction = 0.0f;
};

struct RagdollMapping {
    int32_t bone_a = -1, bone_b = -1;
    Qs a_from_b;
};

struct Ragdoll {
    std::vector<RagdollBody> bodies;
    std::vector<RagdollConstraint> constraints;
    std::vector<int32_t> bone_to_body;         // by ragdoll bone
    std::vector<int16_t> parents;              // the ragdoll skeleton
    std::vector<RagdollMapping> to_ragdoll;    // A the animation skeleton, B the ragdoll's
    std::vector<RagdollMapping> to_animation;  // A the ragdoll's, B the animation skeleton

    // Each body's transform in model space for an animation pose in model space.
    void bodies_from_pose(std::span<const Qs> model_pose, std::vector<Qs>& bodies_out) const;
    // The animation skeleton's local pose from the bodies in model space, in place: mapped bones follow them, the others
    // keep their local transforms (keepUnmappedLocal).
    void pose_from_bodies(std::span<const Qs> bodies_model, std::span<const int16_t> bone_parents,
                          std::span<Qs> local_pose) const;
};

// Null when the graph holds no ragdoll instance. Throws ParseError on one it can't read.
std::shared_ptr<const Ragdoll> read_ragdoll(const osb::Graph& graph);

} // namespace spud::anim
