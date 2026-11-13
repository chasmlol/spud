#pragma once

// .osp — spud physics shapes: the static collision of one mesh. Spec: docs/formats/osp.md
//
// A flat list of primitive, convex and triangle-mesh shapes in mesh space (metres, node transforms baked
// in), each tagged with the Skyrim collision layer and material it came from. The engine builds its own
// acceleration structures; nothing Havok-specific (MOPP code, welding) is kept.

#include "formats/container.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::osp {

inline constexpr container::FourCC k_kind = container::fourcc("PHYS");
inline constexpr uint32_t k_version = 3; // 3: NODE, JOIN, BSXF; 2: RANK (older files still read)

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

// Where a body's rigid body sits in mesh space (metres), and the node that carries it: scripts name bodies by node
// (Game.AddHavokBallAndSocketConstraint), and a body's joints are given from it.
struct BodyNode {
    uint32_t name = 0xFFFFFFFF; // string offset; none for a body with no named node
    float position[3] = {0, 0, 0};
    float rotation[4] = {0, 0, 0, 1}; // quaternion x y z w
};
static_assert(sizeof(BodyNode) == 32);

// The NIF's bhk constraints between two bodies of the model.
enum class JointType : uint8_t {
    ball_and_socket = 0, // pivots only
    hinge = 1,           // axis_a[0] / axis_b[0] the hinge, [1] [2] perpendicular
    limited_hinge = 2,   // as hinge, limits[0..1] the angle range, radians
    ragdoll = 3,         // axis [0] twist, [1] plane, [2] motor; limits: cone max, plane min, plane max, twist min, twist max
    stiff_spring = 4,    // pivots and `length`
    prismatic = 5,       // axis [0] sliding, [1] rotation, [2] plane; limits[0..1] the distance range, metres
};

// Pivots and axes in mesh space (metres), as the model rests: each body's frame there is its BodyNode.
struct Joint {
    JointType type = JointType::ball_and_socket;
    uint8_t reserved0 = 0;
    uint16_t body_a = 0, body_b = 0;
    uint16_t reserved1 = 0;
    float pivot_a[3] = {0, 0, 0};
    float pivot_b[3] = {0, 0, 0};
    float axes_a[9] = {}; // three axes, x y z each
    float axes_b[9] = {};
    float limits[5] = {};
    float friction = 0;
    float length = 0;
};
static_assert(sizeof(Joint) == 132);

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
    // One per body, or none (a file before version 3).
    std::vector<std::string> body_names;
    std::vector<BodyNode> body_nodes; // name offsets resolved into body_names on read
    std::vector<Joint> joints;
    uint32_t bsx_flags = 0; // the root's BSXFlags: 0x80 articulated, 8 complex, 2 Havok
};

// Recomputes bounds_min/max from the shapes.
void compute_bounds(Collision& collision);

std::vector<std::byte> write(const Collision& collision);
Collision read(std::span<const std::byte> file);

} // namespace spud::osp
