#pragma once

// .osp — spud physics shapes: the static collision of one mesh. Spec: docs/formats/osp.md
//
// A flat list of primitive, convex and triangle-mesh shapes in mesh space (metres, node transforms baked
// in), each tagged with the Skyrim collision layer and material it came from. The engine builds its own
// acceleration structures; nothing Havok-specific (MOPP code, welding) is kept.

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace spud::osp {

inline constexpr container::FourCC k_kind = container::fourcc("PHYS");
inline constexpr uint32_t k_version = 2; // 2: RANK (version 1 files still read)

enum class ShapeType : uint32_t {
    box = 0,      // centre, rotation, extents (half)
    sphere = 1,   // centre, radius
    capsule = 2,  // segment centre -> extents, radius
    cylinder = 3, // axis centre -> extents, radius
    convex = 4,   // hull of vertices[first_vertex, + vertex_count]
    mesh = 5,     // triangles: indices[first_index, + index_count] into vertices[first_vertex, ...]
};

enum class Motion : uint8_t {
    fixed = 0,     // architecture, landscape, furniture
    keyframed = 1, // moved by animation (doors, traps)
    dynamic = 2,   // simulated (clutter); the engine may still treat it as static
};

// Skyrim collision layers that matter to the engine (SkyrimLayer in niftools nif.xml). Others are kept as
// numbers.
enum Layer : uint8_t {
    layer_unidentified = 0,
    layer_static = 1,
    layer_anim_static = 2,
    layer_clutter = 4,
    layer_weapon = 5,
    layer_trees = 9,
    layer_props = 10,
    layer_water = 11,
    layer_trigger = 12,
    layer_terrain = 13,
    layer_trap = 14,
    layer_non_collidable = 15,
    layer_ground = 17,
    layer_debris_small = 19,
    layer_debris_large = 20,
    layer_transparent_small = 26,
    layer_invisible_wall = 27,
    layer_transparent_small_anim = 28,
    layer_char_controller = 30,
    layer_stair_helper = 31,
    layer_collision_box = 35,
};

struct Body {
    float mass = 0.0f; // kg-ish, as authored; 0 for fixed bodies
    float friction = 0.5f;
    float restitution = 0.4f;
    Motion motion = Motion::fixed;
    uint8_t layer = layer_static; // the body's own layer; shapes may override per part
    uint16_t node = 0; // an animated object's: 1 + the .osk node it rides on, its shapes in that node's space; else 0
};
static_assert(sizeof(Body) == 16);

struct Shape {
    ShapeType type = ShapeType::mesh;
    uint32_t material = 0;    // SkyrimHavokMaterial ID (footstep and impact sounds)
    uint8_t layer = 0;        // Layer
    uint8_t reserved0 = 0;
    uint16_t body = 0;        // index into the body table
    float convex_radius = 0;  // Havok's shell around boxes, cylinders, hulls and mesh triangles, metres
    float centre[3] = {0, 0, 0};
    float radius = 0;
    float rotation[4] = {0, 0, 0, 1}; // quaternion x y z w
    float extents[3] = {0, 0, 0};
    uint32_t first_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    uint32_t reserved1 = 0;
};
static_assert(sizeof(Shape) == 80);

struct Collision {
    float bounds_min[3] = {0, 0, 0};
    float bounds_max[3] = {0, 0, 0};
    std::vector<Body> bodies;
    std::vector<Shape> shapes;
    std::vector<float> vertices;    // x y z, metres
    std::vector<uint32_t> indices;  // mesh triangles, relative to the shape's first_vertex
    // Per mesh triangle (indices.size() / 3 of them, or none): the order Havok collects the triangle's contacts in,
    // from its MOPP code; 0xFFFFFFFF where unknown.
    std::vector<uint32_t> ranks;
};

// Recomputes bounds_min/max from the shapes.
void compute_bounds(Collision& collision);

std::vector<std::byte> write(const Collision& collision);
Collision read(std::span<const std::byte> file);

} // namespace spud::osp
