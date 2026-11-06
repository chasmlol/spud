#pragma once

#include <array>

// Camera view, per-space environment and the per-frame submission types.

#include "render/frustum.hpp"
#include "render/gpu_types.hpp"
#include "render/sky.hpp"
#include "render/terrain.hpp"

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct ImDrawData;

namespace spud::render {

using MeshId = uint32_t;

struct View {
    glm::vec3 position{0};     // metres, in the coordinates of the space being drawn
    glm::mat4 rotation{1};     // world -> view, rotation only (rendering is camera-relative)
    glm::mat4 projection{1};
    glm::mat4 view_proj{1};    // projection * rotation
    Frustum frustum;
    float fov_y = 1.2f;
    float aspect = 16.0f / 9.0f;
    float near_plane = 0.1f;
    // Camera-relative: what lies on its negative side is clipped (a view through a door, at the far doorway's plane).
    glm::vec4 clip_plane{0.0f, 0.0f, 0.0f, 1.0f};
    bool clipped() const { return clip_plane.x != 0.0f || clip_plane.y != 0.0f || clip_plane.z != 0.0f; }

    // yaw: radians clockwise from north (+Y) towards east (+X); pitch: radians, positive looks up.
    static View make(const glm::vec3& position, float yaw, float pitch, float fov_y, float aspect,
                     float near_plane) {
        View v;
        v.position = position;
        v.fov_y = fov_y;
        v.aspect = aspect;
        v.near_plane = near_plane;
        const glm::vec3 forward(std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch));
        v.rotation = glm::lookAtRH(glm::vec3(0), forward, glm::vec3(0, 0, 1));
        // Reverse-Z, infinite far plane, Vulkan clip space (y down, depth 0..1).
        const float f = 1.0f / std::tan(fov_y * 0.5f);
        glm::mat4 p(0.0f);
        p[0][0] = f / aspect;
        p[1][1] = -f;
        p[2][3] = -1.0f;
        p[3][2] = near_plane;
        v.projection = p;
        v.view_proj = p * v.rotation;
        v.frustum = Frustum::from_view_proj(v.view_proj);
        return v;
    }

    // Turned by `turn` in view space, after the camera's own rotation.
    void turn(const glm::mat4& by) {
        rotation = by * rotation;
        view_proj = projection * rotation;
        frustum = Frustum::from_view_proj(view_proj);
    }

    // An orthographic view: `right` and `up` are the screen's axes in world space (the view looks along
    // up x right), `half_width` / `half_height` the frustum's half extents in metres, depth reverse-Z from
    // `near_plane` (1) to `far_plane` (0).
    static View make_ortho(const glm::vec3& position, const glm::vec3& right, const glm::vec3& up, float half_width,
                           float half_height, float near_plane, float far_plane) {
        View v;
        v.position = position;
        v.aspect = half_width / half_height;
        v.near_plane = near_plane;
        v.fov_y = 0;
        const glm::vec3 back = glm::cross(right, up); // view +z: the camera looks down -z
        glm::mat4 r(1.0f);
        for (int i = 0; i < 3; ++i) { // rows: right, up, back
            r[i][0] = right[i];
            r[i][1] = up[i];
            r[i][2] = back[i];
        }
        v.rotation = r;
        // Vulkan clip space (y down); a point d in front of the camera (view z = -d) has depth (far - d) / (far - near).
        glm::mat4 p(0.0f);
        p[0][0] = 1.0f / half_width;
        p[1][1] = -1.0f / half_height;
        p[2][2] = 1.0f / (far_plane - near_plane);
        p[3][2] = far_plane / (far_plane - near_plane);
        p[3][3] = 1.0f;
        v.projection = p;
        v.view_proj = p * v.rotation;
        v.frustum = Frustum::from_view_proj(v.view_proj);
        return v;
    }
};

// Lighting and atmosphere of one space (exteriors get it from weather later).
struct Environment {
    glm::vec3 sun_dir = glm::normalize(glm::vec3(0.35f, 0.45f, 0.55f));
    glm::vec3 sun_color{1.0f, 0.93f, 0.80f};
    float sun_disc = 4.0f;
    glm::vec3 ambient_sky{0.42f, 0.47f, 0.55f};
    glm::vec3 ambient_ground{0.22f, 0.21f, 0.19f};
    glm::vec3 sky_zenith{0.22f, 0.40f, 0.70f};
    glm::vec3 sky_horizon{0.66f, 0.74f, 0.82f};
    glm::vec3 sky_ground{0.45f, 0.47f, 0.48f};
    float fog_start = 150.0f;
    float fog_end = 6000.0f;
    float fog_max = 0.85f;
    float fog_power = 1.2f;
    glm::vec4 water_color{0.06f, 0.16f, 0.20f, 0.70f};

