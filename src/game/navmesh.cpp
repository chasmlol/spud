#include "game/navmesh.hpp"

#include <chrono>
#include <future>
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_set>

namespace spud::game {

namespace {

constexpr float k_cell_units = 4096.0f;
constexpr size_t k_search_limit = 60000; // triangles looked at before a search gives up
// A navmesh triangle's flags: edge i leads to another navmesh through edge_links.
constexpr uint16_t k_edge_link[3] = {0x1, 0x2, 0x4};

uint64_t exterior_key(uint32_t world, int32_t x, int32_t y) {
    return uint64_t(world) << 32 | uint64_t(uint16_t(int16_t(x))) << 16 | uint64_t(uint16_t(int16_t(y)));
}

float dist(const NavPoint& a, const NavPoint& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

NavPoint vertex(const forms::NavmeshGeometry& g, uint16_t v) { return g.vertices[v]; }

NavPoint midpoint(const NavPoint& a, const NavPoint& b) {
    return {(a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f};
}

// Twice the signed area of the triangle in plan view.
float area(const NavPoint& a, const NavPoint& b, const NavPoint& c) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
}

// The height of the plane through a triangle at (x, y), if the point is inside it in plan view (with a margin).
std::optional<float> height_in(const NavPoint& a, const NavPoint& b, const NavPoint& c, float x, float y, float margin) {
    const float d = area(a, b, c);
    if (std::abs(d) < 1e-4f) {
        return std::nullopt;
    }
    const NavPoint p{x, y, 0};
    const float u = area(b, c, p) / d, v = area(c, a, p) / d, w = 1.0f - u - v;
    if (u < -margin || v < -margin || w < -margin) {
        return std::nullopt;
    }
    return u * a[2] + v * b[2] + w * c[2];
}

// The point of a triangle nearest (x, y) in plan view.
NavPoint nearest_on(const NavPoint& a, const NavPoint& b, const NavPoint& c, float x, float y) {
    NavPoint best = a;
    float best_d = 1e30f;
    const NavPoint* corners[3] = {&a, &b, &c};
    for (int i = 0; i < 3; ++i) {
        const NavPoint &p = *corners[i], &q = *corners[(i + 1) % 3];
        const float ex = q[0] - p[0], ey = q[1] - p[1];
        const float len = ex * ex + ey * ey;
        const float t = len > 0 ? std::clamp(((x - p[0]) * ex + (y - p[1]) * ey) / len, 0.0f, 1.0f) : 0.0f;
        const NavPoint s{p[0] + ex * t, p[1] + ey * t, p[2] + (q[2] - p[2]) * t};
        const float d = (s[0] - x) * (s[0] - x) + (s[1] - y) * (s[1] - y);
        if (d < best_d) {
            best_d = d;
            best = s;
        }
    }
    return best;
}

} // namespace

NavMeshes::NavMeshes(const forms::Database& db) : m_db(&db) {
    const forms::Sig navm = forms::parse_sig("NAVM").value();
    const forms::Sig cell_sig = forms::parse_sig("CELL").value();
    for (const forms::FormEntry* e : db.of_type(navm)) {
        const forms::FormEntry* cell = db.find(e->parent);
        if (!cell || cell->type != cell_sig) {
            continue;
        }
        if (cell->parent) { // exterior: the cell's grid
            const forms::Form c(db, *cell);
            const forms::Value* x = c.get("XCLC.x");
            const forms::Value* y = c.get("XCLC.y");
            if (x && y) {
                m_exterior[exterior_key(cell->parent, int32_t(x->integer), int32_t(y->integer))].push_back(e->form_id);
            }
        } else {
            m_interior[e->parent].push_back(e->form_id);
        }
    }
}

bool NavMeshes::middle(uint32_t cell, NavPoint& out) {
    const auto it = m_interior.find(cell);
    if (it == m_interior.end() || it->second.empty()) {
        return false;
    }
    const Mesh* m = mesh(it->second.front());
    if (!m || m->geometry.vertices.empty()) {
        return false;
    }
    NavPoint lo = m->geometry.vertices[0], hi = lo;
    for (const NavPoint& v : m->geometry.vertices) {
        for (int i = 0; i < 3; ++i) {
            lo[size_t(i)] = std::min(lo[size_t(i)], v[size_t(i)]);
            hi[size_t(i)] = std::max(hi[size_t(i)], v[size_t(i)]);
        }
    }
    const NavPoint centre = midpoint(lo, hi);
    out = *std::min_element(m->geometry.vertices.begin(), m->geometry.vertices.end(),
                            [&](const NavPoint& a, const NavPoint& b) { return dist(a, centre) < dist(b, centre); });
    return true;
}

const NavMeshes::Mesh* NavMeshes::mesh(uint32_t form) {
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_meshes.find(form); it != m_meshes.end()) {
            return it->second.get();
        }
    }
    // Decoded outside the lock: a worker warming the area must never hold up a tick asking for a mesh it already has.
    std::unique_ptr<Mesh> out;
    if (const forms::FormEntry* e = m_db->find(form)) {
        if (auto g = forms::navmesh(forms::Form(*m_db, *e))) {
            out = std::make_unique<Mesh>();
            out->geometry = std::move(*g);
            const forms::NavmeshGeometry& geometry = out->geometry;
            out->boxes.reserve(geometry.triangles.size());
            out->bounds = {1e30f, 1e30f, -1e30f, -1e30f};
            for (const auto& t : geometry.triangles) {
                std::array<float, 4> box{1e30f, 1e30f, -1e30f, -1e30f};
                for (const uint16_t v : t.vertices) {
                    const NavPoint p = vertex(geometry, v);
                    box = {std::min(box[0], p[0]), std::min(box[1], p[1]), std::max(box[2], p[0]), std::max(box[3], p[1])};
                }
                out->boxes.push_back(box);
                out->bounds = {std::min(out->bounds[0], box[0]), std::min(out->bounds[1], box[1]),
                               std::max(out->bounds[2], box[2]), std::max(out->bounds[3], box[3])};
            }
        }
    }
    std::lock_guard lock(m_mutex);
    return m_meshes.emplace(form, std::move(out)).first->second.get();
}

