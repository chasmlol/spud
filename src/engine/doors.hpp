#pragma once

// Load doors as seamless portals. A door's doorway is the rectangle its
// base's bounds span in the door's plane; passing in through one doorway comes out of its partner, so the two
// doorways are joined by a rigid map that turns about Z only.

#include "formats/osw.hpp"

#include <glm/glm.hpp>

#include <optional>

namespace spud::engine {

struct Doorway {
    glm::vec3 centre{0.0f}; // middle of the opening, metres
    glm::vec3 normal{0.0f}; // horizontal: out of the door, into the space it stands in
    glm::vec3 right{0.0f};  // horizontal, along the opening
    float half_width = 0.0f;
    float half_height = 0.0f;

    float side(const glm::vec3& p) const { return glm::dot(p - centre, normal); } // > 0: in front
    glm::vec3 bottom() const { return centre - glm::vec3(0.0f, 0.0f, half_height); }
    // 0 bottom left, 1 bottom right, 2 top right, 3 top left (seen from in front).
    glm::vec3 corner(int i) const;
    // Whether a point on (or near) the plane lies inside the opening, grown by `margin` metres.
    bool within(const glm::vec3& p, float margin) const;
};

// The doorway of a door reference: position, rotation (quaternion xyzw) and scale, its base's OBND (metres). The
// thinner horizontal axis of the bounds is the door's normal, turned towards `front`, a point in front of it (the
// partner door's teleport marker). nullopt for a door with no extent.
std::optional<Doorway> make_doorway(const float position[3], const float rotation[4], float scale,
                                    const float bounds_min[3], const float bounds_max[3], const glm::vec3& front);

// Near space -> far space: in through `near` (against its normal), out of `far` (along its normal); floors meet.
struct PortalTransform {
    glm::vec3 from{0.0f};
    glm::vec3 to{0.0f};
    float angle = 0.0f; // counter-clockwise about +Z, radians
    glm::vec3 vector(const glm::vec3& v) const;
    glm::vec3 point(const glm::vec3& p) const { return to + vector(p - from); }
    float yaw(float y) const { return y - angle; } // camera yaw: clockwise from north
    PortalTransform inverse() const;
};
PortalTransform portal_transform(const Doorway& near, const Doorway& far);

} // namespace spud::engine
