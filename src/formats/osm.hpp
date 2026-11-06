#pragma once

// .osm — spud static mesh. Spec: docs/formats/osm.md
//
// A mesh is a flat list of submeshes in mesh space (node transforms are baked
// in at import). Materials keep the Skyrim lighting-model parameters as data so
// the renderer can reproduce Skyrim's shading for imported content (V1).

#include "formats/container.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::osm {

inline constexpr container::FourCC k_kind = container::fourcc("MESH");
inline constexpr uint32_t k_version = 11;
inline constexpr uint32_t k_min_version = 9; // version 9 files read as they are: only skinned models need the skin chunks

// Skyrim's own vertex data, kept bit for bit so shading matches the game exactly.
// The tangent frame is packed as the game packs it: unorm8 components decoded as 2v - 1, with the bitangent
// spread over position.w (float), normal.w and tangent.w.
struct Vertex {
    float position[3]; // metres, in the submesh's shape space (Submesh::transform maps it into mesh space)
    float bitangent_x;
    float uv[2];
    uint8_t normal[4];  // unorm8 xyz, w = bitangent y
    uint8_t tangent[4]; // unorm8 xyz, w = bitangent z
    uint8_t color[4];   // rgba8, (255,255,255,255) when the source has none
};
static_assert(sizeof(Vertex) == 36);

enum class ShaderModel : uint32_t {
    lighting = 0, // Skyrim BSLightingShaderProperty
    effect = 1,   // Skyrim BSEffectShaderProperty
};

enum MaterialFlag : uint32_t {
    material_alpha_test = 1u << 0,
    material_alpha_blend = 1u << 1,
    material_two_sided = 1u << 2,
    material_decal = 1u << 3,
    material_vertex_colors = 1u << 4,
    material_vertex_alpha = 1u << 5,
    material_z_write = 1u << 6,
    material_z_test = 1u << 7,
    material_tree_anim = 1u << 8,
    material_glow = 1u << 9,
    material_env_map = 1u << 10,
    material_model_space_normals = 1u << 11,
};

// Texture slots follow Skyrim's BSShaderTextureSet order so nothing is lost.
// For the effect model, slot 0 is the source texture and slot 1 the greyscale palette.
enum TextureSlot : uint32_t {
    texture_diffuse = 0,
    texture_normal = 1,
    texture_glow = 2,  // glow / skin / hair layer
    texture_height = 3, // parallax
    texture_environment = 4,
    texture_env_mask = 5,
    texture_inner_layer = 6, // multilayer parallax / tint
    texture_backlight = 7,   // backlight / specular / subsurface
    k_texture_slots = 8,
};

// Plain-old-data part of a material, stored verbatim on disk.
struct MaterialParams {
    ShaderModel model = ShaderModel::lighting;
    uint32_t skyrim_shader_type = 0;
    uint32_t skyrim_flags1 = 0;      // SkyrimShaderPropertyFlags1
    uint32_t skyrim_flags2 = 0;      // SkyrimShaderPropertyFlags2
    uint32_t flags = material_z_write | material_z_test;
    uint16_t blend_src = 6; // NiAlphaProperty blend modes (6 = SRC_ALPHA)
    uint16_t blend_dst = 7; //                              (7 = INV_SRC_ALPHA)
    uint8_t alpha_threshold = 128;
    uint8_t alpha_func = 4; // NiAlphaProperty test function (4 = GREATER)
    uint8_t texture_clamp = 3; // 0 clamp/clamp, 1 clamp/wrap, 2 wrap/clamp, 3 wrap/wrap
    uint8_t lighting_influence = 255; // effect model: LightingInfluence x 255 (a NIF byte of 0 keeps 255)
    float alpha = 1.0f;
    float uv_offset[2] = {0, 0};
    float uv_scale[2] = {1, 1};
    float emissive_color[3] = {0, 0, 0};
    float emissive_multiple = 1.0f;
    float specular_color[3] = {1, 1, 1};
    float specular_strength = 1.0f;
    float glossiness = 80.0f;
    float refraction_strength = 0.0f;
    float lighting_effect1 = 0.3f; // soft lighting / rim
    float lighting_effect2 = 2.0f; // rim power / back lighting
    float env_map_scale = 1.0f;
    float type_params[8] = {};     // shader-type-specific values, see spec
    // effect model
    float base_color[4] = {1, 1, 1, 1};
    float base_color_scale = 1.0f;
    float falloff[4] = {1, 0, 1, 0}; // start angle, stop angle, start opacity, stop opacity
    float soft_falloff_depth = 0.0f;
    uint32_t reserved1[3] = {};
};

