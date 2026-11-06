#include "engine/space_rooms.hpp"

#include "core/units.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace spud::engine {

namespace {

glm::mat3 rotation_of(const float q[4]) {
    return glm::mat3_cast(glm::normalize(glm::quat(q[3], q[0], q[1], q[2])));
}

// The camera's culling point is its position pushed forward by the frustum's near distance
// the game's near plane is 15 units.
constexpr float k_near = 15.0f * units::k_skyrim_unit_m;
// Within this distance of a portal's plane the eye sees through it unrestricted.
constexpr float k_portal_slack = 5.0f * units::k_skyrim_unit_m;
// A parent room's terms are carried through a portal only while they are this short (the game ops).
constexpr uint32_t k_max_carried_planes = 25;
// How many occluders a frame tests against, and the smallest worth one: per-reference cost, not part of the rules.
constexpr size_t k_max_occluders = 16; // a bit each in a cell's mask
constexpr float k_min_occluder_size = 0.001f;

} // namespace

Occluders::Occluders(const osw::World& w) {
    for (const auto& o : w.occluders) {
        const glm::vec3 c(o.centre[0], o.centre[1], o.centre[2]);
        const glm::mat3 m = rotation_of(o.rotation);
        const glm::vec3 h(o.half_extents[0], o.half_extents[1], o.half_extents[2]);
        auto quad = [&](const glm::vec3& at, const glm::vec3& x, const glm::vec3& y, const glm::vec3& z, bool face) {
            if (glm::length(x) > 0.0f && glm::length(z) > 0.0f) {
                m_quads.push_back({at, x, y, z, std::sqrt(glm::dot(x, x) + glm::dot(z, z)), face});
            }
        };
        if (o.type == osw::OccluderType::plane) {
            quad(c, m[0] * h.x, m[1], m[2] * h.z, false);
        } else {
            for (float s : {-1.0f, 1.0f}) {
                quad(c + m[0] * (s * h.x), m[1] * h.y, m[0] * s, m[2] * h.z, true);
                quad(c + m[1] * (s * h.y), m[0] * h.x, m[1] * s, m[2] * h.z, true);
                quad(c + m[2] * (s * h.z), m[0] * h.x, m[2] * s, m[1] * h.y, true);
            }
        }
    }
}

