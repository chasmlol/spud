#pragma once

// Noclip free camera. Z up, X east, Y north. Internally yaw is radians
// clockwise from north and pitch radians with positive looking up; the
// command line and bench scripts use Skyrim's getangle convention (degrees,
// x positive = looking down) and convert at the edge.

#include "render/view.hpp"

#include <glm/glm.hpp>

namespace spud::engine {

struct CameraInput {
    float forward = 0; // -1..1
    float right = 0;
    float up = 0;
    bool fast = false;
    float look_dx = 0; // mouse pixels
    float look_dy = 0;
    float wheel = 0;
};

// The first-person eye trails the body on a spring, in game units.
class FirstPersonLag {
public:
    struct Settings {
        float horz_spring = 0.0001f, horz_velocity = 0.4f; // f1stHorzDampening*:Camera
        float vert_spring = 0.001f, vert_velocity = 0.6f;  // f1stVertDampening*:Camera
    };

    void begin(const glm::vec3& feet);
    glm::vec3 step(const glm::vec3& feet, float dt, bool fade_out, const Settings& s);

private:
    glm::vec3 m_reference{0}, m_velocity{0};
    float m_weight = 1.0f;
};

class FreeCamera {
public:
    glm::vec3 position{0};
    float yaw = 0;   // radians, clockwise from north
    float pitch = 0; // radians, positive up
    float base_speed = 20.0f;   // m/s
    float fast_multiplier = 6.0f;
    float sensitivity = 0.0022f; // radians per pixel
    float fov_y = glm::radians(65.0f);
    float fov_offset = 0; // degrees on the game's FOV scale (horizontal at 16:9), from the player's animation
    glm::vec3 shake{0}; // this frame's camera shake: radians about the camera's right, forward and up
    glm::mat3 sway{1};  // the animated camera node's turn, in view space

    void update(const CameraInput& in, float dt);
    void set_pitch(float p);
    glm::vec3 forward() const;
    render::View view(float aspect) const;

    // Skyrim getangle conventions (degrees).
    void set_skyrim_angles(float heading_deg, float pitch_deg);
    float skyrim_heading_deg() const;
    float skyrim_pitch_deg() const;
};

} // namespace spud::engine
