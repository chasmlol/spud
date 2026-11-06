#pragma once

// The world map's camera: where the map menu
// looks from, and where each marker falls on the screen. Skyrim units, degrees where the settings are.

#include <glm/glm.hpp>

#include <functional>

namespace spud {

class WorldMapCamera {
public:
    // What the camera reads of the worldspace (WRLD MNAM, bounds).
    struct Data {
        float min_height = 0, max_height = 0, initial_pitch = 0; // pitch in degrees
        int nw_cell[2] = {0, 0}, se_cell[2] = {0, 0};
        float bounds_min[2] = {0, 0}, bounds_max[2] = {0, 0};
    };
    using Ground = std::function<float(float x, float y)>; // land height at a point

    struct Input {
        float pan[2] = {0, 0};  // the frame's motion, full speed at length 1 for a second
        float look[2] = {0, 0}; // the frame's turn and tilt, radians; positive turns right and looks down
        float zoom = 0;         // positive moves in
    };
    struct Projection {
        float x = 0, y = 0;     // fractions of the screen, y down
        float depth = 0;        // along the view; negative behind the camera
    };

    // The map opens on `target`: World::SetTarget's pose, reached through the transition from `target` itself (at once
    // when `transition_seconds` is 0).
    void begin(const Data& data, const glm::vec3& target, float aspect, float transition_seconds, Ground ground);
    void set_target(const glm::vec3& target);
    void update(float dt, const Input& input);

    bool settled() const { return m_elapsed >= m_transition; }
    const glm::vec3& target() const { return m_target; }
    glm::vec3 position() const { return m_position; }
    float yaw() const { return m_yaw; }     // radians, clockwise from north
    float pitch() const { return m_pitch; } // radians, positive looking down
    float zoom() const { return m_zoom; }
    float fov_y() const;                    // radians
    float near_plane() const;               // units
    Projection project(const glm::vec3& point) const;
    // the direction through a point of the screen (fractions, y down).
    glm::vec3 ray(float x, float y) const;

private:
    void correct_bounds();
    void place();

    Data m_data;
    Ground m_ground;
    glm::vec3 m_target{0.0f};
    glm::vec2 m_translation{0.0f};
    float m_aspect = 16.0f / 9.0f;
    float m_zoom = 0.25f;
    float m_land = 0;   // the ground height the camera stands over
    float m_elapsed = 0, m_transition = 0;
    glm::vec3 m_position{0.0f}, m_from{0.0f}, m_end{0.0f};
    float m_yaw = 0, m_pitch = 0, m_from_pitch = 0, m_end_yaw = 0, m_end_pitch = 0;
};

} // namespace spud
