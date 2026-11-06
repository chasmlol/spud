#pragma once

// Draws the movie player's draw lists (ui/player/draw.hpp) over the finished scene: the render::Overlay the
// renderer calls once a frame. Each frame's movies render into one multisampled layer with a stencil buffer
// (masks), which is resolved and composited over the scene with premultiplied alpha.
//
// Geometry is pulled in the vertex shader from per-movie buffers written once when a movie is prepared;
// per-draw paint data (transform, colour transform, fill) goes into a per-frame buffer, so a frame records
// nothing but binds and draws. Bitmaps upload in the background and appear when ready.

#include "render/device.hpp"
#include "render/jobs.hpp"
#include "render/overlay.hpp"
#include "render/uploader.hpp"
#include "ui/player/draw.hpp"
#include "ui/player/images.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::ui {

// One movie view's draw list and where its stage goes on the target.
struct MovieLayer {
    const player::DrawList* list = nullptr;
    player::Matrix2D stage_to_pixels; // the view's scale mode and placement
    uint32_t background = 0;          // RGBA8: the movie's background colour at its background alpha, 0 = none
    // A UI 3D scene's place among the menus: no draw list; the overlay draws scene
    // `scene` of the frame (OverlayTarget::scenes) here, over the layers below it.
    int scene = -1;
    // An off-screen page (the Book Menu's render targets 105 + page): this draw
    // list goes into page target `page` instead of the screen, over the page's paper (set_page_paper).
    int page = -1;
};

class MovieRenderer final : public render::Overlay {
public:
    // `extent` is the overlay's size, so the targets exist before the first frame draws.
    MovieRenderer(render::Device& device, render::Uploader& uploader, VkFormat color_format, VkExtent2D extent,
                  const std::filesystem::path& shader_dir, const std::filesystem::path& pipeline_cache);
    ~MovieRenderer() override;
    MovieRenderer(const MovieRenderer&) = delete;
    MovieRenderer& operator=(const MovieRenderer&) = delete;

    // Makes a movie's geometry resident and starts uploading its bitmaps. Call when a movie is loaded,
    // not per frame; drawing a movie that was never prepared prepares it.
    void prepare(const player::MovieDef& movie);
    // The layers record() draws next, bottom first. The draw lists must stay valid until then.
    // Takes the frame's layers by swapping: `layers` gets the previous list back (its storage is reused).
    void set_layers(std::vector<MovieLayer>& layers) { m_layers.swap(layers); }
    bool empty() const { return m_layers.empty(); }

    void record(VkCommandBuffer cmd, const render::OverlayTarget& target) override;

    // Engine images. `images` names them for the draw
    // lists; texture sources are files of `cache_root`, read and decoded on `jobs` and uploaded progressively (an
    // image draws from the frame after it arrives, never stalling one).
    void set_engine_images(const player::ImageRegistry* images, render::JobSystem* jobs,
                           std::filesystem::path cache_root);
    // A render target the engine draws (the local map), sampled under its registered image name. The view must be
    // in SHADER_READ_ONLY_OPTIMAL whenever the overlay records; null removes it.
    void set_render_target_image(const std::string& name, VkImageView view);
    void set_image_pixels(const std::string& name, const std::string& key, uint32_t width, uint32_t height,
                          const std::vector<uint8_t>& rgba);
    // The Book Menu's page targets (render targets 105..108: square, the screen's height, R8G8B8A8, created with the
    // screen targets as the game does). Each frame a page layer is given
    // for (MovieLayer::page), the target is filled with the paper texture and the page's draw list is drawn over it
    // before the menus, so the frame's 3D book samples this frame's pages. The views change when the targets are
    // made again (page_generation); they are kept in SHADER_READ_ONLY_OPTIMAL outside the overlay.
    static constexpr uint32_t k_pages = 4;
    VkImageView page_view(uint32_t page) const { return page < k_pages ? m_pages[page].view : VK_NULL_HANDLE; }
    uint32_t page_size() const { return m_page_size; }
    uint32_t page_generation() const { return m_page_generation; }
    // The texture the game copies into every page first (Base Page1's diffuse, `` of the menu);
    // null: the pages get the movie over transparent black.
    void set_page_paper(VkImageView view);
    // Automation (ui page): page `page` as the last submitted frame left it, RGBA8, page_size() squared. Waits for the
    // GPU and allocates a read-back buffer: never during play.
    bool read_page(uint32_t page, std::vector<uint8_t>& rgba);

