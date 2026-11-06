#include "importer/convert/ui_shapes.hpp"

#include <tesselator.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>

namespace spud::importer {

namespace {

using legacy::swf::Edge;

struct Point {
    float x, y;
};

// Quadratic Bezier flattened so no point is further than `tol` from the curve.
void flatten(const Edge& e, float tol, bool reversed, std::vector<Point>& out) {
    const Point p0{float(reversed ? e.x1 : e.x0), float(reversed ? e.y1 : e.y0)};
    const Point p2{float(reversed ? e.x0 : e.x1), float(reversed ? e.y0 : e.y1)};
    if (!e.curved) {
        out.push_back(p2);
        return;
    }
    const Point c{float(e.cx), float(e.cy)};
    // Distance from the control point to the chord midpoint bounds the deviation (it is twice the maximum).
    const float dx = c.x - (p0.x + p2.x) * 0.5f, dy = c.y - (p0.y + p2.y) * 0.5f;
    const float dev = std::sqrt(dx * dx + dy * dy) * 0.5f;
    const int n = std::clamp(int(std::ceil(std::sqrt(dev / std::max(tol, 1e-6f)))), 1, 256);
    for (int i = 1; i <= n; ++i) {
        const float t = float(i) / float(n), u = 1.0f - t;
        out.push_back({u * u * p0.x + 2 * u * t * c.x + t * t * p2.x, u * u * p0.y + 2 * u * t * c.y + t * t * p2.y});
    }
}

struct TessDeleter {
    void operator()(TESStesselator* t) const { tessDeleteTess(t); }
};

TriMesh run_tess(const std::vector<std::vector<Point>>& contours) {
    TriMesh m;
    std::unique_ptr<TESStesselator, TessDeleter> tess(tessNewTess(nullptr));
    bool any = false;
    for (const auto& c : contours) {
        if (c.size() >= 3) {
            tessAddContour(tess.get(), 2, c.data(), sizeof(Point), static_cast<int>(c.size()));
            any = true;
        }
    }
    if (!any || !tessTesselate(tess.get(), TESS_WINDING_NONZERO, TESS_POLYGONS, 3, 2, nullptr)) {
        return m;
    }
    const int nv = tessGetVertexCount(tess.get());
    const TESSreal* v = tessGetVertices(tess.get());
    m.xy.assign(v, v + nv * 2);
    const int ne = tessGetElementCount(tess.get());
    const TESSindex* el = tessGetElements(tess.get());
    for (int i = 0; i < ne * 3; ++i) {
        if (el[i] != TESS_UNDEF) {
            m.indices.push_back(static_cast<uint32_t>(el[i]));
        }
    }
    if (m.indices.size() % 3 != 0) {
        m.indices.resize(m.indices.size() / 3 * 3);
    }
    return m;
}

// The edges of one fill, each oriented so the fill is on its right, linked into closed contours.
std::vector<std::vector<Point>> fill_contours(const std::vector<Edge>& edges, uint32_t style, float tol) {
    struct Directed {
        const Edge* e;
        bool reversed;
        int32_t sx, sy, ex, ey;
    };
    std::vector<Directed> list;
    for (const auto& e : edges) {
        if (e.fill1 == style) {
            list.push_back({&e, false, e.x0, e.y0, e.x1, e.y1});
        }
        if (e.fill0 == style) {
            list.push_back({&e, true, e.x1, e.y1, e.x0, e.y0});
        }
    }
    std::multimap<std::pair<int32_t, int32_t>, size_t> by_start;
    for (size_t i = 0; i < list.size(); ++i) {
        by_start.emplace(std::pair{list[i].sx, list[i].sy}, i);
    }
    std::vector<bool> used(list.size(), false);
    std::vector<std::vector<Point>> out;
    for (size_t i = 0; i < list.size(); ++i) {
        if (used[i]) {
            continue;
        }
        std::vector<Point> contour{{float(list[i].sx), float(list[i].sy)}};
        size_t cur = i;
        for (;;) {
            used[cur] = true;
            flatten(*list[cur].e, tol, list[cur].reversed, contour);
            const auto key = std::pair{list[cur].ex, list[cur].ey};
            if (key == std::pair{list[i].sx, list[i].sy}) {
                break;
            }
            size_t next = SIZE_MAX;
            auto [b, e] = by_start.equal_range(key);
            for (auto it = b; it != e; ++it) {
                if (!used[it->second]) {
                    next = it->second;
                    break;
                }
            }
            if (next == SIZE_MAX) {
                break; // open chain: closed by the tessellator's implicit last edge
            }
            cur = next;
        }
        if (contour.size() >= 2 && contour.front().x == contour.back().x && contour.front().y == contour.back().y) {
            contour.pop_back();
        }
        out.push_back(std::move(contour));
    }
    return out;
}

float signed_area(const std::vector<Point>& c) {
    float a = 0;
    for (size_t i = 0, j = c.size() - 1; i < c.size(); j = i++) {
        a += (c[j].x * c[i].y - c[i].x * c[j].y);
    }
    return a * 0.5f;
}

void add_oriented(std::vector<std::vector<Point>>& out, std::vector<Point> c) {
    if (c.size() < 3) {
        return;
    }
    if (signed_area(c) < 0) {
        std::reverse(c.begin(), c.end());
    }
    out.push_back(std::move(c));
}

std::vector<Point> circle(Point p, float r, float tol) {
    const float step = 2.0f * std::acos(std::clamp(1.0f - tol / std::max(r, tol), -1.0f, 1.0f));
    const int n = std::clamp(int(std::ceil(2.0f * std::numbers::pi_v<float> / std::max(step, 1e-3f))), 8, 128);
    std::vector<Point> c;
    for (int i = 0; i < n; ++i) {
        const float a = 2.0f * std::numbers::pi_v<float> * float(i) / float(n);
        c.push_back({p.x + r * std::cos(a), p.y + r * std::sin(a)});
    }
    return c;
}

// A stroke as the union of per-segment rectangles and round joins/caps (all oriented the same way, so the
// non-zero rule unions them).
TriMesh stroke(const std::vector<Edge>& edges, uint32_t style, float width, float tol) {
    const float r = std::max(width, 20.0f) * 0.5f; // hairlines: one pixel (20 twips)
    std::vector<std::vector<Point>> polys;
    for (const auto& e : edges) {
        if (e.line != style) {
            continue;
        }
        std::vector<Point> pts{{float(e.x0), float(e.y0)}};
        flatten(e, tol, false, pts);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const Point a = pts[i], b = pts[i + 1];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-4f) {
                continue;
            }
            const float nx = -dy / len * r, ny = dx / len * r;
            add_oriented(polys, {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}});
        }
        for (const auto& p : pts) {
            add_oriented(polys, circle(p, r, tol));
        }
    }
    return run_tess(polys);
}

} // namespace

