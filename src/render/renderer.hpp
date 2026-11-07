#pragma once

// The renderer: owns the Vulkan device, frame resources, streaming managers
// (textures, meshes, uploads) and records one frame from a FrameInput.
//
// Frame structure (graphics queue): the shadow maps, then one dynamic-rendering pass on an offscreen half-float
// target (the game's main render target; multisampled with MSAA, resolved at the end of the pass):
//   opaque meshes -> terrain -> alpha-tested meshes -> sky (depth == far only)
//   -> ambient occlusion -> decals -> the deferred composite (fog) -> water -> blended meshes (back to front)
// then image space (renderer_post.cpp) into an 8-bit target, then [blit + UI] to the swapchain.
// Meshes are instanced per (mesh, submesh) draw group and bucketed by draw
// state, one vkCmdDrawIndexedIndirect per bucket.

#include "render/bindless.hpp"
#include "render/device.hpp"
#include "render/gpu_types.hpp"
#include "render/jobs.hpp"
#include "render/meshes.hpp"
#include "render/output.hpp"
#include "render/overlay.hpp"
#include "render/gpu_types.hpp"
#include "render/pipelines.hpp"
#include "render/swapchain.hpp"
#include "render/textures.hpp"
#include "render/uploader.hpp"
#include "render/view.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;

namespace spud::render {

enum GpuPass : uint32_t {
    pass_prepare, // skinning, offscreen views and the water cube, before anything of the frame
    pass_sun, // the sun's shadow maps
    pass_volume, // the volumetric light's volume
    pass_shadow, // the lights' shadow maps
    pass_opaque,
    pass_terrain,
    pass_alpha_test,
    pass_sky,
    pass_ao, // ambient occlusion
    pass_ssr, // decals, water ripples and mask, the scene copy, reflections
    pass_composite, // snow, the deferred composite, the water
    pass_water,
    pass_blend,
    pass_image_space, // refraction, HDR, bloom, tonemap, distant blur
    pass_ui, // the UI
    pass_post, // blit to the swapchain
    k_gpu_pass_count,
};
const char* gpu_pass_name(uint32_t pass);

struct GpuTiming {
    uint64_t frame = 0; // Renderer::frame_number() of the frame these belong to
    float total_ms = 0;
    float passes_ms[k_gpu_pass_count] = {};
};

struct FrameStats {
    uint32_t draw_calls = 0;     // indirect calls + direct draws
    uint32_t mesh_commands = 0;  // indirect commands (one per visible draw group)
    uint32_t mesh_instances = 0; // submesh instances
    uint64_t mesh_triangles = 0;
    uint32_t blended = 0;
    uint32_t terrain_cells = 0;
    uint32_t buckets = 0;
    uint32_t shadow_draw_calls = 0;
    uint64_t shadow_triangles = 0;
};

struct RendererOptions {
    bool validation = false;
    SDL_Window* window = nullptr; // required: frames are always presented
    // `window` is never shown (headless): frames are presented to it exactly as in play, but nothing
    // reaches the desktop. See docs/AUTOMATION.md.
    bool hidden_window = false;
    uint32_t render_width = 0;    // 0: follow the window
    uint32_t render_height = 0;
    PresentMode present_mode = PresentMode::vsync;
    std::filesystem::path cache_root;
    std::filesystem::path shader_dir;
    std::filesystem::path pipeline_cache;
    bool imgui = true;
    uint32_t msaa = 4; // scene samples: 1, 2, 4 or 8
};

class Renderer {
public:
    explicit Renderer(const RendererOptions& options);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    Device& device() { return *m_device; }
    JobSystem& jobs() { return *m_jobs; }
    Uploader& uploader() { return *m_uploader; }
    TextureManager& textures() { return *m_textures; }
    MeshManager& meshes() { return *m_meshes; }
    const std::filesystem::path& cache_root() const { return m_cache_root; }

    // Waits until the GPU has finished with this frame's slot (begin_frame does it too;
    // separate so callers can time the wait on its own).
    void wait_frame();
    // Waits for this frame's slot, then runs upload completions and streaming
    // scheduling under the per-frame budget. Call before building the frame.
    void begin_frame();
    void render(const FrameInput& input);

