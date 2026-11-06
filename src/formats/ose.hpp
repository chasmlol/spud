#pragma once

// .ose — spud particle effect. Spec: docs/formats/ose.md
//
// Everything a Gamebryo particle effect needs at runtime, converted from a NIF by the importer: the scene
// nodes the effect refers to, its particle systems and their modifiers, emitter meshes, controllers,
// interpolators and keys, controller sequences and the effect-shader materials. Version 2 adds node
// animation (transform interpolators and NiMultiTargetTransformController targets), so a NIF whose only
// content is an animated node (the loading screens' camera paths) is an effect without particle systems. Version 3
// adds the scene graph the UI 3D scenes read: each node's class and world bound, which
// node each .osm submesh came from, and the root's inventory marker. Values keep the game's own
// units (Skyrim units, seconds, radians) and the state the game holds after loading, so the simulation
// (src/fx) can follow the game's arithmetic; the simulation converts its output to metres.

#include "formats/container.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::ose {

inline constexpr container::FourCC k_kind = container::fourcc("PSYS");
inline constexpr uint32_t k_version = 6; // 6: addon nodes, volume emitters; 5: the root's attach techniques (docs/formats/ose.md)
inline constexpr uint32_t k_min_version = 1; // version 1 files have no node animation, versions 1-2 no scene graph
inline constexpr uint32_t k_none = 0xFFFFFFFFu;

// A scene-graph object of the source file (node, shape or particle system).
struct Node {
    uint32_t name;   // string
    uint32_t parent; // node index, k_none for a root
    uint32_t flags;  // NiAVObject flags
    float scale;
    float translation[3]; // Skyrim units, in the parent's space
    float rotation[9];    // 3x3 column-major: parent = rotation * (scale * local) + translation
};
static_assert(sizeof(Node) == 64);

enum SystemFlag : uint16_t {
    system_world_space = 1u << 0,
    system_colors = 1u << 1,
    system_radii = 1u << 2,
    system_sizes = 1u << 3,
    system_rotations = 1u << 4,
    system_rotation_angles = 1u << 5,
    system_rotation_axes = 1u << 6,
    system_texture_indices = 1u << 7,
    system_rotation_speeds = 1u << 8,
};

struct System {
    uint32_t node;     // the NiParticleSystem's own node
    uint32_t material; // index into materials
    uint16_t max_particles;
    uint16_t flags;          // SystemFlag
    uint32_t first_modifier; // modifiers of this system, in update order
    uint32_t modifier_count;
    uint32_t first_subtexture; // into subtextures
    uint32_t subtexture_count;
    float aspect_ratio;
    uint32_t aspect_flags;
    float speed_to_aspect[3]; // aspect 2, speed 1, speed 2
    float bound[4];           // as stored: centre xyz, radius (Skyrim units)
    uint32_t reserved[4];
};
static_assert(sizeof(System) == 80);

enum class ModifierType : uint32_t {
    age_death = 1,
    lod = 2,          // BSPSysLODModifier
    mesh_emitter = 3, // NiPSysMeshEmitter
    volume_emitter = 13, // NiPSysSphereEmitter, NiPSysBoxEmitter, NiPSysCylinderEmitter
    spawn = 4,
    simple_color = 5, // BSPSysSimpleColorModifier
    rotation = 6,
    subtex = 7, // BSPSysSubTexModifier
    scale = 8,  // BSPSysScaleModifier
    gravity = 9,
    drag = 10,
    position = 11,
    bound_update = 12,
};

// One record for every modifier type; the meaning of ref/first/count/u/f per type is in ose.md.
struct Modifier {
    ModifierType type;
    uint32_t name;   // string
    uint32_t order;  // update order (the LOD modifier's is forced to 1 as the game does)
    uint32_t system; // index into systems
    uint8_t active;
    uint8_t flag0;
    uint8_t flag1;
    uint8_t reserved0;
    uint32_t ref;   // node or modifier index, k_none
    uint32_t first; // into floats (scale) or emitter_meshes (mesh emitter)
    uint32_t count;
    uint32_t u0;
    uint32_t u1;
    uint32_t reserved1[2];
    float f[20];
};
static_assert(sizeof(Modifier) == 128);