    // Skyrim's lighting-shader inputs, world space, Skyrim units where the
    // game uses units (fog). `skyrim_inputs` false: derived from the placeholder values above.
    bool skyrim_inputs = false;
    glm::vec3 dir_light_color{1.0f};
    glm::vec4 dir_ambient[3] = {};
    glm::vec4 amb_spec{0.0f};
    glm::vec4 ibl{0.0f};
    glm::vec4 fog_param{0.0f};       // FogParam: x near/(far-near), y 1/(far-near) per unit, z power, w max
    glm::vec4 fog_near_color{0.0f};
    glm::vec4 fog_far_color{0.0f};
    glm::vec4 fog_color{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 output_clamp{65504.0f, 0.0f, 65504.0f, 0.0f};
    glm::vec4 frame_params{0.0f, 1.0f, 1.0f, 0.0f};
    float near_units = 15.0f;        // the game's projection, for the fog distance (5.2)
    float far_units = 353840.0f;
    glm::vec4 wind{0.0f};
    float wind_speed = 0.0f;       // the weather's, 0..1 (WTHR wind speed / 255)
    glm::vec3 ambient_colour{0.0f}; // the weather's Ambient colour
    glm::vec3 effect_light{1.0f}; // DLightColor of effect-lit effect shaders
    SkyFrame sky; // the sky's draws, image space, volumetric lighting and precipitation (exteriors)
    // An interior's point-light fade, game units; negative: the settings' (SpaceView::light_fade_begin).
    float light_fade_begin = -1.0f;
    float light_fade_end = -1.0f;
};

// A run of grass instances drawn with one form's mesh.
struct GrassRun {
    uint32_t group; // the mesh's draw group
    uint32_t first; // instance
    uint32_t count;
};

inline constexpr uint32_t k_no_variant = ~0u;
inline constexpr uint32_t k_all_groups = ~0u;
inline constexpr uint32_t k_group_material = ~0u;

struct DrawItem {
    MeshId mesh;
    uint32_t instance; // slot in the space's instance table
    float distance_sq; // camera distance, for sorting blended draws
    uint32_t variant = k_no_variant; // MeshManager material variant
    bool second_pass = false; // the multi-pass directional material's pass (section 11.2.3), drawn after the opaque scene
    uint8_t lod = 0;          // 0 the full mesh, 1. its simplified levels
    // A part of the mesh: its draw groups [first_group, first_group + group_count) (the UI 3D scenes draw shapes
    // one by one); k_all_groups draws every group.
    uint32_t first_group = 0;
    uint32_t group_count = k_all_groups;
    uint32_t material = k_group_material; // a material slot replacing the groups' own (MeshManager::copy_material)
    bool sky = false; // a weather's sky model: blended with the sky, before the clouds
    uint32_t state = 0; // a packed DrawState in place of the group's: drawn with the blended draws
};

// What one space wants drawn this frame (filled by the engine after culling).
// A skinned decal: built once from the pose its actor stands in, and drawn over its posed vertices.
inline constexpr uint32_t k_skin_decal_slots = 128;       // uMaxSkinDecals is 100
inline constexpr uint32_t k_skin_decal_vertices = 3072;   // a slot's
struct SkinDecalBuild { // one shape's triangles
    uint32_t slot = 0;
    uint32_t first_index = 0, index_count = 0;
    int32_t vertex_offset = 0;
    glm::vec4 hit{0.0f}; // the actor's model space, metres; w 1 / the decal's size
    glm::vec4 axis_x{0.0f}, axis_y{0.0f}, direction{0.0f};
};
struct SkinDecalDraw {
    uint32_t slot = 0, instance = 0, texture = 0;
    float fade = 1.0f;
};

class DrawList {
public:
    void clear() {
        m_items.clear();
        m_skin_decals.clear();
    }
    void add_skin_decal(const SkinDecalDraw& d) { m_skin_decals.push_back(d); }
    std::span<const SkinDecalDraw> skin_decals() const { return m_skin_decals; }
    void add(MeshId mesh, uint32_t instance, float distance_sq, uint32_t variant = k_no_variant,
             bool second_pass = false, uint8_t lod = 0) {
        m_items.push_back({.mesh = mesh, .instance = instance, .distance_sq = distance_sq, .variant = variant,
                           .second_pass = second_pass, .lod = lod});
    }
    void add_sky(MeshId mesh, uint32_t instance) {
        m_items.push_back({.mesh = mesh, .instance = instance, .distance_sq = 0.0f, .sky = true});
    }
    // One draw group of a mesh (index into MeshManager::groups(mesh)), optionally with another material.
    void add_group(MeshId mesh, uint32_t group, uint32_t instance, float distance_sq,
                   uint32_t material = k_group_material) {
        m_items.push_back({.mesh = mesh, .instance = instance, .distance_sq = distance_sq, .first_group = group,
                           .group_count = 1, .material = material});
    }
    // One group of a variant again, over itself: another material and draw state, with the blended draws.
    void add_over(MeshId mesh, uint32_t variant, uint32_t group, uint32_t instance, float distance_sq, uint32_t material,
                  uint32_t state) {
        m_items.push_back({.mesh = mesh, .instance = instance, .distance_sq = distance_sq, .variant = variant,
                           .first_group = group, .group_count = 1, .material = material, .state = state});
    }
    std::span<const DrawItem> items() const { return m_items; }
    size_t size() const { return m_items.size(); }

private:
    std::vector<DrawItem> m_items; // capacity is reused across frames
    std::vector<SkinDecalDraw> m_skin_decals;
};

inline constexpr uint32_t k_max_cascades = 3; // the mask shader reads at most 3
inline constexpr uint32_t k_max_shadow_lights = 4; // the mask has 4 channels, the sun takes one

// [Display] settings as the user's game has them (live settings dump), game units where the game uses units.
struct ShadowSettings {
    bool enabled = true;
    bool hold_far_cascade = true; // on play's clock the far map draws with the world's tick
    float distance = 10000.0f;          // fShadowDistance (SkyrimPrefs.ini; exe default 8000)
    float interior_distance = 3000.0f;  // fInteriorShadowDistance
    float first_slice = 2000.0f;        // fFirstSliceDistance (Skyrim.ini; exe default 1250)
    uint32_t splits = 2;                // iNumSplits
    float split_overlap = 100.0f;       // fSplitOverlap
    uint32_t resolution = 4096;         // iShadowMapResolution
    float poisson_radius = 4.0f;        // fPoissonRadiusScale: the filter radius in texels
    float directional_bias_scale = 0.3f; // fShadowDirectionalBiasScale
    bool disable_high_tree_shadow = true; // bDisableHighTreeShadow
    float static_update_scale = 0.1f;     // fSunStaticTimeUpdateScale
    float max_tree_height = 5000.0f;      // fMaxHeightShadowCastingTrees: trees this far above the camera cast nothing
};

struct RenderSettings {
    uint32_t msaa = 4;           // samples; set at startup (RendererOptions::msaa), shown here for the UI
    bool specular_aa = true;     // Toksvig specular AA; captures turn it off (the game has none)
    float terrain_tiling = 6.0f; // texture repeats per cell
    bool wireframe = false;
    bool flip_green = false;     // normal-map green channel
    bool flip_winding = false;   // debug: swap front faces
    bool lighting_off = false;
    uint32_t debug_view = 0;     // 0 lit, 1 albedo, 2 normals, 3 lighting
    bool draw_terrain = true;
    bool draw_grass = true;
    float grass_fade_start = 7000.0f; // fGrassStartFadeDistance:Grass, clip-space units
    float grass_fade_range = 1000.0f; // fGrassFadeRange:Grass
    bool draw_meshes = true;
    bool draw_water = true;
    // Blended draws (effects, alpha-blended materials). Off for comparisons with the game's lit target before its
    // deferred composite, which the game draws them after.
    bool draw_blended = true;
    // Blended decals.
    // Off with draw_blended off leaves the opaque scene, the game's lit target just before its decal pass.
    bool draw_decals = true;
    // The deferred composite (ambient occlusion and fog). Off for comparisons with the game's lit target before it
    bool composite = true;
    bool reflections = true; // bScreenSpaceReflectionEnabled
    bool door_effects = false; // occlusion and reflections in the view through a door
    bool spread_water_cube = false; // frames between world ticks exist: at most one cube face a frame
    bool hold_effect_state = false; // the audit: eye adaptation and the focus weight at fixed values
    bool lens_flare = true;  // bLensFlare
    bool volumetric = true; // volumetric lighting
    // Everything in the sun's shadow, as the game's shader applies a shadow mask of 0:
    // the sun's direct diffuse and specular off, its rim, soft and back lighting kept. For comparing the pixels the
    // game shadows.
    bool sun_shadow = false;
    // When >= 0, the colour target is cleared to this value (depth kept) just before the blended draws. Every blend
    // mode is affine in the destination, so renders over 0 and over 1 give the blended draws' result over any
    // background: how effects are compared over the game's own composited image.
    float blend_background = -1.0f;
    // Point-light distance fade, game units: fLightLODStartFade:Display as the user's
    // SkyrimPrefs.ini sets it (the game initialises it from fLightLODDefaultStartFade, 1000) and
    // fLightLODRange:Display (exe default).
    bool point_lights = true; // debug: off culls every placed light
    float light_fade_start = 3500.0f;
    float light_fade_range = 500.0f;
    // Projected-UV materials, [Display]: bEnableProjecteUVDiffuseNormals and the two
    // tiling scales as the user's SkyrimPrefs.ini (and the shipped default one) sets them (exe defaults 0.5, 1.2);
    // the snow rim light at the game defaults (fSnowRimLightIntensity, fSnowGeometrySpecPower,
    // fSnowNormalSpecPower, bEnableSnowRimLighting; none is in the user's INIs).
    // LOD fades (section 2.2): fLODFadeOutMultObjects:LOD as the user's SkyrimPrefs.ini sets it (exe default 5);
    // the feature bounds of [LightingShader] at the game defaults, except the decal ones, which the user's Skyrim.ini
    // sets to 0.45 and 0.5.
    float lod_fade_out_mult_objects = 30.0f;
    float lod_fade_out_mult_items = 10.0f;   // fLODFadeOutMultItems:LOD, the user's INI (exe default 3)
    float lod_fade_out_mult_sky_cell = 1.0f; // fLODFadeOutMultSkyCell:LOD, the user's INI (exe default 1)
    float specular_fade[2] = {0.09f, 0.1f};
    float envmap_fade[2] = {0.09f, 0.1f};
    float decal_fade[2] = {0.45f, 0.5f};
    float refraction_fade[2] = {0.025f, 0.03f};
    // Tree leaves (section 12): fLeafAnimDampenDistStart/End:Display (game units), the wind magnitude and wave
    // time scale the tree manager starts with.
    float leaf_dampen[2] = {3600.0f, 4600.0f};
    float tree_wind_magnitude = 3.0f;
    float tree_wave_scale = 0.03f;
    bool projected_textured = true;
    float projected_diffuse_tiling = 0.2f;
    float projected_detail_tiling = 0.8f;
    glm::vec4 snow_rim{0.3f, 3.0f, 2.0f, 1.0f};
    TerrainSettings terrain;
    ShadowSettings shadows;
};

// A placed point light as the space has it this frame (before the view's culling and distance fade).
inline constexpr uint32_t k_water_ripple_segments = 8;
inline constexpr uint32_t k_water_lights = 6;

struct PointLight {
    glm::vec3 position{0.0f}; // metres, world (animated)
    float radius = 0.0f;      // metres
    glm::vec3 color{0.0f};    // colour x dimmer (animated)
    uint32_t flags = 0;       // PointLightFlag
    uint32_t shadow = ~0u;    // point_light_shadow: its entry in SpaceView::shadow_lights this frame, ~0u if not chosen
};
enum PointLightFlag : uint32_t {
    point_light_on = 1u << 0,          // in a loaded cell and enabled
    point_light_never_fades = 1u << 1, // no distance fade
    point_light_shadow = 1u << 2,      // shadow-casting: unlit unless chosen this frame
};

// An open door onto another space, as this space has it (GpuDoorLight), in its world.
struct DoorLight {
    glm::vec3 position{0.0f}; // metres, the middle of the opening
    float radius = 0.0f;
    glm::vec3 normal{0.0f};   // into this space
    float openness = 0.0f;
    glm::vec3 colour{0.0f};
    glm::vec4 ambient[3] = {};
};

struct SunShadows;  // render/shadows.hpp
struct ShadowLight; // render/shadows.hpp


// World particles this frame (engine/world_fx.hpp): one draw per system, back to front.
struct ParticleDraw {
    uint32_t first;
    uint32_t count;
    uint32_t blend_src;
    uint32_t blend_dst;
    float distance;
};
struct ParticleFrame {
    std::span<const GpuParticle> particles;
    std::span<const GpuParticleSystem> systems;
    std::span<const ParticleDraw> draws;
};

// A cell's flow map going into a slot of the flow atlas.
struct WaterFlowUpload {
    uint32_t x, y;
    const uint8_t* texels; // 64 x 64 RGBA8
};

// A space's water this frame: surfaces grouped by technique. World positions,
// so every view of the space shares them.
struct WaterFrame {
    std::span<const GpuWaterMaterial> materials;
    std::span<const GpuWaterInstance> surfaces; // cell water, then with a flow map, then far water
    std::span<const GpuWaterVertex> placed;     // placed water in view: triangles
    uint32_t flow_first = 0;
    uint32_t far_first = 0;
    uint32_t quad_first = 0; // the wading quad, the last surface when there is one
    bool quad_flow = false;
    glm::vec4 quad_rect{0.0f};
    // The ripples: steps of the simulation this frame, the window's shift, and the wader's path, in window coordinates.
    uint32_t ripple_steps = 0;
    glm::vec2 ripple_shift{0.0f};
    std::array<glm::vec4, k_water_ripple_segments> ripple_segments{}; // this step's stamps, in window coordinates
    uint32_t ripple_stamps = 0;
    // The game's light pass: the nearest lights, position from the camera and radius in units, colour.
    std::array<glm::vec4, k_water_lights> light_positions{}, light_colors{};
    uint32_t light_count = 0;
    bool ripple_clear = false;
    std::span<const WaterFlowUpload> flow_uploads;
    uint32_t flow_normals = 0; // texture slot
    glm::vec3 sun_dir{0.0f, 0.0f, 1.0f};
    glm::vec4 sun_color{0.0f};
    bool underwater = false;
    // The game's near-plane test: fog below the waterline's screen v when the camera straddles the surface,
    // the whole screen at 0, none below 0.
    float fog_line = -1.0f;
    bool cube_needed = true; // water in view or close by: only then is the reflection cube kept up
    bool clock_running = false; // the water moves: with the clock stopped there is nothing for the blend to smooth
    glm::vec4 fog{0.0f}; // under water: the fog, rgb and alpha, over its distances in units
    float fog_far = 0.0f;
    float fog_near = 0.0f;
};

struct SpaceView {
    Terrain* terrain = nullptr;
    // The sun's shadow cascades for the main view and the casters culled into each; null
    // when the space has no sun shadows (or for second views).
    const SunShadows* sun_shadows = nullptr;
    const DrawList* shadow_draws[k_max_cascades] = {};
    // The shadow lights chosen for the main view, and their casters.
    std::span<const ShadowLight> shadow_lights;
    const DrawList* shadow_light_draws[k_max_shadow_lights] = {};
    std::span<const PointLight> lights; // indexed like World::lights
    const DrawList* draws = nullptr;
    const WaterFrame* water = nullptr;
    const ParticleFrame* particles = nullptr;
    std::span<const GpuDecal> decals; // in the decal pass, oldest first
    VkDeviceAddress instance_table = 0;
    // Tree impostors: the placements, the impostor table and this frame's runs.
    VkDeviceAddress far_trees = 0;
    VkDeviceAddress far_tree_table = 0;
    std::span<const VkDrawIndirectCommand> far_tree_runs;
    float tree_near = 0.0f; // metres: runs with a first vertex of 6 leave out impostors nearer than this
    VkDeviceAddress grass_instances = 0;
    VkDeviceAddress grass_forms = 0;
    std::span<const GrassRun> grass_runs;
    float cell_size = 58.5216f; // metres, for terrain texture tiling
    Environment environment;
    // Space -> world transform for spaces seen through portals. The current space is identity;
    // only one space is drawn in V0.
    glm::mat4 space_to_world{1};
    std::span<const DoorLight> door_lights;
    // The sky is drawn where nothing else is; interiors without Show Sky leave the cleared black instead
    bool sky = true;
    // Point-light distance fade, game units: an interior's light fade begin and end. Negative: the settings'
    // fLightLODStartFade and range.
    float light_fade_begin = -1.0f;
    float light_fade_end = -1.0f;
};

// The view through an open doorway: the space behind
// it as a whole frame, from the camera carried through the door, before the main frame.
struct OffscreenView;
struct PortalInput {
    const View* view = nullptr; // the far space's camera, clipped at the far doorway's plane
    SpaceView space;
    std::span<const OffscreenView> prepass; // what the far frame reads, as FrameInput::prepass
    glm::vec3 quad[4];          // the doorway's corners in the near space (metres, world)
};

class Overlay;

// A second view of a space drawn into an offscreen target before the overlay, which samples it in the same frame
// It has its own camera and draw list; the sky is not drawn.
struct OffscreenView {
    const View* view = nullptr;
    SpaceView space;
    RenderSettings settings;
    glm::vec4 clear{0, 0, 0, 1};
    uint32_t target = 0;       // Renderer::create_offscreen_target
    bool keep_depth = false;   // leave the depth readable (Renderer::expose_offscreen_depth)
    // ISLocalMap from the target, exposed as Bindless::k_local_map_slot, into a second target, with this as its
    // constants. Empty: no pass.
    std::span<const std::byte> local_map;
    uint32_t local_map_target = 0;
};

// A UI 3D scene: its own camera and
// draws, recorded by the game UI's overlay at its place among the menu movies, over what is drawn so far, with the
// depth cleared first (the game draws straight into the frame).
struct UiSceneInput {
    const View* view = nullptr;
    SpaceView space;
};

// One shape to pose this frame (shaders/skin.comp): byte offsets into the mesh vertex arena (MeshManager).
struct SkinJob {
    uint64_t source = 0;
    uint64_t skin = 0;
    uint64_t morph = 0; // 0: none
    uint64_t target = 0;
    uint32_t vertices = 0;
    float morph_weight = 1.0f; // 0 the light end, 1 the heavy one
    uint32_t flags = 0;        // k_skin_*
    uint32_t first_bone = 0;   // into SkinFrame::bones, in bones (three rows each)
    uint32_t before_bone = ~0u; // into SkinFrame::bones_before; none: it isn't smoothed between ticks
    uint32_t first_face = 0;   // into SkinFrame::face
    uint32_t face_count = 0;
};
// A morph on a head shape this frame.
struct SkinFaceMorph {
    uint64_t deltas = 0; // byte offset into the arena
    float weight = 0.0f; // metres per delta unit, by the morph's weight
    uint32_t unused = 0;
};
inline constexpr uint32_t k_skin_axes = 1u; // model-space normals: the frame carries the skin's axes
inline constexpr uint32_t k_skin_morph = 2u; // set by the renderer: the job has a light end
struct SkinFrame {
    std::span<const SkinJob> jobs;
    std::span<const glm::vec4> bones; // rows of [R * s | t], metres in the model's space
    std::span<const SkinFaceMorph> face;
    std::span<const glm::vec4> bones_before; // the tick before, for the actors in view
    std::span<const SkinDecalBuild> decals;  // new this frame
    bool tick = true;  // false: a frame between ticks: the jobs stand, only those with a tick before are posed again
    float blend = 1.0f; // how far this frame is from that tick to the last
};

struct FrameInput {
    const View* view = nullptr;
    std::span<const SpaceView> spaces; // empty: clear and present only (bench "world none")
    std::span<const OffscreenView> offscreen; // drawn after the scene, before the overlay
    std::span<const OffscreenView> prepass;   // drawn before the scene, which reads them
    std::span<const UiSceneInput> ui_scenes; // drawn by the overlay (Overlay::record's SceneRecorder)
    const PortalInput* portal = nullptr;     // drawn first, seen through its doorway
    SkinFrame skin;                          // shapes to pose before anything draws
    std::span<const GpuSplat> screen_blood;  // over the finished image, under the UI
    uint32_t screen_blood_textures[3] = {};  // alpha, colour, flare
    RenderSettings settings;
    float time = 0.0f;          // the world animation clock (seconds): controllers, leaves, light animation
    float previous_time = 0.0f; // its value one frame back: UV controllers show it
    bool menu_paused = false;   // a menu pauses the game: the eye adapts by fConstHDRAdapt a frame
    ImDrawData* ui = nullptr;   // debug overlay (F1), drawn on the swapchain only
    Overlay* overlay = nullptr; // game UI (render/overlay.hpp), drawn into the frame; null when closed
};

} // namespace spud::render
