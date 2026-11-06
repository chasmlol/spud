#include "render/shadows.hpp"

#include "core/units.hpp"

#include <algorithm>
#include <cmath>

namespace spud::render {

namespace {

using dvec3 = glm::dvec3;

// The game's constants: the light camera stands this far sunwards of the
// camera with an orthographic frustum from 10 to 15000 units, and the fitted near and far planes take the
// corners' projected depth times 14980, widened by 20 units.
constexpr double k_light_back = 15000.0;
constexpr double k_light_near = 10.0;
constexpr double k_depth_scale = 14980.0;
constexpr double k_depth_margin = 20.0;
// The sun light's depth bias (the game constructor: 60) and the two factors the mask setup turns it
// into: x 0.00025 x fShadowDirectionalBiasScale for the first cascade, x 1/12000 for the others.
constexpr double k_sun_bias = 60.0;

// The caster cull's side planes (the game's culling-volume setup): the edges of the 2D
// convex hull of the slice's corners in light x, y, keeping at most 5, the longest.
uint32_t hull_planes(const glm::dvec2* pts, int count, glm::vec3* out) {
    int order[8];
    for (int i = 0; i < count; ++i) {
        order[i] = i;
    }
    std::sort(order, order + count, [&](int a, int b) {
        return pts[a].x != pts[b].x ? pts[a].x < pts[b].x : pts[a].y < pts[b].y;
    });
    auto cross = [](glm::dvec2 o, glm::dvec2 a, glm::dvec2 b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    glm::dvec2 hull[16];
    int k = 0;
    for (int i = 0; i < count; ++i) { // lower chain, then upper: counter-clockwise
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[order[i]]) <= 0.0) {
            --k;
        }
        hull[k++] = pts[order[i]];
    }
    for (int i = count - 2, t = k + 1; i >= 0; --i) {
        while (k >= t && cross(hull[k - 2], hull[k - 1], pts[order[i]]) <= 0.0) {
            --k;
        }
        hull[k++] = pts[order[i]];
    }
    const int edges = k - 1;
    struct Edge {
        glm::vec3 plane;
        double length;
    };
    Edge e[16];
    for (int i = 0; i < edges; ++i) {
        const glm::dvec2 a = hull[i], b = hull[i + 1];
        const glm::dvec2 d = b - a;
        const double l = glm::length(d);
        const glm::dvec2 n = l > 0.0 ? glm::dvec2(-d.y, d.x) / l : glm::dvec2(0.0); // inward for counter-clockwise
        e[i] = {glm::vec3(float(n.x), float(n.y), float(-glm::dot(n, a))), l};
    }
    std::stable_sort(e, e + edges, [](const Edge& a, const Edge& b) { return a.length > b.length; });
    const int kept = std::min(edges, 5);
    for (int i = 0; i < kept; ++i) {
        out[i] = e[i].plane;
    }
    return static_cast<uint32_t>(kept);
}

} // namespace

bool ShadowCascade::sphere(const glm::vec3& centre, float radius) const {
    const glm::vec3 c = centre / units::k_skyrim_unit_m;
    const float r = radius / units::k_skyrim_unit_m;
    const glm::vec3 p = glm::vec3(glm::dot(axis_x, c), glm::dot(axis_y, c), glm::dot(axis_z, c) + float(k_light_back)) -
                        shift;
    // No near plane: the caster pass flattens whatever lies sunwards of the map onto it (shadow.vert).
    if (p.x + r < lo.x || p.x - r > hi.x || p.y + r < lo.y || p.y - r > hi.y || p.z - r > hi.z) {
        return false;
    }
    for (uint32_t i = 0; i < side_count; ++i) {
        if (sides[i].x * p.x + sides[i].y * p.y + sides[i].z < -r) {
            return false;
        }
    }
    return true;
}

