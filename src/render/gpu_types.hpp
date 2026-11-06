#pragma once

// Structures shared with the shaders (scalar layout, see shaders/common.glsl).
// Keep both sides in sync.

#include <cstdint>

#include <glm/glm.hpp>

namespace spud::render {

inline constexpr uint32_t k_shadow_light_slots = 4;

// A far terrain node this frame.
struct GpuFarNode {
    int32_t column;     // first heightfield sample
    int32_t row;
    uint32_t step;      // samples between grid vertices
    float skirt;        // metres the node's edges drop
    float morph_start;  // metres from the camera where it starts morphing into its parent
    float morph_scale;  // 1 / the morph's length, 0 at the root
    float pad[2];
};
static_assert(sizeof(GpuFarNode) == 32);

// Where the land's heightfield and macro maps lie (GpuFrame::far_land).
struct GpuFarLand {
    glm::uvec4 maps{0};   // colour, normal, noise slots, heightfield width in samples
    glm::vec4 map{0};     // world xy -> macro uv: scale x, y, offset x, y
    glm::vec4 samples{0}; // world xy of sample (0, 0), metres per sample, metres of height value 0
    glm::vec4 ring{0};    // the full-resolution ring: fade start, 1 / length; morph start, 1 / length (metres)
    glm::vec4 grass{0};   // fade start, fade length (clip-space units), wind amplitude
    glm::vec4 ambient{0}; // the weather's Ambient colour, which grass takes unscaled
    uint64_t heights = 0; // the heightfield: u16 samples, two per word
    uint64_t pad = 0;
};

// A GRAS form's draw constants (shaders/grass.glsl).
struct GpuGrassForm {
    uint32_t material;
    float wave_period;
    float uniform;
    float pad;
};

// An open door between two spaces, in the frame of one of them: light from
// the other side comes in through it, and past its plane a leaf takes that side's ambient light.
inline constexpr uint32_t k_door_lights = 4;
struct GpuDoorLight {
    glm::vec4 position;   // xyz camera-relative, the middle of the opening; w the light's reach (m)
    glm::vec4 normal;     // xyz into this space; w how open, 0..1
    glm::vec4 colour;     // what comes through, wide open
    glm::vec4 ambient[3]; // the other side's directional ambient, in this space's axes
};

struct GpuFrame {
    glm::mat4 view_proj;     // camera-relative: projection * rotation
    glm::mat4 inv_view_proj;
    glm::vec4 camera_pos;    // xyz metres in the drawn space, w = time (s)
    glm::vec4 sun_dir;       // xyz towards the sun (world space)
    glm::vec4 sun_color;     // rgb, w = sun disc intensity
    glm::vec4 ambient_sky;   // placeholder sky and water only
    glm::vec4 ambient_ground;
    glm::vec4 sky_zenith;
    glm::vec4 sky_horizon;
    glm::vec4 sky_ground;    // below-horizon haze
    glm::vec4 fog;           // placeholder sky and water: x start m, y end m, z max opacity, w power
    glm::vec4 water_color;   // rgb, a = base opacity
    glm::vec4 params;        // x terrain repeats per cell, y 1 / cell size, z normal-map green sign, w the lit limit
    glm::uvec4 flags;        // x debug view, y lighting off, z MSAA samples, w specular AA
    // Skyrim's lighting inputs, as its shaders receive them, in world space.
    glm::vec4 dir_light_color;    // DirLightColor (rgb)
    glm::vec4 dir_ambient[3];     // DirectionalAmbient rows: ambient = M * (N, 1)
    glm::vec4 amb_spec;           // AmbientSpecularTintAndFresnelPower
    glm::vec4 ibl;                // IBLParams
    glm::vec4 fog_param;          // FogParam (vertex)
    glm::vec4 fog_near_color;     // FogNearColor (vertex)
    glm::vec4 fog_far_color;      // FogFarColor (vertex)
    glm::vec4 fog_color;          // FogColor (pixel)
    glm::vec4 output_clamp;       // ColourOutputClamp
    glm::vec4 frame_params;       // the renderer's per-frame parameters (Skyrim cb12[42])
    glm::mat4 fog_view_proj;      // Skyrim's camera-relative view-projection: the fog distance is |clip.xyz|
    glm::vec4 clock;              // x the controllers' clock one frame back (UV values, section 16.3), yzw unused
    glm::vec4 effect_light;       // effect lighting: the weather's Effect Lighting colour x Sunlight Scale
    glm::vec4 clip_plane;         // View::clip_plane
    glm::vec4 depth_params;       // x near plane (m) of the reverse-Z projection, y 1 when the depth copy is valid
    glm::uvec4 projected_textures; // ProjectedNoise, ProjectedDiffuse, ProjectedNormal, ProjectedNormalDetail
    glm::vec4 projected_params;   // ProjectedUVParams3: diffuse/normal tiling, detail tiling, 0, textured
    glm::vec4 snow_rim;           // SnowRimLightParameters: intensity, geometry power, normal power, enabled
    glm::vec4 camera_right;       // view basis (world), for the refraction view normal
    glm::vec4 camera_up;
    glm::vec4 lod_fade;           // x: LOD fade metric per metre (A / M / 7200 / unit), y: viewport width, z: height
    glm::vec4 fade_spec_env;      // specular start, end, environment map start, end (section 2.2)
    glm::vec4 fade_decal_refr;    // decal start, end, refraction start, end
    glm::vec4 tree;               // leaf dampening start, end (metres), wind magnitude, wave time scale
    glm::vec4 lod_fade_types;     // LOD fade metric per metre: items, sky cell, visible when distant
    // The sun's shadow mask: cascade c's u, v and depth rows at [3c, 3c + 2], for
    // camera-relative points in metres.
    glm::vec4 shadow_rows[9];
    glm::vec4 shadow_split;       // view depth (m) where cascades 0, 1 and the last end; w the cascade count
    glm::vec4 shadow_params;      // x blend start (view depth, m), y bias of cascade 0, z of the others, w 1 / fade^2
    glm::vec4 shadow_filter;      // x the 16 taps' radius (map UV)
    glm::uvec4 shadow_info;       // x cascade count (0: no sun shadows), y the cascades' bindless slot, z the lights',
                                  // w the shadow lights this frame
    // The shadow lights: per light 4 rows, then type (0 spot, 1 hemisphere, 2 omni), map layer, bias,
    // radius or falloff exponent.
    glm::vec4 shadow_light_rows[k_shadow_light_slots * 4];
    glm::vec4 shadow_light_params[k_shadow_light_slots];
    glm::vec4 shadow_light_fade;  // per light: 1 / fade distance^2
    GpuFarLand far_land;
    // Doors standing open onto another space here (GpuDoorLight).
    glm::uvec4 door_info; // x how many
    GpuDoorLight doors[k_door_lights];
};

// Texture slots of GpuMaterial::tex, named after the game's shader registers.
enum GpuTextureSlot : uint32_t {
    gtex_diffuse = 0,  // t0
    gtex_normal = 1,   // t1
    gtex_specular = 2, // t2: model-space normals' specular map
    gtex_height = 3,   // t3: parallax height / face detail
    gtex_env = 4,      // t4: environment cube map (a cube descriptor)
    gtex_env_mask = 5, // t5
    gtex_glow = 6,     // t6
    gtex_inner = 7,    // t8: multilayer parallax inner layer
    gtex_back = 8,     // t9: back-lighting mask
    gtex_rim_soft = 9, // t12: rim / soft-lighting mask
    k_gtex_count = 12,
};

struct GpuMaterial {
    uint32_t technique;    // Skyrim lighting technique (type << 24 | feature bits); effect: 0
    uint32_t flags;        // osm::MaterialFlag | k_material_effect_bit
    uint32_t alpha_func;   // NiAlphaProperty test function
    float alpha_threshold; // 0..1
    uint32_t tex[k_gtex_count];
    uint32_t sampler;
    uint32_t effect_flags; // k_effect_*
    uint32_t palette;      // effect: greyscale palette texture
    uint32_t uv_anim;      // index into this frame's GpuMaterialAnim[], ~0u when no controller animates it
    float uv[4];           // TexcoordOffset: offset xy, scale zw
    float specular[4];     // SpecularColor: rgb = colour * strength, w = glossiness
    float emit[4];         // EmitColor rgb (colour * multiple), w = material alpha
    float effect[4];       // LightingEffectParams xy, EnvmapData xy (scale, has mask)
    float tint[4];         // TintColor
    float layer[4];        // MultiLayerParallaxData
    float sparkle[4];      // SparkleParams
    float extra[4];        // x multilayer env strength, y 1 on a skinned model's variant (posed by skin.comp), zw unused
    float xform[3][4];     // submesh: shape space -> mesh space, rows of [R * s | t]
    float base_color[4];   // effect: colour * scale
    float falloff[4];      // effect: start cos, stop cos, start opacity, stop opacity
    float projected[4];    // ProjectedUVParams: (1 - c) scale, -, 1 / noise UV scale, (1 - c) bias + c
    float projected_color[4]; // ProjectedUVParams2: the single-pass colour; multi pass: normal dampener, 1 / UV scale
    float projection[3][4];   // TextureProj rows (world space): projected UV = (P . row 0, P . row 1), row 2 = "up"
    float billboard[3][4];    // under an NiBillboardNode: its frame in mesh space (xform is then below it)
    uint32_t billboard_mode;  // nif.xml BillboardMode (the engine uses & 7), ~0u = none
    uint32_t pad1[3];
};
static_assert(sizeof(GpuMaterial) == 432);

// The values a material's controllers animate, evaluated on the CPU each frame; they
// replace the GpuMaterial fields of the same names.
struct GpuMaterialAnim {
    float uv[4];
    float specular[4];
    float emit[4];
    float effect[4];
    float base_color[4];
    float falloff[4];
    float extra[4];
    float alpha_threshold;
    float pad[3];
};
static_assert(sizeof(GpuMaterialAnim) == 128);

inline constexpr uint32_t k_material_effect_bit = 1u << 31;
inline constexpr uint32_t k_material_refraction = 1u << 30;      // flags1 bit 15 or 2: drawn only as refraction
inline constexpr uint32_t k_material_fire_refraction = 1u << 29; // flags1 bit 16: strength x N.V
inline constexpr uint32_t k_material_refraction_clamp = 1u << 28; // flags1 bit 13: the clamped variant
inline constexpr uint32_t k_material_no_fade = 1u << 27;         // flags2 bit 3: no LOD fades
inline constexpr uint32_t k_material_external_emittance = 1u << 26; // flags1 bit 29 (10.4.6)
inline constexpr uint32_t k_material_billboard = 1u << 25;          // under an NiBillboardNode (billboard_mode)
inline constexpr uint32_t k_material_receive_shadows = 1u << 23; // takes the shadow mask
inline constexpr uint32_t k_material_no_decal_fade = 1u << 24;     // flags2 bit 3 or 9: no decal fade (2.2.6)
inline constexpr uint32_t k_effect_falloff = 1u << 0;
inline constexpr uint32_t k_effect_palette_color = 1u << 1;
inline constexpr uint32_t k_effect_palette_alpha = 1u << 2;
inline constexpr uint32_t k_effect_membrane = 1u << 3; // an effect over a lighting mesh

// Persistent per-reference data, written once when a cell loads: absolute transform rows (R * scale | translation)
// and the reference's point-light list. The defaults are an instance with no point lights, no leaf animation and
// no flags (the UI's scenes and backdrop set only the rows and the sphere).
struct GpuInstanceData {
    glm::vec4 rows[3]{};
    // Point lights reaching the reference: 8 u16 frame light indices, 0xFFFF = none (LightGrid).
    uint32_t lights[4] = {~0u, ~0u, ~0u, ~0u};
    glm::vec4 sphere{0.0f};     // world bound sphere (metres): the fade node's bound for the LOD fade metric
    uint32_t form_id = 0;       // the reference (seeds per-reference phases)
    float leaf_amplitude = 0.0f; // tree leaf animation (section 12): the TREE's CNAM values; 0 = no leaf-anim node
    float leaf_frequency = 0.0f;
    uint32_t flags = 0; // k_instance_*; bits 8-31 the emittance colour (r, g, b unorm8) with k_instance_emittance
};
static_assert(sizeof(GpuInstanceData) == 96);

inline constexpr uint32_t k_instance_leaf_anim = 1u << 0; // the model's root is a leaf-animation node
inline constexpr uint32_t k_instance_emittance = 1u << 1; // an emittance source colours External_Emittance shapes
inline constexpr uint32_t k_instance_sky_fade = 1u << 5;  // a weather's sky model: leaf_amplitude holds its fade
// A sky static: listed by the current, the outgoing weather. Alpha T x the first + (1 - T) x
// the second, T in GpuFrame::clock.y.
inline constexpr uint32_t k_instance_static_current = 1u << 6;
inline constexpr uint32_t k_instance_static_outgoing = 1u << 7;
// Bits 2-4: the fade node's category, which scales its LOD fade metric.
enum class InstanceFade : uint32_t {
    objects = 0,
    items = 1,
    sky_cell = 2,
    visible_when_distant = 3,
    never = 4,
};
inline constexpr uint32_t k_instance_fade_shift = 2;
inline constexpr uint32_t k_instance_fade_mask = 7u << k_instance_fade_shift;

// The emittance colour packed into GpuInstanceData::flags (bits 8-31).
inline uint32_t pack_emittance(const float c[3]) {
    uint32_t v = 0;
    for (int i = 0; i < 3; ++i) {
        const float x = c[i] < 0.0f ? 0.0f : (c[i] > 1.0f ? 1.0f : c[i]);
        v |= static_cast<uint32_t>(x * 255.0f + 0.5f) << (8 + 8 * i);
    }
    return v | k_instance_emittance;
}

// Per drawn instance of a submesh in this frame: which instance and material.
struct GpuDrawInstance {
    uint32_t instance;
    uint32_t material;
};

// The game's land vertex, positions in metres.
struct TerrainVertex {
    float position[3];   // metres, absolute in the space
    float tangent_x;     // (T.x + 1) / 2: stored like this by the game and read undecoded
    uint8_t normal[4];   // record normal + 128 (unorm8), w = T.y packed
    uint8_t binormal[4]; // B packed, w = T.z packed
    uint8_t color[4];
    uint8_t weight[6];   // texture slots 0..5 of the quadrant; slot 0 = clamp(255 - sum of the others)
    uint8_t grid[2];     // column, row within the quadrant (uv = grid * 0.75)
    uint32_t quad;       // index into the quadrant table
};
static_assert(sizeof(TerrainVertex) == 40);

struct GpuTerrainQuad {
    uint32_t base;      // land-texture table index
    uint32_t count;     // layers used (<= 8)
    uint32_t layers[8];
    uint32_t lights[4]; // point lights reaching the quadrant, as GpuInstanceData::lights
};
static_assert(sizeof(GpuTerrainQuad) == 56);

// A placed point light this frame, indexed like World::lights.
struct GpuPointLight {
    glm::vec4 position; // xyz camera-relative (metres), w radius (metres)
    glm::vec4 color;    // rgb = colour x dimmer x distance fade (PointLightColor), w = 1 lit, 0 culled
};
static_assert(sizeof(GpuPointLight) == 32);

struct GpuLandTexture {
    uint32_t diffuse;
    uint32_t normal;
    float specular_power; // LTEX SNAM
    float is_snow;        // 1 for snow land textures
};

struct PushConstants {
    uint64_t frame;
    uint64_t draw_instances;
    uint64_t instance_table;
    uint64_t materials;
    uint64_t terrain_quads;
    uint64_t land_textures;
    uint64_t terrain_vertices; // TerrainVertex[] (vertex pulling)
    uint64_t terrain_cells;    // uint first-vertex per drawn cell, or GpuFarNode, indexed by gl_InstanceIndex
    uint64_t lights;           // GpuPointLight[] this frame
    uint64_t material_anims;   // GpuMaterialAnim[] this frame
    uint64_t post;             // GpuPost this frame
    uint64_t water;            // GpuWater this frame
    uint64_t particles;        // GpuParticles this frame
    uint32_t mode;
    uint32_t pad;
    uint32_t pass[4];          // image-space passes: source slots, then the target's width | height << 16
};
static_assert(sizeof(PushConstants) == 128);

// Water.
struct GpuWaterMaterial {
    glm::vec4 c[14]; // the game's PerMaterial constants
    glm::vec4 uv_offset[2];
    glm::vec4 uv_scale;
    glm::uvec4 noise;
};
static_assert(sizeof(GpuWaterMaterial) == 288);

struct GpuWaterInstance {
    glm::vec4 origin;
    glm::vec4 flow;
    glm::vec4 flow_cell;
    glm::uvec4 info;
};
static_assert(sizeof(GpuWaterInstance) == 64);

// A vertex of placed water: world metres, the texture coordinate, the material.
struct GpuWaterVertex {
    glm::vec4 position; // w: osw::WaterVertex::depth
    glm::vec2 uv;
    uint32_t material;
    uint32_t vertex_uv; // 1: the noise follows uv
    glm::vec4 plane;    // its shape's: n . p = w, metres
};
static_assert(sizeof(GpuWaterVertex) == 48);

struct GpuWater {
    glm::mat4 previous_view_proj;
    glm::vec4 camera_z;
    glm::vec4 screen;
    glm::vec4 camera_forward;
    glm::vec4 sun_dir_range;
    glm::vec4 sun_color;
    glm::vec4 eye;
    glm::vec4 params;
    glm::vec4 default_normal;
    glm::vec4 quad_rect;
    glm::vec4 ripple;
    glm::vec4 ripple_segments[8];
    glm::vec4 light_positions[6];
    glm::vec4 light_colors[6];
    glm::vec4 light_count;
    glm::uvec4 slots;
    glm::uvec4 slots2;
    glm::uvec4 slots3;
    uint64_t materials;
    uint64_t instances;
    uint64_t placed;
};

// World particles.
struct GpuParticle {
    glm::vec4 position;    // xyz world metres, w rotation
    glm::vec4 size_system; // x half extent (metres), y the system's index as float bits
    glm::vec4 color;
    glm::vec4 uv;          // u offset, u size, v offset, v size
};
static_assert(sizeof(GpuParticle) == 64);

struct GpuParticleSystem {
    glm::uvec4 info;       // x texture, y technique, z palette
    glm::vec4 base_color;
    glm::vec4 uv;          // offset, scale
    glm::vec4 params;      // x soft falloff depth, y base colour scale, z lighting influence
};
static_assert(sizeof(GpuParticleSystem) == 64);

struct GpuParticles {
    uint64_t particles;
    uint64_t systems;
};

// A box decal. Camera-relative, so built each frame.
struct GpuDecal {
    glm::vec4 position;  // xyz metres, w the angle test's cosine
    glm::vec4 axes[3];   // xyz the half extents as vectors: width, height, depth; w 1 / their length squared
    glm::vec4 color;     // rgb the DODT's, a the fade
    glm::uvec4 textures; // diffuse, normal (0: the effect shader, unlit), glow (0: none), flags
    glm::uvec4 lights;   // as an instance's
    glm::vec4 params;    // x shininess, y uv scale, zw uv offset
};
static_assert(sizeof(GpuDecal) == 128);
inline constexpr uint32_t k_decal_alpha_blend = 1u << 0, k_decal_alpha_test = 1u << 1, k_decal_specular = 1u << 2;

// skin_decal.comp's. A slot is a VkDrawIndirectCommand, then its vertices (arena index, decal x, y).
struct SkinDecalPushConstants {
    uint64_t vertices, indices, slot;
    glm::vec4 hit, axis_x, axis_y, direction;
    uint32_t first_index, triangles;
    int32_t vertex_offset;
    uint32_t capacity;
};
inline constexpr uint32_t k_skin_decal_stride = 16 + 12 * 3072;

// A splat of the screen blood, screen fractions.
struct GpuSplat {
    glm::vec4 rect;  // centre, half size in x, in y
    glm::vec4 flare; // the same for its flare
    glm::vec4 uv;    // the atlas quadrant's corner, opacity, the flare's
};
static_assert(sizeof(GpuSplat) == 48);

// One sky draw (SkyDraw), packed for the shaders.
struct GpuSkyDraw {
    glm::mat4 world;
    glm::vec4 color[3];
    glm::vec4 uv_offset;
    glm::uvec4 info;  // technique, texture 0, texture 1
    glm::vec4 params; // PParams.x, depth, PParams.y
};
static_assert(sizeof(GpuSkyDraw) == 160);

// One precipitation emitter (Precip in shaders/common.glsl).
struct GpuPrecip {
    glm::vec4 box;
    glm::vec4 shape;
    glm::vec4 scroll;
    glm::vec4 forward;
    glm::vec4 velocity;
    glm::vec4 instance[10];
    glm::uvec4 info;
};

// The volumetric volume's parameters (shaders/vl_volume.comp).
struct GpuVolumeParams {
    glm::vec4 camera_right;
    glm::vec4 camera_up;
    glm::vec4 light;
    glm::vec4 grid;
    glm::vec4 noise;
    glm::vec4 scatter;
    glm::vec4 shadow_rows[9]; // GpuFrame's
    glm::vec4 shadow_split;
    glm::uvec4 shadow_info;
};
struct SkinPushConstants {
    uint64_t source;
    uint64_t skin;
    uint64_t morph;
    uint64_t target;
    uint64_t bones;
    uint64_t bones_before;
    uint64_t face;
    uint32_t count;
    float morph_weight;
    uint32_t flags;
    uint32_t face_count;
    float blend;
    uint32_t pad = 0;
};
struct VolumePushConstants {
    uint64_t params;
    uint32_t mode;
    uint32_t noise_slot;
};

// The image-space constants of a frame (PostRef in shaders/common.glsl).
struct GpuPost {
    uint64_t sky_draws;
    uint64_t pad0;
    glm::vec4 fog_param;
    glm::vec4 fog_near;
    glm::vec4 fog_far;
    glm::vec4 camera;
    glm::vec4 sao_proj;
    glm::vec4 sao_info;
    glm::vec4 sao_screen;
    glm::vec4 sao_info2;
    glm::vec4 sao_screen2;
    glm::vec4 misc;
    glm::vec4 bloom;
    glm::vec4 adapt;
    glm::vec4 tone;
    glm::vec4 cinematic;
    glm::vec4 tint;
    glm::vec4 fade;
    glm::vec4 dof1;
    glm::vec4 dof2;
    glm::vec4 dof3;
    glm::vec4 gauss[8];
    glm::vec4 radial0;
    glm::vec4 radial1;
    glm::vec4 dv0;
    glm::vec4 dv1;
    glm::vec4 vl0;
    glm::vec4 vl1;
    glm::vec4 vl2;
    glm::vec4 vl3;
    glm::uvec4 post_slots;
    uint64_t snow_flag;
    uint64_t lens_flag;
    glm::vec4 sparkle[4]; // SparklesParameters1..4
    glm::vec4 sss[2];     // g_SSSDarkColor, g_SSSParameters
    glm::vec4 ssr;        // fReflectionMarchingRadius, fReflectionRayThickness; the composite's SSRParams.x, .y
    glm::vec4 water_fog;  // ISDistantBlurFogged under water: rgb, a
    glm::vec4 water_fog2; // far, near
    GpuPrecip precip[2];
    glm::mat4 precip_occlusion;
    // The doorway of a view through a door, in clip x, y, w: four edge planes through the eye, then its plane (normal,
    // distance). The final composite reads them (portal_stamp.frag).
    glm::vec4 portal[5];
};

} // namespace spud::render
