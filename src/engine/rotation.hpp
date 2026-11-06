#pragma once

// Reference rotations. The importer stores a quaternion; the debug toggle
// rebuilds it from Skyrim's euler angles with alternative conventions so the
// right one can be confirmed visually.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>

namespace spud::engine {

enum class RotationMode : int {
    stored = 0, // the .osw quaternion
    xyz_neg,    // Rx(-x) Ry(-y) Rz(-z)  (what the importer is expected to store)
    zyx_neg,    // Rz(-z) Ry(-y) Rx(-x)
    xyz_pos,    // Rx(x) Ry(y) Rz(z)
    zyx_pos,    // Rz(z) Ry(y) Rx(x)
};

inline constexpr std::array<const char*, 5> k_rotation_mode_names = {
    "stored quaternion", "Rx(-x)Ry(-y)Rz(-z)", "Rz(-z)Ry(-y)Rx(-x)", "Rx(x)Ry(y)Rz(z)", "Rz(z)Ry(y)Rx(x)"};

inline glm::mat3 rot_x(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return glm::mat3(1, 0, 0, 0, c, s, 0, -s, c); // column-major
}
inline glm::mat3 rot_y(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return glm::mat3(c, 0, -s, 0, 1, 0, s, 0, c);
}
inline glm::mat3 rot_z(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return glm::mat3(c, s, 0, -s, c, 0, 0, 0, 1);
}

// Column-vector convention: R * v. "Rx Ry Rz" means R = Rx * Ry * Rz.
inline glm::mat3 euler_rotation(RotationMode mode, const float e[3]) {
    switch (mode) {
    case RotationMode::xyz_neg: return rot_x(-e[0]) * rot_y(-e[1]) * rot_z(-e[2]);
    case RotationMode::zyx_neg: return rot_z(-e[2]) * rot_y(-e[1]) * rot_x(-e[0]);
    case RotationMode::xyz_pos: return rot_x(e[0]) * rot_y(e[1]) * rot_z(e[2]);
    case RotationMode::zyx_pos: return rot_z(e[2]) * rot_y(e[1]) * rot_x(e[0]);
    case RotationMode::stored: break;
    }
    return glm::mat3(1.0f);
}

inline glm::mat3 ref_rotation(RotationMode mode, const float quat_xyzw[4], const float euler[3]) {
    if (mode == RotationMode::stored) {
        const glm::quat q(quat_xyzw[3], quat_xyzw[0], quat_xyzw[1], quat_xyzw[2]); // glm: (w, x, y, z)
        return glm::mat3_cast(glm::normalize(q));
    }
    return euler_rotation(mode, euler);
}

} // namespace spud::engine
