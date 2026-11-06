#pragma once

// .osw — spud compiled worldspace. Spec: docs/formats/osw.md
//
// One file per worldspace: an exterior cell grid, per-cell terrain, and every
// placed reference with its final (load-order-resolved) transform.

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::osw {

inline constexpr container::FourCC k_kind = container::fourcc("WRLD");
inline constexpr uint32_t k_version = 16; // docs/formats/osw.md, "Versions"

inline constexpr int k_land_verts = 33;   // vertices per cell edge
inline constexpr int k_quad_verts = 17;   // vertices per quadrant edge (shares edges)
inline constexpr uint16_t k_no_texture = 0xFFFF;
inline constexpr uint32_t k_none = 0xFFFFFFFFu;

enum WorldFlag : uint32_t {
    world_has_water = 1u << 0,
    // One interior cell (CELL DATA 0x1), its references on the exterior grid by position so it streams like a
    // worldspace. `lighting` holds its lighting.
    world_interior = 1u << 1,
    world_show_sky = 1u << 2,         // interior: CELL DATA 0x80 (Show Sky)
    world_use_sky_lighting = 1u << 3, // interior: CELL DATA 0x100 (Use Sky Lighting)
    world_no_sky = 1u << 4, // WRLD DATA flag 0x20: lit like an interior, no sun shadows
};

enum CellFlag : uint32_t {
    cell_has_water = 1u << 0,
    cell_has_land = 1u << 1,
};

enum RefFlag : uint32_t {
    ref_initially_disabled = 1u << 0, // hidden at game start (directly or via enable parent)
    ref_persistent = 1u << 1,
    // The category of the reference's fade node, which scales its LOD fade metric (
    // 2.2.4, 2.2.5); none of these: objects (fLODFadeOutMultObjects).
    ref_fade_items = 1u << 2,                 // fLODFadeOutMultItems
    ref_fade_sky_cell = 1u << 3,              // "Is Full LOD": fLODFadeOutMultSkyCell
    ref_fade_visible_when_distant = 1u << 4,  // s = 0.2, without the LOD adjust
    ref_fade_never = 1u << 5,                 // the metric stays 0
    ref_occludable = 1u << 6,     // architecture (STAT, SCOL, MSTT), which occlusion planes hide
    ref_occludable_far = 1u << 7, // trees and flora: hidden too, but only well away from the camera
};

struct Cell {
    int32_t x;
    int32_t y;
    uint32_t form_id;     // load-order-resolved Skyrim FormID (0 for synthetic cells)
    uint32_t flags;       // CellFlag
    float water_height;   // metres
    uint32_t land;        // index into World::lands, or k_none
    uint32_t first_ref;   // refs for this cell are World::refs[first_ref, first_ref + ref_count)
    uint32_t ref_count;
};
static_assert(sizeof(Cell) == 32);

// Quadrants: 0 = south-west, 1 = south-east, 2 = north-west, 3 = north-east.
// A quadrant is drawn as the game draws it: texture slot 0 is the base, slots
// 1..5 its layers; every vertex has a weight byte per slot and the base's is clamp(255 - sum of the layers').
struct LandQuadrant {
    uint16_t base_texture; // index into World::land_textures, or k_no_texture (engine default)
    uint16_t layer_count;  // at most 5
    uint32_t first_layer;  // slots 1..: World::land_layers[first_layer, first_layer + layer_count)
};
static_assert(sizeof(LandQuadrant) == 8);

struct Land {
    float heights[k_land_verts * k_land_verts]; // metres, absolute; row-major, row 0 = south edge, col 0 = west edge
    int8_t normals[k_land_verts * k_land_verts * 3];
    uint8_t colors[k_land_verts * k_land_verts * 3]; // vertex colours, (255,255,255) default
    uint8_t reserved[2];
    LandQuadrant quadrants[4];
};

struct LandLayer {
    uint16_t texture; // index into World::land_textures, or k_no_texture (the slot binds no texture)
    uint16_t reserved;
    uint8_t opacity[k_quad_verts * k_quad_verts]; // the slot's weight byte per quadrant vertex, row-major from the
                                                  // quadrant's south-west (the game's, after its layer merging)
    uint8_t pad[3];
};
static_assert(sizeof(LandLayer) == 296);

struct Ref {
    uint32_t form_id;      // load-order-resolved Skyrim FormID of the reference
    uint32_t base_form_id; // ... of its base object
    uint32_t mesh;         // index into World::meshes
    uint32_t flags;        // RefFlag
    float position[3];     // metres
    float scale;
    float rotation[4];     // quaternion xyzw, world = R * (scale * local) + position
    float source_euler[3]; // Skyrim's stored euler angles (radians), kept for debugging conventions
    uint32_t directional;  // index into World::directional_materials, or k_none
    uint32_t tree;         // index into World::trees, or k_none
    uint32_t emittance;    // index into World::emittances (XEMI), or k_none
    uint32_t room_hint;    // XMBR: the room (index into World::rooms) tried first when placing it, or k_none
    uint32_t reserved;
};
static_assert(sizeof(Ref) == 80);

// A reference's emittance source (XEMI): the colour its shapes with External_Emittance take
// A light gives its DATA colour / 255; a region the Effect Lighting colour of its
// weather, which the engine computes for the hour.
struct Emittance {
    float color[3];   // light source; unused for a region source
    uint32_t weather; // region source: the region's weather (as Light::emittance_weather), 0 for a light source
};
static_assert(sizeof(Emittance) == 16);

// A TREE base whose model's root is a leaf-animation node (BSLeafAnimNode or BSTreeNode): its leaves move
struct Tree {
    uint32_t base_form_id;
    float leaf_amplitude; // CNAM
    float leaf_frequency;
    uint32_t reserved;
};
static_assert(sizeof(Tree) == 16);

enum LightFlag : uint32_t {
    light_initially_disabled = 1u << 0, // as ref_initially_disabled
    light_never_fades = 1u << 1,        // reference flag 0x10000: no distance fade
    light_no_land = 1u << 2,            // reference flag 0x20000: doesn't light landscape
    light_no_water = 1u << 3,           // reference flag 0x100: doesn't light water
    light_flicker = 1u << 4,            // LIGH flag 0x8
    light_pulse = 1u << 5,              // LIGH flag 0x80 or 0x100
    light_shadow = 1u << 6,             // LIGH flag 0x400, 0x800 or 0x1000: a shadow-casting light
    light_region_emittance = 1u << 7,   // XEMI names a region: colour x that weather's Effect Lighting colour
    light_portal_strict = 1u << 8,      // LIGH flag 0x2000: lights only the rooms portals lead it to
};

// A placed point light as the game creates it: the reference's values after the
// engine's load-time rules. References that create no light are not exported.
struct Light {
    uint32_t form_id;      // the reference
    uint32_t base_form_id; // its LIGH
    uint32_t cell;         // index into World::cells
    uint32_t flags;        // LightFlag
    float position[3];     // metres
    float radius;          // metres: DATA radius + XRDS (<= 0 becomes 0.1 units); not scaled by the reference
    float color[3];        // DATA colour / 255, negative for the Negative flag, or the emittance colour
    float dimmer;          // FNAM + XLIG fade offset
    float frequency;       // animation: DATA frequency, clamped to [0, 1]
    float intensity_amplitude;
    float movement_amplitude; // metres
    float phase[3];        // initial animation phases (the light's attenuation values)
    uint32_t emittance_weather; // light_region_emittance: the region's weather (colour x its Effect Lighting)
    float scale;                // the reference's: the animation's movement is in the light's parent space,
    float rotation[4];          // which carries the reference rotation (quaternion xyzw) and scale
    // light_shadow: the shadow map's parameters.
    float shadow_fov;     // radians: spot DATA FOV + XLIG offset; hemisphere pi; omni 2 pi
    float shadow_falloff; // DATA falloff exponent (0 becomes 1)
    float shadow_near;    // metres: DATA near clip
    float shadow_bias;    // XLIG shadow depth bias, 1 without one
};
static_assert(sizeof(Light) == 112);

enum DirectionalFlag : uint32_t {
    directional_projected = 1u << 0, // a single-pass MATO: projected UV (shader flag 23)
    directional_mato_snow = 1u << 1, // the MATO's snow field (shader flag 60)
    directional_base_snow = 1u << 2, // the STAT's DNAM snow flag (shader flag 30)
    directional_multi_pass = 1u << 3, // a multi-pass MATO: a second, type-14 pass
};

// What a base STAT's directional material (DNAM: max angle, MATO) and snow flag do to its references' lighting
// shaders. Shared by every reference of the base.
struct DirectionalMaterial {
    uint32_t base_form_id; // the STAT
    uint32_t mato;         // the MATO, or 0
    uint32_t flags;        // DirectionalFlag
    float max_angle;       // degrees (STAT DNAM)
    float falloff_scale;   // MATO DATA
    float falloff_bias;
    float noise_uv_scale;
    float material_uv_scale;
    float projection[3];   // multi pass: the projection vector
    float normal_dampener; // multi pass
    float color[3];        // single pass: the colour (0..1)
    uint32_t pass;         // multi pass: index into World::directional_passes, or k_none (no second pass)
};
static_assert(sizeof(DirectionalMaterial) == 64);

// The multi-pass MATO's material: the last BSLightingShaderProperty of its DNAM NIF streams (section 11.2.3).
struct DirectionalPass {
    uint32_t shader_type = 0;
    uint32_t flags1 = 0;
    uint32_t flags2 = 0;
    float uv_offset[2] = {0, 0};
    float uv_scale[2] = {1, 1};
    float emissive[3] = {0, 0, 0}; // colour x multiple
    float alpha = 1.0f;
    float specular[3] = {1, 1, 1}; // colour x strength
    float glossiness = 80.0f;
    float sparkle[4] = {0, 0, 0, 0}; // type 14's SparkleParams; 0 otherwise
    std::string diffuse;    // texture set slot 0, normalised cache path
    std::string back_light; // slot 7
};

enum LandTextureFlag : uint8_t {
    land_texture_snow = 1u << 0, // LTEX INAM non-zero: the landscape shader's "is snow"
};

struct LandTexture {
    uint32_t form_id;
    std::string diffuse; // normalised cache paths
    std::string normal;
    uint8_t specular_exponent = 30; // LTEX SNAM
    uint8_t flags = 0;              // LandTextureFlag
    uint32_t first_grass = 0;       // World::land_grass[first_grass, + grass_count): LTEX GNAM in file order
    uint32_t grass_count = 0;
};

enum GrassFlag : uint8_t {
    grass_vertex_lighting = 1u << 0,
    grass_uniform_scale = 1u << 1,
    grass_fit_to_slope = 1u << 2,
};

// A GRAS form (load-order winner).
struct Grass {
    uint32_t form_id;
    uint32_t mesh;   // index into World::meshes, or k_none
    uint8_t density; // DATA: percent
    uint8_t min_slope;
    uint8_t max_slope;
    uint8_t flags;   // GrassFlag
    uint16_t units_from_water;
    uint16_t reserved;
    uint32_t water_type;
    float position_range;
    float height_range;
    float color_range;
    float wave_period;
};
static_assert(sizeof(Grass) == 36);

// A cell's water currents: quads [first_quad, + quad_count) of World::current_quads, in the game's element
// format (docs/formats/osw.md, "Water currents"): velocities in Havok m/s, positions in Havok units (metres).
struct CellCurrent {
    int32_t x;
    int32_t y;
    uint32_t first_quad;
    uint32_t quad_count;
};
static_assert(sizeof(CellCurrent) == 16);

struct CurrentQuad {
    float v[4];
};
static_assert(sizeof(CurrentQuad) == 16);

// A cell's flow map (textures/water/<plugin>/flow.X.Y.dds): World::flow_texels holds its 64 x 64 RGBA8 texels at
// index * k_flow_cell_bytes, row 0 north.
inline constexpr uint32_t k_flow_size = 64;
inline constexpr uint32_t k_flow_cell_bytes = k_flow_size * k_flow_size * 4;
struct FlowCell {
    int32_t x;
    int32_t y;
};
static_assert(sizeof(FlowCell) == 8);

// A placed water reference's surface: triangles of World::water_vertices, world metres.
struct WaterSurface {
    uint32_t water;        // WATR FormID (the activator's WNAM)
    uint32_t first_vertex;
    uint32_t vertex_count;
    float center[3];
    float radius;
    float plane[4]; // n . p = plane[3], metres: the shape's Z axis through its origin, tilted with a sloped stream
    uint32_t vertex_uv; // 1: its noise follows the shape's texture coordinates, not the world
};
static_assert(sizeof(WaterSurface) == 48);
struct WaterVertex {
    float position[3];
    float uv[2];
    float depth; // 0..1 from the vertex alpha of a shape with Vertex Alpha Depth; < 0: what lies behind decides
};
static_assert(sizeof(WaterVertex) == 24);

// A solid collision primitive: a CollisionMarker reference's XPRM shape (invisible walls and the like).
enum class PrimitiveType : uint32_t { box = 1, sphere = 2, plane = 3 };
struct Primitive {
    int32_t cell_x;
    int32_t cell_y;
    uint32_t form_id;     // the reference
    PrimitiveType type;
    uint32_t layer;       // Skyrim collision layer (XTRI's layer number, else 3 TRANSPARENT)
    uint32_t flags;       // RefFlag
    float position[3];    // metres
    float rotation[4];    // quaternion xyzw (the reference's)
    float half_extents[3]; // metres: box half extents, sphere radius in [0], plane (x, -, z)
    uint32_t reserved;
};
static_assert(sizeof(Primitive) == 68);

// A child worldspace merged into this one at import. The cells it
// provides carry its index + 1 in World::cell_regions.
struct Region {
    uint32_t form_id;
    std::string edid;
};

// An interior cell's lighting as the game resolves it: the cell's XCLL with the fields its inherit flags name
// taken from its lighting template (LTMP), or the template's alone when the cell has no XCLL
// Colours are the records' RGBA8 bytes.
struct CellLighting {
    uint8_t ambient[4];
    uint8_t directional[4];
    uint8_t fog_near_color[4];
    float fog_near;          // units
    float fog_far;
    int32_t rotation_xy;     // degrees
    int32_t rotation_z;
    float directional_fade;
    float fog_clip;          // units
    float fog_power;
    uint8_t directional_ambient[6][4]; // DALC order (X+, X-, Y+, Y-, Z+, Z- as xEdit names them)
    uint8_t specular[4];
    float fresnel_power;
    uint8_t fog_far_color[4];
    float fog_max;
    float light_fade_begin;  // units
    float light_fade_end;
    uint32_t inherit;            // the cell's XCLL inherit flags (already applied); ~0u without XCLL
    uint32_t lighting_template;  // LTMP FormID, or 0
    uint32_t image_space;        // XCIM FormID, or 0
    float sunlight_scale;        // HNAM Sunlight Scale of the image space (XCIM, else DefaultImageSpace 0x167)
    uint32_t sky_weather;        // XCCM: the weather of the cell's sky region, most likely first; 0 without
};
static_assert(sizeof(CellLighting) == 108);

// Rooms, portals and occluders: RoomMarker (STAT 0x1F),
// PortalMarker (0x20) and PlaneMarker (0x17) references. Sizes are XPRM's, never scaled by the reference.
enum RoomFlag : uint32_t {
    room_oriented = 1u << 0, // some angle >= 0.01 rad: an oriented box, else axis-aligned
};
struct Room {
    uint32_t form_id;
    uint32_t flags;             // RoomFlag
    float centre[3];            // metres, the reference position
    float rotation[4];          // quaternion xyzw; identity unless room_oriented
    float half_extents[3];      // metres
    uint32_t lighting_template; // XRMR bit 15 + LNAM, or 0
    uint32_t image_space;       // XRMR bit 14, or 0
    uint32_t first_link;        // World::room_links[first_link, + link_count): the rooms its XLRM name
    uint32_t link_count;
    uint32_t lighting;          // World::room_lighting index (the template's values), or k_none
};
static_assert(sizeof(Room) == 68);

struct Portal {
    uint32_t form_id;
    uint32_t rooms[2];      // XPOD: indices into World::rooms, k_none = outside
    float centre[3];        // metres
    float rotation[4];      // quaternion xyzw: half width along local X, half height along local Z, normal local Y
    float half_width;
    float half_height;
};
static_assert(sizeof(Portal) == 48);

enum class OccluderType : uint32_t { box = 1, plane = 3 };
struct Occluder {
    uint32_t form_id;
    OccluderType type;
    float centre[3];
    float rotation[4];
    float half_extents[3]; // plane: (half width, -, half height)
    uint32_t links[4];     // XORD: linked planes (indices into World::occluders), k_none
};
static_assert(sizeof(Occluder) == 64);

enum DoorFlag : uint32_t {
    door_to_interior = 1u << 0, // the destination space is an interior (interiors/<space>.osw), else worlds/
    door_same_space = 1u << 1,  // the destination door is in this file (a merged city's gate)
    door_has_marker = 1u << 2,  // marker_position is set: the destination door teleports back to here
    door_initially_disabled = 1u << 3, // as ref_initially_disabled: not there at game start
    door_automatic = 1u << 4,   // DOOR FNAM 0x02: opens by itself (the invisible AutoLoadDoors)
};

// A load door: a door reference with teleport data (XTEL). Crossing it moves between spaces
struct Door {
    uint32_t form_id;          // the door reference
    uint32_t base_form_id;     // its DOOR
    uint32_t destination;      // the door reference on the other side
    uint32_t flags;            // DoorFlag
    uint32_t teleport_flags;   // XTEL flags
    float position[3];         // metres, the reference's
    float rotation[4];         // quaternion xyzw
    float scale;
    float bounds_min[3];       // the base's OBND, metres, in the door's own frame (unscaled)
    float bounds_max[3];
    float arrival_position[3]; // XTEL: where the player arrives in the destination space (metres)
    float arrival_rotation[3]; // ... facing (euler radians, as stored)
    // The destination door reference in its own space, as position, rotation, scale and its base's OBND are here.
    float dest_position[3];
    float dest_rotation[4];
    float dest_scale;
    float dest_bounds_min[3];
    float dest_bounds_max[3];
    // The destination door's own XTEL position (door_has_marker): where its teleport arrives, in front of this door.
    float marker_position[3];
    std::string destination_space; // EditorID of the destination space, lowercase
};

struct World {
    std::string edid;
    std::string name;
    std::string parent_edid;
    uint32_t form_id = 0;
    uint32_t flags = 0;
    int32_t cell_min[2] = {0, 0}; // inclusive cell-grid bounds of `cells`
    int32_t cell_max[2] = {0, 0};
    float cell_size = 0.0f;           // metres
    float default_land_height = 0.0f; // metres
    float default_water_height = 0.0f;
    std::string default_land_texture; // normalised diffuse path used for k_no_texture

    std::vector<std::string> meshes; // normalised cache paths ("meshes/...osm")
    std::vector<LandTexture> land_textures;
    std::vector<Grass> grasses;
    std::vector<uint32_t> land_grass; // indices into grasses
    std::vector<Cell> cells; // sorted by (y, x)
    std::vector<Land> lands;
    std::vector<LandLayer> land_layers;
    std::vector<Ref> refs; // grouped by cell
    std::vector<Light> lights; // sorted by cell, then reference FormID
    std::vector<DirectionalMaterial> directional_materials;
    std::vector<DirectionalPass> directional_passes;
    std::vector<Tree> trees;
    std::vector<Emittance> emittances;
    std::vector<CellCurrent> currents; // cells with a current, sorted by (y, x)
    std::vector<CurrentQuad> current_quads;
    std::vector<Primitive> primitives; // sorted by cell (y, x)
    std::vector<Region> regions;
    std::vector<uint16_t> cell_regions; // parallel to cells: 0 this world, else 1 + index into regions; empty = all 0
    std::vector<uint32_t> cell_waters;  // parallel to cells: the cell's WATR FormID (osa::Water), 0 without water
    std::vector<FlowCell> flow_cells;   // sorted by (y, x)
    std::vector<uint8_t> flow_texels;
    std::vector<WaterSurface> water_surfaces;
    std::vector<WaterVertex> water_vertices;
    CellLighting lighting{};             // world_interior only
    std::vector<Door> doors;             // sorted by FormID
    std::vector<Room> rooms;             // sorted by FormID
    std::vector<uint32_t> room_links;    // room indices
    std::vector<CellLighting> room_lighting; // interiors: rooms with a lighting template, its values
    std::vector<Portal> portals;         // sorted by FormID
    std::vector<Occluder> occluders;     // sorted by FormID
};

std::vector<std::byte> write(const World& world);
World read(std::span<const std::byte> file);

} // namespace spud::osw