void NavMeshes::warm(uint32_t space, const NavPoint& point, int32_t cells) {
    if (m_warming.valid() && m_warming.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return; // the next cell asks again
    }
    std::vector<uint32_t> forms;
    if (const auto it = m_interior.find(space); it != m_interior.end()) {
        forms = it->second;
    } else {
        const auto cx = int32_t(std::floor(point[0] / k_cell_units)), cy = int32_t(std::floor(point[1] / k_cell_units));
        for (int32_t dy = -cells; dy <= cells; ++dy) {
            for (int32_t dx = -cells; dx <= cells; ++dx) {
                if (const auto it = m_exterior.find(exterior_key(space, cx + dx, cy + dy)); it != m_exterior.end()) {
                    forms.insert(forms.end(), it->second.begin(), it->second.end());
                }
            }
        }
    }
    m_warming = std::async(std::launch::async, [this, forms = std::move(forms)] {
        for (const uint32_t form : forms) {
            mesh(form);
        }
    });
}

std::optional<NavSpot> NavMeshes::locate(uint32_t space, const NavPoint& point, float reach) {
    std::vector<uint32_t> candidates;
    if (const auto it = m_interior.find(space); it != m_interior.end()) {
        candidates = it->second;
    } else {
        const auto cx = int32_t(std::floor(point[0] / k_cell_units)), cy = int32_t(std::floor(point[1] / k_cell_units));
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (const auto it = m_exterior.find(exterior_key(space, cx + dx, cy + dy)); it != m_exterior.end()) {
                    candidates.insert(candidates.end(), it->second.begin(), it->second.end());
                }
            }
        }
    }
    NavSpot best;
    float best_d = reach;
    for (const uint32_t form : candidates) {
        const Mesh* m = mesh(form);
        if (!m) {
            continue;
        }
        // A point on a triangle is at least as far as its box in plan view: what can't beat the best so far isn't looked at.
        const auto too_far = [&](const std::array<float, 4>& box) {
            const float dx = std::max({box[0] - point[0], point[0] - box[2], 0.0f});
            const float dy = std::max({box[1] - point[1], point[1] - box[3], 0.0f});
            return dx * dx + dy * dy >= best_d * best_d;
        };
        if (too_far(m->bounds)) {
            continue;
        }
        const forms::NavmeshGeometry& g = m->geometry;
        for (size_t t = 0; t < g.triangles.size(); ++t) {
            if (too_far(m->boxes[t])) {
                continue;
            }
            const auto& v = g.triangles[t].vertices;
            const NavPoint a = vertex(g, v[0]), b = vertex(g, v[1]), c = vertex(g, v[2]);
            NavPoint on;
            if (const auto h = height_in(a, b, c, point[0], point[1], 0.0f)) {
                on = {point[0], point[1], *h};
            } else {
                on = nearest_on(a, b, c, point[0], point[1]);
            }
            const float d = dist(on, point);
            if (d < best_d) {
                best_d = d;
                best = {form, int32_t(t), on};
            }
        }
    }
    return best.valid() ? std::optional<NavSpot>(best) : std::nullopt;
}