    void on_window_resized() { m_output->on_resize(); }
    void set_present_mode(PresentMode mode);
    PresentMode present_mode() const { return m_present_mode; }
    std::string present_mode_name() const { return m_output->present_mode_name(); }
    void set_render_size(uint32_t width, uint32_t height); // 0, 0 = follow the output
    VkExtent2D render_extent() const { return m_target_extent; }
    bool headless() const { return m_options.hidden_window; }

    // True while any streaming work is queued, in flight or awaiting its completion.
    bool busy() const;

    void request_capture() { m_capture_requested = true; }
    // Like request_capture, but taken after the overlay (game UI) instead of before it.
    void request_overlay_capture() { m_capture_overlay_requested = true; }
    VkFormat color_format() const { return m_color_format; }
    bool capture_pending() const { return m_capture_frame != 0; }
    // Blocks until the captured frame is done; RGBA8, tightly packed.
    bool take_capture(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
    // The same at about `width` x `height`, sampled: the frame's size is never copied. Height follows the frame's shape.
    bool take_capture_scaled(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
    // The resolved HDR scene of the next frame, before tonemapping: RGBA16F, tightly packed.
    void request_hdr_capture() { m_hdr_capture_requested = true; }
    bool hdr_capture_pending() const { return m_hdr_capture_frame != 0; }
    bool take_hdr_capture(std::vector<uint16_t>& rgba_half, uint32_t& width, uint32_t& height);
    // Checks: from now on the HDR chain reads this RGBA16F image instead of the scene (a capture's target
    // before the tonemap, so the chain can be compared alone). Empty: back to the scene.
    void inject_hdr(std::vector<std::byte> rgba_half);
    // The sky's geometry (engine/weather.hpp), once: the sky draws index it.
    // A crossing: the far frame's view is now the main one.
    void cross_view() {
        m_adapt_valid = false;
        std::swap(m_water_views[0], m_water_views[1]);
    }
    void set_sky_mesh(std::span<const SkyVertex> vertices, std::span<const uint32_t> indices);
    uint32_t msaa() const { return m_samples; }
    // The sun's shadow cascades of the next frame that draws them: uint16 depth, layer-major.
    void request_shadow_capture() { m_shadow_capture_requested = true; }
    bool take_shadow_capture(std::vector<uint16_t>& texels, uint32_t& layers, uint32_t& size);

    // GPU timings that have become available since the last call, oldest first.
    void take_gpu_timings(std::vector<GpuTiming>& out); // replaces `out`, keeping both buffers' capacity
    // Waits for the GPU and makes the timings of every submitted frame available.
    void flush_gpu_timings();
    const FrameStats& frame_stats() const { return m_stats; }
    uint64_t frame_number() const { return m_frame_number; }
    double pipeline_creation_ms() const { return m_pipelines->creation_ms(); }
    void wait_idle();
    void set_shadow_resolution(uint32_t resolution); // iShadowMapResolution; waits for the GPU if it changes
    // Finishes all streaming work and GPU work and runs deferred destruction.
    // Call before destroying objects that jobs, uploads or deferred frees refer to.
    void drain();

    // Offscreen colour targets that OffscreenViews draw into and the overlay samples (renderer_offscreen.cpp): a
    // fixed size, made once (at start-up, so no device memory is allocated later); between frames the image is in
    // SHADER_READ_ONLY_OPTIMAL and holds what its last view drew (a cache, docs/RENDERING.md).
    uint32_t create_offscreen_target(VkExtent2D extent, const char* name);
    VkImageView offscreen_target_view(uint32_t target) const;
    // Shaders read this target's depth through `slot` (a view with OffscreenView::keep_depth draws it).
    void expose_offscreen_depth(uint32_t target, uint32_t slot);
    void expose_offscreen_color(uint32_t target, uint32_t slot);
    VkExtent2D offscreen_target_extent(uint32_t target) const;

    // Per-frame streaming budget on the render thread.
    struct Budget {
        size_t max_completions = 48;
        size_t max_completion_bytes = 96ull << 20;
    };
    Budget& budget() { return m_budget; }

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkQueryPool queries = VK_NULL_HANDLE;
        uint64_t number = 0; // frame number recorded into this slot (0 = never)
        bool timestamps_written = false;
        Buffer arena;
        VkDeviceSize arena_used = 0;
        std::vector<Buffer> old_arenas; // outgrown this frame, with commands still pointing at them
    };

    void record_skinning(Frame& fr, const SkinFrame& skin);

    struct Bucket {
        std::vector<uint32_t> groups;
    };
    struct BlendItem {
        float distance_sq;
        uint32_t group;
        uint32_t instance;
        uint32_t order; // submission order, the tie-break that keeps equal-distance blends deterministic
        uint32_t material;
        bool sky = false;
        uint32_t state = 0; // the run's: the group's, or the item's
    };
    struct SecondPassItem {
        uint32_t group;
        uint32_t instance;
        uint32_t material;
    };

    struct Run {
        uint32_t state;
        VkDeviceSize offset; // into the arena
        uint32_t count;
    };

    void create_targets(VkExtent2D extent);
    void destroy_targets();
    // renderer_post.cpp: the image-space chain and the sky's draws.
    struct PostTarget {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        uint32_t slot = 0;
    };
    PostTarget make_post_target(VkFormat format, VkExtent2D extent, uint32_t slot, Lifetime lifetime, const char* name);
    void create_post_targets(VkExtent2D extent);
    void destroy_post_targets();
    VkDeviceAddress write_post_data(Frame& frame, const View& view, const SpaceView& space, const RenderSettings& rs,
                                    float dt);
    void fullscreen(VkCommandBuffer cmd, Program program, VkImageView view, VkExtent2D extent, PushConstants& pc,
                    uint32_t mode, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);
    void fullscreen_draw(VkCommandBuffer cmd, Program program, VkExtent2D extent, PushConstants& pc, uint32_t mode,
                         uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);
    void post_pass(VkCommandBuffer cmd, Program program, PostTarget& out, PushConstants& pc, uint32_t mode,
                   uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);
    void record_ao(VkCommandBuffer cmd, PushConstants& pc);
    void record_composite(VkCommandBuffer cmd, PushConstants& pc);
    void record_decals(VkCommandBuffer cmd, Frame& frame, const PushConstants& pc, std::span<const GpuDecal> decals);
    void record_skin_decal_builds(Frame& frame, std::span<const SkinDecalBuild> builds);
    void record_skin_decals(VkCommandBuffer cmd, const PushConstants& pc, std::span<const SkinDecalDraw> decals, bool flip);
    void record_screen_blood(VkCommandBuffer cmd, Frame& frame, const PushConstants& pc, VkImageView out, VkExtent2D extent,
                             const FrameInput& input);
    void record_scene_copy(VkCommandBuffer cmd);
    void record_lens_flare(VkCommandBuffer cmd, PushConstants& pc);
    void record_ssr(VkCommandBuffer cmd, PushConstants& pc, bool water);
    // Water.
    void create_water_targets(VkExtent2D extent);
    VkDeviceSize water_arena_need(const FrameInput& input) const;
    void destroy_water_targets();
    // `second_view`: an offscreen view's, which leaves the main view's water history as it is.
    VkDeviceAddress write_water_data(Frame& frame, const View& view, const SpaceView& space, const RenderSettings& rs,
                                     bool second_view = false);
    void draw_water_surfaces(VkCommandBuffer cmd, PushConstants& pc, Program near, Program flow, Program far,
                             const WaterFrame& water, bool wire, const Pipelines* pipelines = nullptr);
    void record_water_flow(VkCommandBuffer cmd, Frame& frame, const WaterFrame& water);
    void record_water_ripples(VkCommandBuffer cmd, PushConstants& pc, const WaterFrame& water);
    void record_water_mask(VkCommandBuffer cmd, PushConstants& pc, const WaterFrame& water);
    void record_water(VkCommandBuffer cmd, PushConstants& pc, const WaterFrame& water);
    VkDeviceAddress write_particle_data(Frame& frame, const ParticleFrame& particles);
    void record_particles(VkCommandBuffer cmd, PushConstants& pc, const ParticleFrame& particles, bool flip);
    void record_water_cube(VkCommandBuffer cmd, Frame& frame, const SpaceView& space, const FrameInput& input,
                           VkDeviceAddress post);
    void record_snow(VkCommandBuffer cmd, PushConstants& pc);
    VkDeviceAddress write_sky_draws(Frame& frame, const SkyFrame& sky);
    void record_sky(VkCommandBuffer cmd, Frame& frame, const SkyFrame& sky, PushConstants& pc, bool sky_model = true,
                    bool cube = false);
    void record_image_space(VkCommandBuffer cmd, const SkyFrame& sky, PushConstants& pc, uint32_t scene_slot);
    void record_volume(VkCommandBuffer cmd, Frame& frame, const View& view, const Environment& env, VkDeviceSize frame_offset);
    void record_volumetric(VkCommandBuffer cmd, PushConstants& pc, bool volumetric, bool lens);
    void record_precipitation(VkCommandBuffer cmd, const SkyFrame& sky, PushConstants& pc);
    void read_timestamps(Frame& frame);
    VkDeviceSize arena_alloc(Frame& frame, VkDeviceSize size, VkDeviceSize align);
    void ensure_arena(Frame& frame, VkDeviceSize size);
    void build_mesh_draws(Frame& frame, const SpaceView& space, std::vector<Run>& opaque, std::vector<Run>& alpha,
                          std::vector<Run>& blend, VkDeviceAddress& draw_instances);
    // `pipelines`: the set built for the target being drawn (default: the scene's).
    void record_runs(VkCommandBuffer cmd, Frame& frame, const std::vector<Run>& runs, bool wireframe,
                     bool flip_winding, const Pipelines* pipelines = nullptr);
    VkDeviceAddress write_point_lights(Frame& frame, const View& view, const SpaceView& space, const RenderSettings& rs);
    // renderer_shadows.cpp: the shadow maps, made once at startup.
    void create_shadow_maps();
    void destroy_shadow_maps();
    // A caster list's runs: one indirect command per draw group, bucketed by ShadowCaster state (Run::state).
    void build_shadow_runs(Frame& frame, const DrawList& draws, std::vector<Run>& runs,
                           VkDeviceAddress& draw_instances, uint32_t casters = 1);
    // Groups of the space's caster lists, for sizing the arena.
    size_t shadow_group_count(const SpaceView& space) const;
    // The sun's cascades into their map, then the map ready to sample. `frame_offset` is the main view's frame data.
    void record_sun_shadows(VkCommandBuffer cmd, Frame& frame, const SpaceView& space, VkDeviceSize frame_offset,
                            const PushConstants& base);
    // The chosen shadow lights' maps, one layer each.
    void record_light_shadows(VkCommandBuffer cmd, Frame& frame, const SpaceView& space, VkDeviceSize frame_offset,
                              const PushConstants& base);
    // One caster pass: `space`'s list drawn with `pipeline_all` set (every run through the alpha-test program, whose
    // fragment shader the paraboloids' hemisphere needs), in the current rendering scope.
    void record_shadow_runs(VkCommandBuffer cmd, Frame& frame, const std::vector<Run>& runs, const PushConstants& pc,
                            bool always_fragment);
    VkDeviceAddress write_material_anims(Frame& frame, const FrameInput& input);
    void apply_state(VkCommandBuffer cmd, const DrawState& s, bool flip_winding);
    // renderer_overlay.cpp. True if it left a finished image in the colour target.
    bool record_overlay(VkCommandBuffer cmd, const FrameInput& input, Frame& frame, bool has_scene);
    // renderer_offscreen.cpp: arena bytes an offscreen view needs at most, new targets' first layout, the pass.
    VkDeviceSize offscreen_arena_need(std::span<const OffscreenView> views) const;
    void init_offscreen_targets(VkCommandBuffer cmd);
    void record_offscreen(VkCommandBuffer cmd, Frame& frame, const OffscreenView& view, const FrameInput& input);
    // renderer_portal.cpp: the view through a doorway. Arena bytes it needs, its frame data and draws (before the
    // main view's), and its passes, recorded into the scene targets before the main pass. True if it drew.
    void prepare_portal(const FrameInput& input);
    bool record_portal(VkCommandBuffer cmd, Frame& frame, const PushConstants& base, const glm::vec3& clear);
    void record_portal_stamp(VkCommandBuffer cmd, const PushConstants& base, uint32_t scene_slot, uint32_t adapted_slot);
    struct PortalState {
        bool ready = false;
        VkRect2D scissor{};
        glm::vec4 clip[4];    // the doorway's corners, clip space
        glm::vec4 doorway[5]; // GpuPost::portal
    } m_portal;
    bool m_far_pass = false; // recording a door's far frame
    PostTarget m_portal_scene, m_portal_image; // what it leaves: its scene and its finished image

    struct OffscreenTarget {
        VkExtent2D extent{};
        VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
        VmaAllocation color_alloc = nullptr, depth_alloc = nullptr;
        VkImageView color_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE;
        bool initialised = false; // has had its first layout transition (cleared)
    };
    std::vector<OffscreenTarget> m_offscreen;
    TerrainDraws m_offscreen_terrain;
    // renderer_overlay.cpp: the frame's UI 3D scenes. Their arena need (sized before anything is allocated), their
    // frame data and draws (after the world's), and the recorder the overlay draws them with.
    VkDeviceSize ui_scenes_arena(const FrameInput& input) const;
    void prepare_ui_scenes(Frame& frame, const FrameInput& input);
    // A view's frame constants: `extent` and `samples` are the target's it draws into.
    // A view's land in the frame's arena, and its draws (near cells, then far nodes); `pc` gets their addresses.
    TerrainFrame write_terrain(Frame& frame, const TerrainDraws& draws);
    void record_terrain(VkCommandBuffer cmd, const Terrain& terrain, const TerrainFrame& land, PushConstants& pc,
                        const Pipelines& pipelines, bool wireframe, bool flip_winding,
                        Program near_program = program_terrain, Program far_program = program_terrain_far);
    VkDeviceSize write_frame_data(Frame& frame, const View& view, const SpaceView& space, const RenderSettings& rs,
                                  float time, float previous_time, VkExtent2D extent, uint32_t samples);
    class UiSceneRecorder;
    struct UiSceneRuns {
        VkDeviceSize frame_offset = 0;
        VkDeviceAddress draw_instances = 0;
        VkDeviceAddress instance_table = 0;
        VkDeviceAddress lights = 0;
        VkDeviceAddress material_anims = 0;
        std::vector<Run> opaque, alpha, blend;
    };

    RendererOptions m_options;
    std::unique_ptr<Device> m_device;
    std::unique_ptr<JobSystem> m_jobs;
    std::filesystem::path m_cache_root;
    std::unique_ptr<Bindless> m_bindless;
    std::unique_ptr<Uploader> m_uploader;
    std::unique_ptr<TextureManager> m_textures;
    std::unique_ptr<MeshManager> m_meshes;
    // The projected-UV textures every projected material reads: noise, diffuse,
    // normal, normal detail.
    TextureId m_projected_textures[4] = {};
    std::unique_ptr<Pipelines> m_pipelines;
    std::unique_ptr<Pipelines> m_ui_pipelines; // single-sampled, colour format: the UI's scene targets
    VkDeviceAddress m_material_anims_address = 0; // this frame's (write_material_anims)
    std::unique_ptr<Output> m_output;

    std::array<Frame, k_frames_in_flight> m_frames;
    Buffer m_skin_decals; // the slots: geometry a decal keeps through its life
    std::array<Buffer, k_frames_in_flight + 1> m_skin_rows; // the bones of a tick, read until the next
    uint32_t m_skin_ring = 0;
    size_t m_skin_uploaded_jobs = 0; // the last tick's jobs, whose rows are in the ring
    uint64_t m_skin_recorded_frame = ~0ull;
    VkDeviceSize m_skin_before_at = 0, m_skin_face_at = 0;
    uint64_t m_frame_number = 0; // frames submitted so far
    uint32_t m_slot = 0;
    bool m_door_effects = false; // this frame's RenderSettings::door_effects
    bool m_frame_begun = false;
    float m_timestamp_period_ns = 1.0f;
    std::vector<GpuTiming> m_gpu_timings;

    PresentMode m_present_mode = PresentMode::vsync;
    VkFormat m_color_format = VK_FORMAT_B8G8R8A8_UNORM;
    static constexpr VkFormat k_hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT; // the game's main target format
    static constexpr VkFormat k_depth_format = VK_FORMAT_D32_SFLOAT;
    uint32_t m_samples = 1;
    VkExtent2D m_target_extent{0, 0};
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    Target m_hdr;          // scene colour, m_samples samples
    Target m_hdr_resolved; // single-sampled copy when multisampling (else m_hdr is read directly)
    Target m_depth_target;
    Target m_depth_copy;   // single-sampled copy of the opaque scene's depth, read by soft effects
    Target m_water_depth;  // ... with the water in it
    Target m_water_cube_depth;
    Target m_refraction;   // the refraction target (RGBA8): screen offset and strength per pixel (section 13)
    Target m_hdr_refracted; // the scene after ISRefraction (single-sampled), read by tonemapping
    Target m_ui_depth;      // the UI 3D scenes' depth (single-sampled, cleared by each scene)
    Target m_normal;        // the opaque pass's normals, m_samples samples (the game's RT 68)
    Target m_normal_resolved; // single-sampled when multisampling
    PostTarget m_ao_raw, m_ao_tmp, m_ao;
    PostTarget m_ssr[2]; // reflections at half size, and the blur's half way
    PostTarget m_ssr_raw; // ... before the blur: water reads both
    PostTarget m_water_mask;       // the stencil technique's view cosine
    PostTarget m_water_ripples[2]; // the ripples' state, ping-ponged
    uint32_t m_water_ripple_current = 0;
    bool m_water_ripples_ready = false;
    Target m_water_flow; // 11 x 11 cells of flow map, wrapping
    bool m_water_flow_ready = false;
    // What water keeps from frame to frame for one view: the main one and a door's far one, which swap at a crossing.
    struct WaterView {
        PostTarget history[2]; // ISWaterBlend's image, this frame's and the last
        uint32_t current = 0;
        VkImage cube = VK_NULL_HANDLE;
        VmaAllocation cube_alloc = nullptr;
        VkImageView cube_view = VK_NULL_HANDLE;
        VkImageView cube_faces[6] = {};
        uint32_t cube_slot = 0;
        bool cube_ready = false;
        uint32_t cube_next = 0; // the next face to draw
        uint32_t cube_warm = 0; // frames drawn: the first ones fill every face
        float cube_credit = 0.0f; // faces the world's clock has earned
        glm::mat4 last_view_proj{1.0f};
        glm::vec3 last_camera{0.0f};
        bool have_last = false;
    };
    WaterView m_water_views[2];
    WaterView& water_view() { return m_water_views[m_far_pass ? 1 : 0]; }
    PostTarget m_quarter, m_bloom[2], m_ldr, m_dof[2], m_radial;
    std::array<PostTarget, 6> m_lum;
    uint32_t m_lum_count = 0;
    PostTarget m_adapt[2]; // eye adaptation: this frame's and the last (effect state, docs/RENDERING.md)
    uint32_t m_adapt_index = 0;
    bool m_adapt_valid = false;
    VkImage m_camz_image = VK_NULL_HANDLE;
    VmaAllocation m_camz_alloc = nullptr;
    std::array<VkImageView, 14> m_camz_views{};
    VkImageView m_camz_full = VK_NULL_HANDLE;
    uint32_t m_camz_mips = 0;
    int m_bloom_taps = 7;
    float m_dv_clock = 0.0f; // double vision's timer
    Buffer m_sky_vertices, m_sky_indices;
    Buffer m_inject; // inject_hdr's image
    PostTarget m_vl[2], m_blurred;
    Target m_snow_source;  // the scene as the composite takes it: reflections and ISSnowSSS read it
    Buffer m_snow_flags;   // per frame slot: the composite met a snow pixel; then per slot, a pixel over 1
    bool m_snow_visible = false;
    bool m_lens_visible = false; // the lens flare has something to draw (a slot's last frame)
    PostTarget m_lens[4];        // the streaks: a sixteenth and a thirty-second wide, a quarter high, two of each
    // The summed froxels (rewritten every frame) and the noise (built once), R16F like the game's, and their set.
    struct VolumeImage {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    VolumeImage m_vl_volume, m_vl_noise;
    VkDescriptorPool m_vl_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_vl_set = VK_NULL_HANDLE;
    bool m_vl_noise_ready = false;
    // The sun's cascades: one D16 layer each, sampled through an array view (Bindless::k_sun_shadow_slot).
    Target m_sun_shadow;
    VkImageView m_sun_shadow_layers[k_max_cascades] = {};
    // The god-ray map: the same cascades for land and what casts no ordinary shadow.
    Target m_god_ray_map;
    VkImageView m_god_ray_layers[k_max_cascades] = {};
    TerrainDraws m_god_ray_land[k_max_cascades];
    // The shadow lights' maps: one D16 layer each (Bindless::k_light_shadow_slot).
    Target m_light_shadow;
    VkImageView m_light_shadow_layers[k_max_shadow_lights] = {};
    bool m_light_shadow_ready = false;
    uint32_t m_shadow_resolution = 0;
    bool m_sun_shadow_ready = false; // left in SHADER_READ_ONLY_OPTIMAL at least once
    bool m_shadow_capture_requested = false;
    uint64_t m_shadow_capture_frame = 0;
    uint32_t m_shadow_capture_slot = 0;
    uint32_t m_shadow_capture_layers = 0;
    Buffer m_shadow_capture_buffer;
    std::vector<Run> m_runs_shadow;
    std::array<std::vector<uint32_t>, 8> m_shadow_buckets; // build_shadow_runs scratch, by ShadowCaster state
    VkImage m_color = VK_NULL_HANDLE; // tonemapped, 8-bit
    VmaAllocation m_color_alloc = nullptr;
    VkImageView m_color_view = VK_NULL_HANDLE;

    bool m_hdr_capture_requested = false;
    uint64_t m_hdr_capture_frame = 0;
    uint32_t m_hdr_capture_slot = 0;
    Buffer m_hdr_capture_buffer;
    VkExtent2D m_hdr_capture_extent{};

    bool m_capture_requested = false;
    bool m_capture_overlay_requested = false;
    uint64_t m_capture_frame = 0;
    uint32_t m_capture_slot = 0;
    Buffer m_capture_buffer;
    VkExtent2D m_capture_extent{};

    Budget m_budget;
    uint64_t m_last_allocations = 0;
    FrameStats m_stats;

    // Per-frame scratch (capacity reused, no per-frame heap churn).
    std::vector<uint32_t> m_group_counts;
    std::vector<Bucket> m_buckets; // by state slot
    std::vector<uint32_t> m_bucket_order;
    size_t m_bucket_order_states = 0;
    std::vector<BlendItem> m_blend_items;
    TerrainDraws m_terrain_draws;
    std::vector<Run> m_runs_opaque, m_runs_alpha, m_runs_blend, m_runs_refraction, m_runs_second;
    std::vector<Run> m_runs_subset; // scratch: the blended runs a comparison render keeps
    std::vector<Run> m_runs_sky;    // a weather's sky model, drawn before the clouds
    std::vector<SecondPassItem> m_second_items;
    std::vector<UiSceneRuns> m_ui_runs; // per FrameInput::ui_scenes entry, this frame
    bool m_ui_depth_written = false;    // a UI scene pass has used the depth target this frame
    std::vector<VkSemaphoreSubmitInfo> m_waits, m_signals;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

} // namespace spud::render
