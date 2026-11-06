#pragma once

// Interior rooms and portals, culled as the game culls them:
// RoomMarker boxes hold the references whose bound centre they contain, PortalMarker rectangles join rooms, and from
// the camera's room the rooms seen through portals are found breadth first, each through the frusta of the portals
// on the way. While the outside is not reached, only what those rooms and portals hold is drawn.

#include "formats/osw.hpp"
#include "render/view.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace spud::engine {

// Occlusion planes and boxes (PlaneMarker references): each hides what lies wholly in the shadow it casts from the eye
// A box acts through the faces turned to the eye.
class Occluders {
public:
    explicit Occluders(const osw::World& world);
    bool empty() const { return m_quads.empty(); }
    // Once per frame: the largest occluders on screen, as volumes relative to the camera.
    void update(const render::View& view);
    // The occluders (a bit each) that a box, relative to the camera, reaches into at all: a cell's, so that most
    // references test none.
    uint32_t reaching(const glm::vec3& lo, const glm::vec3& hi) const;
    // Whether a sphere (relative to the camera) lies wholly inside one of `mask`'s occluders.
    bool hides(const glm::vec3& rel, float radius, uint32_t mask = ~0u) const;
    uint32_t active() const { return uint32_t(m_active.size()); }

private:
    struct Quad {
        glm::vec3 centre, x, y, z; // half width along x, half height along z, normal y
        float reach;               // the half diagonal
        bool box_face;             // shows only from its outer side
    };
    struct Volume {
        glm::vec4 planes[5]; // the quad's plane, then its four edges through the eye; inward normals
    };
    std::vector<Quad> m_quads;
    std::vector<Volume> m_active;
    std::vector<std::pair<float, uint32_t>> m_ranked; // scratch: (size on screen, quad)
};

class Rooms {
public:
    // Where a reference is drawn from: a room (its index), a portal's shared node, or the cell itself.
    static constexpr uint32_t k_cell = ~0u;
    static constexpr uint32_t k_portal_bit = 0x80000000u;

    explicit Rooms(const osw::World& world);
    bool empty() const { return m_rooms.empty(); }

    // `bound` is the reference's world bound (centre, radius), `hint` its XMBR room.
    uint32_t place(const glm::vec4& bound, uint32_t hint) const;

    // The portal-strict lights that can't light what is placed in `group`, sorted. A strict light lights only the rooms
    // and portals its walk from the rooms holding it comes to; the cell's own group takes every light.
    std::span<const uint16_t> unlit(uint32_t group) const;

    // Once per frame for the view that draws the space: the camera's room and what it sees.
    void update(const render::View& view);
    // Whether a reference placed in `group` may be drawn (the camera frustum is tested by the caller). `rel` is the
    // bound's centre relative to the camera.
    bool visible(uint32_t group, const glm::vec3& rel, float radius) const;
    // The room the camera is in (the first, when it is in several), or -1.
    int32_t camera_room() const { return m_camera_room; }
    bool outside_visible() const { return m_outside; }
    uint32_t rooms_visible() const;

private:
    struct Box {
        glm::vec3 centre;
        glm::mat3 rotation;
        glm::vec3 half;
        bool contains(const glm::vec3& p) const;
    };
    struct Portal {
        glm::vec3 centre;
        glm::vec3 x, y, z; // local axes: width, normal, height
        float half_width, half_height;
        uint32_t rooms[2];
        bool straddles(const glm::vec4& sphere) const;
    };
    struct Term {
        uint32_t first; // into m_planes
        uint32_t count;
    };
    enum class Walk {
        view,       // the camera's: stops at the outside
        sight,      // a shadow light's: goes on through the outside
        connection, // a shadowless strict light's: every portal joined up, seen or not
    };
    void reset();
    // From the rooms in m_queue, through the portals, breadth first: what it comes to in m_reached and m_used. A
    // portal must lie in `volume`: inward planes relative to the eye.
    void walk(const glm::vec3& eye, std::span<const glm::vec4> volume, Walk mode);
    void reach(uint32_t room);
    bool passes(const Term& t, const glm::vec3& rel, float radius) const;
    bool quad_passes(const Term& t, const glm::vec3* quad) const;

    std::vector<Box> m_rooms;
    std::vector<Portal> m_portals;
    // Indexed by room, the outside last (it stands for a portal side with no room).
    std::vector<std::vector<uint32_t>> m_room_portals; // portals of each room
    std::vector<std::vector<uint32_t>> m_room_links;   // rooms joined to each by XLRM, directly or not, both ways
    std::vector<std::vector<uint16_t>> m_unlit;        // portal-strict lights each room is out of reach of
    std::vector<std::vector<uint16_t>> m_portal_unlit; // and each portal's shared node

    // This frame.
    int32_t m_camera_room = -1;
    bool m_outside = true;
    std::vector<uint8_t> m_reached;
    std::vector<uint8_t> m_used;            // per portal
    std::vector<std::vector<Term>> m_terms; // per room: visible through any of these
    std::vector<glm::vec4> m_planes;        // camera-relative, xyz inward normal, w offset
    std::vector<uint32_t> m_queue;
};

} // namespace spud::engine