NavMeshes::Link NavMeshes::neighbour(const Mesh& m, uint32_t mesh_form, int32_t triangle, int edge) const {
    const forms::NavmeshTriangle& t = m.geometry.triangles[size_t(triangle)];
    const int16_t e = t.edges[size_t(edge)];
    if (e < 0) {
        return {};
    }
    if (t.flags & k_edge_link[edge]) {
        if (size_t(e) >= m.geometry.edge_links.size()) {
            return {};
        }
        const forms::NavmeshEdgeLink& l = m.geometry.edge_links[size_t(e)];
        return {l.navmesh, l.triangle};
    }
    return {mesh_form, e};
}

bool NavMeshes::walk(NavSpot& spot, float x, float y) {
    NavSpot at = spot;
    for (int step = 0; step < 32; ++step) {
        const Mesh* m = mesh(at.mesh);
        if (!m || at.triangle < 0 || size_t(at.triangle) >= m->geometry.triangles.size()) {
            return false;
        }
        const forms::NavmeshGeometry& g = m->geometry;
        const auto& v = g.triangles[size_t(at.triangle)].vertices;
        const NavPoint p[3] = {vertex(g, v[0]), vertex(g, v[1]), vertex(g, v[2])};
        if (const auto h = height_in(p[0], p[1], p[2], x, y, 0.01f)) {
            spot = {at.mesh, at.triangle, {x, y, *h}};
            return true;
        }
        // Over the edge the point lies beyond.
        const float winding = area(p[0], p[1], p[2]) > 0 ? 1.0f : -1.0f;
        const NavPoint q{x, y, 0};
        Link next;
        for (int e = 0; e < 3 && next.triangle < 0; ++e) {
            if (area(p[e], p[(e + 1) % 3], q) * winding < 0) {
                next = neighbour(*m, at.mesh, at.triangle, e);
            }
        }
        if (next.triangle < 0) {
            return false;
        }
        at.mesh = next.mesh;
        at.triangle = next.triangle;
    }
    return false;
}