// A shape a mesh emitter emits from: the half-precision "particle data" of a BSTriShape, decoded.
struct EmitterMesh {
    uint32_t node;         // the shape's node
    uint32_t first_vertex; // into mesh_positions / mesh_normals
    uint32_t vertex_count;
    uint32_t first_triangle; // into mesh_triangles
    uint32_t triangle_count;
    uint32_t reserved[3];
};
static_assert(sizeof(EmitterMesh) == 32);

struct Vec3 {
    float x, y, z;
};
static_assert(sizeof(Vec3) == 12);

struct Triangle {
    uint16_t v[3];
};
static_assert(sizeof(Triangle) == 6);

enum class ControllerType : uint32_t {
    modifier_active = 1, // NiPSysModifierActiveCtlr
    emitter = 2,         // NiPSysEmitterCtlr
    update = 3,          // NiPSysUpdateCtlr
    effect_float = 4,    // BSEffectShaderPropertyFloatController
    effect_color = 5,    // BSEffectShaderPropertyColorController (stored, not simulated)
    transform = 6,       // NiMultiTargetTransformController: its targets are in controller_targets
    other = 7,           // any other NiPSys*Ctlr (stored by type name, not simulated)
};

enum class TargetKind : uint32_t {
    none = 0,
    system = 1,   // index into systems
    material = 2, // index into materials
    node = 3,     // index into nodes
};

struct Controller {
    ControllerType type;
    uint16_t flags; // NiTimeController flags as the game holds them after loading
    uint16_t reserved0;
    float frequency;
    float phase;
    float start_time; // "lo key time"
    float stop_time;  // "hi key time"
    TargetKind target_kind;
    uint32_t target;
    uint32_t interpolator;  // index into interpolators, k_none
    uint32_t interpolator2; // emitter: visibility interpolator
    uint32_t modifier;      // modifier index (modifier controllers)
    uint32_t variable;      // effect controllers: controlled variable
    uint32_t name;          // string: modifier name (modifier controllers) or type name (other)
    uint32_t first_target;  // transform: controller_targets[first_target, + target_count)
    uint32_t target_count;
    uint32_t reserved1;
};
static_assert(sizeof(Controller) == 64);

enum class InterpolatorType : uint32_t {
    float_value = 1,   // NiFloatInterpolator
    bool_value = 2,    // NiBoolInterpolator
    bool_timeline = 3, // NiBoolTimelineInterpolator
    blend_float = 4,   // NiBlendFloatInterpolator
    blend_bool = 5,    // NiBlendBoolInterpolator
    transform = 6,     // NiTransformInterpolator: key_group indexes transforms
};

struct Interpolator {
    InterpolatorType type;
    float float_value;   // -FLT_MAX: none
    uint8_t bool_value;  // 0, 1, 2 = none
    uint8_t blend_flags; // bit 0 manager controlled
    uint8_t array_size;
    uint8_t reserved0;
    float weight_threshold;
    uint32_t key_group; // index into key_groups (transform: into transforms), k_none
    uint32_t reserved1[3];
};
static_assert(sizeof(Interpolator) == 32);

// A NiTransformInterpolator's pose and keys (NiTransformData). Components of the pose are -FLT_MAX where the
// interpolator holds none; a part with keys is evaluated from them instead.
struct TransformInterp {
    float translation[3];       // Skyrim units
    float rotation[4];          // quaternion w, x, y, z
    float scale;
    uint32_t rotation_type;     // 0 none, 1 linear, 2 quadratic, 3 TBC (quat_keys), 4 XYZ Euler angles, 5 constant
    uint32_t rotation_first;    // quaternion keys: into quat_keys; XYZ: the key group of X (Y and Z follow it)
    uint32_t rotation_count;    // quaternion keys; 3 for XYZ
    uint32_t translation_type;  // 1 linear, 2 quadratic, 3 TBC, 5 constant
    uint32_t translation_first; // into vec3_keys
    uint32_t translation_count;
    uint32_t scale_group; // float key group, k_none
    uint32_t reserved;
};
static_assert(sizeof(TransformInterp) == 64);

