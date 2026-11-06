#include "physics/simplex.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace spud::physics::simplex {

namespace {

constexpr float k_eps = 1.1920929e-7f;

struct Solver {
    const Input& in;
    std::span<const Plane> planes;
    Output& out;

    bool penetrating(const glm::vec3& v, int i) const {
        return glm::dot(v - planes[i].velocity, planes[i].n) < -0.001f;
    }

    bool too_fast(const glm::vec3& v) const {
        return std::abs(v.x) > in.max_surface_velocity.x || std::abs(v.y) > in.max_surface_velocity.y ||
               std::abs(v.z) > in.max_surface_velocity.z;
    }

    // One plane with static, extra and dynamic friction.
    glm::vec3 one(const glm::vec3& v, int i) const {
        const Plane& p = planes[i];
        const glm::vec3 r = v - p.velocity;
        const float rn = glm::dot(p.n, r);
        glm::vec3 t = r - rn * p.n;
        const float ms = p.static_friction;
        const float e = glm::dot(in.up, t) <= 0.0f ? p.extra_down_static_friction : p.extra_up_static_friction;
        if (e <= 0.0f) {
            if (ms * ms * rn * rn >= glm::dot(t, t)) {
                return p.velocity;
            }
        } else {
            glm::vec3 side(0.0f);
            float s = 0;
            const glm::vec3 c = glm::cross(in.up, p.n);
            if (glm::dot(c, c) > k_eps) {
                side = glm::normalize(c);
                s = glm::dot(side, t);
            }
            if (ms * ms * rn * rn >= s * s) {
                t -= s * side;
                s = 0;
            }
            if ((e + ms) * (e + ms) * rn * rn >= glm::dot(r, r) - s * s - rn * rn) {
                if (s == 0) {
                    return p.velocity;
                }
                t = s * side;
            }
        }
        const float md = p.dynamic_friction;
        const float tt = glm::dot(t, t), rr = glm::dot(r, r);
        if (md < 1.0f && tt >= k_eps && tt > 0.001f * rr) {
            t *= std::sqrt(rr / tt) * (1.0f - md) + md;
            t -= glm::dot(p.n, t) * p.n;
        }
        return p.velocity + t;
    }

    // Two planes: along their edge, or stuck in the crease.
    glm::vec3 two(const glm::vec3& v, int a, int b) {
        const Plane& p1 = planes[a];
        const Plane& p2 = planes[b];
        const glm::vec3 d = glm::cross(p1.n, p2.n);
        const float dd = glm::dot(d, d);
        glm::vec3 ve(0.0f);
        bool degenerate = dd <= k_eps;
        glm::vec3 dh(0.0f);
        if (!degenerate) {
            dh = d / std::sqrt(dd);
            // n1 . ve = n1 . v1, n2 . ve = n2 . v2, dh . ve = mean of the planes' speeds along the edge.
            const glm::mat3 m = glm::transpose(glm::mat3(p1.n, p2.n, dh));
            const glm::vec3 rhs(glm::dot(p1.n, p1.velocity), glm::dot(p2.n, p2.velocity),
                                0.5f * glm::dot(dh, p1.velocity + p2.velocity));
            if (rhs != glm::vec3(0.0f)) {
                ve = glm::inverse(m) * rhs;
            }
            degenerate = too_fast(ve);
        }
        if (degenerate) {
            out.interactions[a].status = out.interactions[b].status = 2;
            const bool a_first = p1.priority <= p2.priority;
            const glm::vec3 first = one(v, a_first ? a : b);
            return one(first, a_first ? b : a);
        }
        const glm::vec3 r = v - ve;
        float along = glm::dot(r, dh);
        const float ud = glm::dot(in.up, dh);
        const float e = along * ud <= 0.0f ? p1.extra_down_static_friction + p2.extra_down_static_friction
                                           : p1.extra_up_static_friction + p2.extra_up_static_friction;
        const float c = (p1.static_friction + p2.static_friction) + e * ud;
        const float rr = glm::dot(r, r);
        if ((c * 0.5f) * (c * 0.5f) * (rr - along * along) >= along * along) {
            return ve;
        }
        const float md = 0.5f * (p1.dynamic_friction + p2.dynamic_friction);
        if (md < 1.0f && along * along > 0.001f * rr) {
            along *= (std::sqrt(rr) / std::abs(along)) * (1.0f - md) + md;
        }
        return ve + along * dh;
    }