ShapeMeshes tessellate_shape(const legacy::swf::Shape& shape, float tolerance) {
    ShapeMeshes out;
    const size_t groups = std::max(shape.group_fill_start.size(), size_t(1));
    for (size_t g = 0; g < groups; ++g) {
        const uint32_t f0 = g < shape.group_fill_start.size() ? shape.group_fill_start[g] : 0;
        const uint32_t f1 = g + 1 < shape.group_fill_start.size() ? shape.group_fill_start[g + 1]
                                                                  : static_cast<uint32_t>(shape.fills.size());
        for (uint32_t f = f0; f < f1; ++f) {
            TriMesh m = run_tess(fill_contours(shape.edges, f + 1, tolerance));
            if (!m.indices.empty()) {
                out.parts.push_back({false, f, std::move(m)});
            }
        }
        const uint32_t l0 = g < shape.group_line_start.size() ? shape.group_line_start[g] : 0;
        const uint32_t l1 = g + 1 < shape.group_line_start.size() ? shape.group_line_start[g + 1]
                                                                  : static_cast<uint32_t>(shape.lines.size());
        for (uint32_t l = l0; l < l1; ++l) {
            TriMesh m = stroke(shape.edges, l + 1, shape.lines[l].width, tolerance);
            if (!m.indices.empty()) {
                out.parts.push_back({true, l, std::move(m)});
            }
        }
    }
    return out;
}

} // namespace spud::importer
