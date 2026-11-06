#pragma once

// The character proxy's simplex solver, as the game runs it ("Character
// proxy"): moves a point through a set of constraint planes over a time step, with Havok's static, extra-static
// and dynamic friction. Plain maths; no Jolt.

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace spud::physics::simplex {

// A constraint plane: n . x + d >= 0 must hold for the displacement x; the plane itself moves at `velocity`.
struct Plane {
    glm::vec3 n{0, 0, 1};
    float d = 0;
    glm::vec3 velocity{0};
    float static_friction = 0;
    float extra_up_static_friction = 0;
    float extra_down_static_friction = 0;
    float dynamic_friction = 1;
    int priority = 0;
};

struct Interaction {
    bool touched = false;
    float surface_time = 0;
    float penalty = 0;
    int status = 0;
};

struct Input {
    glm::vec3 velocity{0};
    glm::vec3 max_surface_velocity{10.0f};
    glm::vec3 up{0, 0, 1};
    float delta_time = 0;
    float min_delta_time = 0;
};

struct Output {
    glm::vec3 position{0}; // the displacement
    glm::vec3 velocity{0};
    float delta_time = 0;
    std::vector<Interaction> interactions;
};

void solve(const Input& in, std::span<const Plane> planes, Output& out);

} // namespace spud::physics::simplex
