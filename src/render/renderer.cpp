#include "render/renderer.hpp"

#include "core/log.hpp"
#include "core/trace.hpp"
#include "core/units.hpp"
#include "render/shadows.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace spud::render {

namespace {

constexpr uint32_t k_query_count = k_gpu_pass_count + 1; // begin + one after each pass
constexpr VkDeviceSize k_cmd_stride = sizeof(VkDrawIndexedIndirectCommand);
constexpr uint32_t k_lod_slots = osm::k_lod_levels + 1;

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                   VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

bool is_alpha_test_program(Program p) { return p == program_mesh_alpha_test || p == program_effect_alpha_test; }

// Skyrim's lighting constants from the space's environment. Until a space
// has weather-derived values, they stand in for the placeholder environment: a hemisphere ambient written as a
// directional-ambient matrix and linear fog.
void fill_skyrim_inputs(GpuFrame& gf, const Environment& env, const View& view) {
    constexpr float k_unit = units::k_skyrim_unit_m;
    if (env.skyrim_inputs) {
        gf.dir_light_color = glm::vec4(env.dir_light_color, 0.0f);
        for (int i = 0; i < 3; ++i) {
            gf.dir_ambient[i] = env.dir_ambient[i];
        }
        gf.amb_spec = env.amb_spec;
        gf.ibl = env.ibl;
        // FogParam.y is per Skyrim unit; positions here are metres.
        gf.fog_param = glm::vec4(env.fog_param.x, env.fog_param.y / k_unit, env.fog_param.z, env.fog_param.w);
        gf.fog_near_color = env.fog_near_color;
        gf.fog_far_color = env.fog_far_color;
        gf.fog_color = env.fog_color;
        gf.output_clamp = env.output_clamp;
        gf.frame_params = env.frame_params;
    } else {
        gf.dir_light_color = glm::vec4(env.sun_color, 0.0f);
        for (int c = 0; c < 3; ++c) {
            gf.dir_ambient[c] = glm::vec4(0.0f, 0.0f, (env.ambient_sky[c] - env.ambient_ground[c]) * 0.5f,
                                          (env.ambient_sky[c] + env.ambient_ground[c]) * 0.5f);
        }
        const float span = std::max(env.fog_end - env.fog_start, 1.0f);
        // Linear in the clip-space distance, which is close to the view distance at these ranges.
        gf.fog_param = glm::vec4(env.fog_start / span, 1.0f / span, env.fog_power, env.fog_max);
        gf.fog_near_color = glm::vec4(env.sky_horizon, 0.0f);
        gf.fog_far_color = glm::vec4(env.sky_horizon, 0.0f);
        gf.fog_color = glm::vec4(env.sky_horizon, 1.0f);
        gf.output_clamp = glm::vec4(65504.0f, 0.0f, 65504.0f, 0.0f);
        gf.frame_params = glm::vec4(0.0f, 1.0f, 1.0f, 0.0f);
    }
    // The game's projection (D3D, depth 0..1, finite far plane): the fog distance is the length of its clip-space
    // xyz (5.2). Camera-relative, metres.
    const float f = 1.0f / std::tan(view.fov_y * 0.5f);
    const float n = env.near_units * k_unit;
    const float fz = env.far_units / (env.far_units - env.near_units);
    glm::mat4 p(0.0f);
    p[0][0] = f / view.aspect;
    p[1][1] = f;
    p[2][2] = -fz;
    p[2][3] = -1.0f;
    p[3][2] = -n * fz;
    gf.fog_view_proj = p * view.rotation;
    gf.effect_light = glm::vec4(env.effect_light, 0.0f);
    gf.clip_plane = view.clip_plane;
    gf.depth_params = glm::vec4(view.near_plane, 1.0f, 0.0f, 0.0f);
}

} // namespace

const char* gpu_pass_name(uint32_t pass) {
    switch (pass) {
    case pass_prepare: return "prepare";
    case pass_sun: return "sun";
    case pass_volume: return "volume";
    case pass_shadow: return "shadow";
    case pass_ao: return "ao";
    case pass_ssr: return "ssr";
    case pass_opaque: return "opaque";
    case pass_terrain: return "terrain";
    case pass_alpha_test: return "alpha_test";
    case pass_sky: return "sky";
    case pass_composite: return "composite";
    case pass_image_space: return "image_space";
    case pass_water: return "water";
    case pass_blend: return "alpha";
    case pass_ui: return "ui";
    case pass_post: return "post";
    default: return "?";
    }
}

Renderer::Renderer(const RendererOptions& options) : m_options(options), m_present_mode(options.present_mode) {
    if (!options.window) {
        throw Error("renderer: no window (headless runs use a hidden one)");
    }
    DeviceOptions dev_opts;
    dev_opts.validation = options.validation;
    dev_opts.need_present = true;
    std::function<VkSurfaceKHR(VkInstance)> make_surface;
    if (options.window) {
        Uint32 count = 0;
        const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&count);
        for (Uint32 i = 0; i < count; ++i) {
            dev_opts.instance_extensions.push_back(exts[i]);
        }
        make_surface = [window = options.window](VkInstance instance) {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
                throw Error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
            }
            return surface;
        };
    }
    m_device = std::make_unique<Device>(dev_opts, make_surface);
    m_jobs = std::make_unique<JobSystem>();
    m_bindless = std::make_unique<Bindless>(*m_device);
    m_uploader = std::make_unique<Uploader>(*m_device);
    m_cache_root = options.cache_root;
    m_textures = std::make_unique<TextureManager>(*m_device, *m_bindless, *m_uploader, *m_jobs, options.cache_root);

    uint64_t used = 0, budget = 0;
    m_device->memory_budget(used, budget);
    const uint64_t gib = 1ull << 30;
    // Arenas sized from VRAM, capped well above what streaming needs (Tamriel touring peaks around
    // 130 MiB of vertices and 50 MiB of indices; the LRU keeps them under 70% full). Bigger arenas only
    // take VRAM from the texture cache and, when VRAM is contested, from other processes.
    const VkDeviceSize vcap = std::clamp<uint64_t>(budget / 16, 256ull << 20, 1536ull << 20);
    const VkDeviceSize icap = std::clamp<uint64_t>(budget / 32, 128ull << 20, 768ull << 20);
    m_meshes = std::make_unique<MeshManager>(*m_device, *m_uploader, *m_jobs, *m_textures, options.cache_root, vcap,
                                             icap);
    const uint64_t texture_budget = std::clamp<uint64_t>(budget / 3, 512ull << 20, 10 * gib);
    m_textures->set_budget(texture_budget);
    m_textures->reserve(texture_budget * 6 / 10);
    // Global textures of projected-UV materials, loaded once like the game's.
    m_projected_textures[0] = m_textures->acquire("textures/effects/projectednoise.dds", TextureKind::color, 0.0f);
    m_projected_textures[1] = m_textures->acquire("textures/effects/projecteddiffuse.dds", TextureKind::color, 0.0f);
    m_projected_textures[2] = m_textures->acquire("textures/effects/projectednormal.dds", TextureKind::normal, 0.0f);
    m_projected_textures[3] =
        m_textures->acquire("textures/effects/projectednormaldetail.dds", TextureKind::normal, 0.0f);

    {
        const auto& limits = m_device->properties().limits;
        const VkSampleCountFlags supported = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts;
        m_samples = 1;
        for (uint32_t n : {8u, 4u, 2u}) {
            if (n <= options.msaa && (supported & n)) {
                m_samples = n;
                break;
            }
        }
        if (m_samples != std::max(options.msaa, 1u)) {
            log::warn("renderer: {}x MSAA not supported, using {}x", options.msaa, m_samples);
        }
    }
    m_pipelines = std::make_unique<Pipelines>(
        *m_device, *m_jobs, m_bindless->layout(), options.shader_dir, options.pipeline_cache,
        PipelineFormats{k_hdr_format, k_depth_format, static_cast<VkSampleCountFlagBits>(m_samples), m_color_format,
                        VK_FORMAT_R8G8B8A8_UNORM, k_normal_format, true});
    // The game UI's scene targets (UI 3D scenes over the 8-bit colour target, the offscreen views) are single-sampled
    // in the colour format: the same programs built for them, at startup like the scene's.
    std::filesystem::path ui_cache;
    if (!options.pipeline_cache.empty()) {
        ui_cache = std::filesystem::path(options.pipeline_cache);
        ui_cache.replace_filename(ui_cache.stem().string() + "_ui_targets" + ui_cache.extension().string());
    }
    m_ui_pipelines = std::make_unique<Pipelines>(
        *m_device, *m_jobs, m_bindless->layout(), options.shader_dir, ui_cache,
        PipelineFormats{m_color_format, k_depth_format, VK_SAMPLE_COUNT_1_BIT, m_color_format,
                        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, false});

    VkDevice dev = m_device->device();
    for (auto& f : m_frames) {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = m_device->queue_family();
        vk_check(vkCreateCommandPool(dev, &pool, nullptr, &f.pool));
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = f.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vk_check(vkAllocateCommandBuffers(dev, &alloc, &f.cmd));
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vk_check(vkCreateFence(dev, &fence, nullptr, &f.fence));
        VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        q.queryType = VK_QUERY_TYPE_TIMESTAMP;
        q.queryCount = k_query_count;
        vk_check(vkCreateQueryPool(dev, &q, nullptr, &f.queries));
        vkResetQueryPool(dev, f.queries, 0, k_query_count);
        ensure_arena(f, 8ull << 20);
    }
    m_timestamp_period_ns = m_device->properties().limits.timestampPeriod;

    VkExtent2D extent{options.render_width, options.render_height};
    m_output = std::make_unique<SwapchainOutput>(*m_device, options.window, m_present_mode, options.imgui);
    if (extent.width == 0 || extent.height == 0) {
        extent = m_output->extent();
    }
    create_targets(extent);
    create_shadow_maps();
}