std::vector<NavPoint> NavMeshes::path(const NavSpot& from, const NavSpot& to) {
    struct Node {
        uint32_t mesh;
        int32_t triangle;
        int32_t parent;     // index into `nodes`
        int8_t via;         // the parent's edge that led here
        NavPoint entry;     // where the search came in
        float cost;
    };
    std::vector<Node> nodes;
    nodes.reserve(1024);
    const auto key = [](uint32_t mesh_form, int32_t triangle) { return uint64_t(mesh_form) << 32 | uint32_t(triangle); };
    std::unordered_map<uint64_t, float> best;
    using Open = std::pair<float, int32_t>;
    std::priority_queue<Open, std::vector<Open>, std::greater<>> open;
    nodes.push_back({from.mesh, from.triangle, -1, -1, from.position, 0});
    best[key(from.mesh, from.triangle)] = 0;
    open.push({dist(from.position, to.position), 0});
    int32_t goal = -1;
    while (!open.empty() && nodes.size() < k_search_limit) {
        const int32_t i = open.top().second;
        open.pop();
        const Node n = nodes[size_t(i)];
        if (n.mesh == to.mesh && n.triangle == to.triangle) {
            goal = i;
            break;
        }
        const Mesh* m = mesh(n.mesh);
        if (!m || size_t(n.triangle) >= m->geometry.triangles.size()) {
            continue;
        }
        const forms::NavmeshGeometry& g = m->geometry;
        const auto& v = g.triangles[size_t(n.triangle)].vertices;
        for (int e = 0; e < 3; ++e) {
            const Link l = neighbour(*m, n.mesh, n.triangle, e);
            if (l.triangle < 0) {
                continue;
            }
            const NavPoint a = vertex(g, v[size_t(e)]), b = vertex(g, v[size_t((e + 1) % 3)]);
            const NavPoint mid = midpoint(a, b);
            const float cost = n.cost + dist(n.entry, mid);
            const auto [it, fresh] = best.try_emplace(key(l.mesh, l.triangle), cost);
            if (!fresh) {
                if (it->second <= cost) {
                    continue;
                }
                it->second = cost;
            }
            nodes.push_back({l.mesh, l.triangle, i, int8_t(e), mid, cost});
            open.push({cost + dist(mid, to.position), int32_t(nodes.size() - 1)});
        }
    }
    if (goal < 0) {
        return {};
    }
    // The corridor's portals, in order: each the left and right ends of the edge crossed.
    std::vector<std::pair<NavPoint, NavPoint>> portals;
    for (int32_t i = goal; nodes[size_t(i)].parent >= 0; i = nodes[size_t(i)].parent) {
        const Node& n = nodes[size_t(i)];
        const Node& p = nodes[size_t(n.parent)];
        const Mesh* m = mesh(p.mesh);
        const forms::NavmeshGeometry& g = m->geometry;
        const auto& v = g.triangles[size_t(p.triangle)].vertices;
        NavPoint a = vertex(g, v[size_t(n.via)]), b = vertex(g, v[size_t((n.via + 1) % 3)]);
        const NavPoint c = vertex(g, v[size_t((n.via + 2) % 3)]);
        if (area(a, b, c) < 0) { // the triangle's own winding says which end is left of the way out
            std::swap(a, b);
        }
        portals.emplace_back(b, a);
    }
    std::reverse(portals.begin(), portals.end());
    portals.emplace_back(to.position, to.position);
    // The funnel: the shortest way through the portals.
    std::vector<NavPoint> out{from.position};
    NavPoint apex = from.position, left = from.position, right = from.position;
    size_t left_at = 0, right_at = 0;
    for (size_t i = 0; i < portals.size(); ++i) {
        const auto& [l, r] = portals[i];
        if (area(apex, right, r) >= 0) {
            if (apex == right || area(apex, left, r) < 0) {
                right = r;
                right_at = i;
            } else {
                out.push_back(left);
                apex = left;
                right = apex;
                i = left_at;
                left_at = right_at = i;
                continue;
            }
        }
        if (area(apex, left, l) <= 0) {
            if (apex == left || area(apex, right, l) > 0) {
                left = l;
                left_at = i;
            } else {
                out.push_back(right);
                apex = right;
                left = apex;
                i = right_at;
                left_at = right_at = i;
                continue;
            }
        }
    }
    out.push_back(to.position);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace spud::game
