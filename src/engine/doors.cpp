#include "engine/doors.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace spud::engine {

namespace {

constexpr float k_marker_extent = 0.25f; // metres: a door this small is a marker, not a door
constexpr float k_marker_half = 1.5f;
constexpr float k_widest_door_m = 30.0f; // scaled cave doors come to 22 m; only HHDoor02 passes this

} // namespace

glm::vec3 Doorway::corner(int i) const {
    const glm::vec3 r = right * half_width;
    const glm::vec3 u(0.0f, 0.0f, half_height);
    switch (i & 3) {
    case 0: return centre - r - u;
    case 1: return centre + r - u;
    case 2: return centre + r + u;
    default: return centre - r + u;
    }
}

bool Doorway::within(const glm::vec3& p, float margin) const {
    const glm::vec3 d = p - centre;
    return std::abs(glm::dot(d, right)) <= half_width + margin && std::abs(d.z) <= half_height + margin;
}

std::optional<Doorway> make_doorway(const float position[3], const float rotation[4], float scale,
                                    const float bounds_min[3], const float bounds_max[3], const glm::vec3& front) {
    const glm::mat3 m = glm::mat3_cast(glm::normalize(glm::quat(rotation[3], rotation[0], rotation[1], rotation[2])));
    const glm::vec3 lo(bounds_min[0], bounds_min[1], bounds_min[2]);
    const glm::vec3 hi(bounds_max[0], bounds_max[1], bounds_max[2]);
    const glm::vec3 half = (hi - lo) * 0.5f * std::abs(scale);
    const glm::vec3 pos(position[0], position[1], position[2]);
    Doorway d;
    // The thin horizontal axis is the normal; the other spans the opening.
    int thin = half.x <= half.y ? 0 : 1;
    // HHDoor02's bounds run 52 m back from the door, round the black behind it: that way is the normal, not the width.
    if (half[1 - thin] * 2.0f > k_widest_door_m) {
        thin = 1 - thin;
    }
    // The doorway is where the door shuts, the model's origin: its bounds run on behind that (DoorBlack). A leaf then
    // has a face on each side of the plane, each lit by its own space.
    glm::vec3 middle = (lo + hi) * 0.5f;
    middle[thin] = std::clamp(0.0f, lo[thin], hi[thin]);
    d.centre = pos + m * (middle * scale);
    glm::vec3 n = m[thin];
    n.z = 0.0f;
    // AutoLoadDoors are small markers with no opening of their own: they get a 3 m doorway standing on the floor
    // at the teleport marker's height and facing it.
    const bool marker = half[1 - thin] < k_marker_extent || half.z < k_marker_extent;
    if (marker) {
        n = glm::vec3(front.x - pos.x, front.y - pos.y, 0.0f);
        d.centre = glm::vec3(pos.x, pos.y, front.z + k_marker_half);
    }
    if (glm::length(n) < 1e-4f) {
        return std::nullopt;
    }
    n = glm::normalize(n);
    if (glm::dot(front - d.centre, n) < 0.0f) {
        n = -n;
    }
    d.normal = n;
    d.right = glm::vec3(n.y, -n.x, 0.0f); // to the right when facing the door from in front
    d.half_width = marker ? k_marker_half : half[1 - thin];
    d.half_height = marker ? k_marker_half : half.z;
    return d;
}

glm::vec3 PortalTransform::vector(const glm::vec3& v) const {
    const float c = std::cos(angle), s = std::sin(angle);
    return {c * v.x - s * v.y, s * v.x + c * v.y, v.z};
}

PortalTransform PortalTransform::inverse() const { return {to, from, -angle}; }

PortalTransform portal_transform(const Doorway& near, const Doorway& far) {
    // Walking in through `near` is moving along -near.normal; coming out of `far` is moving along far.normal.
    PortalTransform t;
    t.from = near.bottom();
    t.to = far.bottom();
    t.angle = std::atan2(far.normal.y, far.normal.x) - std::atan2(-near.normal.y, -near.normal.x);
    return t;
}

} // namespace spud::engine
