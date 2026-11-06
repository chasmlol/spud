#pragma once

// Gamebryo NIF files as written by Skyrim SE/AE (version 20.2.0.7, user version 12, BS version 100) and
// Skyrim LE (BS version 83). Reference: niftools nif.xml.
//
// A NIF is a flat list of blocks whose sizes are stored in the header. The block types needed for static
// render geometry, particle systems and their controllers, interpolators, keys and sequences are parsed into
// typed structs; every other block is kept as an opaque byte range. Each
// parsed block must consume exactly its declared size, which catches layout mistakes immediately: a block
// that does not is downgraded to opaque and reported instead of failing the whole file.

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace spud::legacy {

using NifRef = int32_t; // block index, -1 = none

inline constexpr uint32_t k_nif_version = 0x14020007; // 20.2.0.7
inline constexpr uint32_t k_bs_version_le = 83;       // Skyrim LE
inline constexpr uint32_t k_bs_version_se = 100;      // Skyrim SE/AE

struct NifHeader {
    uint32_t version = 0;
    uint32_t user_version = 0;
    uint32_t bs_version = 0;
    std::string author;
    std::string process_script;
    std::string export_script;
    std::vector<std::string> block_types;
    std::vector<std::string> strings; // the string table; string fields in blocks index it
};

// Vertex attributes decoded to plain arrays; an attribute the source lacks is empty.
struct NifGeometry {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec2> uvs;
    std::vector<glm::vec3> normals;
    // SE packed vertices, as stored: unorm8 normal and tangent (w = bitangent y and z) and the float
    // bitangent x that follows the position. The game feeds these bytes to its shaders unchanged.
    std::vector<glm::u8vec4> packed_normals;
    std::vector<glm::u8vec4> packed_tangents;
    std::vector<float> bitangent_x;
    // NiTriShapeData stores float tangents and bitangents instead.
    std::vector<glm::vec3> tangents;
    std::vector<glm::vec3> bitangents;
    std::vector<glm::u8vec4> colors;
    std::vector<glm::vec4> bone_weights;   // SE packed skinned vertices: 4 weights per vertex
    std::vector<glm::u8vec4> bone_indices; // ... and the bones they refer to
    std::vector<std::array<uint16_t, 3>> triangles;
};

// NiTransform: note the order differs from NiAVObject (rotation first).
struct NiTransform {
    glm::mat3 rotation{1.0f};
    glm::vec3 translation{0.0f};
    float scale = 1.0f;
};

// BSVertexDesc: packed vertex layout of BSTriShape and the SE skin partition.
struct BSVertexDesc {
    enum Attribute : uint32_t {
        attr_vertex = 0x1,
        attr_uv = 0x2,
        attr_uv2 = 0x4,
        attr_normal = 0x8,
        attr_tangent = 0x10,
        attr_colors = 0x20,
        attr_skinned = 0x40,
        attr_land_data = 0x80,
        attr_eye_data = 0x100,
        attr_instance = 0x200,
        attr_full_precision = 0x400,
    };

    uint64_t raw = 0;

    // Offsets and sizes are stored in units of 4 bytes.
    uint32_t vertex_size() const { return nibble(0) * 4; }
    uint32_t uv_offset() const { return nibble(2) * 4; }
    uint32_t uv2_offset() const { return nibble(3) * 4; }
    uint32_t normal_offset() const { return nibble(4) * 4; }
    uint32_t tangent_offset() const { return nibble(5) * 4; }
    uint32_t color_offset() const { return nibble(6) * 4; }
    uint32_t skinning_offset() const { return nibble(7) * 4; }
    uint32_t eye_offset() const { return nibble(9) * 4; }
    uint32_t attributes() const { return static_cast<uint32_t>(raw >> 44) & 0xFFF; }
    bool has(Attribute a) const { return (attributes() & a) != 0; }

private:
    uint32_t nibble(int i) const { return static_cast<uint32_t>(raw >> (i * 4)) & 0xF; }
};

struct NiObjectNET {
    std::string name;
    std::vector<NifRef> extra_data;
    NifRef controller = -1;
};

struct NiAVObject : NiObjectNET {
    uint32_t flags = 0; // bit 0: hidden (APP_CULLED)
    glm::vec3 translation{0.0f};
    glm::mat3 rotation{1.0f}; // column vectors: parent = rotation * (scale * local) + translation
    float scale = 1.0f;
    NifRef collision = -1;

    bool hidden() const { return (flags & 1) != 0; }
};

// NiNode and its subclasses. They all share the NiNode prefix; the subclass fields that affect what is
// drawn are decoded, the rest are skipped.
struct NiNode : NiAVObject {
    std::vector<NifRef> children;
    std::vector<NifRef> effects;
    std::optional<uint32_t> switch_index; // NiSwitchNode / NiLODNode: the child that is drawn
    uint16_t switch_flags = 0;            // NiSwitchNode: bit 0 UpdateOnlyActiveChild
    NifRef lod_data = -1;                 // NiLODNode -> NiRangeLODData
    NifRef multi_bound = -1;              // BSMultiBoundNode -> BSMultiBound
    glm::vec4 alpha_sort_bound{0.0f};     // BSOrderedNode: centre, radius
    bool static_bound = false;            // BSOrderedNode: alpha_sort_bound is the node's bound
    std::array<uint8_t, 3> range{};       // BSRangeNode family: min, max, current
    std::optional<uint16_t> billboard_mode; // NiBillboardNode: its BillboardMode (nif.xml)
};

struct NiPoint3Interpolator {
    glm::vec3 value{0.0f}; // used when there is no data
    NifRef data = -1;      // NiPosData
};

// NiKeyGroup<Vector3>, split into one float key group per component (the engine evaluates each component with the
// float key formulas).
struct NiPosData {
    uint32_t key_type = 0;
    std::array<std::vector<std::array<float, 5>>, 3> keys;
};

// BSTriShape and its subclasses (BSMeshLODTriShape, BSSubIndexTriShape, BSDynamicTriShape).
struct BSTriShape : NiAVObject {
    glm::vec3 bound_center{0.0f};
    float bound_radius = 0.0f;
    NifRef skin = -1;
    NifRef shader_property = -1;
    NifRef alpha_property = -1;
    BSVertexDesc vertex_desc;
    uint32_t num_triangles = 0;
    uint32_t num_vertices = 0;
    // Empty when the shape stores no vertex data itself (SE skinned shapes keep it in NiSkinPartition).
    NifGeometry geometry;
    // BSDynamicTriShape: geometry.positions and geometry.bitangent_x come from its separate float4 array
    // (xyz, w = bitangent x), which the game binds as the vertex position.
    bool dynamic = false;
    // BSMeshLODTriShape: three consecutive triangle runs that add up to the full mesh; lower LODs draw
    // only the first runs.
    std::array<uint32_t, 3> lod_sizes{};
    bool has_lods = false;
    // SE "particle data": a half-precision copy of the positions, normals and triangles that
    // NiPSysMeshEmitter samples (decoded to floats; empty when the shape has none).
    std::vector<glm::vec3> particle_positions;
    std::vector<glm::vec3> particle_normals;
    std::vector<std::array<uint16_t, 3>> particle_triangles;
};

// NiTriShape, NiTriStrips, BSLODTriShape, BSSegmentedTriShape: geometry lives in a separate data block.
struct NiGeometry : NiAVObject {
    NifRef data = -1;
    NifRef skin = -1;
    NifRef shader_property = -1;
    NifRef alpha_property = -1;
    std::array<uint32_t, 3> lod_sizes{}; // BSLODTriShape
    bool has_lods = false;
};

// NiTriShapeData / NiTriStripsData (strips are converted to a triangle list).
struct NiGeometryData {
    NifGeometry geometry;
    glm::vec3 bound_center{0.0f};
    float bound_radius = 0.0f;
};

// BSMultiBound -> BSMultiBoundAABB / BSMultiBoundOBB / BSMultiBoundSphere: the shape a BSMultiBoundNode's bound
// comes from.
struct BSMultiBound {
    NifRef data = -1;
};
struct BSMultiBoundShape {
    glm::vec3 center{0.0f};
    glm::vec3 extent{0.0f}; // AABB extent, OBB size (both half extents)
    float radius = 0.0f;    // sphere
    bool sphere = false;
};

struct NiSkinInstance {
    NifRef data = -1;
    NifRef skin_partition = -1;
    NifRef skeleton_root = -1;
    std::vector<NifRef> bones;
    std::vector<uint16_t> body_parts; // BSDismemberSkinInstance: each partition's body part
};

struct NiSkinData {
    struct Bone {
        NiTransform skin_to_bone; // skin space -> bone space (inverse bind pose)
        glm::vec3 bound_center{0.0f}; // the bone's bounding sphere, bone space
        float bound_radius = 0.0f;
        // (vertex, weight); empty when the weights live in the skin partition instead.
        std::vector<std::pair<uint16_t, float>> weights;
    };
    NiTransform skin_transform; // overall
    std::vector<Bone> bones;    // parallel to the game
};

struct NiSkinPartition {
    struct Partition {
        std::vector<uint16_t> bones;      // partition bone -> NiSkinInstance bone
        std::vector<uint16_t> vertex_map; // partition-local vertex -> shape vertex
        // Triangles in shape vertex indices. LE stores them partition-local (mapped here through the vertex
        // map, strips expanded); SE stores them in shape indices already, twice.
        std::vector<std::array<uint16_t, 3>> triangles;
    };
    BSVertexDesc vertex_desc; // SE: the shape's vertex data lives here
    NifGeometry geometry;     // SE: decoded vertex data (no triangles)
    std::vector<Partition> partitions;
};

struct BSLightingShaderProperty : NiObjectNET {
    uint32_t shader_type = 0;
    uint32_t flags1 = 0;      // SkyrimShaderPropertyFlags1
    uint32_t flags2 = 0;      // SkyrimShaderPropertyFlags2
    glm::vec2 uv_offset{0.0f};
    glm::vec2 uv_scale{1.0f};
    NifRef texture_set = -1;
    glm::vec3 emissive_color{0.0f};
    float emissive_multiple = 1.0f;
    uint32_t texture_clamp = 3;
    float alpha = 1.0f;
    float refraction_strength = 0.0f;
    float glossiness = 80.0f;
    glm::vec3 specular_color{1.0f};
    float specular_strength = 1.0f;
    float lighting_effect1 = 0.3f;
    float lighting_effect2 = 2.0f;
    std::array<float, 8> type_params{}; // shader-type-specific values (same layout as osm MaterialParams)
};

struct BSEffectShaderProperty : NiObjectNET {
    uint32_t flags1 = 0;
    uint32_t flags2 = 0;
    glm::vec2 uv_offset{0.0f};
    glm::vec2 uv_scale{1.0f};
    std::string source_texture;
    uint8_t texture_clamp = 3;
    uint8_t lighting_influence = 0;
    uint8_t env_map_min_lod = 0;
    glm::vec4 falloff{1.0f, 0.0f, 1.0f, 0.0f}; // start angle, stop angle, start opacity, stop opacity
    glm::vec4 base_color{1.0f};
    float base_color_scale = 1.0f;
    float soft_falloff_depth = 0.0f;
    std::string greyscale_texture;
};

struct BSShaderTextureSet {
    std::vector<std::string> textures; // as written in the file
};

// BSSkyShaderProperty (nif.xml): the sky's meshes (atmosphere dome, cloud layers, stars).
struct BSSkyShaderProperty : NiObjectNET {
    uint32_t flags1 = 0;
    uint32_t flags2 = 0;
    glm::vec2 uv_offset{0.0f};
    glm::vec2 uv_scale{1.0f};
    std::string source_texture;
    uint32_t sky_object_type = 0; // SkyObjectType: 0 texture, 1 sun glare, 2 sky, 3 clouds, 5 stars, 7 moon mask
};

struct NiAlphaProperty : NiObjectNET {
    uint16_t flags = 0; // bit 0 blend, 1-4 src, 5-8 dst, 9 test, 10-12 test func
    uint8_t threshold = 0;
};

// NiIntegerExtraData and BSXFlags.
struct NiIntegerExtraData {
    std::string name;
    uint32_t value = 0;
};

struct NiStringExtraData {
    std::string name;
    std::string value;
};

// BSFurnitureMarkerNode (nif.xml): where a user of the furniture ends up. Offset in units, heading in radians.
struct FurnitureMarkerPosition {
    glm::vec3 offset{0.0f};
    float heading = 0.0f;
    uint16_t animation = 0;
    uint16_t entry = 0;
};
// NiCamera: only the frustum is kept; the block's transform is what NiAVObject holds.
struct NiCamera : NiAVObject {
    float frustum_left = 0, frustum_right = 0, frustum_top = 0, frustum_bottom = 0, frustum_near = 0, frustum_far = 0;
};

struct BSFurnitureMarker {
    std::string name;
    std::vector<FurnitureMarkerPosition> positions;
};

struct BSBound {
    std::string name;
    glm::vec3 center{0.0f};
    glm::vec3 half_extents{0.0f};
};

struct NiRangeLODData {
    glm::vec3 center{0.0f};
    std::vector<std::array<float, 2>> ranges; // near, far per child
};

// ---- Havok collision (bhk*) -------------------------------------------------------------------------
// Values are in Havok units: k_havok_scale Skyrim units each. Materials are SkyrimHavokMaterial IDs, layers
// SkyrimLayer values; both are kept raw for footsteps and impacts.

// Skyrim's Havok world scale (Skyrim units per Havok unit), as used by NifSkope and the niftools exporters.
inline constexpr float k_havok_scale = 69.99124f;

struct HavokFilter {
    uint8_t layer = 0;           // SkyrimLayer
    uint8_t flags_and_part = 0;  // bit 7 no collision, bit 6 linked group, bits 0-4 part
    uint16_t group = 0;
};

// bhkCollisionObject and its phantom (bhkSPCollisionObject) and ragdoll (bhkBlendCollisionObject) kin.
struct bhkCollisionObject {
    NifRef target = -1;
    uint16_t flags = 0;
    NifRef body = -1;
};

// bhkRigidBody and bhkRigidBodyT. Only T applies its translation and rotation to the shape; plain bodies
// store their export-time pose, which the game ignores.
struct bhkRigidBody {
    NifRef shape = -1;
    HavokFilter filter;
    glm::vec4 translation{0.0f};
    glm::vec4 rotation{0.0f, 0.0f, 0.0f, 1.0f}; // quaternion x y z w
    float mass = 0.0f;
    float friction = 0.5f;
    float restitution = 0.4f;
    glm::vec3 inertia{0.0f}; // the tensor's diagonal
    float linear_damping = 0.0f;
    float angular_damping = 0.0f;
    float max_linear_velocity = 0.0f;
    float max_angular_velocity = 0.0f;
    uint8_t motion_system = 0; // hkMotionType: 6 keyframed, 7 fixed, else dynamic
    uint8_t quality_type = 0;
    std::vector<NifRef> constraints;
    uint16_t body_flags = 0;
};

// bhkRagdollConstraint and bhkLimitedHingeConstraint, radians.
struct bhkJoint {
    bool hinge = false;
    float cone_max = 0.0f;
    float plane_min = 0.0f, plane_max = 0.0f;
    float twist_min = 0.0f, twist_max = 0.0f;
    float hinge_min = 0.0f, hinge_max = 0.0f;
    float max_friction = 0.0f;
};

struct bhkMoppBvTreeShape {
    NifRef shape = -1;
    float shape_scale = 1.0f;
    std::vector<uint8_t> code; // the MOPP byte code: its leaves give the order Havok visits the shape's keys in
};

struct bhkCompressedMeshShape {
    NifRef target = -1;
    float radius = 0.0f;
    glm::vec4 scale{1.0f};
    NifRef data = -1;
};

struct bhkCompressedMeshShapeData {
    struct Material {
        uint32_t material = 0;
        HavokFilter filter;
    };
    struct Transform {
        glm::vec4 translation{0.0f};
        glm::vec4 rotation{0.0f, 0.0f, 0.0f, 1.0f}; // x y z w
    };
    struct BigTriangle {
        std::array<uint16_t, 3> vertices{};
        uint32_t material = 0; // into materials
        uint16_t welding = 0;
    };
    struct Chunk {
        glm::vec4 translation{0.0f};
        uint32_t material = 0; // into materials
        uint16_t transform = 0;
        std::vector<uint16_t> vertices; // x y z per vertex, quantised by `error`
        std::vector<uint16_t> indices;  // the strips first, then a triangle list
        std::vector<uint16_t> strips;   // strip lengths
    };
    uint32_t bits_per_index = 0;   // the winding bit of a shape key sits here
    uint32_t bits_per_w_index = 0; // the chunk number + 1 sits above these bits
    uint32_t w_index_mask = 0;
    uint32_t index_mask = 0;
    float error = 0.001f; // vertex quantisation step
    glm::vec4 aabb_min{0.0f};
    glm::vec4 aabb_max{0.0f};
    std::vector<Material> materials;
    std::vector<Transform> transforms;
    std::vector<glm::vec4> big_vertices;
    std::vector<BigTriangle> big_triangles;
    std::vector<Chunk> chunks;
};

struct bhkConvexVerticesShape {
    uint32_t material = 0;
    float radius = 0.0f;
    std::vector<glm::vec4> vertices;
    std::vector<glm::vec4> planes; // outward normal xyz, w = -distance
};

struct bhkBoxShape {
    uint32_t material = 0;
    float radius = 0.0f;
    glm::vec3 half_extents{0.0f};
};

struct bhkSphereShape {
    uint32_t material = 0;
    float radius = 0.0f;
};

struct bhkCapsuleShape {
    uint32_t material = 0;
    float radius = 0.0f;
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
};

struct bhkCylinderShape {
    uint32_t material = 0;
    float radius = 0.0f; // convex shell
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
    float cylinder_radius = 0.0f;
};

struct bhkListShape {
    std::vector<NifRef> shapes;
    uint32_t material = 0;
    std::vector<HavokFilter> filters;
};

// bhkTransformShape and bhkConvexTransformShape.
struct bhkTransformShape {
    NifRef shape = -1;
    uint32_t material = 0;
    float radius = 0.0f;
    glm::mat4 transform{1.0f}; // child -> parent, translation in Havok units
};

// Skyrim LE: bhkPackedNiTriStripsShape -> hkPackedNiTriStripsData.
struct bhkPackedNiTriStripsShape {
    float radius = 0.0f;
    glm::vec4 scale{1.0f};
    NifRef data = -1;
};

struct hkPackedNiTriStripsData {
    struct SubShape {
        HavokFilter filter;
        uint32_t vertex_count = 0; // sub shapes own consecutive runs of the vertices
        uint32_t material = 0;
    };
    std::vector<std::array<uint16_t, 3>> triangles;
    std::vector<glm::vec3> vertices;
    std::vector<SubShape> sub_shapes;
};

// Oblivion-era collision on NiTriStripsData, still found in a few Skyrim meshes.
struct bhkNiTriStripsShape {
    uint32_t material = 0;
    float radius = 0.0f;
    glm::vec4 scale{1.0f};
    std::vector<NifRef> strips; // NiTriStripsData
    std::vector<HavokFilter> filters;
};

// ---- Particle systems (SE layouts, BS version 100) ----

// NiParticleSystem: NiAVObject, then the SE NiGeometry prefix (bound, skin, shader, alpha), then the
// particle-system fields.
struct NiParticleSystem : NiAVObject {
    glm::vec3 bound_center{0.0f};
    float bound_radius = 0.0f;
    NifRef skin = -1;
    NifRef shader_property = -1;
    NifRef alpha_property = -1;
    BSVertexDesc vertex_desc;
    std::array<uint16_t, 4> lod_distances{}; // far begin, far end, near begin, near end
    NifRef data = -1;
    bool world_space = true;
    std::vector<NifRef> modifiers;
};

// NiPSysData (NiGeometryData + NiParticlesData + NiPSysData as Skyrim streams them).
struct NiPSysData {
    uint16_t max_particles = 0; // "BS Max Vertices"
    uint16_t num_active = 0;
    glm::vec3 bound_center{0.0f};
    float bound_radius = 0.0f;
    bool has_colors = false;
    bool has_radii = false;
    bool has_sizes = false;
    bool has_rotations = false;
    bool has_rotation_angles = false;
    bool has_rotation_axes = false;
    bool has_texture_indices = false;
    bool has_rotation_speeds = false;
    std::vector<glm::vec4> subtexture_offsets;
    float aspect_ratio = 0.0f;
    uint16_t aspect_flags = 0;
    std::array<float, 3> speed_to_aspect{}; // aspect 2, speed 1, speed 2
};

// The NiPSysModifier prefix every modifier and emitter shares.
struct NiPSysModifierBase {
    std::string name;
    uint32_t order = 0;
    NifRef target = -1; // the particle system
    bool active = true;
};

struct NiPSysAgeDeathModifier : NiPSysModifierBase {
    bool spawn_on_death = false;
    NifRef spawn_modifier = -1;
};

struct BSPSysLODModifier : NiPSysModifierBase {
    float begin_distance = 0.0f;
    float end_distance = 0.0f;
    float end_emit_scale = 1.0f;
    float end_size = 1.0f;
};

// NiPSysEmitter fields, then what the volume emitters add: their object and extents.
enum class VolumeShape : uint8_t { sphere, box, cylinder };
struct NiPSysVolumeEmitter : NiPSysModifierBase {
    float speed = 0.0f;
    float speed_variation = 0.0f;
    float declination = 0.0f;
    float declination_variation = 0.0f;
    float planar_angle = 0.0f;
    float planar_angle_variation = 0.0f;
    glm::vec4 initial_color{1.0f};
    float initial_radius = 1.0f;
    float radius_variation = 0.0f;
    float life_span = 0.0f;
    float life_span_variation = 0.0f;
    NifRef emitter_object = -1;
    VolumeShape shape = VolumeShape::sphere;
    float extents[3] = {}; // sphere: radius; box: width, height, depth; cylinder: radius, height
};

// NiPSysEmitter fields, then NiPSysMeshEmitter's.
struct NiPSysMeshEmitter : NiPSysModifierBase {
    float speed = 0.0f;
    float speed_variation = 0.0f;
    float declination = 0.0f;
    float declination_variation = 0.0f;
    float planar_angle = 0.0f;
    float planar_angle_variation = 0.0f;
    glm::vec4 initial_color{1.0f};
    float initial_radius = 1.0f;
    float radius_variation = 0.0f;
    float life_span = 0.0f;
    float life_span_variation = 0.0f;
    std::vector<NifRef> meshes;
    uint32_t velocity_type = 0; // 0 normals, 1 random, 2 direction
    uint32_t emission_type = 0; // 0 vertices, 1 face centre, 2 edge centre, 3 face surface, 4 edge surface
    glm::vec3 emission_axis{1.0f, 0.0f, 0.0f};
};

struct NiPSysSpawnModifier : NiPSysModifierBase {
    uint16_t num_generations = 0;
    float percentage_spawned = 0.0f;
    uint16_t min_spawned = 0;
    uint16_t max_spawned = 0;
    float speed_variation = 0.0f;
    float direction_variation = 0.0f;
    float life_span = 0.0f;
    float life_span_variation = 0.0f;
};

// Field order as streamed.
struct BSPSysSimpleColorModifier : NiPSysModifierBase {
    float fade_in = 0.0f;
    float fade_out = 0.0f;
    float color1_start = 0.0f; // colour 0 -> 1 blend starts (age fraction)
    float color1_end = 0.0f;   // ... and ends
    float color2_start = 0.0f; // colour 1 -> 2 blend starts
    float color2_end = 0.0f;   // ... and ends
    std::array<glm::vec4, 3> colors{};
};

struct NiPSysRotationModifier : NiPSysModifierBase {
    float speed = 0.0f;
    float speed_variation = 0.0f;
    float angle = 0.0f;
    float angle_variation = 0.0f;
    bool random_speed_sign = false;
    bool random_axis = false;
    glm::vec3 axis{1.0f, 0.0f, 0.0f};
};

struct BSPSysSubTexModifier : NiPSysModifierBase {
    float start_frame = 0.0f;
    float start_frame_fudge = 0.0f;
    float end_frame = 0.0f;
    float loop_start_frame = 0.0f;
    float loop_start_frame_fudge = 0.0f;
    float frame_count = 0.0f;
    float frame_count_fudge = 0.0f;
};

struct BSPSysScaleModifier : NiPSysModifierBase {
    std::vector<float> scales;
};

struct NiPSysGravityModifier : NiPSysModifierBase {
    NifRef gravity_object = -1;
    glm::vec3 axis{0.0f, 0.0f, 1.0f};
    float decay = 0.0f;
    float strength = 0.0f;
    uint32_t force_type = 0; // 0 planar, 1 spherical
    float turbulence = 0.0f;
    float turbulence_scale = 1.0f;
    bool world_aligned = false;
};

struct NiPSysDragModifier : NiPSysModifierBase {
    NifRef parent = -1;
    glm::vec3 axis{1.0f, 0.0f, 0.0f};
    float percentage = 0.0f;
    float range = 0.0f;
    float range_falloff = 0.0f;
};

struct NiPSysPositionModifier : NiPSysModifierBase {};

struct NiPSysBoundUpdateModifier : NiPSysModifierBase {
    uint16_t update_skip = 0;
};

// ---- Animation: controllers, interpolators, keys, sequences ----

// Every NiTimeController subclass decoded here; the fields a type lacks keep their defaults.
struct NiTimeController {
    NifRef next = -1;
    uint16_t flags = 0;
    float frequency = 1.0f;
    float phase = 0.0f;
    float start_time = 0.0f;
    float stop_time = 0.0f;
    NifRef target = -1;
    NifRef interpolator = -1;            // NiSingleInterpController
    std::string modifier_name;           // NiPSysModifierCtlr
    NifRef visibility_interpolator = -1; // NiPSysEmitterCtlr
    uint32_t controlled_variable = 0;    // BSEffectShaderProperty{Float,Color}Controller
    std::vector<NifRef> extra_targets;   // NiMultiTargetTransformController
};

// BSNiAlphaPropertyTestRefController: an NiSingleInterpController on an NiAlphaProperty's threshold. Its own type
// (not NiTimeController) so the particle importer's controller chains skip it as before; the materials' own
// animation reads it.
struct BSNiAlphaPropertyTestRefController {
    NiTimeController base; // next .. interpolator; no modifier, no controlled variable
};

struct NiControllerManager {
    NiTimeController base;
    bool cumulative = false;
    std::vector<NifRef> sequences;
    NifRef object_palette = -1;
};

struct NiControlledBlock {
    NifRef interpolator = -1;
    NifRef controller = -1;
    uint8_t priority = 0;
    std::string node_name;
    std::string property_type;
    std::string controller_type;
    std::string controller_id;
    std::string interpolator_id;
};

struct NiControllerSequence {
    std::string name;
    std::vector<NiControlledBlock> blocks;
    float weight = 1.0f;
    NifRef text_keys = -1;
    uint32_t cycle_type = 0; // 0 loop, 1 reverse, 2 clamp
    float frequency = 1.0f;
    float start_time = 0.0f;
    float stop_time = 0.0f;
    NifRef manager = -1;
    std::string accum_root_name;
    std::vector<NifRef> anim_notes;
};

// NiFloatInterpolator, NiBoolInterpolator, NiBoolTimelineInterpolator, NiBlendFloatInterpolator,
// NiBlendBoolInterpolator.
struct NiInterpolator {
    float float_value = -3.402823466e+38f; // -FLT_MAX: no value
    uint8_t bool_value = 2;                // 2 = no value
    NifRef data = -1;
    // NiBlendInterpolator
    uint8_t blend_flags = 0; // bit 0: manager controlled
    uint8_t array_size = 0;
    float weight_threshold = 0.0f;
};

struct NiFloatKey {
    float time = 0.0f;
    float value = 0.0f;
    float forward = 0.0f;  // quadratic: in tangent
    float backward = 0.0f; // quadratic: out tangent
    std::array<float, 3> tbc{};
};

struct NiFloatData {
    uint32_t key_type = 0; // 1 linear, 2 quadratic, 3 TBC, 5 constant
    std::vector<NiFloatKey> keys;
};

// NiTransformInterpolator: the pose it holds when it has no keys (-FLT_MAX components: none), and its keys.
struct NiTransformInterpolator {
    glm::vec3 translation{-3.402823466e+38f};
    glm::vec4 rotation{-3.402823466e+38f}; // quaternion w, x, y, z
    float scale = -3.402823466e+38f;
    NifRef data = -1; // NiTransformData
};

struct NiVec3Key {
    float time = 0.0f;
    glm::vec3 value{0.0f};
    glm::vec3 forward{0.0f};  // quadratic: in tangent
    glm::vec3 backward{0.0f}; // quadratic: out tangent
    std::array<float, 3> tbc{};
};

struct NiQuatKey {
    float time = 0.0f;
    glm::vec4 value{1.0f, 0.0f, 0.0f, 0.0f}; // w, x, y, z
    std::array<float, 3> tbc{};
};

// NiTransformData (NiKeyframeData): rotation keys as quaternions or, for key type 4 (XYZ), three float key
// groups of Euler angles in radians; translation keys; scale keys.
struct NiTransformData {
    uint32_t rotation_type = 0; // 0 none, 1 linear, 2 quadratic, 3 TBC, 4 XYZ, 5 constant
    std::vector<NiQuatKey> rotation_keys;
    std::array<NiFloatData, 3> xyz;
    uint32_t translation_type = 0;
    std::vector<NiVec3Key> translation_keys;
    NiFloatData scales;
};

struct NiBoolData {
    uint32_t key_type = 0;
    std::vector<std::pair<float, uint8_t>> keys;
};

struct NiTextKeyExtraData {
    std::string name;
    std::vector<std::pair<float, std::string>> keys;
};

struct NiDefaultAVObjectPalette {
    NifRef scene = -1;
    std::vector<std::pair<std::string, NifRef>> objects;
};

struct BSBehaviorGraphExtraData {
    std::string name;
    std::string graph;
    bool controls_base_skeleton = false;
};

using NifBlockData =
    std::variant<std::monostate, NiNode, BSTriShape, NiGeometry, NiGeometryData, NiSkinInstance, NiSkinData,
                 NiSkinPartition, BSLightingShaderProperty, BSEffectShaderProperty, BSSkyShaderProperty, BSShaderTextureSet,
                 NiAlphaProperty, NiIntegerExtraData, NiStringExtraData, BSBound, BSFurnitureMarker, NiRangeLODData, NiParticleSystem,
                 NiPSysData, NiPSysAgeDeathModifier, BSPSysLODModifier, NiPSysMeshEmitter, NiPSysVolumeEmitter,
                 NiPSysSpawnModifier,
                 BSPSysSimpleColorModifier, NiPSysRotationModifier, BSPSysSubTexModifier, BSPSysScaleModifier,
                 NiPSysGravityModifier, NiPSysDragModifier, NiPSysPositionModifier, NiPSysBoundUpdateModifier,
                 NiTimeController, NiControllerManager, NiControllerSequence, NiInterpolator, NiFloatData,
                 NiBoolData, NiTextKeyExtraData, NiDefaultAVObjectPalette, BSBehaviorGraphExtraData,
                 NiCamera, NiTransformInterpolator, NiTransformData, BSNiAlphaPropertyTestRefController, NiPoint3Interpolator,
                 NiPosData, BSMultiBound, BSMultiBoundShape, bhkCollisionObject, bhkRigidBody, bhkMoppBvTreeShape,
                 bhkCompressedMeshShape, bhkCompressedMeshShapeData, bhkConvexVerticesShape, bhkBoxShape,
                 bhkJoint, bhkSphereShape, bhkCapsuleShape, bhkCylinderShape, bhkListShape, bhkTransformShape,
                 bhkPackedNiTriStripsShape, hkPackedNiTriStripsData, bhkNiTriStripsShape>;

enum class NifBlockStatus : uint8_t {
    parsed,   // typed data is valid
    opaque,   // type not decoded by this reader
    mismatch, // decoding did not consume exactly the declared size; treated as opaque
};

struct NifBlock {
    uint16_t type = 0; // index into the game
    NifBlockStatus status = NifBlockStatus::opaque;
    uint32_t offset = 0;
    uint32_t size = 0;
    std::string error; // why a mismatch happened
    NifBlockData data;
    // Every NiTimeController starts with its "next controller" link; it is kept even for controller types
    // this reader does not decode, so a controller chain can be followed past them.
    NifRef next_controller = -1;
};

struct NifFile {
    NifHeader header;
    std::vector<NifBlock> blocks;
    std::vector<NifRef> roots;

    bool valid(NifRef ref) const { return ref >= 0 && static_cast<size_t>(ref) < blocks.size(); }
    std::string_view type_name(NifRef ref) const;

    // The block's typed data if `ref` is valid, parsed and of type T; nullptr otherwise.
    template <typename T>
    const T* get(NifRef ref) const {
        return valid(ref) ? std::get_if<T>(&blocks[static_cast<size_t>(ref)].data) : nullptr;
    }

    // The NiAVObject part of a node or shape block, or nullptr.
    const NiAVObject* av_object(NifRef ref) const;
};

// Parses a whole file. Throws ParseError when the header or block table is malformed or the version is
// not Skyrim's; problems inside individual blocks are recorded in the game instead.
NifFile read_nif(std::span<const std::byte> data);

// True for the block types read_nif decodes into NiNode.
bool is_nif_node_type(std::string_view type);

} // namespace spud::legacy
