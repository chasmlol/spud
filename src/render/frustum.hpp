#pragma once

// View frustum in camera-relative space (planes from the reverse-Z infinite
// projection: left, right, bottom, top, near; the far plane is at infinity).

#include <glm/glm.hpp>

namespace spud::render {

enum class Cull { outside, intersect, inside };

struct Frustum {
    glm::vec4 planes[5]{}; // xyz normal (pointing inside), w distance

    static Frustum from_view_proj(const glm::mat4& m) {
        // Gribb-Hartmann on rows of the column-major matrix.
        auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
        const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        Frustum f;
        f.planes[0] = r3 + r0;
        f.planes[1] = r3 - r0;
        f.planes[2] = r3 + r1;
        f.planes[3] = r3 - r1;
        f.planes[4] = r3 - r2; // reverse-Z: depth <= 1 is the near side
        for (auto& p : f.planes) {
            p /= glm::length(glm::vec3(p));
        }
        return f;
    }

    // `c` is camera-relative.
    bool sphere(const glm::vec3& c, float r) const {
        for (const auto& p : planes) {
            if (glm::dot(glm::vec3(p), c) + p.w < -r) {
                return false;
            }
        }
        return true;
    }

    // Camera-relative AABB.
    Cull aabb(const glm::vec3& lo, const glm::vec3& hi) const {
        Cull result = Cull::inside;
        for (const auto& p : planes) {
            const glm::vec3 n(p);
            const glm::vec3 positive(n.x >= 0 ? hi.x : lo.x, n.y >= 0 ? hi.y : lo.y, n.z >= 0 ? hi.z : lo.z);
            const glm::vec3 negative(n.x >= 0 ? lo.x : hi.x, n.y >= 0 ? lo.y : hi.y, n.z >= 0 ? lo.z : hi.z);
            if (glm::dot(n, positive) + p.w < 0) {
                return Cull::outside;
            }
            if (glm::dot(n, negative) + p.w < 0) {
                result = Cull::intersect;
            }
        }
        return result;
    }
};

} // namespace spud::render