    struct Stats {
        uint32_t draws = 0;    // in the last recorded frame
        uint32_t layers = 0;   // layer passes (filters, blend modes) in the last frame
        uint32_t movies = 0;   // prepared
        uint32_t bitmaps_pending = 0;
        float gpu_ms = 0;      // last measured pass
    };
    const Stats& stats() const { return m_stats; }

private:
    enum Pipeline : uint32_t {
        pipe_content,
        pipe_mask_add,
        pipe_mask_remove,
        pipe_composite,
        pipe_filter,
        // A finished layer over its parent, one per blend mode the fixed-function blender can do.
        pipe_layer_normal,
        pipe_layer_add,
        pipe_layer_multiply,
        pipe_layer_screen,
        pipe_layer_lighten,
        pipe_layer_darken,
        pipe_layer_subtract,
        k_pipeline_count
    };

    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool ready = false;
    };
    struct MovieGpu {
        render::Buffer vertices, indices, stops;
        std::unordered_map<uint32_t, uint32_t> bitmaps; // character index -> texture slot
    };
    // One recorded step: a draw (mesh, glyph or rect) or a stencil change.
    struct Op {
        uint32_t pipeline;
        uint32_t stencil;       // reference value
        uint32_t draw;          // index into the frame's draws
        const MovieGpu* movie;  // index buffer to bind (null: rect)
        uint32_t first_index, index_count, vertex_offset;
        uint32_t texture;       // slot + 1, 0 = none
        int layer = -1;         // >= 0: composite that layer pass here
        uint32_t vertex_count = 0; // no movie: a triangle list of this many vertices (0: a rect's 6)
        float rect[4] = {0, 0, 0, 0}; // composite: where, in the pixels of the pass it is drawn in
    };
    // An object with filters or a blend mode: its content renders into a region of the layer atlas, its filters
    // run there, and its parent composites the result.
    struct LayerPass {
        VkRect2D region{};                 // in the atlas
        int origin[2] = {0, 0};            // the target pixel at the region's top-left
        std::vector<Op> ops;
        const osu::Filter* filters = nullptr;
        uint32_t filter_count = 0;
        const float* filter_values = nullptr;
        float scale = 1;                   // stage pixels -> target pixels, for filter sizes
        osu::Blend blend = osu::Blend::normal;
        player::ColorXform cx;
        uint32_t result = 0;               // atlas image holding the finished layer
        int scratch[2] = {0, 0};           // where it is drawn, in the shared targets
    };
    struct FrameData {
        render::Buffer draws;
        render::Buffer vertices; // the drawing API's triangles this frame
        VkQueryPool queries = VK_NULL_HANDLE;
        bool queries_written = false;
    };

    void create_pipelines(const std::filesystem::path& shader_dir, const std::filesystem::path& cache_file);
    void ensure_targets(VkExtent2D extent);
    void destroy_targets();
    uint32_t create_texture(uint32_t width, uint32_t height, std::vector<std::byte> rgba);
    // The texture slot of a movie's bitmap character (made and uploaded on first use), or ~0u if it is none.
    uint32_t bitmap_texture(const player::MovieDef& def, uint32_t character);
    // The texture slot of an engine image (an ImageRegistry id), or ~0u while it is loading or has none.
    uint32_t engine_image_texture(uint32_t id);
    // The draw flags an engine image's texture needs: 4 when it holds straight alpha (a texture of the cache,
    // sampled and filtered as it is, as GFx does with the loader's images), else 0.
    uint32_t engine_image_flags(uint32_t id) const;
    // Takes the engine images the workers finished (a few per frame) and starts their uploads.
    void poll_engine_images();
    uint32_t external_texture(VkImageView view);
    MovieGpu& movie_gpu(const player::MovieDef& movie);
    void build(const render::OverlayTarget& target);
    void record_ops(VkCommandBuffer cmd, std::span<const Op> ops, VkDeviceAddress draws, VkExtent2D extent);
    // The main pass for ops [first, end): drawn into the multisampled layer, resolved and composited over the target
    // (cleared to black first when `clear`).
    void record_main(VkCommandBuffer cmd, const render::OverlayTarget& target, size_t first, size_t end, bool clear,
                     VkDeviceAddress draws);
    void record_pages(VkCommandBuffer cmd, VkDeviceAddress draws);
    void create_pages(uint32_t size);
    void destroy_pages();
    void record_layers(VkCommandBuffer cmd, VkDeviceAddress draws);
    void draw_layers(VkCommandBuffer cmd, std::span<LayerPass* const> batch, VkDeviceAddress draws);
    void plan_filters(LayerPass& pass, std::vector<struct FilterStep>& steps);
    void filter_pass(VkCommandBuffer cmd, uint32_t out, const VkRect2D& region, const struct FilterPush& push,
                     uint32_t in_a, uint32_t in_b);
    bool pack(int w, int h, VkRect2D& out);

    render::Device& m_device;
    render::Uploader& m_uploader;
    VkFormat m_color_format;
    VkFormat m_stencil_format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_4_BIT;

    VkDescriptorSetLayout m_texture_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_sampler_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    std::array<VkPipeline, k_pipeline_count> m_pipelines{};
    std::array<VkSampler, 4> m_samplers{}; // clamp / repeat, nearest / smooth
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_sampler_set = VK_NULL_HANDLE;

    // Frame-lifetime targets at the overlay's size: the multisampled layer, its stencil, the resolve.
    VkExtent2D m_extent{};
    VkImage m_msaa = VK_NULL_HANDLE, m_stencil = VK_NULL_HANDLE, m_resolved = VK_NULL_HANDLE;
    VmaAllocation m_msaa_alloc = nullptr, m_stencil_alloc = nullptr, m_resolved_alloc = nullptr;
    VkImageView m_msaa_view = VK_NULL_HANDLE, m_stencil_view = VK_NULL_HANDLE, m_resolved_view = VK_NULL_HANDLE;
    VkDescriptorSet m_resolved_set = VK_NULL_HANDLE;
    // The layer atlas, the target's width and twice its height: three single-sample images in GENERAL layout, the
    // first for layer content (drawn through the targets above at its place on screen, then copied in) and two
    // for filter passes.
    VkExtent2D m_atlas_extent{};
    std::array<VkImage, 3> m_atlas{};
    std::array<VmaAllocation, 3> m_atlas_alloc{};
    std::array<VkImageView, 3> m_atlas_view{};
    std::array<VkDescriptorSet, 3> m_atlas_set{};
    VkPipelineLayout m_filter_layout = VK_NULL_HANDLE;
    int m_pack_x = 0, m_pack_y = 0, m_pack_row = 0;
    std::vector<LayerPass> m_passes; // in opening order: a layer before those inside it
    std::vector<VkImageCopy2> m_layer_copies;
    bool m_warned_atlas = false, m_warned_filter = false;
    // Where the UI 3D scenes go among the main ops: before op `at`, scene `scene`.
    struct SceneMark {
        size_t at;
        int scene;
    };
    std::vector<SceneMark> m_scene_marks;
    // The page targets and this frame's page ops (the paper first).
    struct Page {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    std::array<Page, k_pages> m_pages{};
    std::array<std::vector<Op>, k_pages> m_page_ops;
    uint32_t m_page_size = 0;
    uint32_t m_page_generation = 0;
    VkImageView m_paper_view = VK_NULL_HANDLE;
    uint32_t m_paper_texture = 0; // texture slot + 1 of the paper, 0 = none
    std::unordered_map<VkImageView, uint32_t> m_external; // views of other owners -> texture slot

    std::unordered_map<const player::MovieDef*, MovieGpu> m_movies;
    std::vector<Texture> m_textures;
    // Engine images (movie_renderer_images.cpp): by registry id, the texture slot (~0u none yet) and whether a load
    // was started; the workers' results, shared with them so a job finishing after the renderer is gone is harmless.
    struct EngineImages;
    const player::ImageRegistry* m_images = nullptr;
    render::JobSystem* m_jobs = nullptr;
    std::filesystem::path m_cache_root;
    std::vector<uint32_t> m_engine_slots;
    std::unordered_map<std::string, uint32_t> m_pixel_images; // by key: pictures the engine handed over
    std::vector<uint8_t> m_engine_requested;
    std::shared_ptr<EngineImages> m_engine_loaded;
    std::unordered_map<std::string, uint32_t> m_render_targets; // image name -> texture slot
    Texture m_blank; // bound when a draw has no bitmap
    std::vector<MovieLayer> m_layers;
    std::array<FrameData, render::k_frames_in_flight> m_frames;
    float m_timestamp_period_ns = 1.0f;

    // Per-frame scratch (capacity reused).
    std::vector<std::byte> m_draw_data;
    std::vector<osu::Vertex> m_dyn_vertices;              // this frame's drawing-API triangles
    std::vector<std::pair<uint32_t, uint32_t>> m_dyn_draws; // (draw, first vertex): their vertex addresses
    std::vector<Op> m_ops;
    Stats m_stats;
};

} // namespace spud::ui
