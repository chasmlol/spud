#pragma once

// Private to src/physics: Jolt headers, layers and the shared World internals.

#include "physics/physics.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace spud::physics {

namespace object_layer {
inline constexpr JPH::ObjectLayer fixed = 0;  // static world collision
inline constexpr JPH::ObjectLayer moving = 1; // dynamic bodies (none yet)
inline constexpr JPH::ObjectLayer actor = 2;  // other actors' controllers: only the character proxy meets them
inline constexpr uint32_t count = 3;
} // namespace object_layer

namespace broad_layer {
inline constexpr JPH::BroadPhaseLayer fixed(0);
inline constexpr JPH::BroadPhaseLayer moving(1);
inline constexpr JPH::BroadPhaseLayer actor(2);
inline constexpr uint32_t count = 3;
} // namespace broad_layer

// The game's controller hull, for half extents and a step height.
JPH::Ref<JPH::Shape> character_hull(const glm::vec3& half_extents, float step_up);
// How far its centre stands above the feet.
float character_hull_centre(const glm::vec3& half_extents);

// Once per process; throws if the Jolt library was built with different feature defines.
void init_jolt();

inline JPH::Vec3 to_jolt(const glm::vec3& v) { return {v.x, v.y, v.z}; }
inline glm::vec3 to_glm(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }

// Leaf shape user data: Skyrim layer in the low byte, havok material in the next 32 bits, and the shape's Havok
// convex radius (tenths of a millimetre) above that. Jolt holds the bare geometry; Havok's surface lies that
// radius further out, which the character controller adds back.
inline uint64_t shape_user_data(uint8_t layer, uint32_t material, float convex_radius = 0.0f) {
    const auto r = static_cast<uint64_t>(std::clamp(std::lround(convex_radius * 1e4f), 0L, 0xFFFFL));
    return r << 40 | uint64_t(material) << 8 | layer;
}
inline uint8_t user_data_layer(uint64_t d) { return static_cast<uint8_t>(d & 0xFF); }
inline uint32_t user_data_material(uint64_t d) { return static_cast<uint32_t>((d >> 8) & 0xFFFFFFFFu); }
inline float user_data_radius(uint64_t d) { return static_cast<float>((d >> 40) & 0xFFFF) * 1e-4f; }
// Bit 63: a mesh whose triangles carry their rank in Havok's collection order as per-triangle user data.
inline constexpr uint64_t k_user_data_ranked = 1ull << 63;

class World;

// What Character needs from the world.
struct WorldAccess {
    JPH::PhysicsSystem& system;
};

WorldAccess access(World& world);

} // namespace spud::physics
