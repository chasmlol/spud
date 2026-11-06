#pragma once

// The game's navmeshes (NAVM) at run time: finding where a point is on them and a path between two points, across cell
// edges. Skyrim units.

#include "formats/forms_typed.hpp"

#include <future>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace spud::game {

using NavPoint = std::array<float, 3>;

// A triangle of a navmesh.
struct NavSpot {
    uint32_t mesh = 0; // NAVM
    int32_t triangle = -1;
    NavPoint position{};
    bool valid() const { return triangle >= 0; }
};

class NavMeshes {
public:
    explicit NavMeshes(const forms::Database& db);

    // The spot on a space's navmeshes under or nearest `point`, within `reach`. `space` is the WRLD, or an interior's
    // CELL. Decodes what it needs: any thread but one at a time.
    std::optional<NavSpot> locate(uint32_t space, const NavPoint& point, float reach = 256.0f);
    // Decodes on a worker what the cells round `point` hold, so actors arriving there find their meshes ready.
    void warm(uint32_t space, const NavPoint& point, int32_t cells);
    // The height of the navmesh at (x, y), starting the search at `spot`, which it moves along; the position's z.
    bool walk(NavSpot& spot, float x, float y);
    // The middle of an interior's navmesh: a vertex near the centre of its bounds.
    bool middle(uint32_t cell, NavPoint& out);
    // A path of corners from one spot to another, the two included; empty when none connects them.
    std::vector<NavPoint> path(const NavSpot& from, const NavSpot& to);

private:
    struct Mesh {
        forms::NavmeshGeometry geometry;
        // Each triangle's box in plan view (x0, y0, x1, y1), and all of them together: what locate skips by.
        std::vector<std::array<float, 4>> boxes;
        std::array<float, 4> bounds{};
    };
    const Mesh* mesh(uint32_t form);
    // Each edge's neighbour: mesh and triangle, across cells too.
    struct Link {
        uint32_t mesh = 0;
        int32_t triangle = -1;
    };
    Link neighbour(const Mesh& m, uint32_t mesh_form, int32_t triangle, int edge) const;

    const forms::Database* m_db;
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_exterior; // (WRLD, cell x, cell y) -> NAVMs
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_interior; // CELL -> NAVMs
    std::unordered_map<uint32_t, std::unique_ptr<Mesh>> m_meshes;
    std::mutex m_mutex; // decoded meshes stay put: the map alone is guarded
    std::future<void> m_warming;
};

} // namespace spud::game