struct Vec3Key {
    float time;
    float value[3];
    float forward[3];  // quadratic: in tangent
    float backward[3]; // quadratic: out tangent
    float tension, bias, continuity;
    float reserved;
};
static_assert(sizeof(Vec3Key) == 56);

struct QuatKey {
    float time;
    float value[4]; // w, x, y, z
    float tension, bias, continuity;
};
static_assert(sizeof(QuatKey) == 32);

enum class KeyKind : uint32_t {
    float_keys = 1,
    bool_keys = 2,
};

struct KeyGroup {
    KeyKind kind;
    uint32_t key_type; // 1 linear, 2 quadratic, 3 TBC, 5 constant/step
    uint32_t first;    // into float_keys or bool_keys
    uint32_t count;
};
static_assert(sizeof(KeyGroup) == 16);

struct FloatKey {
    float time;
    float value;
    float forward;  // quadratic: in tangent
    float backward; // quadratic: out tangent
    float tension;
    float bias;
    float continuity;
    float reserved;
};
static_assert(sizeof(FloatKey) == 32);

struct BoolKey {
    float time;
    uint8_t value;
    uint8_t reserved[3];
};
static_assert(sizeof(BoolKey) == 8);

struct Sequence {
    uint32_t name;
    float weight;
    uint32_t cycle_type; // 0 loop, 1 reverse, 2 clamp
    float frequency;
    float start_time;
    float stop_time;
    uint32_t first_block; // into blocks
    uint32_t block_count;
    uint32_t first_text_key;
    uint32_t text_key_count;
    uint32_t accum_root; // string
    uint32_t reserved;
};
static_assert(sizeof(Sequence) == 48);

struct Block {
    uint32_t interpolator; // the sequence's interpolator, k_none
    uint32_t controller;   // the controller it drives, k_none
    uint8_t priority;
    uint8_t reserved[3];
    uint32_t node_name; // strings: the ID tag the game binds by
    uint32_t property_type;
    uint32_t controller_type;
    uint32_t controller_id;
    uint32_t interpolator_id;
};
static_assert(sizeof(Block) == 32);

struct TextKey {
    float time;
    uint32_t text; // string
};
static_assert(sizeof(TextKey) == 8);

enum class MaterialOwner : uint32_t {
    system = 1,     // owner = index into systems
    mesh_shape = 2, // owner = submesh index in the .osm converted from the same NIF (k_none: not drawn)
};

// A BSEffectShaderProperty and NiAlphaProperty, for a particle system or for a mesh shape that one of the
// effect's controllers animates.
struct EffectMaterial {
    MaterialOwner owner_kind;
    uint32_t owner;
    uint32_t node;          // the system's or shape's node
    uint32_t skyrim_flags1; // SkyrimShaderPropertyFlags1
    uint32_t skyrim_flags2;
    uint32_t flags;     // osm::MaterialFlag bits (alpha blend/test, z test/write, two-sided, ...)
    uint16_t blend_src; // NiAlphaProperty blend modes
    uint16_t blend_dst;
    uint8_t alpha_threshold;
    uint8_t alpha_func;
    uint8_t texture_clamp;
    uint8_t lighting_influence;
    uint8_t env_map_min_lod;
    uint8_t reserved0[3];
    float uv_offset[2];
    float uv_scale[2];
    float base_color[4];
    float base_color_scale;
    float falloff[4]; // start angle, stop angle, start opacity, stop opacity (as stored)
    float soft_falloff_depth;
    uint32_t source_texture; // string: normalised cache path
    uint32_t greyscale_texture;
    uint32_t alpha_property_flags; // raw NiAlphaProperty flags, 0 without one
    uint32_t reserved1[6];
};
static_assert(sizeof(EffectMaterial) == 128);