bool spot_light_visible(const View& view, const glm::vec3& position, const glm::vec4& rotation, float radius,
                        float fov) {
    const dvec3 u(rotation.x, rotation.y, rotation.z);
    auto rotated = [&](dvec3 v) {
        const dvec3 t = 2.0 * glm::cross(u, v);
        return v + double(rotation.w) * t + glm::cross(u, t);
    };
    const dvec3 dir = rotated({1, 0, 0}), up = rotated({0, 1, 0}), right = rotated({0, 0, 1});
    const dvec3 apex(position);
    const double t = std::tan(std::clamp(double(fov) * 0.5, 0.017453292, 2.792527));
    const double r = radius;
    // The spot's corners against the view's planes.
    const dvec3 spot[5] = {apex, apex + (dir + (right + up) * t) * r, apex + (dir + (right - up) * t) * r,
                           apex + (dir + (up - right) * t) * r, apex + (dir - (right + up) * t) * r};
    for (const glm::vec4& p : view.frustum.planes) {
        bool outside = true;
        for (const dvec3& c : spot) {
            outside = outside && glm::dot(dvec3(p), c) + double(p.w) < 0.0;
        }
        if (outside) {
            return false;
        }
    }
    // The view's corners against the spot's planes, the far ones at the game's far plane.
    const dvec3 vr(view.rotation[0][0], view.rotation[1][0], view.rotation[2][0]);
    const dvec3 vu(view.rotation[0][1], view.rotation[1][1], view.rotation[2][1]);
    const dvec3 vf = -dvec3(view.rotation[0][2], view.rotation[1][2], view.rotation[2][2]);
    const double ty = std::tan(double(view.fov_y) * 0.5), tx = ty * double(view.aspect);
    const double far_m = 353840.0 * units::k_skyrim_unit_m;
    const dvec3 eye[5] = {dvec3(0.0), (vf + vr * tx + vu * ty) * far_m, (vf + vr * tx - vu * ty) * far_m,
                          (vf - vr * tx + vu * ty) * far_m, (vf - vr * tx - vu * ty) * far_m};
    const dvec3 normals[4] = {dir * t - right, dir * t + right, dir * t - up, dir * t + up};
    for (const dvec3& n : normals) {
        bool outside = true;
        for (const dvec3& c : eye) {
            outside = outside && glm::dot(n, c - apex) < 0.0;
        }
        if (outside) {
            return false;
        }
    }
    bool beyond = true;
    for (const dvec3& c : eye) {
        beyond = beyond && glm::dot(dir, c - apex) > r;
    }
    return !beyond;
}

ShadowLight compute_shadow_light(const glm::vec3& position, const glm::vec4& rotation, float radius, float fov,
                                 float falloff, float near_clip, float bias, bool fades, float distance) {
    const double k = units::k_skyrim_unit_m;
    ShadowLight out;
    // The light's camera axes: Gamebryo cameras look along local X, up local Y, right local Z.
    const dvec3 u(rotation.x, rotation.y, rotation.z);
    const double w = rotation.w;
    auto rotated = [&](dvec3 v) {
        const dvec3 t = 2.0 * glm::cross(u, v);
        return v + w * t + glm::cross(u, t);
    };
    const dvec3 dir = rotated({1, 0, 0}), up = rotated({0, 1, 0}), right = rotated({0, 0, 1});
    const dvec3 p = dvec3(position) / k; // game units
    const double r = double(radius) / k;
    // A row of a point in metres: (a . (P / k - p)) s + o.
    auto row = [&](const dvec3& a, double s, double o) {
        return glm::vec4(glm::vec3(a * (s / k)), float(-glm::dot(a, p) * s + o));
    };
    auto matrix = [](const glm::vec4& a, const glm::vec4& b, const glm::vec4& c, const glm::vec4& d) {
        return glm::transpose(glm::mat4(a, b, c, d));
    };
    // below pi a frustum, below 2 pi one paraboloid, else two.
    if (fov < 3.1415927f) {
        out.type = ShadowLightType::spot;
        const double t = std::tan(std::clamp(double(fov) * 0.5, 0.017453292, 2.792527));
        const double n = double(near_clip) / k;
        const double a = r / (r - n); // the far plane is the radius
        const glm::vec4 x = row(right, 1.0 / t, 0.0), y = row(up, 1.0 / t, 0.0);
        const glm::vec4 z = row(dir, a, -a * n), ww = row(dir, 1.0, 0.0);
        out.rows[0] = x, out.rows[1] = -y, out.rows[2] = z, out.rows[3] = ww;
        out.render[0] = matrix(x, -y, z, ww);
        out.param = std::max(falloff, 1.0f);
    } else {
        out.type = fov < 6.2831855f ? ShadowLightType::hemisphere : ShadowLightType::omni;
        // The camera's orthographic frustum, near 0.1 and far 1: depth (d - 0.1) / 0.9 before the paraboloid.
        const glm::vec4 x = row(right, 1.0, 0.0), y = row(up, 1.0, 0.0);
        const glm::vec4 z = row(dir, 1.0 / 0.9, -0.1 / 0.9), ww(0.0f, 0.0f, 0.0f, 1.0f);
        out.rows[0] = x, out.rows[1] = -y, out.rows[2] = z, out.rows[3] = ww;
        out.render[0] = matrix(x, y, z, ww);
        out.render[1] = matrix(x, -y, z, ww);
        out.param = float(r);
    }
    out.bias = bias * 0.00025f; // the mask's ShadowSampleParam.y
    // Lights that never fade take a fixed 10000 units.
    out.fade_distance = float((fades ? double(distance) : 10000.0) * k);
    return out;
}