    // Three planes: the velocity that keeps all three.
    glm::vec3 three(const glm::vec3& v, int a, int b, int c, bool sort) {
        std::array<int, 3> ix{a, b, c};
        const glm::vec3& n1 = planes[a].n;
        const glm::vec3& n2 = planes[b].n;
        const glm::vec3& n3 = planes[c].n;
        const float det = glm::dot(n1, glm::cross(n2, n3));
        glm::vec3 result(0.0f);
        bool degenerate = std::abs(det) < k_eps;
        if (!degenerate) {
            const glm::vec3 rhs(glm::dot(n1, planes[a].velocity), glm::dot(n2, planes[b].velocity),
                                glm::dot(n3, planes[c].velocity));
            if (rhs != glm::vec3(0.0f)) {
                result = (rhs.x * glm::cross(n2, n3) + rhs.y * glm::cross(n3, n1) + rhs.z * glm::cross(n1, n2)) / det;
            }
            degenerate = too_fast(result);
        }
        if (!degenerate) {
            return result;
        }
        if (sort) {
            std::ranges::sort(ix, [&](int x, int y) { return less_priority(x, y); });
        }
        for (int i : ix) {
            out.interactions[i].status = 1;
        }
        glm::vec3 w = two(v, ix[0], ix[1]);
        w = two(w, ix[0], ix[2]);
        return two(w, ix[1], ix[2]);
    }

    bool less_priority(int x, int y) const {
        if (planes[x].priority != planes[y].priority) {
            return planes[x].priority < planes[y].priority;
        }
        return glm::length(planes[x].velocity) < glm::length(planes[y].velocity);
    }

    // The velocity for the active planes, always from the input velocity; drops planes that no longer hold it.
    glm::vec3 solve_active(std::vector<int>& active) {
        const glm::vec3& v = in.velocity;
        switch (active.size()) {
        case 1:
            return one(v, active[0]);
        case 2: {
            const glm::vec3 alone = one(v, active[1]);
            if (!penetrating(alone, active[0])) {
                active.erase(active.begin());
                return alone;
            }
            return two(v, active[0], active[1]);
        }
        case 3: {
            const int a0 = active[0], a1 = active[1], a2 = active[2];
            const glm::vec3 alone = one(v, a2);
            if (!penetrating(alone, a0) && !penetrating(alone, a1)) {
                active = {a2};
                return alone;
            }
            if (!penetrating(two(v, a0, a2), a1)) {
                active = {a0, a2};
                return solve_active(active);
            }
            if (!penetrating(two(v, a1, a2), a0)) {
                active = {a1, a2};
                return solve_active(active);
            }
            return three(v, a0, a1, a2, true);
        }
        default: {
            std::vector<int> sorted(active.begin(), active.end());
            std::ranges::sort(sorted, [&](int x, int y) { return less_priority(x, y); });
            const int newest = active.back();
            std::vector<int> old;
            for (int i : sorted) {
                if (i != newest) {
                    old.push_back(i);
                }
            }
            for (size_t j = 0; j < old.size() && j < 3; ++j) {
                std::array<int, 2> others{};
                size_t k = 0;
                for (size_t o = 0; o < old.size(); ++o) {
                    if (o != j && k < 2) {
                        others[k++] = old[o];
                    }
                }
                const glm::vec3 w = three(v, others[0], others[1], newest, false);
                if (!penetrating(w, old[j])) {
                    active.erase(std::ranges::find(active, old[j]));
                    return solve_active(active);
                }
            }
            // No three of them hold: the plane worked hardest is replaced by the newest.
            int worst = old[0];
            for (int i : old) {
                if (out.interactions[i].status > out.interactions[worst].status) {
                    worst = i;
                }
            }
            active.erase(std::ranges::find(active, worst));
            for (int i : active) {
                out.interactions[i].status = 0;
            }
            return solve_active(active);
        }
        }
    }

    void run() {
        const size_t n = planes.size();
        out.position = glm::vec3(0.0f);
        out.velocity = in.velocity;
        out.delta_time = 0;
        out.interactions.assign(n, {});
        std::vector<int> active;
        float t = 0, remaining = in.delta_time;
        for (int guard = 0; guard < 256; ++guard) {
            int next = -1;
            float time = remaining;
            for (size_t i = 0; i < n; ++i) {
                if (std::ranges::find(active, int(i)) != active.end() || out.interactions[i].status != 0) {
                    continue;
                }
                const Plane& p = planes[i];
                const float a = -glm::dot(p.n, out.velocity - p.velocity);
                if (a <= 0.0f) {
                    continue;
                }
                float d = glm::dot(p.n, out.position - t * p.velocity) + p.d;
                if (d <= k_eps) {
                    d = 0;
                }
                d += out.interactions[i].penalty;
                if (d / a < time) {
                    time = d / a;
                    next = int(i);
                }
            }
            if (time > 1e-4f) {
                out.position += time * out.velocity;
                t += time;
                remaining -= time;
                for (int i : active) {
                    out.interactions[i].touched = true;
                    out.interactions[i].surface_time += time;
                }
                out.delta_time = t;
                if (t > in.min_delta_time) {
                    return;
                }
            }
            if (next < 0) {
                out.delta_time = in.delta_time;
                return;
            }
            active.push_back(next);
            out.interactions[next].penalty = 2.0f * out.interactions[next].penalty + 2.3841858e-7f;
            out.velocity = solve_active(active);
        }
        out.delta_time = t;
    }
};

} // namespace

void solve(const Input& in, std::span<const Plane> planes, Output& out) { Solver{in, planes, out}.run(); }

} // namespace spud::physics::simplex