void Occluders::update(const render::View& view) {
    m_active.clear();
    m_ranked.clear();
    for (uint32_t i = 0; i < m_quads.size(); ++i) {
        const Quad& q = m_quads[i];
        const glm::vec3 c = q.centre - view.position;
        const float d2 = glm::dot(c, c);
        if (!view.frustum.sphere(c, q.reach)) {
            continue;
        }
        // A view through a door starts at the doorway: what stands before it, the building the camera is carried into, is
        // no more than cut away.
        if (view.clipped()) {
            const glm::vec3 n(view.clip_plane);
            if (glm::dot(n, c) + view.clip_plane.w < std::abs(glm::dot(n, q.x)) + std::abs(glm::dot(n, q.z))) {
                continue;
            }
        }
        const float side = glm::dot(c, q.y);
        if (std::abs(side) <= k_portal_slack || (q.box_face && side > 0.0f)) {
            continue; // edge on, or a box's far face
        }
        // Solid angle, roughly: what it can hide.
        const float size = q.reach * q.reach * std::abs(side) / std::max(d2 * std::sqrt(d2), 1e-3f);
        if (size > k_min_occluder_size) {
            m_ranked.emplace_back(size, i);
        }
    }
    const size_t keep = std::min<size_t>(m_ranked.size(), k_max_occluders);
    std::partial_sort(m_ranked.begin(), m_ranked.begin() + ptrdiff_t(keep), m_ranked.end(),
                      [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    for (size_t k = 0; k < keep; ++k) {
        const Quad& q = m_quads[m_ranked[k].second];
        const glm::vec3 c = q.centre - view.position;
        const glm::vec3 corner[4] = {c + q.x + q.z, c + q.x - q.z, c - q.x - q.z, c - q.x + q.z};
        Volume v;
        const glm::vec3 n = glm::normalize(glm::dot(c, q.y) > 0.0f ? q.y : -q.y); // away from the eye
        v.planes[0] = glm::vec4(n, -glm::dot(n, c));
        bool ok = true;
        for (int e = 0; e < 4; ++e) {
            glm::vec3 p = glm::cross(corner[e], corner[(e + 1) & 3]);
            const float len = glm::length(p);
            ok = ok && len > 1e-6f;
            p = len > 1e-6f ? p / len : p;
            v.planes[1 + e] = glm::vec4(glm::dot(p, c) < 0.0f ? -p : p, 0.0f);
        }
        if (ok) {
            m_active.push_back(v);
        }
    }
}

uint32_t Occluders::reaching(const glm::vec3& lo, const glm::vec3& hi) const {
    uint32_t mask = 0;
    const glm::vec3 c = (lo + hi) * 0.5f, h = (hi - lo) * 0.5f;
    for (size_t i = 0; i < m_active.size(); ++i) {
        bool reaches = true;
        for (const glm::vec4& p : m_active[i].planes) {
            const glm::vec3 n(p);
            if (glm::dot(n, c) + p.w + glm::dot(glm::abs(n), h) < 0.0f) {
                reaches = false;
                break;
            }
        }
        mask |= reaches ? 1u << i : 0u;
    }
    return mask;
}

bool Occluders::hides(const glm::vec3& rel, float radius, uint32_t mask) const {
    for (size_t i = 0; i < m_active.size(); ++i) {
        if (!((mask >> i) & 1u)) {
            continue;
        }
        const Volume& v = m_active[i];
        bool inside = true;
        for (const glm::vec4& p : v.planes) {
            if (glm::dot(glm::vec3(p), rel) + p.w < radius) {
                inside = false;
                break;
            }
        }
        if (inside) {
            return true;
        }
    }
    return false;
}

bool Rooms::Box::contains(const glm::vec3& p) const {
    const glm::vec3 l = glm::transpose(rotation) * (p - centre);
    return std::abs(l.x) <= half.x && std::abs(l.y) <= half.y && std::abs(l.z) <= half.z;
}

bool Rooms::Portal::straddles(const glm::vec4& s) const {
    const glm::vec3 d = glm::vec3(s) - centre;
    const float ax = std::abs(glm::dot(d, x)), ay = std::abs(glm::dot(d, y)), az = std::abs(glm::dot(d, z));
    const float r = s.w;
    if (!(ax - r <= half_width && az - r <= half_height && ay - r <= 0.0f) || r <= 0.0f) {
        return false;
    }
    // As the game writes it: the second test compares |y| (not |z|) against the half height.
    const float c = std::cos((r - ay) / r) * r;
    return ax - c <= half_width && ay - c <= half_height;
}

Rooms::Rooms(const osw::World& w) {
    for (const auto& r : w.rooms) {
        Box b;
        b.centre = glm::vec3(r.centre[0], r.centre[1], r.centre[2]);
        b.rotation = rotation_of(r.rotation);
        b.half = glm::vec3(r.half_extents[0], r.half_extents[1], r.half_extents[2]);
        m_rooms.push_back(b);
    }
    m_room_portals.resize(m_rooms.size() + 1);
    for (uint32_t i = 0; i < w.portals.size(); ++i) {
        const auto& p = w.portals[i];
        Portal q;
        q.centre = glm::vec3(p.centre[0], p.centre[1], p.centre[2]);
        const glm::mat3 m = rotation_of(p.rotation);
        q.x = m[0];
        q.y = m[1];
        q.z = m[2];
        q.half_width = p.half_width;
        q.half_height = p.half_height;
        q.rooms[0] = std::min(p.rooms[0], uint32_t(m_rooms.size()));
        q.rooms[1] = std::min(p.rooms[1], uint32_t(m_rooms.size()));
        m_portals.push_back(q);
        for (uint32_t side : q.rooms) {
            if (std::find(m_room_portals[side].begin(), m_room_portals[side].end(), i) == m_room_portals[side].end()) {
                m_room_portals[side].push_back(i);
            }
        }
    }
    // XLRM joins rooms both ways: they are seen as one.
    m_room_links.resize(m_rooms.size() + 1);
    std::vector<uint32_t> joined(m_rooms.size());
    for (uint32_t i = 0; i < joined.size(); ++i) {
        joined[i] = i;
    }
    auto root = [&](uint32_t i) {
        while (joined[i] != i) {
            i = joined[i] = joined[joined[i]];
        }
        return i;
    };
    for (uint32_t i = 0; i < w.rooms.size(); ++i) {
        for (uint32_t k = 0; k < w.rooms[i].link_count; ++k) {
            joined[root(w.room_links[w.rooms[i].first_link + k])] = root(i);
        }
    }
    for (uint32_t i = 0; i < m_rooms.size(); ++i) {
        for (uint32_t k = 0; k < m_rooms.size(); ++k) {
            if (k != i && root(k) == root(i)) {
                m_room_links[i].push_back(k);
            }
        }
    }
    m_reached.resize(m_rooms.size() + 1);
    m_terms.resize(m_rooms.size() + 1);
    m_used.resize(m_portals.size());

    // A portal-strict light walks the portals from the rooms holding it, or from the outside when none does
    // A shadowless one goes by connection alone, a shadow light through
    // the portals it sees, and a spot only those in its frustum.
    const uint32_t outside = uint32_t(m_rooms.size());
    m_unlit.resize(outside);
    m_portal_unlit.resize(m_portals.size());
    for (size_t l = 0; l < w.lights.size(); ++l) {
        const osw::Light& light = w.lights[l];
        if (!(light.flags & osw::light_portal_strict)) {
            continue;
        }
        const glm::vec3 at(light.position[0], light.position[1], light.position[2]);
        const bool shadow = (light.flags & osw::light_shadow) != 0;
        const bool spot = shadow && light.shadow_fov < 3.1415927f; // below pi a frustum
        // Gamebryo cameras look along local X, up local Y, right local Z.
        const glm::vec3 u(light.rotation[0], light.rotation[1], light.rotation[2]);
        auto rotated = [&](const glm::vec3& v) {
            const glm::vec3 t = 2.0f * glm::cross(u, v);
            return v + light.rotation[3] * t + glm::cross(u, t);
        };
        const glm::vec3 dir = rotated({1, 0, 0}), up = rotated({0, 1, 0}), right = rotated({0, 0, 1});
        const float near = std::max(light.shadow_near, units::k_skyrim_unit_m);
        const float t = std::tan(std::clamp(light.shadow_fov * 0.5f, 0.017453292f, 2.792527f));
        const glm::vec4 volume[6] = {
            glm::vec4(glm::normalize(dir * t - right), 0.0f), glm::vec4(glm::normalize(dir * t + right), 0.0f),
            glm::vec4(glm::normalize(dir * t - up), 0.0f),    glm::vec4(glm::normalize(dir * t + up), 0.0f),
            glm::vec4(dir, -near),                            glm::vec4(-dir, light.radius),
        };
        // The walk starts where the light's camera's near plane is.
        const glm::vec3 start = spot ? at + dir * near : at;
        reset();
        for (uint32_t i = 0; i < outside; ++i) {
            if (m_rooms[i].contains(start)) {
                m_reached[i] = 1;
                m_queue.push_back(i);
            }
        }
        if (m_queue.empty()) {
            m_reached[outside] = 1;
            m_queue.push_back(outside);
        }
        walk(at, spot ? std::span<const glm::vec4>(volume) : std::span<const glm::vec4>(),
             shadow ? Walk::sight : Walk::connection);
        for (uint32_t i = 0; i < outside; ++i) {
            if (!m_reached[i]) {
                m_unlit[i].push_back(static_cast<uint16_t>(l));
            }
        }
        for (uint32_t i = 0; i < m_portals.size(); ++i) {
            if (!m_used[i]) {
                m_portal_unlit[i].push_back(static_cast<uint16_t>(l));
            }
        }
    }
    reset();
}

std::span<const uint16_t> Rooms::unlit(uint32_t group) const {
    if (group == k_cell) {
        return {};
    }
    return (group & k_portal_bit) ? m_portal_unlit[group & ~k_portal_bit] : m_unlit[group];
}

uint32_t Rooms::place(const glm::vec4& bound, uint32_t hint) const {
    const glm::vec3 c(bound);
    uint32_t room = k_cell;
    if (hint < m_rooms.size() && m_rooms[hint].contains(c)) {
        room = hint;
    } else {
        for (uint32_t i = 0; i < m_rooms.size(); ++i) {
            if (m_rooms[i].contains(c)) {
                room = i;
                break;
            }
        }
    }
    // Straddling exactly one portal (of the room, or leading outside) hangs it under that portal's shared node;
    // two or more leave it on the cell's node.
    const auto& candidates = m_room_portals[room == k_cell ? m_rooms.size() : room];
    uint32_t straddled = 0, which = 0;
    for (uint32_t p : candidates) {
        if (m_portals[p].straddles(bound)) {
            ++straddled;
            which = p;
        }
    }
    if (straddled == 1) {
        return k_portal_bit | which;
    }
    return straddled == 0 ? room : k_cell;
}

bool Rooms::passes(const Term& t, const glm::vec3& rel, float radius) const {
    for (uint32_t i = 0; i < t.count; ++i) {
        const glm::vec4& p = m_planes[t.first + i];
        if (glm::dot(glm::vec3(p), rel) + p.w < -radius) {
            return false;
        }
    }
    return true;
}

bool Rooms::quad_passes(const Term& t, const glm::vec3* quad) const {
    for (uint32_t i = 0; i < t.count; ++i) {
        const glm::vec4& p = m_planes[t.first + i];
        bool any = false;
        for (int k = 0; k < 4 && !any; ++k) {
            any = glm::dot(glm::vec3(p), quad[k]) + p.w >= 0.0f;
        }
        if (!any) {
            return false;
        }
    }
    return true;
}

void Rooms::reset() {
    std::fill(m_reached.begin(), m_reached.end(), 0);
    std::fill(m_used.begin(), m_used.end(), 0);
    for (auto& t : m_terms) {
        t.clear();
    }
    m_planes.clear();
    m_queue.clear();
}

void Rooms::update(const render::View& view) {
    reset();
    const glm::vec3 eye = view.position;
    const glm::vec3 forward(-view.rotation[0][2], -view.rotation[1][2], -view.rotation[2][2]);
    const glm::vec3 point = eye + forward * k_near;

    // The camera's rooms: last frame's while it still holds the point, else every room that does.
    if (m_camera_room >= 0 && m_rooms[m_camera_room].contains(point)) {
        m_reached[m_camera_room] = 1;
        m_queue.push_back(uint32_t(m_camera_room));
    } else {
        m_camera_room = -1;
        for (uint32_t i = 0; i < m_rooms.size(); ++i) {
            if (m_rooms[i].contains(point)) {
                if (m_camera_room < 0) {
                    m_camera_room = int32_t(i);
                }
                m_reached[i] = 1;
                m_queue.push_back(i);
            }
        }
    }
    m_outside = m_queue.empty();
    if (m_outside) {
        return; // everything is culled normally
    }
    walk(eye, view.frustum.planes, Walk::view);
}

void Rooms::walk(const glm::vec3& eye, std::span<const glm::vec4> volume, Walk mode) {
    for (size_t i = 0, n = m_queue.size(); i < n; ++i) {
        for (uint32_t l : m_room_links[m_queue[i]]) {
            if (!m_reached[l]) {
                m_reached[l] = 1;
                m_queue.push_back(l);
            }
        }
    }
    const size_t starts = m_queue.size();
    for (uint32_t r : m_queue) {
        m_terms[r].push_back({0, 0}); // the camera frustum alone
    }
    for (size_t head = 0; head < m_queue.size(); ++head) {
        const uint32_t r = m_queue[head];
        for (uint32_t pi : m_room_portals[r]) {
            const Portal& p = m_portals[pi];
            if (m_used[pi] || (p.rooms[0] == r && p.rooms[1] == r)) {
                continue;
            }
            const uint32_t q = p.rooms[0] == r ? p.rooms[1] : p.rooms[0];
            if (mode == Walk::connection) {
                m_used[pi] = 1;
                reach(q);
                continue;
            }
            const glm::vec3 c = p.centre - eye;
            const glm::vec3 w = p.x * p.half_width, h = p.z * p.half_height;
            const glm::vec3 quad[4] = {c + w + h, c + w - h, c - w - h, c - w + h};
            // In the camera frustum and in some term of this room.
            bool in_view = true;
            for (const auto& plane : volume) {
                bool any = false;
                for (int k = 0; k < 4 && !any; ++k) {
                    any = glm::dot(glm::vec3(plane), quad[k]) + plane.w >= 0.0f;
                }
                in_view = in_view && any;
            }
            bool through = false;
            for (const Term& t : m_terms[r]) {
                through = through || quad_passes(t, quad);
            }
            if (!in_view || !through) {
                continue;
            }
            m_used[pi] = 1;
            if (q == m_rooms.size() && mode == Walk::view) {
                m_outside = true; // the outside is reached: everything is culled normally
                return;
            }
            if (std::find(m_queue.begin(), m_queue.begin() + ptrdiff_t(starts), q) != m_queue.begin() + ptrdiff_t(starts)) {
                continue; // back into a start room: only the portal's own node shows
            }
            // The portal's frustum: its plane, the eye outside it, and the
            // planes through the eye and each edge. Close to the plane it restricts nothing.
            const float dist = glm::dot(-c, p.y);
            const uint32_t first = static_cast<uint32_t>(m_planes.size());
            uint32_t count = 0;
            if (std::abs(dist) > k_portal_slack) {
                const glm::vec3 n = dist < 0.0f ? p.y : -p.y; // the eye on its negative side
                m_planes.emplace_back(n, -glm::dot(n, c));
                for (int k = 0; k < 4; ++k) {
                    glm::vec3 e = glm::cross(quad[k], quad[(k + 1) & 3]);
                    if (glm::dot(e, c) < 0.0f) {
                        e = -e;
                    }
                    const float len = glm::length(e);
                    if (len > 1e-8f) {
                        m_planes.emplace_back(e / len, 0.0f);
                    }
                }
                count = static_cast<uint32_t>(m_planes.size()) - first;
            }
            // AND with each of this room's terms (short ones only), OR-ed into the far room's.
            const size_t terms = m_terms[r].size();
            const size_t first_new = m_terms[q].size();
            for (size_t ti = 0; ti < terms; ++ti) {
                const Term t = m_terms[r][ti];
                const uint32_t start = static_cast<uint32_t>(m_planes.size());
                if (t.count <= k_max_carried_planes) {
                    for (uint32_t k = 0; k < t.count; ++k) {
                        m_planes.push_back(m_planes[t.first + k]);
                    }
                }
                for (uint32_t k = 0; k < count; ++k) {
                    m_planes.push_back(m_planes[first + k]);
                }
                m_terms[q].push_back({start, static_cast<uint32_t>(m_planes.size()) - start});
            }
            for (uint32_t l : m_room_links[q]) {
                m_terms[l].insert(m_terms[l].end(), m_terms[q].begin() + ptrdiff_t(first_new), m_terms[q].end());
            }
            reach(q);
        }
    }
}

void Rooms::reach(uint32_t room) {
    auto add = [&](uint32_t r) {
        if (!m_reached[r]) {
            m_reached[r] = 1;
            m_queue.push_back(r);
        }
    };
    add(room);
    for (uint32_t l : m_room_links[room]) {
        add(l);
    }
}

bool Rooms::visible(uint32_t group, const glm::vec3& rel, float radius) const {
    if (m_outside) {
        return true;
    }
    if (group == k_cell) {
        // On the cell's own node: outside every room, or across two portals. Dungeon shells end up here, and the
        // game shows them, so only the camera frustum applies.
        return true;
    }
    auto seen_from = [&](uint32_t room) {
        if (room >= m_rooms.size() || !m_reached[room]) {
            return false;
        }
        for (const Term& t : m_terms[room]) {
            if (passes(t, rel, radius)) {
                return true;
            }
        }
        return false;
    };
    if (group & k_portal_bit) {
        // A portal's shared node shows with the visible rooms on either side of it, whether or not the portal
        // itself is in view (the game culls the shared nodes of the visible rooms' portals).
        const uint32_t p = group & ~k_portal_bit;
        return p < m_portals.size() && (seen_from(m_portals[p].rooms[0]) || seen_from(m_portals[p].rooms[1]));
    }
    return seen_from(group);
}

uint32_t Rooms::rooms_visible() const {
    return static_cast<uint32_t>(std::count(m_reached.begin(), m_reached.end(), uint8_t(1)));
}

} // namespace spud::engine