// What a shader-property controller animates. Colour controllers are stored as one
// controller per component.
enum MaterialVariable : uint8_t {
    uv_offset_u = 0, uv_offset_v = 1, uv_scale_u = 2, uv_scale_v = 3, // shown one frame late (16.5)
    var_alpha = 4,                 // lighting: material alpha
    var_emissive_multiple = 5,     // lighting
    var_specular_strength = 6,     // lighting
    var_glossiness = 7,            // lighting
    var_env_map_scale = 8,         // lighting: EnvmapData.x
    var_refraction_strength = 9,   // lighting
    var_emissive_r = 10, var_emissive_g = 11, var_emissive_b = 12, // lighting colour 1
    var_specular_r = 13, var_specular_g = 14, var_specular_b = 15, // lighting colour 0
    var_base_color_scale = 16,     // effect
    var_falloff_start = 17,        // effect: degrees (the material stores the cosine)
    var_falloff_stop = 18,         // effect: degrees
    var_falloff_start_opacity = 19, // effect
    var_falloff_stop_opacity = 20, // effect
    var_base_alpha = 21,           // effect: base colour alpha, max(value, 0)
    var_base_r = 22, var_base_g = 23, var_base_b = 24, // effect colour 0
    var_alpha_test_ref = 25,       // NiAlphaProperty threshold: the value clamped to [0, 255], truncated
    k_material_variable_count = 26,
};
struct MaterialController {
    uint8_t variable = uv_offset_u; // MaterialVariable
    uint8_t key_type = 1;           // nif.xml KeyType: 1 linear, 2 quadratic, 3 TBC, 5 constant
    uint16_t flags = 0;             // NiTimeController flags: bits 1-2 cycle type, bit 3 active
    float frequency = 1.0f;
    float phase = 0.0f;
    float start = 0.0f;
    float stop = 0.0f;
    float value = 0.0f;             // the interpolator's value when it has no keys
    std::vector<std::array<float, 5>> keys; // time, value, then forward/backward tangents or tension/bias/continuity
};

struct Material {
    MaterialParams params;
    std::array<std::string, k_texture_slots> textures; // normalised cache paths ("textures/...dds"), "" if unused
    std::vector<MaterialController> controllers;
};

inline constexpr uint32_t k_no_billboard = 0xFFFFFFFFu;

struct Submesh {
    uint32_t first_index;
    uint32_t index_count;
    uint32_t base_vertex;
    uint32_t vertex_count;
    uint32_t material;
    uint32_t billboard_mode = k_no_billboard; // under an NiBillboardNode: its mode (nif.xml BillboardMode)
    uint32_t reserved[2];
    // Shape space -> mesh space, rows of [R * scale | t] (metres). NIF node transforms stay out of the
    // vertices so the packed tangent frame keeps the game's exact bytes. Under a billboard: shape space -> the
    // billboard node's space.
    float transform[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    // Under a billboard: the billboard node's frame in mesh space, rows of [R * scale | t] (metres), as the node's
    // parent chain and own transform place it before the engine re-orients it.
    float billboard[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
};
static_assert(sizeof(Submesh) == 128);

// A skinned model's data (docs/formats/osm.md, "Skinned models"). A vertex's four bones index its shape's bones.
struct SkinVertex {
    uint16_t weight[4]; // IEEE half floats, as the game's vertices store them
    uint8_t bone[4];
};
static_assert(sizeof(SkinVertex) == 12);

struct SkinBone {
    std::string name;        // the skeleton node it follows
    float skin_to_bone[3][4]; // skin space -> bone space, rows of [R * scale | t] (metres)
};

// Parallel to Mesh::submeshes.
struct SkinShape {
    uint32_t first_bone = 0;
    uint32_t bone_count = 0;
    uint32_t first_part = 0; // into Skin::parts: the shape's dismember partitions
    uint32_t part_count = 0;
};

struct Skin {
    std::vector<SkinVertex> vertices; // parallel to Mesh::vertices
    std::vector<SkinBone> bones;
    std::vector<SkinShape> shapes;
    std::vector<std::string> shape_names; // parallel to `shapes`; empty before version 11
    std::vector<uint16_t> parts;      // BSDismemberSkinInstance body parts
    // Parallel to `parts`: the indices of each partition, in order from the submesh's first index. Empty for a shape
    // whose triangles aren't kept by partition.
    std::vector<uint32_t> part_indices;
    // The light variant of a weight-morphed model, parallel to Mesh::vertices; empty when the model has none.
    std::vector<Vertex> morph;
    std::string attach; // the skeleton node the model's Prn names, "" without one
};

// A simplified level of a submesh: more of Mesh::indices over the same vertices.
inline constexpr uint32_t k_lod_levels = 3;
struct SubmeshLod {
    uint32_t first_index = 0;
    uint32_t index_count = 0; // 0: this level adds nothing
    float error = 0.0f;       // metres in mesh space: how far it strays from the full shape
};
static_assert(sizeof(SubmeshLod) == 12);

struct Mesh {
    float bounds_min[3] = {0, 0, 0};
    float bounds_max[3] = {0, 0, 0};
    // The root node's bound sphere as the original engine merges it from the model's nodes and shapes (centre in
    // mesh space, radius; metres): what LOD fades and light reach measure from.
    // Radius 0: none (the engine falls back to the box).
    float fade_bound[4] = {0, 0, 0, 0};
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // relative to the submesh's base_vertex
    std::vector<Submesh> submeshes;
    std::vector<SubmeshLod> lods; // k_lod_levels per submesh, or empty
    std::vector<Material> materials;
    Skin skin; // empty for static models
};

// Recomputes bounds_min/max from the vertices, through each submesh's transform.
void compute_bounds(Mesh& mesh);

std::vector<std::byte> write(const Mesh& mesh);
Mesh read(std::span<const std::byte> file);

// The model of a destructible at one of its BSDamageStage stages, beside `mesh` (a cache path ending in .osm).
inline std::string damage_stage_path(std::string_view mesh, int stage) {
    return std::string(mesh.substr(0, mesh.size() - 4)) + ".stage" + std::to_string(stage) + ".osm";
}

} // namespace spud::osm