glm::vec3 SunShadowDirection::update(const glm::vec3& to_light, const glm::vec3& camera_position, float dt,
                                     float update_scale) {
    const bool moved = glm::length(camera_position - camera) / units::k_skyrim_unit_m > 0.01f;
    camera = camera_position;
    if (!valid || moved || wait > 1.0f) {
        direction = to_light;
        wait = 0.0f;
        valid = true;
    } else {
        wait += dt / std::max(update_scale, 0.000001f);
    }
    return direction;
}

SunShadows compute_sun_shadows(const View& view, const glm::vec3& to_light, float near_units, float far_units,
                               const ShadowSettings& settings, bool interior) {
    SunShadows out;
    const double len = glm::length(dvec3(to_light));
    if (!settings.enabled || settings.splits == 0 || len < 1e-6) {
        return out;
    }
    const uint32_t splits = std::min(settings.splits, k_max_cascades);
    const double k = units::k_skyrim_unit_m;

    // Light axes: the travel direction L, x = normalize(-Lz, 0, Lx) (the light's rotation built against the world Y
    // axis), y = x cross L.
    const dvec3 L = -dvec3(to_light) / len;
    dvec3 ax(-L.z, 0.0, L.x);
    const double ax_len = glm::length(ax);
    ax = ax_len > 1e-6 ? ax / ax_len : dvec3(0.0);
    const dvec3 ay = glm::cross(ax, L);

    // Every slice is fitted around the camera rounded down to whole units along the light camera's axes, -x, -y and L,
    // so that it stands up to a unit off the real one. In floats and in this order, as the rounding sees them.
    const glm::vec3 P = view.position / units::k_skyrim_unit_m;
    auto rounded = [&](const glm::vec3& a) { return double(std::floor((a.y * P.y + a.x * P.x) + a.z * P.z)); };
    const dvec3 fit = -ax * rounded(glm::vec3(-ax)) - ay * rounded(glm::vec3(-ay)) + L * rounded(glm::vec3(L));
    const dvec3 offset = fit - dvec3(P);
    const dvec3 shift(glm::dot(ax, offset), glm::dot(ay, offset), glm::dot(L, offset));

    // The camera: its axes and the frustum's half extents at unit distance.
    const dvec3 right(view.rotation[0][0], view.rotation[1][0], view.rotation[2][0]);
    const dvec3 up(view.rotation[0][1], view.rotation[1][1], view.rotation[2][1]);
    const dvec3 forward = -dvec3(view.rotation[0][2], view.rotation[1][2], view.rotation[2][2]);
    const double ty = std::tan(double(view.fov_y) * 0.5);
    const double tx = ty * double(view.aspect);

    // Split distances (game units, view depth): the first slice is min(D / N, max(fFirstSliceDistance, 500)) of the
    // INI shadow distance D, the rest share what's left evenly; each split is the mean of the uniform and the
    // logarithmic split between the near plane and the current shadow distance, plus fSplitOverlap. A cascade starts
    // two overlaps before the previous one ends.
    const double n = near_units;
    const double current = interior ? settings.interior_distance : settings.distance;
    const double f = std::min(current, double(far_units));
    const double D = settings.distance;
    double t[k_max_cascades] = {};
    t[0] = std::min(D / splits, std::max(double(settings.first_slice), 500.0)) / D;
    for (uint32_t i = 1; i < splits; ++i) {
        t[i] = (1.0 - t[0]) * double(i) * (1.0 / double(splits - 1)) + t[0];
    }
    double ends[k_max_cascades] = {};
    for (uint32_t i = 0; i < splits; ++i) {
        ends[i] = ((f - n) * t[i] + n + std::pow(f / n, t[i]) * n) * 0.5 + settings.split_overlap;
    }

    auto corners = [&](double d, dvec3* c) {
        int i = 0;
        for (double sx : {-tx, tx}) {
            for (double sy : {ty, -ty}) {
                c[i++] = forward * d + right * (sx * d) + up * (sy * d);
            }
        }
    };
    dvec3 pts[8];
    corners(n, pts); // the first slice starts at the near plane, each later one at the previous slice's far corners
    for (uint32_t i = 0; i < splits; ++i) {
        corners(ends[i], pts + 4);
        // Project the slice's corners into the light camera (orthographic, 1 unit per screen unit).
        double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30, z0 = 1e30, z1 = -1e30;
        glm::dvec2 xy[8];
        int corner = 0;
        for (const dvec3& p : pts) {
            const double x = glm::dot(ax, p), y = glm::dot(ay, p);
            const double z = (glm::dot(L, p) + k_light_back - k_light_near) / (k_light_back - k_light_near);
            x0 = std::min(x0, x), x1 = std::max(x1, x);
            y0 = std::min(y0, y), y1 = std::max(y1, y);
            z0 = std::min(z0, z), z1 = std::max(z1, z);
            xy[corner++] = glm::dvec2(x, y);
        }
        const double near_d = z0 * k_depth_scale - k_depth_margin;
        const double far_d = z1 * k_depth_scale + k_depth_margin;
        for (int c = 0; c < 4; ++c) {
            pts[c] = pts[c + 4];
        }

        ShadowCascade& sc = out.cascades[i];
        // u = (x - x0) / (x1 - x0), v = 1 - (y - y0) / (y1 - y0), depth = (L.P + 15000 - near) / (far - near), with
        // P in units from the rounded camera; stored per metre of camera-relative position.
        const double sx = 1.0 / (x1 - x0), sy = 1.0 / (y1 - y0), sz = 1.0 / (far_d - near_d);
        const dvec3 r0 = ax * sx, r1 = -ay * sy, r2 = L * sz;
        const double w0 = -(x0 + shift.x) * sx, w1 = 1.0 + (y0 + shift.y) * sy;
        const double w2 = (k_light_back - near_d - shift.z) * sz;
        sc.rows[0] = glm::vec4(glm::vec3(r0 / k), float(w0));
        sc.rows[1] = glm::vec4(glm::vec3(r1 / k), float(w1));
        sc.rows[2] = glm::vec4(glm::vec3(r2 / k), float(w2));
        glm::mat4 m(0.0f);
        for (int c = 0; c < 3; ++c) {
            m[c][0] = float(2.0 * r0[c] / k);
            m[c][1] = float(2.0 * r1[c] / k);
            m[c][2] = float(r2[c] / k);
        }
        m[3] = glm::vec4(float(2.0 * w0 - 1.0), float(2.0 * w1 - 1.0), float(w2), 1.0f);
        sc.clip = m;
        sc.bias = float(i == 0 ? k_sun_bias * 0.00025 * settings.directional_bias_scale : k_sun_bias / 12000.0);
        sc.end_z = float(ends[i] * k);
        sc.axis_x = glm::vec3(ax);
        sc.axis_y = glm::vec3(ay);
        sc.axis_z = glm::vec3(L);
        sc.lo = glm::vec3(float(x0), float(y0), float(near_d));
        sc.hi = glm::vec3(float(x1), float(y1), float(far_d));
        sc.shift = glm::vec3(shift);
        sc.side_count = hull_planes(xy, 8, sc.sides);
    }
    out.count = splits;
    out.blend_start_z = splits > 1 ? float((ends[0] - 2.0 * settings.split_overlap) * k) : 1e30f;
    out.fade_distance = float(current * k);
    out.filter_radius = settings.poisson_radius / float(std::max(settings.resolution, 1u));
    return out;
}

} // namespace spud::render
