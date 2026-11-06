#pragma once

// Skyrim's third-person camera (ThirdPersonState): pivot, shoulder offset, zoom, pitch zoom-out, free rotation
// around an idle player, the yaw chase and collision with the world. One update per frame, in the game's order.
// ("Third-person camera") gives the source of every value; distances are Skyrim units,
// angles radians with the game's signs (heading clockwise from north, pitch positive looking down).

#include <glm/glm.hpp>

#include <functional>
#include <limits>
#include <optional>

namespace spud::engine::camera3 {

// INI settings ([Camera], [Controls]), executable defaults.
struct Settings {
    float over_shoulder_pos_x = 30;          // fOverShoulderPosX
    float over_shoulder_pos_z = -10;         // fOverShoulderPosZ
    float over_shoulder_add_y = 0;           // a mount's: the race's mount camera offset
    float over_shoulder_combat_pos_x = 0;    // fOverShoulderCombatPosX
    float over_shoulder_combat_add_y = -100; // fOverShoulderCombatAddY
    float over_shoulder_combat_pos_z = 20;   // fOverShoulderCombatPosZ
    float shoulder_dolly_speed = 3;          // fShoulderDollySpeed, 1/s
    float pitch_zoom_out_max_dist = 100;     // fPitchZoomOutMaxDist
    float collision_recovery_speed = 3;      // fCollisionRecoverySpeed, 1/s
    float collision_recovery_min_dist = 4000; // fCollisionRecoveryMinDist, squared units
    float chase_camera_speed = 10;           // fChaseCameraSpeed, rad/s
    float chase_camera_max_angle = 30;       // fChaseCameraMaxAngle, degrees
    float vanity_mode_min_dist = 155;        // fVanityModeMinDist
    float vanity_mode_max_dist = 600;        // fVanityModeMaxDist
    float min_current_zoom = -0.2f;          // fMinCurrentZoom
    float mouse_wheel_zoom_increment = 0.075f; // fMouseWheelZoomIncrement
    float mouse_wheel_zoom_speed = 0.8f;     // fMouseWheelZoomSpeed, per second
    float free_rotation_speed = 3;           // fFreeRotationSpeed, rad/s per unit of look input
    float camera_caster_size = 15;           // fCameraCasterSize: the collision sphere's radius
};

// What the camera follows this frame.
struct Target {
    glm::vec3 root{0};       // the actor's 3D root: where it was drawn last frame
    glm::vec3 position{0};   // the actor's position now (the collision cast starts above it)
    float height = 0;        // pivot height above the feet (the Cam3 node)
    float heading = 0;
    float pitch = 0;
    bool weapon_drawn = false;
};

// A sphere swept from `from` to `to`: the first surface's contact point, if any.
using SphereCast = std::function<std::optional<glm::vec3>(const glm::vec3& from, const glm::vec3& to, float radius)>;

class ThirdPerson {
public:
    explicit ThirdPerson(const Settings& s = {}) : m_settings(s) {}

    // Switching from first person: the offset jumps to its target, the zoom starts at the minimum and eases
    // to the saved one (the minimum until the player zooms).
    void begin(const Target& t);

    // Free rotation (the camera circles an idle, unarmed player without turning it). `want` is asked every
    // frame; it holds only while the player stood still last frame. Turning it off hands the camera's
    // heading and pitch to the player: returns them then.
    struct Facing {
        float heading, pitch;
    };
    std::optional<Facing> set_free_rotation(bool want, bool moving, const Target& t);
    bool free_rotation() const { return m_free; }

    // One frame: the look input (x yaw, y pitch, the controls' look vector), then smoothing, rotation and
    // position. `cast` may be empty (no collision).
    void update(const Target& t, glm::vec2 look, float dt, const SphereCast& cast);
    // Mouse wheel: +1 per notch away from the player.
    void zoom(float notches);

    glm::vec3 position() const { return m_position; }
    float yaw() const { return m_yaw; }     // the camera's heading
    float pitch() const { return m_pitch; } // the camera's pitch, positive looking down
    float zoom_level() const { return m_zoom; }

private:
    Settings m_settings;
    glm::vec3 m_offset_target{0}, m_offset{0}; // camera-local: x right, y forward, z up
    float m_zoom_target = 0, m_zoom = 0, m_saved_zoom = 0;
    float m_pitch_zoom = 0;
    float m_yaw_target = 0, m_yaw = 0;
    float m_pitch = 0;
    bool m_free = false;
    float m_free_yaw = 0, m_free_pitch = 0;
    glm::vec3 m_collided{0};
    float m_recovery = std::numeric_limits<float>::max(); // max: not recovering
    glm::vec3 m_position{0};
    bool m_saved = false;
};

} // namespace spud::engine::camera3