Renderer::~Renderer() {
    drain();
    VkDevice dev = m_device->device();
    m_output.reset();
    for (auto& f : m_frames) {
        vkDestroyQueryPool(dev, f.queries, nullptr);
        vkDestroyFence(dev, f.fence, nullptr);
        vkDestroyCommandPool(dev, f.pool, nullptr);
        m_device->destroy_buffer(f.arena);
    }
    destroy_targets();
    destroy_shadow_maps();
    for (auto& t : m_offscreen) {
        vkDestroyImageView(dev, t.color_view, nullptr);
        vkDestroyImageView(dev, t.depth_view, nullptr);
        vmaDestroyImage(m_device->allocator(), t.color, t.color_alloc);
        vmaDestroyImage(m_device->allocator(), t.depth, t.depth_alloc);
    }
    m_device->destroy_buffer(m_skin_decals);
    for (Buffer& ring : m_skin_rows) {
        m_device->destroy_buffer(ring);
    }
    m_device->destroy_buffer(m_snow_flags);
    m_device->destroy_buffer(m_sky_vertices);
    m_device->destroy_buffer(m_sky_indices);
    if (m_water_flow.view) {
        vkDestroyImageView(dev, m_water_flow.view, nullptr);
        vmaDestroyImage(m_device->allocator(), m_water_flow.image, m_water_flow.alloc);
    }
    for (WaterView& w : m_water_views) {
        if (w.cube) {
            for (VkImageView face : w.cube_faces) {
                vkDestroyImageView(dev, face, nullptr);
            }
            vkDestroyImageView(dev, w.cube_view, nullptr);
            vmaDestroyImage(m_device->allocator(), w.cube, w.cube_alloc);
        }
    }
    m_device->destroy_buffer(m_capture_buffer);
    m_device->destroy_buffer(m_hdr_capture_buffer);
    m_device->destroy_buffer(m_shadow_capture_buffer);
    m_pipelines.reset();
    m_ui_pipelines.reset();
    m_meshes.reset();
    m_device->flush_all_garbage();
    m_textures.reset();
    m_uploader.reset();
    m_bindless.reset();
    m_jobs.reset();
    m_device.reset();
}

void Renderer::drain() {
    m_jobs->wait_idle();
    while (m_uploader->busy() > 0) {
        m_uploader->poll(SIZE_MAX, SIZE_MAX);
        m_textures->update(); // failures posted by workers
        m_meshes->update();
        m_jobs->wait_idle();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    m_device->wait_idle();
    m_device->set_frame_serials(m_frame_number + 1, m_frame_number + 1);
    m_device->flush_all_garbage();
}

void Renderer::wait_idle() { m_device->wait_idle(); }

void Renderer::create_targets(VkExtent2D extent) {
    m_target_extent = extent;
    auto make = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t samples,
                    VkImage& image, VmaAllocation& alloc, VkImageView& view, const char* name) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = static_cast<VkSampleCountFlagBits>(samples);
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        // Cleared and fully redrawn every frame: nothing in them may be read by the next frame.
        vk_check(m_device->create_image(info, ai, Lifetime::frame, name, image, alloc));
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = image;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = format;
        v.subresourceRange = {aspect, 0, 1, 0, 1};
        vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &view));
    };
    const VkImageUsageFlags read = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    make(k_hdr_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (m_samples == 1 ? read : 0),
         VK_IMAGE_ASPECT_COLOR_BIT,
         m_samples, m_hdr.image, m_hdr.alloc, m_hdr.view, "scene hdr");
    if (m_samples > 1) {
        make(k_hdr_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | read, VK_IMAGE_ASPECT_COLOR_BIT, 1,
             m_hdr_resolved.image, m_hdr_resolved.alloc, m_hdr_resolved.view, "scene hdr resolved");
    }
    make(k_depth_format,
         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
             (m_samples == 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VK_IMAGE_USAGE_SAMPLED_BIT),
         VK_IMAGE_ASPECT_DEPTH_BIT, m_samples, m_depth_target.image, m_depth_target.alloc, m_depth_target.view,
         "scene depth");
    if (m_samples > 1) {
        m_bindless->write(Bindless::k_scene_depth_slot, m_depth_target.view);
    }
    make(k_normal_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (m_samples == 1 ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
         VK_IMAGE_ASPECT_COLOR_BIT, m_samples, m_normal.image, m_normal.alloc, m_normal.view, "scene normals");
    if (m_samples > 1) {
        make(k_normal_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
             VK_IMAGE_ASPECT_COLOR_BIT, 1, m_normal_resolved.image, m_normal_resolved.alloc, m_normal_resolved.view,
             "scene normals resolved");
    }
    m_bindless->write(Bindless::k_normal_slot, m_samples > 1 ? m_normal_resolved.view : m_normal.view);
    make(k_depth_format,
         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
             (m_samples == 1 ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0),
         VK_IMAGE_ASPECT_DEPTH_BIT, 1, m_depth_copy.image, m_depth_copy.alloc, m_depth_copy.view, "scene depth copy");
    m_bindless->write(Bindless::k_depth_slot, m_depth_copy.view);
    make(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
         VK_IMAGE_ASPECT_COLOR_BIT, 1, m_refraction.image, m_refraction.alloc, m_refraction.view, "refraction");
    m_bindless->write(Bindless::k_refraction_slot, m_refraction.view);
    make(k_hdr_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
         1, m_hdr_refracted.image, m_hdr_refracted.alloc, m_hdr_refracted.view, "scene hdr refracted");
    m_bindless->write(Bindless::k_refracted_slot, m_hdr_refracted.view);
    make(m_color_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         VK_IMAGE_ASPECT_COLOR_BIT, 1, m_color, m_color_alloc, m_color_view, "scene color");
    make(k_depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, 1, m_ui_depth.image,
         m_ui_depth.alloc, m_ui_depth.view, "ui scene depth");
    m_bindless->write(Bindless::k_scene_slot, m_samples > 1 ? m_hdr_resolved.view : m_hdr.view);
    create_post_targets(extent);
}

void Renderer::destroy_targets() {
    VkDevice dev = m_device->device();
    destroy_post_targets();
    for (Target* t : {&m_hdr, &m_hdr_resolved, &m_depth_target, &m_depth_copy, &m_refraction, &m_hdr_refracted,
                      &m_ui_depth, &m_normal, &m_normal_resolved}) {
        if (t->view) {
            vkDestroyImageView(dev, t->view, nullptr);
            vmaDestroyImage(m_device->allocator(), t->image, t->alloc);
        }
        *t = Target{};
    }
    if (m_color_view) {
        vkDestroyImageView(dev, m_color_view, nullptr);
        vmaDestroyImage(m_device->allocator(), m_color, m_color_alloc);
    }
    m_color_view = VK_NULL_HANDLE;
    m_color = VK_NULL_HANDLE;
}

void Renderer::set_present_mode(PresentMode mode) {
    m_present_mode = mode;
    m_output->set_present_mode(mode);
}

void Renderer::set_render_size(uint32_t width, uint32_t height) {
    m_options.render_width = width;
    m_options.render_height = height;
    VkExtent2D e{width, height};
    if (width == 0 || height == 0) {
        e = m_output->extent();
    }
    if (e.width != m_target_extent.width || e.height != m_target_extent.height) {
        m_device->wait_idle();
        destroy_targets();
        create_targets(e);
    }
}

bool Renderer::busy() const {
    return m_jobs->pending() > 0 || m_uploader->busy() > 0 || m_textures->busy() > 0 || m_meshes->busy() > 0;
}

void Renderer::ensure_arena(Frame& frame, VkDeviceSize size) {
    if (frame.arena && frame.arena.size >= frame.arena_used + size) {
        return;
    }
    // A door's far frame is already in the arena: it stays, for the commands that point at it, and the next frame's
    // arena holds both. Otherwise only ever called for a slot whose previous GPU work has completed.
    VkDeviceSize need = size;
    if (frame.arena_used > 0) {
        need += frame.arena.size;
        frame.old_arenas.push_back(frame.arena);
        frame.arena = {};
        frame.arena_used = 0;
    } else {
        m_device->destroy_buffer(frame.arena);
    }
    const VkDeviceSize cap = std::max<VkDeviceSize>(need + need / 2, 8ull << 20);
    frame.arena = m_device->create_buffer(cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          MemoryUsage::dynamic, Lifetime::frame, "frame arena");
}

VkDeviceSize Renderer::arena_alloc(Frame& frame, VkDeviceSize size, VkDeviceSize align) {
    const VkDeviceSize offset = (frame.arena_used + align - 1) / align * align;
    frame.arena_used = offset + size;
    return offset;
}