// Version 3: what a node is, beyond its transform (NODX, one per node).
enum class NodeKind : uint16_t {
    other = 0,     // any other NiAVObject (particle system, light, camera, ...)
    node = 1,      // NiNode and the subclasses that only group (BSFadeNode, BSOrderedNode, ...)
    billboard = 2, // NiBillboardNode: billboard_mode is its BillboardMode (nif.xml)
    shape = 3,     // BSTriShape and its subclasses, NiTriShape, NiTriStrips
};

struct NodeExtra {
    NodeKind kind;
    uint16_t billboard_mode;  // NiBillboardNode only
    // Shapes: the bound sphere in the shape's own space (Skyrim units): the stored one, or for a skinned shape the
    // skin's at the file's rest pose;
    // radius 0 when there is none.
    float bound_center[3];
    float bound_radius;
    // Shapes with at least 4 vertices and UVs: u min, u max, v min, v max of the UVs clamped to [0, 1], hidden shapes
    // included (the Book Menu sizes its movie's viewport from the hidden PageText's);
    // (0, 1, 0, 1) otherwise.
    float uv_rect[4];
    uint32_t reserved[3];
};
static_assert(sizeof(NodeExtra) == 48);

// Version 3: the root's BSInvMarker named "INV", which the game's 3D views place models with (INVM, 0 or 1).
struct InvMarker {
    uint16_t rotation[3]; // x, y, z: radians x 1000, as stored (the engine multiplies by 0.001)
    uint16_t reserved0;
    float zoom;
    uint32_t reserved1[2];
};
static_assert(sizeof(InvMarker) == 20);

// A BSValueNode "AddOnNode<index>": the ADDN of that index plays there (docs/formats/ose.md).
struct AddonNode {
    uint32_t index;
    float lifetime; // the longest stop - start of the file's controllers, below 0 without any
    float rotation[9]; // column-major, in the model's space
    float translation[3]; // Skyrim units
    float scale;
};
static_assert(sizeof(AddonNode) == 60);

struct Effect {
    uint32_t root = k_none; // the NIF's root node
    std::vector<Node> nodes;
    std::vector<System> systems;
    std::vector<Modifier> modifiers;
    std::vector<float> floats;
    std::vector<std::array<float, 4>>
        subtextures; // per frame, as stored (u offset, u size, v offset, v size)
    std::vector<EmitterMesh> emitter_meshes;
    std::vector<Vec3> mesh_positions; // Skyrim units, shape space
    std::vector<Vec3> mesh_normals;
    std::vector<Triangle> mesh_triangles;
    std::vector<Controller> controllers; // in update order
    std::vector<Interpolator> interpolators;
    std::vector<KeyGroup> key_groups;
    std::vector<FloatKey> float_keys;
    std::vector<BoolKey> bool_keys;
    std::vector<Sequence> sequences;
    std::vector<Block> blocks;
    std::vector<TextKey> text_keys;
    std::vector<EffectMaterial> materials;
    // Version 2: node animation.
    std::vector<TransformInterp> transforms;
    std::vector<Vec3Key> vec3_keys;
    std::vector<QuatKey> quat_keys;
    std::vector<uint32_t> controller_targets; // node indices (k_none: a target the file left empty)
    // Version 3: the scene graph. Empty when read from an older file.
    std::vector<NodeExtra> node_extras;    // one per node
    std::vector<uint32_t> submesh_nodes;   // per submesh of the .osm converted from the same NIF: its shape's node
    std::vector<InvMarker> inv_marker;     // the root's "INV" marker, if it has one
    // Version 5: the root's NiStringsExtraData "AttachT", string offsets: `Name` or `Name&Arg`.
    std::vector<uint32_t> attach_techniques;
    std::vector<AddonNode> addon_nodes; // chunk ADDN, in files that have any

    // Strings are deduplicated into one table; records hold offsets.
    uint32_t add_string(std::string_view s);
    std::string_view string(uint32_t offset) const;
    const std::vector<char>& string_data() const { return m_strings; }
    void set_string_data(std::vector<char> data);

private:
    std::vector<char> m_strings;
    std::unordered_map<std::string, uint32_t> m_string_index;
};

std::vector<std::byte> write(const Effect& effect);
Effect read(std::span<const std::byte> file);

} // namespace spud::ose