void Renderer::read_timestamps(Frame& frame) {
    if (!frame.timestamps_written || frame.number == 0) {
        return;
    }
    uint64_t ts[k_query_count];
    const VkResult r = vkGetQueryPoolResults(m_device->device(), frame.queries, 0, k_query_count, sizeof(ts), ts,
                                             sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    frame.timestamps_written = false;
    if (r != VK_SUCCESS) {
        return;
    }
    GpuTiming t;
    t.frame = frame.number;
    const double to_ms = m_timestamp_period_ns / 1e6;
    t.total_ms = static_cast<float>((ts[k_query_count - 1] - ts[0]) * to_ms);
    for (uint32_t p = 0; p < k_gpu_pass_count; ++p) {
        t.passes_ms[p] = static_cast<float>((ts[p + 1] - ts[p]) * to_ms);
    }
    m_gpu_timings.push_back(t);
}

void Renderer::take_gpu_timings(std::vector<GpuTiming>& out) {
    out.clear();
    out.swap(m_gpu_timings);
}

void Renderer::wait_frame() {
    Frame& fr = m_frames[m_slot];
    vk_check(vkWaitForFences(m_device->device(), 1, &fr.fence, VK_TRUE, UINT64_MAX));
}

void Renderer::flush_gpu_timings() {
    m_device->wait_idle();
    // Oldest slot first so timings stay in frame order.
    for (uint32_t i = 0; i < k_frames_in_flight; ++i) {
        read_timestamps(m_frames[(m_slot + i) % k_frames_in_flight]);
    }
}

void Renderer::begin_frame() {
    Frame& fr = m_frames[m_slot];
    VkDevice dev = m_device->device();
    vk_check(vkWaitForFences(dev, 1, &fr.fence, VK_TRUE, UINT64_MAX));
    read_timestamps(fr);
    SPUD_TRACE("renderer begin_frame");
    const uint64_t recording = m_frame_number + 1;
    const uint64_t completed = recording > k_frames_in_flight ? recording - k_frames_in_flight : 0;
    m_device->set_frame_serials(recording, completed);
    // Descriptor writes queued for this slot first: deferred frees may destroy their old views.
    {
        SPUD_TRACE("bindless");
        m_bindless->begin_frame(m_slot);
    }
    {
        SPUD_TRACE("garbage");
        m_device->collect_garbage();
    }
    {
        SPUD_TRACE("upload completions");
        m_uploader->poll(m_budget.max_completions, m_budget.max_completion_bytes);
    }
    if (const uint64_t a = m_device->memory_events().allocations.load(); a != m_last_allocations) {
        if (m_frame_number > 0 && a > m_last_allocations) {
            log::warn("renderer: {} device-memory allocation(s) after startup (frame {}); "
                      "these stall the GPU, reserve more up front",
                      a - m_last_allocations, m_frame_number);
        }
        m_last_allocations = a;
    }
    {
        SPUD_TRACE("textures update");
        m_textures->update();
    }
    {
        SPUD_TRACE("meshes update");
        m_meshes->update();
    }
    fr.arena_used = 0;
    for (auto& old : fr.old_arenas) {
        m_device->destroy_buffer(old);
    }
    fr.old_arenas.clear();
    m_frame_begun = true;
}

TerrainFrame Renderer::write_terrain(Frame& frame, const TerrainDraws& draws) {
    TerrainFrame land;
    land.near_count = static_cast<uint32_t>(draws.near.size());
    land.far_count = static_cast<uint32_t>(draws.far.size());
    if (land.near_count > 0) {
        const VkDeviceSize offset = arena_alloc(frame, draws.near.size() * sizeof(uint32_t), 16);
        std::memcpy(frame.arena.as<std::byte>() + offset, draws.near.data(), draws.near.size() * sizeof(uint32_t));
        land.near = frame.arena.address + offset;
    }
    if (land.far_count > 0) {
        const VkDeviceSize offset = arena_alloc(frame, draws.far.size() * sizeof(GpuFarNode), 16);
        std::memcpy(frame.arena.as<std::byte>() + offset, draws.far.data(), draws.far.size() * sizeof(GpuFarNode));
        land.far = frame.arena.address + offset;
    }
    return land;
}

void Renderer::record_terrain(VkCommandBuffer cmd, const Terrain& terrain, const TerrainFrame& land, PushConstants& pc,
                              const Pipelines& pipelines, bool wireframe, bool flip_winding, Program near_program,
                              Program far_program) {
    if (land.near_count + land.far_count == 0) {
        return;
    }
    apply_state(cmd, DrawState{}, flip_winding);
    vkCmdBindIndexBuffer(cmd, terrain.index_buffer(), 0, VK_INDEX_TYPE_UINT32);
    if (land.near_count > 0) {
        pc.terrain_cells = land.near;
        vkCmdPushConstants(cmd, pipelines.layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.get(near_program, wireframe));
        vkCmdDrawIndexed(cmd, terrain.near_index_count(), land.near_count, 0, 0, 0);
        ++m_stats.draw_calls;
    }
    if (land.far_count > 0) {
        pc.terrain_cells = land.far;
        vkCmdPushConstants(cmd, pipelines.layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.get(far_program, wireframe));
        vkCmdDrawIndexed(cmd, terrain.far_index_count(), land.far_count, terrain.far_first_index(), 0, 0);
        ++m_stats.draw_calls;
    }
}

void Renderer::apply_state(VkCommandBuffer cmd, const DrawState& s, bool flip_winding) {
    vkCmdSetCullMode(cmd, s.two_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
    vkCmdSetFrontFace(cmd, flip_winding ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthTestEnable(cmd, s.z_test ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(cmd, s.z_write ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthBiasEnable(cmd, s.decal || s.membrane ? VK_TRUE : VK_FALSE);
    if (s.membrane) {
        vkCmdSetDepthBias(cmd, 3.0f, 100.0f, 0.4f); // depth bias mode 1, reversed as the decal's
    } else if (s.decal) {
        // The game's decal depth-bias state (DepthBias -1, SlopeScaledDepthBias -0.65, clamp -100 on its standard-Z
        // target); reverse-Z flips the signs. Its viewport depth-range offset has no
        // reverse-Z equivalent and is left out.
        vkCmdSetDepthBias(cmd, 1.0f, 100.0f, 0.65f);
    }
}

void Renderer::record_runs(VkCommandBuffer cmd, Frame& frame, const std::vector<Run>& runs, bool wireframe,
                           bool flip_winding, const Pipelines* pipelines) {
    const Pipelines& set = pipelines ? *pipelines : *m_pipelines;
    VkPipeline bound = VK_NULL_HANDLE;
    for (const auto& run : runs) {
        const DrawState s = DrawState::unpack(run.state);
        VkPipeline p = set.get(s.program, wireframe, s.blend_src, s.blend_dst, s.variant);
        if (p != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            bound = p;
        }
        apply_state(cmd, s, flip_winding);
        vkCmdDrawIndexedIndirect(cmd, frame.arena.buffer, run.offset, run.count, k_cmd_stride);
        ++m_stats.draw_calls;
    }
}

namespace {

// The draw groups an item draws: all of its mesh's (its directional variant's), or the part it names.
std::span<const uint32_t> item_groups(const MeshManager& meshes, const DrawItem& item) {
    const std::span<const uint32_t> all = meshes.groups(item.mesh, item.variant);
    if (item.group_count == k_all_groups) {
        return all;
    }
    if (item.first_group >= all.size()) {
        return {};
    }
    return all.subspan(item.first_group, std::min<size_t>(item.group_count, all.size() - item.first_group));
}

} // namespace

void Renderer::build_mesh_draws(Frame& frame, const SpaceView& space, std::vector<Run>& opaque,
                                std::vector<Run>& alpha, std::vector<Run>& blend, VkDeviceAddress& draw_instances) {
    opaque.clear();
    alpha.clear();
    blend.clear();
    m_runs_refraction.clear();
    m_runs_second.clear();
    m_runs_sky.clear();
    draw_instances = 0;
    const MeshManager& meshes = *m_meshes;
    if (!space.draws || space.draws->size() == 0) {
        return;
    }
    if (m_group_counts.size() < size_t(meshes.group_count()) * k_lod_slots) {
        m_group_counts.resize((size_t(meshes.group_count()) + 1024) * k_lod_slots, 0);
    }
    const auto states = meshes.states();
    if (m_buckets.size() < states.size()) {
        m_buckets.resize(states.size());
    }
    if (m_bucket_order_states != states.size()) {
        m_bucket_order.resize(states.size());
        for (uint32_t i = 0; i < states.size(); ++i) {
            m_bucket_order[i] = i;
        }
        std::sort(m_bucket_order.begin(), m_bucket_order.end(),
                  [&](uint32_t a, uint32_t b) { return states[a] < states[b]; });
        m_bucket_order_states = states.size();
    }

    // Pass 1: count instances per draw group; collect blended draws separately.
    m_blend_items.clear();
    uint32_t instanced = 0;
    m_second_items.clear();
    for (const DrawItem& item : space.draws->items()) {
        if (item.second_pass) {
            for (uint32_t g : meshes.groups(item.mesh)) {
                const uint32_t material = meshes.variant_material(item.variant, meshes.group(g).local);
                if (material != MeshManager::k_no_material) {
                    m_second_items.push_back({g, item.instance, material});
                }
            }
            continue;
        }
        for (uint32_t g : item_groups(meshes, item)) {
            const DrawGroup& dg = meshes.group(g);
            if (dg.blood && item.state == 0) {
                continue;
            }
            if (item.state != 0 || is_blend_program(static_cast<Program>(dg.state >> 16))) {
                // A variant's groups carry its materials; a UI scene's item may replace them.
                m_blend_items.push_back({item.distance_sq, g, item.instance, static_cast<uint32_t>(m_blend_items.size()),
                                         item.material == k_group_material ? dg.material : item.material, item.sky,
                                         item.state != 0 ? item.state : dg.state});
                continue;
            }
            const uint32_t key = g * k_lod_slots + item.lod;
            if (m_group_counts[key]++ == 0) {
                m_buckets[dg.state_slot].groups.push_back(key);
            }
            ++instanced;
        }
    }
    uint32_t group_cmds = 0;
    for (const auto& b : m_buckets) {
        group_cmds += static_cast<uint32_t>(b.groups.size());
    }
    const uint32_t total_instances =
        instanced + static_cast<uint32_t>(m_blend_items.size() + m_second_items.size());
    const uint32_t total_cmds = group_cmds + static_cast<uint32_t>(m_blend_items.size() + m_second_items.size());
    if (total_instances == 0) {
        return;
    }

    const VkDeviceSize inst_offset = arena_alloc(frame, VkDeviceSize(total_instances) * sizeof(GpuDrawInstance), 16);
    const VkDeviceSize cmd_offset = arena_alloc(frame, VkDeviceSize(total_cmds) * k_cmd_stride, 16);
    auto* inst = reinterpret_cast<GpuDrawInstance*>(frame.arena.as<std::byte>() + inst_offset);
    auto* cmds = reinterpret_cast<VkDrawIndexedIndirectCommand*>(frame.arena.as<std::byte>() + cmd_offset);
    draw_instances = frame.arena.address + inst_offset;

    // Pass 2: commands in draw-state order; counts become write cursors.
    uint32_t next_instance = 0;
    uint32_t next_cmd = 0;
    for (uint32_t slot : m_bucket_order) {
        const auto& b = m_buckets[slot];
        if (b.groups.empty()) {
            continue;
        }
        const uint32_t state = states[slot];
        const uint32_t first_cmd = next_cmd;
        for (uint32_t key : b.groups) {
            const DrawGroup& dg = meshes.group(key / k_lod_slots);
            const uint32_t lod = key % k_lod_slots;
            const uint32_t count = m_group_counts[key];
            cmds[next_cmd++] = {dg.lod_count[lod], count, dg.lod_first[lod], dg.vertex_offset, next_instance};
            m_stats.mesh_triangles += uint64_t(dg.lod_count[lod] / 3) * count;
            m_group_counts[key] = next_instance;
            next_instance += count;
        }
        Run run{state, cmd_offset + VkDeviceSize(first_cmd) * k_cmd_stride, next_cmd - first_cmd};
        const auto program = static_cast<Program>(state >> 16);
        (program == program_refraction ? m_runs_refraction : is_alpha_test_program(program) ? alpha : opaque)
            .push_back(run);
    }
    // Pass 3: instance entries.
    for (const DrawItem& item : space.draws->items()) {
        if (item.second_pass) {
            continue;
        }
        for (uint32_t g : item_groups(meshes, item)) {
            const DrawGroup& dg = meshes.group(g);
            if (item.state != 0 || is_blend_program(static_cast<Program>(dg.state >> 16))) {
                continue;
            }
            inst[m_group_counts[g * k_lod_slots + item.lod]++] = {
                item.instance, item.material == k_group_material ? dg.material : item.material};
        }
    }
    // Pass 4: reset scratch.
    for (uint32_t slot : m_bucket_order) {
        for (uint32_t g : m_buckets[slot].groups) {
            m_group_counts[g] = 0;
        }
        m_buckets[slot].groups.clear();
    }

    // Blended: back to front, one command each, merged into runs of equal state.
    std::sort(m_blend_items.begin(), m_blend_items.end(),
              [](const BlendItem& a, const BlendItem& b) {
                  return a.distance_sq != b.distance_sq ? a.distance_sq > b.distance_sq : a.order < b.order;
              });
    for (const auto& bi : m_blend_items) {
        const DrawGroup& dg = meshes.group(bi.group);
        inst[next_instance] = {bi.instance, bi.material};
        cmds[next_cmd] = {dg.index_count, 1, dg.first_index, dg.vertex_offset, next_instance};
        if (bi.sky) {
            DrawState s = DrawState::unpack(dg.state);
            s.program = program_effect_sky;
            s.z_test = true;
            s.z_write = false;
            m_runs_sky.push_back({s.pack(), cmd_offset + VkDeviceSize(next_cmd) * k_cmd_stride, 1});
        } else if (!blend.empty() && blend.back().state == bi.state) {
            ++blend.back().count;
        } else {
            blend.push_back({bi.state, cmd_offset + VkDeviceSize(next_cmd) * k_cmd_stride, 1});
        }
        ++next_instance;
        ++next_cmd;
    }
    // The multi-pass directional materials' second pass: one command each, the object's state with the decal depth
    // bias (11.2.3: depth test and write, bias mode 8).
    for (const auto& si : m_second_items) {
        const DrawGroup& dg = meshes.group(si.group);
        DrawState s = DrawState::unpack(dg.state);
        s.program = program_mesh_opaque;
        s.variant = mesh_variant_full; // type 14 discards
        s.decal = true;
        s.z_test = true;
        s.z_write = true;
        const uint32_t state = s.pack();
        inst[next_instance] = {si.instance, si.material};
        cmds[next_cmd] = {dg.index_count, 1, dg.first_index, dg.vertex_offset, next_instance};
        if (!m_runs_second.empty() && m_runs_second.back().state == state) {
            ++m_runs_second.back().count;
        } else {
            m_runs_second.push_back({state, cmd_offset + VkDeviceSize(next_cmd) * k_cmd_stride, 1});
        }
        ++next_instance;
        ++next_cmd;
    }
    m_stats.mesh_commands = total_cmds;
    m_stats.mesh_instances = total_instances;
    m_stats.blended = static_cast<uint32_t>(m_blend_items.size());
    m_stats.buckets = static_cast<uint32_t>(opaque.size() + alpha.size() + blend.size());
}

VkDeviceSize Renderer::ui_scenes_arena(const FrameInput& input) const {
    // The frame's animated materials, when no world view wrote them.
    VkDeviceSize bytes = input.ui_scenes.empty() ? 0 : m_meshes->material_anim_count() * sizeof(GpuMaterialAnim);
    for (const UiSceneInput& scene : input.ui_scenes) {
        size_t groups = 0;
        if (scene.space.draws && input.settings.draw_meshes) {
            for (const auto& item : scene.space.draws->items()) {
                groups += item_groups(*m_meshes, item).size();
            }
        }
        bytes += 512 + sizeof(GpuFrame) + groups * (sizeof(GpuDrawInstance) + k_cmd_stride) +
                 scene.space.lights.size() * sizeof(GpuPointLight);
    }
    return bytes;
}

void Renderer::prepare_ui_scenes(Frame& fr, const FrameInput& input) {
    if (m_ui_runs.size() < input.ui_scenes.size()) {
        m_ui_runs.resize(input.ui_scenes.size());
    }
    const FrameStats world = m_stats;
    for (size_t i = 0; i < input.ui_scenes.size(); ++i) {
        const UiSceneInput& scene = input.ui_scenes[i];
        UiSceneRuns& runs = m_ui_runs[i];
        runs.opaque.clear();
        runs.alpha.clear();
        runs.blend.clear();
        runs.draw_instances = 0;
        runs.instance_table = scene.space.instance_table;
        if (!scene.view) {
            continue;
        }
        runs.frame_offset = write_frame_data(fr, *scene.view, scene.space, input.settings, input.time,
                                             input.previous_time, m_target_extent, 1);
        runs.lights = write_point_lights(fr, *scene.view, scene.space, input.settings);
        runs.material_anims = write_material_anims(fr, input);
        if (scene.space.draws && input.settings.draw_meshes) {
            build_mesh_draws(fr, scene.space, runs.opaque, runs.alpha, runs.blend, runs.draw_instances);
        }
    }
    // build_mesh_draws reports one space's counts: keep the world's (triangles and draw calls add up anyway).
    m_stats.mesh_commands = world.mesh_commands;
    m_stats.mesh_instances = world.mesh_instances;
    m_stats.blended = world.blended;
    m_stats.buckets = world.buckets;
}

VkDeviceSize Renderer::write_frame_data(Frame& fr, const View& view, const SpaceView& space,
                                       const RenderSettings& rs, float time, float previous_time,
                                       VkExtent2D extent, uint32_t samples) {
    const VkDeviceSize frame_offset = arena_alloc(fr, sizeof(GpuFrame), 256);
    GpuFrame gf{};
    gf.view_proj = view.view_proj;
    gf.inv_view_proj = glm::inverse(view.view_proj);
    gf.camera_pos = glm::vec4(view.position, time);
    gf.door_info.x = uint32_t(std::min<size_t>(space.door_lights.size(), k_door_lights));
    for (uint32_t i = 0; i < gf.door_info.x; ++i) {
        const DoorLight& d = space.door_lights[i];
        gf.doors[i].position = glm::vec4(d.position - view.position, d.radius);
        gf.doors[i].normal = glm::vec4(d.normal, d.openness);
        gf.doors[i].colour = glm::vec4(d.colour, 0.0f);
        std::copy(std::begin(d.ambient), std::end(d.ambient), gf.doors[i].ambient);
    }
    const Environment& env = space.environment;
    gf.clock = glm::vec4(previous_time, env.sky.transition, 0.0f, 0.0f);
    gf.sun_dir = glm::vec4(glm::normalize(env.sun_dir), 0);
    gf.sun_color = glm::vec4(env.sun_color, env.sun_disc);
    gf.ambient_sky = glm::vec4(env.ambient_sky, 0);
    gf.ambient_ground = glm::vec4(env.ambient_ground, 0);
    gf.sky_zenith = glm::vec4(env.sky_zenith, 0);
    gf.sky_horizon = glm::vec4(env.sky_horizon, 0);
    gf.sky_ground = glm::vec4(env.sky_ground, 0);
    gf.fog = glm::vec4(env.fog_start, env.fog_end, env.fog_max, env.fog_power);
    gf.water_color = env.water_color;
    gf.params = glm::vec4(rs.terrain_tiling, 1.0f / std::max(space.cell_size, 1.0f), rs.flip_green ? -1.0f : 1.0f,
                          rs.composite && env.sky.image_space_valid && !env.sky.ui_scene ? 1.2f : 65504.0f);
    gf.flags = glm::uvec4(rs.debug_view, rs.lighting_off ? 1u : 0u, samples, rs.specular_aa ? 1u : 0u);
    fill_skyrim_inputs(gf, env, view);
    if (space.terrain) {
        space.terrain->far_params(rs.terrain, gf.far_land);
    }
    // fGrassWindMagnitudeMin + (Max - Min) x wind speed, at most 60, x fWindGrassMultiplier.
    gf.far_land.grass = glm::vec4(rs.grass_fade_start, rs.grass_fade_range,
                                  std::min(5.0f + 120.0f * space.environment.wind_speed, 60.0f), 0.0f);
    gf.far_land.ambient = glm::vec4(space.environment.ambient_colour, 0.0f);
    gf.dir_light_color.w = rs.sun_shadow ? 0.0f : 1.0f; // the sun's shadow factor
    gf.projected_params = glm::vec4(rs.projected_diffuse_tiling, rs.projected_detail_tiling, 0.0f,
                                    rs.projected_textured ? 1.0f : 0.0f);
    gf.snow_rim = rs.snow_rim;
    gf.projected_textures = glm::uvec4(m_projected_textures[0], m_projected_textures[1], m_projected_textures[2],
                                       m_projected_textures[3]);
    // The view basis (rows of the world -> view rotation) and the LOD fade metric of section 2.2:
    // m = |bound centre - camera| x A / M / 7200 with A the camera's LOD adjust, the horizontal field of view over
    // fDefaultFOV (80), and M fLODFadeOutMultObjects.
    gf.camera_right = glm::vec4(view.rotation[0][0], view.rotation[1][0], view.rotation[2][0], 0.0f);
    gf.camera_up = glm::vec4(view.rotation[0][1], view.rotation[1][1], view.rotation[2][1], 0.0f);
    {
        const float fov_h = 2.0f * std::atan(std::tan(view.fov_y * 0.5f) * view.aspect);
        const float lod_adjust = glm::degrees(fov_h) / 80.0f;
        gf.lod_fade = glm::vec4(lod_adjust / rs.lod_fade_out_mult_objects / 7200.0f / units::k_skyrim_unit_m,
                                float(extent.width), float(extent.height), space.tree_near);
        // Items and sky-cell references use their own multipliers; visible-when-distant ones s = 0.2,
        // without the LOD adjust (2.2.2, 2.2.4).
        const float per_metre = 1.0f / 7200.0f / units::k_skyrim_unit_m;
        gf.lod_fade_types =
            glm::vec4(lod_adjust / rs.lod_fade_out_mult_items * per_metre,
                      lod_adjust / rs.lod_fade_out_mult_sky_cell * per_metre, 0.2f * per_metre, 0.0f);
    }
    gf.fade_spec_env = glm::vec4(rs.specular_fade[0], rs.specular_fade[1], rs.envmap_fade[0], rs.envmap_fade[1]);
    gf.fade_decal_refr =
        glm::vec4(rs.decal_fade[0], rs.decal_fade[1], rs.refraction_fade[0], rs.refraction_fade[1]);
    gf.tree = glm::vec4(units::to_metres(rs.leaf_dampen[0]), units::to_metres(rs.leaf_dampen[1]),
                        rs.tree_wind_magnitude, rs.tree_wave_scale);
    // The sun's shadow mask: only the main view of a space with cascades has it.
    if (const SunShadows* sun = space.sun_shadows; sun && sun->count > 0) {
        for (uint32_t c = 0; c < sun->count; ++c) {
            for (int r = 0; r < 3; ++r) {
                gf.shadow_rows[3 * c + r] = sun->cascades[c].rows[r];
            }
        }
        const uint32_t last = sun->count - 1;
        gf.shadow_split = glm::vec4(sun->cascades[0].end_z, sun->cascades[std::min(1u, last)].end_z,
                                    sun->cascades[last].end_z, float(sun->count));
        const float fade = sun->fade_distance;
        gf.shadow_params = glm::vec4(sun->blend_start_z, sun->cascades[0].bias, sun->cascades[std::min(1u, last)].bias,
                                     fade > 0.0f ? 1.0f / (fade * fade) : 0.0f);
        gf.shadow_filter = glm::vec4(sun->filter_radius, 0.0f, 0.0f, 0.0f);
        gf.shadow_info.x = sun->count;
        gf.shadow_info.y = Bindless::k_sun_shadow_slot;
    }
    gf.shadow_info.z = Bindless::k_light_shadow_slot;
    gf.shadow_info.w = static_cast<uint32_t>(std::min<size_t>(space.shadow_lights.size(), k_shadow_light_slots));
    for (size_t i = 0; i < std::min<size_t>(space.shadow_lights.size(), k_shadow_light_slots); ++i) {
        const ShadowLight& l = space.shadow_lights[i];
        for (int r = 0; r < 4; ++r) {
            gf.shadow_light_rows[4 * i + r] = l.rows[r];
        }
        gf.shadow_light_params[i] = glm::vec4(float(l.type), float(i), l.bias, l.param);
        gf.shadow_light_fade[int(i)] = 1.0f / (l.fade_distance * l.fade_distance);
    }
    std::memcpy(fr.arena.as<std::byte>() + frame_offset, &gf, sizeof(gf));
    return frame_offset;
}

// Point lights against a view: culled outside the frustum, faded by
// q = |P - camera|^2 - r^2 between the start and end distances squared, culled beyond the end. Positions are
// relative to the view's camera, as the frame constants are.
VkDeviceAddress Renderer::write_point_lights(Frame& fr, const View& view, const SpaceView& space,
                                             const RenderSettings& rs) {
    if (!space.lights.empty()) {
        const VkDeviceSize offset = arena_alloc(fr, space.lights.size() * sizeof(GpuPointLight), 16);
        auto* dst = reinterpret_cast<GpuPointLight*>(fr.arena.as<std::byte>() + offset);
        // An interior's own light fade (g_LightFadeStartSq / EndSq), else the settings'.
        const float start = units::to_metres(space.light_fade_begin >= 0.0f ? space.light_fade_begin
                                                                            : rs.light_fade_start);
        const float end = units::to_metres(space.light_fade_end >= 0.0f ? space.light_fade_end
                                                                        : rs.light_fade_start + rs.light_fade_range);
        const float start_sq = start * start;
        const float end_sq = end * end;
        for (size_t i = 0; i < space.lights.size(); ++i) {
            const PointLight& l = space.lights[i];
            const glm::vec3 rel = l.position - view.position;
            float fade = 0.0f;
            if (rs.point_lights && (l.flags & point_light_on) && view.frustum.sphere(rel, l.radius)) {
                const float q = glm::dot(rel, rel) - l.radius * l.radius;
                if ((l.flags & point_light_never_fades) || q < start_sq || end_sq == 0.0f) {
                    fade = 1.0f;
                } else if (q <= end_sq) {
                    fade = 1.0f - (q - start_sq) / (end_sq - start_sq);
                }
            }
            dst[i].position = glm::vec4(rel, l.radius);
            dst[i].color = fade > 0.0f ? glm::vec4(l.color * fade, 1.0f) : glm::vec4(0.0f);
            // A shadow light lights only when it is chosen for the frame's four. Second
            // views have no maps and light with it plainly.
            if ((l.flags & point_light_shadow) && !space.shadow_lights.empty()) {
                dst[i].color.w = l.shadow != ~0u && fade > 0.0f ? 2.0f + float(l.shadow) : 0.0f;
            }
        }
        return fr.arena.address + offset;
    }
    return 0;
}

// The values of animated materials (16.5) for this frame, once: controllers on this frame's clock, UV values one
// frame back. Every view of the frame reads the same ones.
VkDeviceAddress Renderer::write_material_anims(Frame& fr, const FrameInput& input) {
    if (m_material_anims_address == 0) {
        if (const uint32_t n = m_meshes->material_anim_count()) {
            const VkDeviceSize offset = arena_alloc(fr, VkDeviceSize(n) * sizeof(GpuMaterialAnim), 16);
            m_meshes->write_material_anims(input.time, input.previous_time,
                                           reinterpret_cast<GpuMaterialAnim*>(fr.arena.as<std::byte>() + offset));
            m_material_anims_address = fr.arena.address + offset;
        }
    }
    return m_material_anims_address;
}

void Renderer::record_skinning(Frame& fr, const SkinFrame& skin) {
    // A frame between ticks reads what the last tick uploaded, so it runs that tick's jobs and no others: a later job's
    // rows and face morphs were never written, and the shader would follow whatever is there as an address.
    if (skin.tick) {
        m_skin_uploaded_jobs = skin.jobs.size();
    }
    const std::span<const SkinJob> jobs = skin.jobs.first(std::min(skin.jobs.size(), m_skin_uploaded_jobs));
    if (jobs.empty()) {
        return;
    }
    VkCommandBuffer cmd = fr.cmd;
    const VkDeviceAddress vertices = m_meshes->vertices().buffer().address;
    // A tick's rows are written once and read by every frame until the next: three sets, for the frames in flight.
    if (skin.tick) {
        const VkDeviceSize need = skin.bones.size_bytes() + skin.bones_before.size_bytes() + skin.face.size_bytes();
        m_skin_ring = (m_skin_ring + 1) % m_skin_rows.size();
        Buffer& ring = m_skin_rows[m_skin_ring];
        if (ring.size < need) {
            if (ring) {
                m_device->defer([device = m_device.get(), old = ring]() mutable { device->destroy_buffer(old); });
            }
            ring = m_device->create_buffer(std::max<VkDeviceSize>(need * 2, 1u << 20), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                           MemoryUsage::dynamic, Lifetime::frame, "skin rows");
        }
        std::byte* out = ring.as<std::byte>();
        std::memcpy(out, skin.bones.data(), skin.bones.size_bytes());
        m_skin_before_at = skin.bones.size_bytes();
        std::memcpy(out + m_skin_before_at, skin.bones_before.data(), skin.bones_before.size_bytes());
        m_skin_face_at = m_skin_before_at + skin.bones_before.size_bytes();
        auto* rows = reinterpret_cast<SkinFaceMorph*>(out + m_skin_face_at);
        for (size_t i = 0; i < skin.face.size(); ++i) {
            rows[i] = skin.face[i];
            rows[i].deltas += vertices;
        }
    }
    const Buffer& ring = m_skin_rows[m_skin_ring];
    const VkDeviceAddress bones = ring.address;
    const VkDeviceAddress before = ring.address + m_skin_before_at;
    const VkDeviceAddress face = skin.face.empty() ? 0 : ring.address + m_skin_face_at;

    // What the frame before drew from these vertices stays ahead of the writes, and the writes ahead of every draw.
    auto barrier = [&](VkPipelineStageFlags2 src, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst,
                       VkAccessFlags2 dst_access) {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = src;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst;
        b.dstAccessMask = dst_access;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1;
        d.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
    };
    barrier(VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines->skin_pipeline());
    for (const SkinJob& job : jobs) {
        const bool smooth = job.before_bone != ~0u;
        if (!skin.tick && !smooth) {
            continue;
        }
        SkinPushConstants pc{};
        pc.source = vertices + job.source;
        pc.skin = vertices + job.skin;
        pc.morph = job.morph ? vertices + job.morph : 0;
        pc.target = vertices + job.target;
        pc.bones = bones + VkDeviceAddress(job.first_bone) * 3 * sizeof(glm::vec4);
        pc.bones_before = smooth ? before + VkDeviceAddress(job.before_bone) * 3 * sizeof(glm::vec4) : pc.bones;
        pc.blend = smooth ? skin.blend : 1.0f;
        pc.face = job.face_count ? face + VkDeviceAddress(job.first_face) * sizeof(SkinFaceMorph) : 0;
        pc.face_count = job.face_count;
        pc.count = job.vertices;
        pc.morph_weight = job.morph_weight;
        pc.flags = job.flags | (job.morph ? k_skin_morph : 0u);
        vkCmdPushConstants(cmd, m_pipelines->skin_layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cmd, (job.vertices + 63) / 64, 1, 1);
    }
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
}

// Decals made this frame, from the pose just written.
void Renderer::record_skin_decal_builds(Frame& fr, std::span<const SkinDecalBuild> builds) {
    if (builds.empty()) {
        return;
    }
    if (!m_skin_decals) {
        m_skin_decals = m_device->create_buffer(VkDeviceSize(k_skin_decal_slots) * k_skin_decal_stride,
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                MemoryUsage::gpu_only, Lifetime::cache, "skin decals");
    }
    VkCommandBuffer cmd = fr.cmd;
    auto barrier = [&](VkPipelineStageFlags2 src, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = src;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst;
        b.dstAccessMask = dst_access;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1;
        d.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
    };
    // Each slot's draw starts empty; what drew from a slot before is long done, as a slot rests before it is used again.
    const uint32_t empty[4] = {0, 1, 0, 0};
    uint32_t last = ~0u;
    for (const SkinDecalBuild& b : builds) {
        if (b.slot != last) {
            vkCmdUpdateBuffer(cmd, m_skin_decals.buffer, VkDeviceSize(b.slot) * k_skin_decal_stride, sizeof empty, empty);
            last = b.slot;
        }
    }
    barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines->skin_decal_pipeline());
    for (const SkinDecalBuild& b : builds) {
        SkinDecalPushConstants pc{};
        pc.vertices = m_meshes->vertices().buffer().address;
        pc.indices = m_meshes->indices().buffer().address;
        pc.slot = m_skin_decals.address + VkDeviceSize(b.slot) * k_skin_decal_stride;
        pc.hit = b.hit;
        pc.axis_x = b.axis_x;
        pc.axis_y = b.axis_y;
        pc.direction = b.direction;
        pc.first_index = b.first_index;
        pc.triangles = b.index_count / 3;
        pc.vertex_offset = b.vertex_offset;
        pc.capacity = k_skin_decal_vertices;
        vkCmdPushConstants(cmd, m_pipelines->skin_decal_layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cmd, (pc.triangles + 63) / 64, 1, 1);
    }
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
}

void Renderer::record_skin_decals(VkCommandBuffer cmd, const PushConstants& pc, std::span<const SkinDecalDraw> decals, bool flip) {
    if (decals.empty() || !m_skin_decals) {
        return;
    }
    DrawState s;
    s.z_write = false;
    s.decal = true;
    apply_state(cmd, s, flip);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_skin_decal, false, 4, 7));
    const VkDeviceAddress arena = m_meshes->vertices().buffer().address;
    for (const SkinDecalDraw& d : decals) {
        PushConstants p = pc;
        p.particles = m_skin_decals.address + VkDeviceSize(d.slot) * k_skin_decal_stride;
        p.pass[0] = uint32_t(arena);
        p.pass[1] = uint32_t(arena >> 32);
        p.pass[2] = d.instance;
        p.mode = d.texture;
        p.pad = std::bit_cast<uint32_t>(d.fade);
        vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(p), &p);
        vkCmdDrawIndirect(cmd, m_skin_decals.buffer, VkDeviceSize(d.slot) * k_skin_decal_stride, 1, 16);
        ++m_stats.draw_calls;
    }
}

void Renderer::render(const FrameInput& input) {
    if (!m_frame_begun) {
        begin_frame();
    }
    m_frame_begun = false;
    Frame& fr = m_frames[m_slot];
    VkDevice dev = m_device->device();
    m_stats = {};
    m_material_anims_address = 0;

    SPUD_TRACE("acquire+reset");
    if (!m_output->acquire(m_slot)) {
        return; // fence untouched: the slot stays usable
    }
    if (m_output->take_resized() && (m_options.render_width == 0 || m_options.render_height == 0)) {
        const VkExtent2D e = m_output->extent();
        if (e.width != m_target_extent.width || e.height != m_target_extent.height) {
            m_device->wait_idle();
            destroy_targets();
            create_targets(e);
        }
    }

    vk_check(vkResetFences(dev, 1, &fr.fence));
    vk_check(vkResetCommandPool(dev, fr.pool, 0));
    VkCommandBuffer cmd = fr.cmd;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &begin));
    vkCmdResetQueryPool(cmd, fr.queries, 0, k_query_count);
    uint32_t query = 0;
    uint32_t mark = 0;
    auto stamp = [&] { m_device->checkpoint(cmd, (m_far_pass ? 1000u : 0u) + ++mark); if (m_far_pass) { return; } vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, fr.queries, query++); };
    stamp();
    init_offscreen_targets(cmd);

    m_door_effects = input.settings.door_effects;
    const bool has_scene = !input.spaces.empty() && input.view != nullptr;
    const VkExtent2D extent = m_target_extent;
    const bool capture = m_capture_requested && has_scene;

    // The scene: a door's far frame goes through this first, then the frame's own (renderer_portal.cpp).
    const auto record_scene = [&](const FrameInput& input) {
        const SpaceView& space = input.spaces[0];
        const View& view = *input.view;
        const RenderSettings& rs = input.settings;

        // --- Frame data -----------------------------------------------------------------
        m_terrain_draws.clear();
        if (space.terrain && rs.draw_terrain) {
            SPUD_TRACE("terrain collect");
            space.terrain->collect(view.frustum, view.position, rs.terrain, m_terrain_draws);
        }
        size_t god_ray_land = 0;
        for (uint32_t c = 0; c < k_max_cascades; ++c) {
            m_god_ray_land[c].clear();
            if (space.terrain && rs.draw_terrain && space.sun_shadows && c < space.sun_shadows->drawn()) {
                space.terrain->collect_cascade(space.sun_shadows->cascades[c], view.position, rs.terrain, m_god_ray_land[c]);
                god_ray_land += m_god_ray_land[c].bytes();
            }
        }
        // Worst case for this frame's arena, so it is sized before anything is written.
        size_t mesh_groups = 0;
        if (space.draws && rs.draw_meshes) {
            for (const auto& item : space.draws->items()) {
                mesh_groups += item_groups(*m_meshes, item).size();
            }
        }
        SPUD_TRACE("ensure_arena");
        ensure_arena(fr, 4096 + mesh_groups * (sizeof(GpuDrawInstance) + k_cmd_stride) +
                             m_terrain_draws.bytes() + space.far_tree_runs.size_bytes() + 64 +
                             space.grass_runs.size() * k_cmd_stride +
                             space.lights.size() * sizeof(GpuPointLight) +
                             m_meshes->material_anim_count() * sizeof(GpuMaterialAnim) +
                             offscreen_arena_need(input.offscreen) + offscreen_arena_need(input.prepass) + water_arena_need(input) + ui_scenes_arena(input) +
                             sizeof(GpuPost) +
                             space.environment.sky.draw_count * sizeof(GpuSkyDraw) + sizeof(GpuVolumeParams) + 64 +
                             (k_max_cascades + 2 * k_max_shadow_lights) * (sizeof(GpuFrame) + 256) +
                             shadow_group_count(space) * (sizeof(GpuDrawInstance) + k_cmd_stride) +
                             god_ray_land + k_max_cascades * 256 + input.skin.bones.size_bytes() + 64);
        record_skinning(fr, input.skin);
        record_skin_decal_builds(fr, input.skin.decals);

        const VkDeviceSize frame_offset =
            write_frame_data(fr, view, space, rs, input.time, input.previous_time, extent, m_samples);

        // Point lights against this view.
        const VkDeviceAddress lights_address = write_point_lights(fr, view, space, rs);

        const SkyFrame& sky = space.environment.sky;
        // Whether the composite met snow in this slot's last frame: ISSnowSSS costs a copy of the scene, paid only then.
        // Once a frame: a door's far frame comes first, and what the flags say holds for both.
        if (m_snow_flags && (m_far_pass || !m_portal.ready)) {
            m_snow_visible = m_snow_flags.as<uint32_t>()[m_slot] != 0;
            m_snow_flags.as<uint32_t>()[m_slot] = 0;
            m_lens_visible = m_snow_flags.as<uint32_t>()[k_frames_in_flight + m_slot] != 0;
            m_snow_flags.as<uint32_t>()[k_frames_in_flight + m_slot] = 0;
        }
        const VkDeviceAddress post_address =
            write_post_data(fr, view, space, rs, input.menu_paused ? 0.3f : std::max(input.time - input.previous_time, 0.0f));
        const bool composite = rs.composite && sky.image_space_valid && !sky.ui_scene;

        // The values of animated materials (16.5).
        const VkDeviceAddress material_anims_address = write_material_anims(fr, input);

        const bool volumetric = composite && sky.volumetric.enabled && rs.volumetric;
        const bool lens_flare = composite && rs.lens_flare;
        // Views the scene reads: the precipitation occlusion map.
        if (composite && rs.draw_water && space.water) {
            record_water_cube(cmd, fr, space, input, post_address);
            record_water_flow(cmd, fr, *space.water);
        }
        for (const OffscreenView& ov : input.prepass) {
            record_offscreen(cmd, fr, ov, input);
        }

        VkDeviceAddress draw_instances = 0;
        if (space.draws && rs.draw_meshes) {
            SPUD_TRACE("build_mesh_draws");
            build_mesh_draws(fr, space, m_runs_opaque, m_runs_alpha, m_runs_blend, draw_instances);
        } else {
            m_runs_opaque.clear();
            m_runs_alpha.clear();
            m_runs_blend.clear();
            m_runs_refraction.clear();
        }
        const TerrainFrame land = write_terrain(fr, m_terrain_draws);
        m_stats.terrain_cells = static_cast<uint32_t>(m_terrain_draws.total());

        // Push constants of the scene's draws; the shadow casters share them with their own frame and instances.
        PushConstants pc{};
        pc.frame = fr.arena.address + frame_offset;
        pc.draw_instances = draw_instances;
        pc.instance_table = space.instance_table;
        pc.materials = m_meshes->materials_address();
        pc.terrain_quads = space.terrain ? space.terrain->quads_address() : 0;
        pc.land_textures = space.terrain ? space.terrain->land_textures_address() : 0;
        pc.terrain_vertices = space.terrain ? space.terrain->vertex_address() : 0;
        pc.lights = lights_address;
        pc.material_anims = material_anims_address;
        pc.post = post_address;
        const bool water = composite && rs.draw_water && space.water;
        pc.water = water ? write_water_data(fr, view, space, rs) : 0;
        pc.particles = space.particles && rs.draw_blended ? write_particle_data(fr, *space.particles) : 0;
        VkDescriptorSet set = m_bindless->set(m_slot);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->layout(), 0, 1, &set, 0, nullptr);

        // --- Shadow maps -----------------------------------------
        stamp();
        record_sun_shadows(cmd, fr, space, frame_offset, pc);
        stamp();
        if (volumetric) {
            record_volume(cmd, fr, view, space.environment, frame_offset);
        }
        stamp();
        record_light_shadows(cmd, fr, space, frame_offset, pc);
        stamp();

        // --- Scene pass -----------------------------------------------------------------
        SPUD_TRACE("record scene");
        // One set of targets is shared by the frames in flight: order against the previous frame's use.
        const VkPipelineStageFlags2 prev_use = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        for (VkImage img : {m_hdr.image, m_hdr_resolved.image, m_normal.image, m_normal_resolved.image}) {
            if (img) {
                image_barrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, prev_use, VK_ACCESS_2_NONE,
                              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
            }
        }
        image_barrier(cmd, m_depth_target.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

        if (m_samples > 1) {
            image_barrier(cmd, m_depth_copy.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        }
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = m_hdr.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // Two passes: the opaque scene, then (after a copy of its depth for soft effects) water and blended draws.
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        // Under a sky the scene clears to the weather's Fog Far through the framebuffer's range: it shows where the
        // atmosphere's mesh stops, at the horizon.
        const glm::vec3 clear = sky.exterior ? glm::vec3(space.environment.fog_far_color) *
                                                   space.environment.fog_near_color.w
                                             : glm::vec3(0.0f);
        color.clearValue.color = {{clear.r, clear.g, clear.b, 1.0f}};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = m_depth_target.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (m_samples > 1) {
            depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            depth.resolveImageView = m_depth_copy.view;
            depth.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        }
        depth.clearValue.depthStencil = {0.0f, 0}; // reverse-Z: 0 is infinitely far
        if (record_portal(cmd, fr, pc, clear)) {
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, extent};
        ri.layerCount = 1;
        // The opaque pass also keeps the normals, for the ambient occlusion; the game clears them to (0, 0.5, 0.5, 0).
        VkRenderingAttachmentInfo opaque_colors[2] = {color, color};
        opaque_colors[1].imageView = m_normal.view;
        opaque_colors[1].clearValue.color = {{0.0f, 0.5f, 0.0f, 0.0f}};
        opaque_colors[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // even over a door's view, which keeps only colour
        if (m_samples > 1) {
            opaque_colors[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            opaque_colors[1].resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            opaque_colors[1].resolveImageView = m_normal_resolved.view;
            opaque_colors[1].resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        ri.colorAttachmentCount = 2;
        ri.pColorAttachments = opaque_colors;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;

        VkViewport vp{0, 0, float(extent.width), float(extent.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent};
        // A door's far scene shows only through the doorway: its draws stop a margin past it, for what reads round a pixel.
        if (m_far_pass) {
            constexpr int32_t k_margin = 128;
            const int32_t x0 = std::max(m_portal.scissor.offset.x - k_margin, 0), y0 = std::max(m_portal.scissor.offset.y - k_margin, 0);
            const int32_t x1 = std::min(m_portal.scissor.offset.x + int32_t(m_portal.scissor.extent.width) + k_margin, int32_t(extent.width));
            const int32_t y1 = std::min(m_portal.scissor.offset.y + int32_t(m_portal.scissor.extent.height) + k_margin, int32_t(extent.height));
            scissor = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
        }
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->layout(), 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
        vkCmdSetDepthBias(cmd, 0, 0, 0);

        const bool wire = rs.wireframe;
        const bool flip = rs.flip_winding;
        auto bind_mesh_buffers = [&] {
            VkBuffer vb = m_meshes->vertices().buffer().buffer;
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
            vkCmdBindIndexBuffer(cmd, m_meshes->indices().buffer().buffer, 0, VK_INDEX_TYPE_UINT32);
        };

        // Opaque meshes first: big occluders save terrain shading.
        if (!m_runs_opaque.empty()) {
            bind_mesh_buffers();
            record_runs(cmd, fr, m_runs_opaque, wire, flip);
        }
        stamp();
        if (space.terrain) {
            record_terrain(cmd, *space.terrain, land, pc, *m_pipelines, wire, flip);
        }
        stamp();
        if (!m_runs_alpha.empty()) {
            bind_mesh_buffers();
            record_runs(cmd, fr, m_runs_alpha, wire, flip);
        }
        if (!m_runs_second.empty()) {
            bind_mesh_buffers();
            record_runs(cmd, fr, m_runs_second, wire, flip);
        }
        if (!space.grass_runs.empty() && rs.draw_grass) {
            const VkDeviceSize offset = arena_alloc(fr, space.grass_runs.size() * k_cmd_stride, 16);
            auto* cmds = reinterpret_cast<VkDrawIndexedIndirectCommand*>(fr.arena.as<std::byte>() + offset);
            for (size_t i = 0; i < space.grass_runs.size(); ++i) {
                const GrassRun& run = space.grass_runs[i];
                const DrawGroup& dg = m_meshes->group(run.group);
                cmds[i] = {dg.index_count, run.count, dg.first_index, dg.vertex_offset, run.first};
            }
            bind_mesh_buffers();
            PushConstants grass_pc = pc;
            grass_pc.terrain_cells = space.grass_instances;
            grass_pc.terrain_vertices = space.grass_forms;
            vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(grass_pc), &grass_pc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_grass, wire));
            DrawState s;
            s.two_sided = true;
            apply_state(cmd, s, flip);
            vkCmdDrawIndexedIndirect(cmd, fr.arena.buffer, offset, uint32_t(space.grass_runs.size()), k_cmd_stride);
            ++m_stats.draw_calls;
            vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
        }
        if (!space.far_tree_runs.empty()) {
            const VkDeviceSize bytes = space.far_tree_runs.size_bytes();
            const VkDeviceSize offset = arena_alloc(fr, bytes, 16);
            std::memcpy(fr.arena.as<std::byte>() + offset, space.far_tree_runs.data(), bytes);
            PushConstants tree_pc = pc;
            tree_pc.terrain_cells = space.far_trees;
            tree_pc.terrain_vertices = space.far_tree_table;
            vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(tree_pc), &tree_pc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_impostor, wire));
            DrawState s;
            s.two_sided = true;
            apply_state(cmd, s, flip);
            vkCmdDrawIndirect(cmd, fr.arena.buffer, offset, uint32_t(space.far_tree_runs.size()),
                              sizeof(VkDrawIndirectCommand));
            ++m_stats.draw_calls;
            vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
        }
        stamp();
        if (sky.exterior && sky.draw_count > 0 && m_sky_vertices) {
            record_sky(cmd, fr, sky, pc);
            pc.mode = 0;
        } else if (space.sky) {
            // Only where nothing was drawn (depth still at the cleared far value).
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_sky, false));
            DrawState s;
            s.two_sided = true;
            s.z_write = false;
            apply_state(cmd, s, flip);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            ++m_stats.draw_calls;
        }
        vkCmdEndRendering(cmd);
        stamp();
        // The opaque scene's depth for soft effects: resolved (sample 0) by the pass above, or copied.
        if (m_samples == 1) {
            image_barrier(cmd, m_depth_target.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT);
            image_barrier(cmd, m_depth_copy.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            region.dstSubresource = region.srcSubresource;
            region.extent = {extent.width, extent.height, 1};
            vkCmdCopyImage(cmd, m_depth_target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_depth_copy.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            image_barrier(cmd, m_depth_target.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                          VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            image_barrier(cmd, m_depth_copy.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        } else {
            image_barrier(cmd, m_depth_copy.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.resolveMode = VK_RESOLVE_MODE_NONE;
        depth.resolveImageView = VK_NULL_HANDLE;
        // The game's order around its composite: the occlusion, the blended decals, then the composite itself; water
        // and every other blended draw go over the composited image.
        if (composite) {
            const VkImage normals = m_samples > 1 ? m_normal_resolved.image : m_normal.image;
            image_barrier(cmd, normals, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            // The view through a door is a second or two of a sliver of the screen, on frames that already pay for two
            // scenes: it goes without occlusion and reflections.
            const bool plain = m_far_pass && !rs.door_effects;
            if (!plain) {
                record_ao(cmd, pc);
            }
            stamp();
            m_runs_subset.clear();
            for (const auto& run : m_runs_blend) {
                if (DrawState::unpack(run.state).decal) {
                    m_runs_subset.push_back(run);
                }
            }
            if (!m_runs_subset.empty()) {
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                vkCmdBeginRendering(cmd, &ri);
                vkCmdSetViewport(cmd, 0, 1, &vp);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->layout(), 0, 1, &set, 0,
                                        nullptr);
                vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
                bind_mesh_buffers();
                record_runs(cmd, fr, m_runs_subset, wire, flip);
                vkCmdEndRendering(cmd);
            }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->layout(), 0, 1, &set, 0, nullptr);
            if (!space.decals.empty() && rs.draw_decals) {
                record_decals(cmd, fr, pc, space.decals);
            }
            if (water) {
                record_water_ripples(cmd, pc, *space.water);
                record_water_mask(cmd, pc, *space.water);
            }
            if ((rs.reflections && !plain) || m_snow_visible) {
                record_scene_copy(cmd);
            }
            if (rs.reflections && !plain) {
                record_ssr(cmd, pc, water);
            }
            stamp();
            if (m_snow_visible) {
                record_snow(cmd, pc);
            }
            record_composite(cmd, pc);
            pc.mode = 0;
            if (water) {
                record_scene_copy(cmd); // the refraction's source: the composited scene
                record_water(cmd, pc, *space.water);
            }
        } else {
            stamp();
            stamp();
        }
        stamp();
        color.storeOp = m_samples > 1 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        if (m_samples > 1) {
            color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            color.resolveImageView = m_hdr_resolved.view;
            color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->layout(), 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
        if (water) {
            DrawState s;
            s.two_sided = true;
            apply_state(cmd, s, flip);
            draw_water_surfaces(cmd, pc, program_water_resolve, program_water_resolve, program_water_resolve, *space.water,
                                wire);
        }
        stamp();
        // Comparisons with the game: its blended decals (10.3, hint 3) are
        // drawn before its deferred composite, every other blended draw after it. Without blended draws, the
        // decals stay (unless draw_decals is off) and are the ones drawn over a constant background; with
        // blended draws, the others are.
        const bool comparison = !rs.draw_blended || rs.blend_background >= 0.0f;
        if (rs.blend_background >= 0.0f) {
            VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
            const float v = rs.blend_background;
            clear.clearValue.color = {{v, v, v, 1.0f}};
            VkClearRect rect{{{0, 0}, m_target_extent}, 0, 1};
            vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
        }
        if (!m_runs_blend.empty()) {
            bind_mesh_buffers();
            if (!comparison && !composite) {
                record_runs(cmd, fr, m_runs_blend, wire, flip);
            } else if (!comparison) {
                m_runs_subset.clear();
                for (const auto& run : m_runs_blend) {
                    if (!DrawState::unpack(run.state).decal) {
                        m_runs_subset.push_back(run);
                    }
                }
                record_runs(cmd, fr, m_runs_subset, wire, flip);
            } else {
                m_runs_subset.clear();
                for (const auto& run : m_runs_blend) {
                    const bool decal = DrawState::unpack(run.state).decal;
                    if (decal == !rs.draw_blended && (!decal || rs.draw_decals)) {
                        m_runs_subset.push_back(run);
                    }
                }
                record_runs(cmd, fr, m_runs_subset, wire, flip);
            }
        }
        if (rs.draw_decals) {
            record_skin_decals(cmd, pc, space.draws->skin_decals(), flip);
        }
        if (pc.particles) {
            record_particles(cmd, pc, *space.particles, flip);
        }
        if (sky.exterior && rs.draw_blended) {
            record_precipitation(cmd, sky, pc);
        }
        stamp();
        vkCmdEndRendering(cmd);

        // The resolved HDR scene: optionally copied out (captures), then read by tonemapping.
        const VkImage scene = m_samples > 1 ? m_hdr_resolved.image : m_hdr.image;
        VkImageLayout scene_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkPipelineStageFlags2 scene_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkAccessFlags2 scene_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        // Volumetric lighting, added to the scene before refraction reads it (event order, spec 13.3.1).
        if (volumetric || lens_flare) {
            image_barrier(cmd, scene, VK_IMAGE_ASPECT_COLOR_BIT, scene_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          scene_stage, scene_access, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->layout(), 0, 1, &set, 0, nullptr);
            if (lens_flare) {
                record_lens_flare(cmd, pc);
            }
            if (volumetric || (lens_flare && m_lens_visible)) {
                record_volumetric(cmd, pc, volumetric, lens_flare && m_lens_visible);
            }
            scene_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            scene_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            scene_access = VK_ACCESS_2_NONE;
        }
        if (m_hdr_capture_requested && !m_far_pass) {
            image_barrier(cmd, scene, VK_IMAGE_ASPECT_COLOR_BIT, scene_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          scene_stage, scene_access, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT);
            const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * 8;
            if (!m_hdr_capture_buffer || m_hdr_capture_buffer.size < bytes) {
                m_device->destroy_buffer(m_hdr_capture_buffer);
                m_hdr_capture_buffer = m_device->create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                               MemoryUsage::readback, Lifetime::transfer,
                                                               "hdr capture");
            }
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(cmd, scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_hdr_capture_buffer.buffer, 1,
                                   &region);
            m_hdr_capture_requested = false;
            m_hdr_capture_frame = m_frame_number + 1;
            m_hdr_capture_slot = m_slot;
            m_hdr_capture_extent = extent;
            scene_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            scene_stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            scene_access = VK_ACCESS_2_NONE;
        }
        image_barrier(cmd, scene, VK_IMAGE_ASPECT_COLOR_BIT, scene_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      scene_stage, scene_access, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        if (m_inject) {
            image_barrier(cmd, scene, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyBufferToImage(cmd, m_inject.buffer, scene, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            image_barrier(cmd, scene, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }

        // Refraction (section 13): after all scene drawing, the refraction surfaces write a screen offset and strength
        // into their own target, depth-tested against the scene's single-sampled depth; ISRefraction then writes the
        // displaced scene into a separate HDR target that tonemapping reads. Frames without refraction skip both.
        const bool refraction = !m_runs_refraction.empty();
        if (refraction) {
            image_barrier(cmd, m_refraction.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            image_barrier(cmd, m_depth_copy.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
            VkRenderingAttachmentInfo rc{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            rc.imageView = m_refraction.view;
            rc.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            rc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            rc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            rc.clearValue.color = {{0.5f, 0.5f, 0.0f, 0.0f}};
            VkRenderingAttachmentInfo rd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            rd.imageView = m_depth_copy.view;
            rd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
            rd.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            rd.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
            VkRenderingInfo rri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rri.renderArea = {{0, 0}, extent};
            rri.layerCount = 1;
            rri.colorAttachmentCount = 1;
            rri.pColorAttachments = &rc;
            rri.pDepthAttachment = &rd;
            vkCmdBeginRendering(cmd, &rri);
            VkViewport rvp{0, 0, float(extent.width), float(extent.height), 0.0f, 1.0f};
            VkRect2D rsc{{0, 0}, extent};
            vkCmdSetViewport(cmd, 0, 1, &rvp);
            vkCmdSetScissor(cmd, 0, 1, &rsc);
            bind_mesh_buffers();
            record_runs(cmd, fr, m_runs_refraction, false, flip);
            vkCmdEndRendering(cmd);
            image_barrier(cmd, m_depth_copy.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            image_barrier(cmd, m_refraction.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            image_barrier(cmd, m_hdr_refracted.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo hc{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            hc.imageView = m_hdr_refracted.view;
            hc.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            hc.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            hc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo hi{VK_STRUCTURE_TYPE_RENDERING_INFO};
            hi.renderArea = {{0, 0}, extent};
            hi.layerCount = 1;
            hi.colorAttachmentCount = 1;
            hi.pColorAttachments = &hc;
            vkCmdBeginRendering(cmd, &hi);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_is_refraction, false));
            DrawState s;
            s.two_sided = true;
            s.z_test = false;
            s.z_write = false;
            apply_state(cmd, s, false);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            ++m_stats.draw_calls;
            vkCmdEndRendering(cmd);
            image_barrier(cmd, m_hdr_refracted.image, VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        const uint32_t scene_slot = refraction ? Bindless::k_refracted_slot : Bindless::k_scene_slot;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->layout(), 0, 1, &set, 0, nullptr);
        if (sky.image_space_valid) {
            record_image_space(cmd, sky, pc, scene_slot);
        } else {
            post_pass(cmd, program_tonemap, m_ldr, pc, 1, 0, scene_slot, 0);
        }
        if (m_far_pass) {
            post_pass(cmd, program_portal_save, m_portal_scene, pc, 0, scene_slot);
        }
        const VkImage out_image = m_far_pass ? m_portal_image.image : m_color;
        const VkImageView out_view = m_far_pass ? m_portal_image.view : m_color_view;
        image_barrier(cmd, out_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        {
            // ISDistantBlur draws the last pass into the output target (ISDoubleVision while a modifier asks for it).
            const bool plain = !sky.image_space_valid || sky.ui_scene;
            if (!plain && sky.image_space.double_vision > 0.0f) {
                fullscreen(cmd, program_double_vision, out_view, extent, pc, 0, Bindless::k_ldr_slot,
                           Bindless::k_dof_slot);
            } else {
                fullscreen(cmd, program_dof, out_view, extent, pc, plain ? 1u : 0u, Bindless::k_ldr_slot,
                           Bindless::k_dof_slot, water ? Bindless::k_water_depth_slot : Bindless::k_depth_slot);
            }
        }
        if (!m_far_pass && !input.screen_blood.empty()) {
            record_screen_blood(cmd, fr, pc, out_view, extent, input);
        }
        if (m_far_pass) {
            image_barrier(cmd, out_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            return;
        }
        record_portal_stamp(cmd, pc, scene_slot, sky.image_space_valid ? m_adapt[m_adapt_index ^ 1].slot : 0);
        stamp();

        image_barrier(cmd, m_color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT);
        if (capture) {
            const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * 4;
            if (!m_capture_buffer || m_capture_buffer.size < bytes) {
                m_device->destroy_buffer(m_capture_buffer);
                m_capture_buffer = m_device->create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                           MemoryUsage::readback, Lifetime::transfer, "capture");
            }
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(cmd, m_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_capture_buffer.buffer, 1,
                                   &region);
            m_capture_requested = false;
            m_capture_frame = m_frame_number + 1;
            m_capture_slot = m_slot;
            m_capture_extent = extent;
        }
        // Second views of the space (the local map) after the pre-UI capture, which never contains them.
        for (const OffscreenView& ov : input.offscreen) {
            record_offscreen(cmd, fr, ov, input);
        }
    };
    if (has_scene) {
        prepare_portal(input);
        if (m_portal.ready) {
            FrameInput far = input;
            far.view = input.portal->view;
            far.spaces = std::span<const SpaceView>(&input.portal->space, 1);
            far.prepass = input.portal->prepass;
            far.offscreen = {};
            far.ui_scenes = {};
            far.portal = nullptr;
            far.settings.lens_flare = false; // the main frame's sees the doorway
            m_far_pass = true;
            record_scene(far);
            m_far_pass = false;
        } else {
            m_water_views[1].have_last = false; // a door opened later shows from then on
        }
        record_scene(input);
    } else {
        if (!input.ui_scenes.empty()) {
            ensure_arena(fr, 4096 + ui_scenes_arena(input));
        }
        for (uint32_t p = 0; p < pass_ui; ++p) {
            stamp();
        }
    }
    {
        SPUD_TRACE("ui scenes");
        prepare_ui_scenes(fr, input);
    }

    SPUD_TRACE("record output");
    // Game UI over the finished scene, after the pre-UI capture, with the UI 3D scenes among its menus
    // (renderer_overlay.cpp).
    const bool composed = record_overlay(cmd, input, fr, has_scene);
    stamp();
    m_output->record(cmd, m_color, extent, has_scene || composed, input.ui);
    stamp();
    fr.timestamps_written = true;
    vk_check(vkEndCommandBuffer(cmd));

    // Submit: wait for the output (swapchain image) and for the uploads this frame consumed (already signalled).
    m_waits.clear();
    m_signals.clear();
    m_output->add_waits(m_slot, m_waits);
    if (m_uploader->wait_value() > 0) {
        VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        w.semaphore = m_uploader->timeline();
        w.value = m_uploader->wait_value();
        w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        m_waits.push_back(w);
    }
    m_output->add_signals(m_signals);
    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = static_cast<uint32_t>(m_waits.size());
    submit.pWaitSemaphoreInfos = m_waits.data();
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    submit.signalSemaphoreInfoCount = static_cast<uint32_t>(m_signals.size());
    submit.pSignalSemaphoreInfos = m_signals.data();
    {
        SPUD_TRACE("submit+present");
        std::lock_guard lock(m_device->queue_mutex());
        vk_check(vkQueueSubmit2(m_device->queue(), 1, &submit, fr.fence));
        m_output->present(fr.fence);
    }
    ++m_frame_number;
    fr.number = m_frame_number;
    m_slot = (m_slot + 1) % k_frames_in_flight;
}

bool Renderer::take_hdr_capture(std::vector<uint16_t>& rgba_half, uint32_t& width, uint32_t& height) {
    if (m_hdr_capture_frame == 0) {
        return false;
    }
    Frame& fr = m_frames[m_hdr_capture_slot];
    if (fr.number == m_hdr_capture_frame) {
        vk_check(vkWaitForFences(m_device->device(), 1, &fr.fence, VK_TRUE, UINT64_MAX));
    }
    vmaInvalidateAllocation(m_device->allocator(), m_hdr_capture_buffer.allocation, 0, VK_WHOLE_SIZE);
    width = m_hdr_capture_extent.width;
    height = m_hdr_capture_extent.height;
    rgba_half.resize(size_t(width) * height * 4);
    std::memcpy(rgba_half.data(), m_hdr_capture_buffer.as<uint8_t>(), rgba_half.size() * sizeof(uint16_t));
    m_hdr_capture_frame = 0;
    return true;
}

bool Renderer::take_capture_scaled(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    if (m_capture_frame == 0) {
        return false;
    }
    Frame& fr = m_frames[m_capture_slot];
    if (fr.number == m_capture_frame) {
        vk_check(vkWaitForFences(m_device->device(), 1, &fr.fence, VK_TRUE, UINT64_MAX));
    }
    vmaInvalidateAllocation(m_device->allocator(), m_capture_buffer.allocation, 0, VK_WHOLE_SIZE);
    const uint32_t sw = m_capture_extent.width, sh = m_capture_extent.height;
    rgba.resize(size_t(width) * height * 4);
    const auto* src = m_capture_buffer.as<uint8_t>();
    const bool bgra = m_color_format == VK_FORMAT_B8G8R8A8_UNORM;
    for (uint32_t y = 0; y < height; ++y) {
        const size_t row = size_t(uint64_t(y) * sh / height) * sw;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* p = src + (row + size_t(uint64_t(x) * sw / width)) * 4;
            uint8_t* o = rgba.data() + (size_t(y) * width + x) * 4;
            o[0] = p[bgra ? 2 : 0];
            o[1] = p[1];
            o[2] = p[bgra ? 0 : 2];
            o[3] = 255;
        }
    }
    m_capture_frame = 0;
    return true;
}

bool Renderer::take_capture(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    if (m_capture_frame == 0) {
        return false;
    }
    Frame& fr = m_frames[m_capture_slot];
    if (fr.number == m_capture_frame) {
        vk_check(vkWaitForFences(m_device->device(), 1, &fr.fence, VK_TRUE, UINT64_MAX));
    }
    vmaInvalidateAllocation(m_device->allocator(), m_capture_buffer.allocation, 0, VK_WHOLE_SIZE);
    width = m_capture_extent.width;
    height = m_capture_extent.height;
    rgba.resize(size_t(width) * height * 4);
    const auto* src = m_capture_buffer.as<uint8_t>();
    const bool bgra = m_color_format == VK_FORMAT_B8G8R8A8_UNORM;
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        rgba[i * 4 + 0] = src[i * 4 + (bgra ? 2 : 0)];
        rgba[i * 4 + 1] = src[i * 4 + 1];
        rgba[i * 4 + 2] = src[i * 4 + (bgra ? 0 : 2)];
        rgba[i * 4 + 3] = 255;
    }
    m_capture_frame = 0;
    return true;
}

} // namespace spud::render
