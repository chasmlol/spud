// The image-space chain and the sky's draws: ambient occlusion, the
// deferred composite with its fog, then HDR, bloom, the tonemap and the distant blur, each pass the game's own.

#include "core/log.hpp"
#include "core/trace.hpp"
#include "core/units.hpp"
#include "render/gpu_types.hpp"
#include "render/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace spud::render {

namespace {

// [Display] settings at the user's values (settings dump; exe defaults noted where they differ).
constexpr float k_sao_intensity = 15.0f;     // fSAOIntensity
constexpr float k_sao_radius = 250.0f;       // fSAORadius
constexpr float k_sao_bias = 2.5f;           // fSAOBias
constexpr float k_sao_exp = 0.11f;           // fSAOExpFactor
constexpr float k_dof_max_depth = 10000.0f;  // fDOFMaxDepthParticipation (exe 50000)
constexpr float k_eye_speed_scale = 2.0f;    // fGlobalEyeAdaptSpeedScale
constexpr float k_eye_strength_scale = 0.4f; // fGlobalEyeAdaptStrengthScale
constexpr float k_reinhard_white = 1.1f;     // fReinhardWhiteScale
constexpr float k_gamma = 1.0f;              // fGamma:Display
constexpr float k_range = 1.2f;              // 1 / g_FramebufferRangeRcp
// The dynamic depth of field's constants as captured (fDynamicDOFNear/FarDist, Range, Blur x BlurMultiplier).
constexpr float k_ddof_near = 100.0f, k_ddof_far = 1000.0f;
constexpr float k_ddof_near_range = 100.0f, k_ddof_far_range = 10000.0f;
constexpr float k_ddof_near_blur = 0.8f, k_ddof_far_blur = 0.32f;
// iVolumetricLightingQuality 2: iVolumetricLightingTextureWidthHigh, Height, Depth.
// [Display] snow: fSparklesIntensity, fSparklesSpecularPower, fSparklesDensity, fSparklesSize, bDeactivateAOOnSnow,
// fSparklesMaxDistance, fShadowSparkleIntensity, fSnowSSSDarkColorIntensity, fSnowSSSStrength, fSnowSSSDepthDiff.
constexpr float k_sparkle_intensity = 1.0f, k_sparkle_power = 2.0f, k_sparkle_density = 0.85f, k_sparkle_size = 6.0f;
constexpr float k_sparkle_distance = 1000.0f, k_sparkle_shadow = 0.25f;
constexpr float k_sss_dark = 0.1f, k_sss_strength = 50.0f, k_sss_depth_diff = 0.5f;
constexpr uint32_t k_vl_width = 320, k_vl_height = 192, k_vl_depth = 90;

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
             VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
             uint32_t mip = 0, uint32_t mips = 1, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
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
    b.subresourceRange = {aspect, mip, mips, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

constexpr auto k_color_out = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
constexpr auto k_fragment = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr auto k_color_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
constexpr auto k_sampled = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

VkExtent2D quarter(VkExtent2D e) { return {(e.width + 3) / 4, (e.height + 3) / 4}; }

} // namespace

Renderer::PostTarget Renderer::make_post_target(VkFormat format, VkExtent2D extent, uint32_t slot, Lifetime lifetime,
                                                const char* name) {
    PostTarget t;
    t.format = format;
    t.extent = extent;
    t.slot = slot;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vk_check(m_device->create_image(info, ai, lifetime, name, t.image, t.alloc));
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = t.image;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v.format = format;
    v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &t.view));
    m_bindless->write(slot, t.view);
    return t;
}

void Renderer::create_post_targets(VkExtent2D extent) {
    using B = Bindless;
    const VkFormat hdr = k_hdr_format;
    m_ao_raw = make_post_target(k_ao_format, extent, B::k_ao_raw_slot, Lifetime::frame, "ao raw");
    m_ao_tmp = make_post_target(k_ao_format, extent, B::k_ao_tmp_slot, Lifetime::frame, "ao blur");
    m_ao = make_post_target(k_ao_format, extent, B::k_ao_slot, Lifetime::frame, "ao");
    m_portal_scene = make_post_target(k_hdr_format, extent, B::k_portal_scene_slot, Lifetime::frame, "door scene");
    m_portal_image = make_post_target(m_color_format, extent, B::k_portal_image_slot, Lifetime::frame, "door image");
    for (uint32_t i = 0; i < 4; ++i) {
        const VkExtent2D e{std::max(extent.width / (i < 2 ? 16 : 32), 1u), std::max(extent.height / 4, 1u)};
        m_lens[i] = make_post_target(VK_FORMAT_R16G16B16A16_SFLOAT, e, B::k_lens_slot + i, Lifetime::frame, "lens flare");
    }
    const VkExtent2D half{std::max(extent.width / 2, 1u), std::max(extent.height / 2, 1u)}; // iReflectionResolutionDivider
    m_ssr[0] = make_post_target(VK_FORMAT_R16G16B16A16_SFLOAT, half, B::k_ssr_slot, Lifetime::frame, "reflections");
    m_ssr[1] = make_post_target(VK_FORMAT_R16G16B16A16_SFLOAT, half, B::k_ssr_slot + 1, Lifetime::frame, "reflection blur");
    m_ssr_raw = make_post_target(VK_FORMAT_R16G16B16A16_SFLOAT, half, B::k_ssr_raw_slot, Lifetime::frame, "reflections raw");
    create_water_targets(extent);
    const VkExtent2D q = quarter(extent);
    m_quarter = make_post_target(hdr, q, B::k_quarter_slot, Lifetime::frame, "hdr quarter");
    m_bloom[0] = make_post_target(hdr, q, B::k_bloom_slot, Lifetime::frame, "bloom v");
    m_bloom[1] = make_post_target(hdr, q, B::k_bloom_slot + 1, Lifetime::frame, "bloom h");
    m_ldr = make_post_target(k_ldr_format, extent, B::k_ldr_slot, Lifetime::frame, "tonemapped");
    m_dof[0] = make_post_target(k_ldr_format, q, B::k_dof_slot, Lifetime::frame, "dof quarter");
    m_dof[1] = make_post_target(k_ldr_format, q, B::k_dof_slot + 1, Lifetime::frame, "dof blur");
    m_radial = make_post_target(hdr, extent, B::k_radial_slot, Lifetime::frame, "radial blur");
    m_blurred = make_post_target(hdr, extent, B::k_blurred_slot, Lifetime::frame, "full-screen blur");
    {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = k_hdr_format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        vk_check(m_device->create_image(info, ai, Lifetime::frame, "snow source", m_snow_source.image,
                                        m_snow_source.alloc));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = m_snow_source.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = info.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_check(vkCreateImageView(m_device->device(), &vi, nullptr, &m_snow_source.view));
        m_bindless->write(B::k_snow_source_slot, m_snow_source.view);
    }
    if (!m_snow_flags) {
        m_snow_flags = m_device->create_buffer(2 * k_frames_in_flight * sizeof(uint32_t),
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                               MemoryUsage::readback, Lifetime::transfer, "snow flags");
        std::memset(m_snow_flags.mapped, 0, 2 * k_frames_in_flight * sizeof(uint32_t));
    }
    m_vl[0] = make_post_target(k_vl_format, extent, B::k_vl_slot, Lifetime::frame, "volumetric lighting");
    m_vl[1] = make_post_target(k_vl_format, extent, B::k_vl_slot + 1, Lifetime::frame, "volumetric lighting blur");
    if (!m_vl_volume.image) {
        auto make3d = [&](VolumeImage& v, VkExtent3D size, Lifetime lifetime, const char* name, uint32_t slot,
                          VkImageLayout layout) {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_3D;
            info.format = VK_FORMAT_R16_SFLOAT;
            info.extent = size;
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            vk_check(m_device->create_image(info, ai, lifetime, name, v.image, v.alloc));
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = v.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
            vi.format = info.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk_check(vkCreateImageView(m_device->device(), &vi, nullptr, &v.view));
            m_bindless->write(slot, v.view, layout);
        };
        make3d(m_vl_volume, {k_vl_width, k_vl_height, k_vl_depth}, Lifetime::frame, "volumetric volume",
               B::k_vl_volume_slot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        make3d(m_vl_noise, {32, 32, 32}, Lifetime::asset, "volumetric noise", B::k_vl_noise_slot, VK_IMAGE_LAYOUT_GENERAL);
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        vk_check(vkCreateDescriptorPool(m_device->device(), &pool, nullptr, &m_vl_pool));
        const VkDescriptorSetLayout layout = m_pipelines->volume_set_layout();
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = m_vl_pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &layout;
        vk_check(vkAllocateDescriptorSets(m_device->device(), &alloc, &m_vl_set));
        VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, m_vl_volume.view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, m_vl_noise.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t b = 0; b < 2; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = m_vl_set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[b].pImageInfo = &images[b];
        }
        vkUpdateDescriptorSets(m_device->device(), 2, writes, 0, nullptr);
    }
    // Luminance levels: each a quarter of the last until the size is at most 2, which is the adapted target.
    m_lum_count = 0;
    VkExtent2D e = quarter(q);
    while (true) {
        const VkExtent2D next = quarter(e);
        m_lum[m_lum_count] = make_post_target(hdr, e, B::k_lum_slot + m_lum_count, Lifetime::frame, "luminance");
        ++m_lum_count;
        e = next;
        if ((e.width <= 2 && e.height <= 2) || m_lum_count == m_lum.size()) {
            break;
        }
    }
    m_adapt[0] = make_post_target(hdr, e, B::k_adapt_slot, Lifetime::simulation, "eye adaptation");
    m_adapt[1] = make_post_target(hdr, e, B::k_adapt_slot + 1, Lifetime::simulation, "eye adaptation");
    m_adapt_valid = false;

    // The camera-Z pyramid: one image, a view per level to draw into and read, and one of the chain for the AO.
    uint32_t mips = 1;
    for (uint32_t s = std::max(extent.width, extent.height); s > 1; s >>= 1) {
        ++mips;
    }
    // Level 0 is never stored: level 1 picks from the depth buffer itself, so the image starts at level 1.
    m_camz_mips = std::min<uint32_t>(mips - 1, static_cast<uint32_t>(m_camz_views.size()));
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = k_depth_z_format;
    info.extent = {std::max(extent.width >> 1, 1u), std::max(extent.height >> 1, 1u), 1};
    info.mipLevels = m_camz_mips;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vk_check(m_device->create_image(info, ai, Lifetime::frame, "camera z", m_camz_image, m_camz_alloc));
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = m_camz_image;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v.format = k_depth_z_format;
    for (uint32_t m = 0; m < m_camz_mips; ++m) {
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
        vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &m_camz_views[m]));
        m_bindless->write(B::k_camz_slot + m, m_camz_views[m]);
    }
    v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, m_camz_mips, 0, 1};
    vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &m_camz_full));
    m_bindless->write(B::k_camz_chain_slot, m_camz_full);
}

void Renderer::destroy_post_targets() {
    VkDevice dev = m_device->device();
    auto drop = [&](PostTarget& t) {
        if (t.view) {
            vkDestroyImageView(dev, t.view, nullptr);
            vmaDestroyImage(m_device->allocator(), t.image, t.alloc);
        }
        t = PostTarget{};
    };
    destroy_water_targets();
    for (PostTarget* t : {&m_portal_scene, &m_portal_image, &m_lens[0], &m_lens[1], &m_lens[2], &m_lens[3], &m_ssr[0], &m_ssr[1], &m_ssr_raw, &m_ao_raw, &m_ao_tmp, &m_ao, &m_quarter, &m_bloom[0], &m_bloom[1], &m_ldr, &m_dof[0],
                          &m_dof[1], &m_radial, &m_adapt[0], &m_adapt[1], &m_vl[0], &m_vl[1], &m_blurred}) {
        drop(*t);
    }
    for (auto& t : m_lum) {
        drop(t);
    }
    if (m_snow_source.view) {
        vkDestroyImageView(dev, m_snow_source.view, nullptr);
        vmaDestroyImage(m_device->allocator(), m_snow_source.image, m_snow_source.alloc);
        m_snow_source = Target{};
    }
    m_lum_count = 0;
    if (m_vl_pool) {
        vkDestroyDescriptorPool(dev, m_vl_pool, nullptr);
        m_vl_pool = VK_NULL_HANDLE;
        for (VolumeImage* v : {&m_vl_volume, &m_vl_noise}) {
            vkDestroyImageView(dev, v->view, nullptr);
            vmaDestroyImage(m_device->allocator(), v->image, v->alloc);
            *v = VolumeImage{};
        }
        m_vl_noise_ready = false;
    }
    if (m_camz_image) {
        for (uint32_t m = 0; m < m_camz_mips; ++m) {
            vkDestroyImageView(dev, m_camz_views[m], nullptr);
            m_camz_views[m] = VK_NULL_HANDLE;
        }
        vkDestroyImageView(dev, m_camz_full, nullptr);
        vmaDestroyImage(m_device->allocator(), m_camz_image, m_camz_alloc);
        m_camz_image = VK_NULL_HANDLE;
        m_camz_full = VK_NULL_HANDLE;
    }
}

void Renderer::set_sky_mesh(std::span<const SkyVertex> vertices, std::span<const uint32_t> indices) {
    wait_idle();
    m_device->destroy_buffer(m_sky_vertices);
    m_device->destroy_buffer(m_sky_indices);
    m_sky_vertices = m_device->create_buffer(vertices.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                             MemoryUsage::dynamic, Lifetime::asset, "sky vertices");
    m_sky_indices = m_device->create_buffer(indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                            MemoryUsage::dynamic, Lifetime::asset, "sky indices");
    std::memcpy(m_sky_vertices.mapped, vertices.data(), vertices.size_bytes());
    std::memcpy(m_sky_indices.mapped, indices.data(), indices.size_bytes());
}

void Renderer::inject_hdr(std::vector<std::byte> rgba_half) {
    wait_idle();
    m_device->destroy_buffer(m_inject);
    if (!rgba_half.empty()) {
        m_inject = m_device->create_buffer(rgba_half.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::upload,
                                           Lifetime::transfer, "hdr injection");
        std::memcpy(m_inject.mapped, rgba_half.data(), rgba_half.size());
    }
}

VkDeviceAddress Renderer::write_sky_draws(Frame& fr, const SkyFrame& sky) {
    if (sky.draw_count == 0) {
        return 0;
    }
    const VkDeviceSize offset = arena_alloc(fr, sky.draw_count * sizeof(GpuSkyDraw), 16);
    auto* dst = reinterpret_cast<GpuSkyDraw*>(fr.arena.as<std::byte>() + offset);
    for (uint32_t i = 0; i < sky.draw_count; ++i) {
        const SkyDraw& d = sky.draws[i];
        GpuSkyDraw g{};
        g.world = d.world;
        g.color[0] = d.color[0];
        g.color[1] = d.color[1];
        g.color[2] = d.color[2];
        g.uv_offset = d.uv_offset;
        g.info = glm::uvec4(d.technique, d.texture0, d.texture1, 0);
        // PParams.y is 0 for the moons (types 1 and 6); the sun is type 0.
        g.params = glm::vec4(d.blend, d.depth, d.moon ? 0.0f : sky.sky_scale, 0.0f);
        dst[i] = g;
    }
    return fr.arena.address + offset;
}

VkDeviceAddress Renderer::write_post_data(Frame& fr, const View& view, const SpaceView& space, const RenderSettings& rs,
                                          float dt) {
    const Environment& env = space.environment;
    const SkyFrame& sky = env.sky;
    const ImageSpaceData& is = sky.image_space;
    const VkExtent2D extent = m_target_extent;
    const float w = float(extent.width), h = float(extent.height);

    GpuPost p{};
    p.sky_draws = write_sky_draws(fr, sky);
    p.fog_param = env.fog_param;
    p.fog_near = env.fog_near_color;
    p.fog_far = env.fog_far_color;
    const float n = env.near_units, f = env.far_units;
    p.camera = glm::vec4(n, f, 1.0f / units::k_skyrim_unit_m, view.near_plane);
    const float p00 = view.projection[0][0], p11 = std::abs(view.projection[1][1]);
    p.sao_proj = glm::vec4(-2.0f / (w * p00), -2.0f / (h * p11), 1.0f / p00, 1.0f / p11);
    p.sao_info = glm::vec4(0.0f, 100.0f * k_sao_radius, k_sao_bias, k_sao_intensity / std::pow(k_sao_radius, 6.0f));
    p.sao_screen = glm::vec4(w, h, 1.0f / w, 1.0f / h);
    p.sao_info2 = glm::vec4(k_sao_exp, k_sao_radius * k_sao_radius, 0.3f, n * f / ((n - f) * 0.01f + f));
    p.sao_screen2 = glm::vec4(k_dof_max_depth, 0.0f, 0.0f, 0.0f);
    p.misc = glm::vec4(1.0f / ((rs.hold_effect_state ? 2.0f : sky.dof_center_weight) + 1.19e-7f), 1.0f / k_gamma, 1.0f / k_range, 0.0f);
    // The UI's scene stands outside the framebuffer range: its bright pass takes 1, 1 where the world's takes 1/1.2, 1.2 (capture).
    const float bloom_range = sky.ui_scene ? 1.0f : k_range;
    p.bloom = glm::vec4(is.bloom_threshold, is.bloom_scale, 1.0f / bloom_range, bloom_range);
    // The bloom's weights: a normalised Gaussian of sigma n / 2 (table).
    m_bloom_taps = std::clamp(int(is.bloom_blur_radius) - 1, 0, 6) + 1;
    {
        const float sigma = float(m_bloom_taps) * 0.5f;
        float sum = 0.0f;
        for (int i = -m_bloom_taps; i <= m_bloom_taps; ++i) {
            sum += std::exp(-float(i * i) / (2.0f * sigma * sigma));
        }
        for (int i = 0; i <= m_bloom_taps; ++i) {
            p.gauss[i].x = std::exp(-float(i * i) / (2.0f * sigma * sigma)) / sum;
        }
    }
    // Eye adaptation rates (constants).
    {
        const float sp = k_eye_speed_scale * is.eye_adapt_speed;
        const float frames = 30.0f * dt;
        const float strength = k_eye_strength_scale * is.eye_adapt_strength;
        p.adapt.x = 1.0f - std::pow(std::max(1.0f - 0.01f * sp, 0.0f), frames);
        p.adapt.y = 1.0f - std::pow(std::max(1.0f - 0.01f * sp / std::max(strength, 1e-6f), 0.0f), frames);
        // A door's far frame has no history of its own: the eye stands at what is seen.
        p.adapt.z = (m_far_pass || sky.ui_scene || !m_adapt_valid || sky.reset_adaptation) ? 1.0f : 0.0f;
        p.adapt.w = rs.hold_effect_state ? 0.25f : 0.0f;
    }
    const float white = k_reinhard_white * is.white;
    p.tone = glm::vec4(is.receive_bloom_threshold, 1.0f / (white * white), 0.0f, 0.0f);
    p.cinematic = glm::vec4(is.saturation, 0.0f, is.contrast, is.brightness);
    p.tint = glm::vec4(is.tint, is.tint_amount);
    p.fade = glm::vec4(is.fade, is.fade_amount);
    p.dof1 = glm::vec4(k_ddof_near_range, k_ddof_far_range, n, f);
    p.dof2 = glm::vec4(k_ddof_near_blur, 0.0f, 1.0f, k_ddof_far_blur);
    const float span = 1.0f / (k_ddof_far - k_ddof_near);
    p.dof3 = glm::vec4(5.0f, f, span, k_ddof_near * span);
    // A modifier's distant blur (flags & 3 = 2; 0x80 would be the dynamic one above) in the block's own terms
    // ISMTween behind the menus.
    if (is.world_map) {
        p.dof2 = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f); // its image space's DOF strength is 0
        p.dof3 = glm::vec4(5.0f, f, -1.0e8f, -1.0e8f);
    } else if (const int flags = int(is.dof.w); is.dof.x > 0.0f && (flags & 3) == 2 && !(flags & 0x80)) {
        p.dof1 = glm::vec4(is.dof.z, is.dof.y, n, f);
        p.dof2 = glm::vec4(is.dof.x, 0.0f, (flags & 1) ? 0.0f : 1.0f, (flags & 4) ? 1.0f : 0.0f);
        p.dof3 = glm::vec4(5.0f, f, -1.0e8f, -1.0e8f);
    }
    p.radial0 = glm::vec4(is.radial_strength, is.radial_ramp_up, is.radial_start, is.radial_ramp_down);
    p.radial1 = glm::vec4(is.radial_center, is.radial_down_start, is.blur_radius);
    const float theta = m_dv_clock * 0.0017453278f;
    p.dv0 = glm::vec4(std::sin(theta) * is.double_vision, std::cos(theta) * is.double_vision, 1.0f, 1.0f);
    p.dv1 = glm::vec4(view.aspect, 1.0f, 1.0f, 0.0f);
    p.post_slots = glm::uvec4(Bindless::k_camz_slot + m_camz_mips - 1, 0, 0, 0);
    std::memcpy(p.portal, m_portal.doorway, sizeof(p.portal));
    (void)rs;
    // Volumetric lighting.
    const VolumetricParams& vl = sky.volumetric;
    p.vl0 = glm::vec4(k_vl_width, k_vl_height, k_vl_depth, vl.intensity / float(k_vl_depth));
    p.vl1 = glm::vec4(vl.range, 0.0f, 0.0f, 0.0f);
    p.vl2 = glm::vec4(vl.color, 0.0f);
    p.post_slots.y = Bindless::k_vl_volume_slot;
    p.post_slots.z = Bindless::k_normal_slot;
    p.post_slots.w = Bindless::k_ssr_slot;
    // The capture's constants; intensity 0 is the composite's SSRParams.z off.
    p.ssr = glm::vec4(0.4f, 0.001f, rs.reflections ? 0.64f : 0.0f, 0.4f);
    if (space.water && space.water->fog_line >= 0.0f && rs.draw_water) {
        p.water_fog = space.water->fog;
        p.water_fog2 = glm::vec4(space.water->fog_far, space.water->fog_near, space.water->fog_line, 0.0f);
    }
    p.snow_flag = m_snow_flags.address + m_slot * sizeof(uint32_t);
    p.lens_flag = m_snow_flags.address + (k_frames_in_flight + m_slot) * sizeof(uint32_t);
    p.sparkle[0] = glm::vec4(k_sparkle_intensity, 0.0f, k_sparkle_power, k_sparkle_density);
    p.sparkle[1] = glm::vec4(k_sparkle_size, 0.0f, 1.0f, 0.0f);
    p.sparkle[2] = glm::vec4(1.0f, k_sparkle_distance, 0.0f, 0.0f);
    p.sparkle[3] = glm::vec4(k_sparkle_shadow, sky.sunlight);
    p.sss[0] = glm::vec4(sky.sunlight, k_sss_dark);
    p.sss[1] = glm::vec4(k_sss_strength, k_sss_depth_diff, 0.0f, 0.0f);
    // Precipitation (SetupGeometry).
    p.precip_occlusion = sky.occlusion;
    p.misc.w = sky.occlusion_valid ? 1.0f : 0.0f;
    const float tan_half = std::tan(std::atan(std::tan(view.fov_y * 0.5f) * view.aspect));
    const float k = tan_half * 1.3032254f;
    const glm::vec3 forward = glm::vec3(-view.rotation[0][2], -view.rotation[1][2], -view.rotation[2][2]);
    for (int e = 0; e < 2; ++e) {
        const PrecipitationParams& src = sky.precipitation[e];
        GpuPrecip& g = p.precip[e];
        g.box = glm::vec4(src.box, float(src.snow), 1.0f / k, view.aspect / k);
        g.shape = glm::vec4(src.size_x, src.size_y, src.offset_max - src.offset_min, glm::radians(src.rotation_range));
        g.scroll = glm::vec4(src.scroll, src.rotation);
        g.forward = glm::vec4(forward * (src.box * 0.5f), src.amount);
        g.velocity = glm::vec4((src.wind + glm::vec3(0.0f, 0.0f, -src.gravity)) / 15.0f, 0.0f);
        for (int i = 0; i < 10; ++i) {
            g.instance[i] = glm::vec4(src.instance_offset[i], 0.0f);
        }
        g.info = glm::uvec4(src.seed, src.texture, src.subtextures_x, src.subtextures_y);
    }

    const VkDeviceSize offset = arena_alloc(fr, sizeof(GpuPost), 16);
    std::memcpy(fr.arena.as<std::byte>() + offset, &p, sizeof(p));
    return fr.arena.address + offset;
}

void Renderer::fullscreen(VkCommandBuffer cmd, Program program, VkImageView view, VkExtent2D extent, PushConstants& pc,
                          uint32_t mode, uint32_t a, uint32_t b, uint32_t c) {
    VkRenderingAttachmentInfo out{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    out.imageView = view;
    out.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    out.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    out.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &out;
    vkCmdBeginRendering(cmd, &ri);
    fullscreen_draw(cmd, program, extent, pc, mode, a, b, c);
    vkCmdEndRendering(cmd);
}

void Renderer::fullscreen_draw(VkCommandBuffer cmd, Program program, VkExtent2D extent, PushConstants& pc, uint32_t mode,
                               uint32_t a, uint32_t b, uint32_t c) {
    VkViewport vp{0, 0, float(extent.width), float(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    // A door's far frame is only seen through the doorway: its full-size passes stop a margin past it, scaled to their
    // target. The small targets of the chains stay whole: they cost little and read widely.
    if (m_far_pass && extent.width * 2 >= m_target_extent.width) {
        const float sx = float(extent.width) / float(m_target_extent.width), sy = float(extent.height) / float(m_target_extent.height);
        constexpr float k_margin = 128.0f;
        const VkRect2D& door = m_portal.scissor;
        const int32_t x0 = std::max(int32_t((float(door.offset.x) - k_margin) * sx), 0);
        const int32_t y0 = std::max(int32_t((float(door.offset.y) - k_margin) * sy), 0);
        const int32_t x1 = std::min(int32_t((float(door.offset.x + int32_t(door.extent.width)) + k_margin) * sx), int32_t(extent.width));
        const int32_t y1 = std::min(int32_t((float(door.offset.y + int32_t(door.extent.height)) + k_margin) * sy), int32_t(extent.height));
        scissor = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    }
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program, false));
    DrawState s;
    s.two_sided = true;
    s.z_test = false;
    s.z_write = false;
    apply_state(cmd, s, false);
    pc.mode = mode;
    pc.pass[0] = a;
    pc.pass[1] = b;
    pc.pass[2] = c;
    pc.pass[3] = extent.width | (extent.height << 16);
    vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    ++m_stats.draw_calls;
}

void Renderer::post_pass(VkCommandBuffer cmd, Program program, PostTarget& out, PushConstants& pc, uint32_t mode,
                         uint32_t a, uint32_t b, uint32_t c) {
    barrier(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, k_fragment | k_color_out,
            VK_ACCESS_2_NONE, k_color_out, k_color_write);
    fullscreen(cmd, program, out.view, out.extent, pc, mode, a, b, c);
    barrier(cmd, out.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, k_color_out,
            k_color_write, k_fragment, k_sampled);
}

// SAO_RenderAOOrComposite, first call: camera Z, its pyramid, the raw occlusion and its blur.
void Renderer::record_ao(VkCommandBuffer cmd, PushConstants& pc) {
    using B = Bindless;
    const VkExtent2D extent = m_target_extent;
    // The contrast weights replace the plain minify once, at the first level whose source height is at most 16.
    bool contrast_done = false;
    for (uint32_t i = 0; i < m_camz_mips; ++i) {
        const uint32_t m = i + 1; // the game's level
        const VkExtent2D e{std::max(extent.width >> m, 1u), std::max(extent.height >> m, 1u)};
        barrier(cmd, m_camz_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                k_fragment | k_color_out, VK_ACCESS_2_NONE, k_color_out, k_color_write, i);
        uint32_t mode = m > 4 ? 1u : 0u;
        const uint32_t source_height = std::max(extent.height >> (m - 1), 1u);
        if (!contrast_done && source_height <= 16) {
            mode |= 2u;
            contrast_done = true;
        }
        if (m == 1) {
            mode |= 4u;
        }
        fullscreen(cmd, program_ao_minify, m_camz_views[i], e, pc, mode, m == 1 ? B::k_depth_slot : B::k_camz_slot + i - 1);
        barrier(cmd, m_camz_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                k_color_out, k_color_write, k_fragment, k_sampled, i);
    }
    post_pass(cmd, program_ao_raw, m_ao_raw, pc, 0, B::k_camz_chain_slot, B::k_normal_slot, B::k_depth_slot);
    post_pass(cmd, program_ao_blur, m_ao_tmp, pc, 0, B::k_ao_raw_slot);
    post_pass(cmd, program_ao_blur, m_ao, pc, 1, B::k_ao_tmp_slot);
}

// The scene as one sample per pixel: the game's composite input (RT 47 and 55) is single-sampled, the reflections march
// over it and the composite's limit on them reads it.
void Renderer::record_scene_copy(VkCommandBuffer cmd) {
    const VkExtent2D extent = m_target_extent;
    constexpr auto transfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    barrier(cmd, m_hdr.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, k_color_out,
            k_color_write, transfer, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cmd, m_snow_source.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, k_fragment,
            VK_ACCESS_2_NONE, transfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (m_samples > 1) {
        VkImageResolve region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = {extent.width, extent.height, 1};
        vkCmdResolveImage(cmd, m_hdr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_snow_source.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = {extent.width, extent.height, 1};
        vkCmdCopyImage(cmd, m_hdr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_snow_source.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    barrier(cmd, m_snow_source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            transfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, k_fragment, k_sampled);
    barrier(cmd, m_hdr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, transfer,
            VK_ACCESS_2_NONE, k_color_out, k_color_write);
}

// The reflection passes (events 82205-82261): traced at half size, then blurred.
void Renderer::record_ssr(VkCommandBuffer cmd, PushConstants& pc, bool water) {
    using B = Bindless;
    post_pass(cmd, program_ssr_trace, m_ssr_raw, pc, water ? 1 : 0, B::k_normal_slot,
              water ? B::k_water_depth_slot : B::k_depth_slot, B::k_snow_source_slot);
    post_pass(cmd, program_ssr_blur, m_ssr[1], pc, 0, B::k_ssr_raw_slot);
    post_pass(cmd, program_ssr_blur, m_ssr[0], pc, 1, B::k_ssr_slot + 1);
}

// snow scatters the light under it. ps 212 reads the scene and writes every pixel; here a copy
// of the scene is the source and only snow pixels are written back. Runs when the composite last met snow.
void Renderer::record_snow(VkCommandBuffer cmd, PushConstants& pc) {
    using B = Bindless;
    const VkExtent2D extent = m_target_extent;
    VkRenderingAttachmentInfo out{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    out.imageView = m_hdr.view;
    out.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    out.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    out.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &out;
    vkCmdBeginRendering(cmd, &ri);
    fullscreen_draw(cmd, program_snow_sss, extent, pc, 0, B::k_snow_source_slot, B::k_normal_slot, B::k_depth_slot);
    vkCmdEndRendering(cmd);
    barrier(cmd, m_hdr.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            k_color_out, k_color_write, k_color_out, k_color_write | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
}

// The deferred composite, as a blended draw into the scene's colour. Its saturate needs no pass of its own: lit surfaces
// leave their shaders at most 1.2 (GpuFrame::params.w), which the composite's 1 / 1.2 brings to 1.
void Renderer::record_composite(VkCommandBuffer cmd, PushConstants& pc) {
    using B = Bindless;
    const VkExtent2D extent = m_target_extent;
    const bool msaa = m_samples > 1;
    if (msaa) {
        barrier(cmd, m_depth_target.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, k_fragment, k_sampled, 0, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    VkRenderingAttachmentInfo out{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    out.imageView = m_hdr.view;
    out.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    out.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    out.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &out;
    vkCmdBeginRendering(cmd, &ri);
    const uint32_t plain = m_far_pass && !m_door_effects ? 2u : 0u; // no occlusion or reflections to read
    fullscreen_draw(cmd, program_composite, extent, pc, (msaa ? 1u : 0u) | plain, B::k_ao_slot,
                    msaa ? B::k_scene_depth_slot : B::k_depth_slot, B::k_snow_source_slot);
    vkCmdEndRendering(cmd);
    if (msaa) {
        barrier(cmd, m_depth_target.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, k_fragment, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0, 1,
                VK_IMAGE_ASPECT_DEPTH_BIT);
    }
}

// Box decals: the game clips its target's triangles to the box; here each pixel the box covers
// asks the depth what surface it holds. Per sample, as the composite, so their edges stay as sharp as the scene's.
void Renderer::record_decals(VkCommandBuffer cmd, Frame& frame, const PushConstants& pc, std::span<const GpuDecal> decals) {
    using B = Bindless;
    const VkDeviceSize offset = arena_alloc(frame, decals.size_bytes(), 16);
    std::memcpy(frame.arena.as<std::byte>() + offset, decals.data(), decals.size_bytes());
    const VkExtent2D extent = m_target_extent;
    const bool msaa = m_samples > 1;
    constexpr VkPipelineStageFlags2 k_depth_stages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    if (msaa) {
        barrier(cmd, m_depth_target.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                k_depth_stages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, k_fragment, k_sampled, 0, 1,
                VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    VkRenderingAttachmentInfo out{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    out.imageView = m_hdr.view;
    out.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    out.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    out.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &out;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0, 0, float(extent.width), float(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_decal, false));
    DrawState s;
    s.two_sided = true; // decal.vert keeps the faces away from the camera
    s.z_test = false;
    s.z_write = false;
    apply_state(cmd, s, false);
    PushConstants p = pc;
    p.particles = frame.arena.address + offset;
    p.mode = msaa ? 1u : 0u;
    p.pass[0] = msaa ? B::k_scene_depth_slot : B::k_depth_slot;
    p.pass[1] = B::k_normal_slot;
    p.pass[3] = extent.width | extent.height << 16;
    vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 36, uint32_t(decals.size()), 0, 0);
    ++m_stats.draw_calls;
    vkCmdEndRendering(cmd);
    if (msaa) {
        barrier(cmd, m_depth_target.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                k_fragment, VK_ACCESS_2_NONE, k_depth_stages,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0, 1,
                VK_IMAGE_ASPECT_DEPTH_BIT);
    }
}

// The screen blood: one pass over the finished image while any splat lives.
void Renderer::record_screen_blood(VkCommandBuffer cmd, Frame& frame, const PushConstants& pc, VkImageView view,
                                   VkExtent2D extent, const FrameInput& input) {
    const VkDeviceSize offset = arena_alloc(frame, input.screen_blood.size_bytes(), 16);
    std::memcpy(frame.arena.as<std::byte>() + offset, input.screen_blood.data(), input.screen_blood.size_bytes());
    VkRenderingAttachmentInfo out{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    out.imageView = view;
    out.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    out.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    out.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &out;
    vkCmdBeginRendering(cmd, &ri);
    PushConstants p = pc;
    p.particles = frame.arena.address + offset;
    const auto& t = input.screen_blood_textures;
    fullscreen_draw(cmd, program_screen_blood, extent, p, uint32_t(input.screen_blood.size()), t[0], t[1], t[2]);
    vkCmdEndRendering(cmd);
}

// the volume. It reads nothing of the scene, so it is recorded at the start
// of the frame, where the GPU can overlap it with the opaque pass.
void Renderer::record_volume(VkCommandBuffer cmd, Frame& fr, const View& view, const Environment& env,
                             VkDeviceSize frame_offset) {
    const VolumetricParams& vl = env.sky.volumetric;
    GpuVolumeParams q{};
    const float p00 = view.projection[0][0], p11 = std::abs(view.projection[1][1]);
    q.camera_right = glm::vec4(view.rotation[0][0], view.rotation[1][0], view.rotation[2][0], 1.0f / p00);
    q.camera_up = glm::vec4(view.rotation[0][1], view.rotation[1][1], view.rotation[2][1], 1.0f / p11);
    q.light = glm::vec4(glm::normalize(env.sun_dir), env.near_units);
    q.grid = glm::vec4(k_vl_width, k_vl_height, k_vl_depth, env.far_units);
    q.noise = glm::vec4(vl.wind_offset + view.position / units::k_skyrim_unit_m, 1.0f / vl.density_size);
    q.scatter = glm::vec4(vl.phase_contribution, vl.phase_scattering, vl.density_contribution, 1.0f / vl.range);
    const auto* gf = reinterpret_cast<const GpuFrame*>(fr.arena.as<std::byte>() + frame_offset);
    std::copy(std::begin(gf->shadow_rows), std::end(gf->shadow_rows), q.shadow_rows);
    q.shadow_split = gf->shadow_split;
    q.shadow_info = gf->shadow_info;
    q.shadow_info.z = Bindless::k_god_ray_slot;
    const VkDeviceSize offset = arena_alloc(fr, sizeof(q), 16);
    std::memcpy(fr.arena.as<std::byte>() + offset, &q, sizeof(q));

    // The shadow maps end up readable by fragment shaders; the volume reads them too.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = k_fragment;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.dstAccessMask = k_sampled;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);

    VolumePushConstants vp{fr.arena.address + offset, 0, Bindless::k_vl_noise_slot};
    constexpr auto compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr auto storage_write = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines->volume_pipeline());
    const VkDescriptorSet sets[2] = {m_bindless->set(m_slot), m_vl_set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines->volume_layout(), 0, 2, sets, 0, nullptr);
    // Both images are in the set each dispatch binds, so both are GENERAL for both: the noise stays there, sampled from it.
    barrier(cmd, m_vl_volume.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, k_fragment, VK_ACCESS_2_NONE,
            compute, storage_write);
    if (!m_vl_noise_ready) {
        barrier(cmd, m_vl_noise.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, compute, VK_ACCESS_2_NONE,
                compute, storage_write);
        vp.mode = 1;
        vkCmdPushConstants(cmd, m_pipelines->volume_layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
        vkCmdDispatch(cmd, 4, 4, 11); // 32 x 32 x 33 threads
        barrier(cmd, m_vl_noise.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, compute, storage_write, compute,
                k_sampled);
        vp.mode = 0;
        m_vl_noise_ready = true;
    }
    vkCmdPushConstants(cmd, m_pipelines->volume_layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
    vkCmdDispatch(cmd, (k_vl_width + 7) / 8, (k_vl_height + 7) / 8, 1);
}

// the volume's apply and blur, added to the resolved scene.
// The image-based lens flare (events 88259-88718). Nothing reaches it below 1, so the first pass marks the frame and the
// streaks only run after one that had something: at most a frame late. The bright pass's own blur and the 120x67 level
// have no weight in the capture's constants and are left out.
void Renderer::record_lens_flare(VkCommandBuffer cmd, PushConstants& pc) {
    using B = Bindless;
    post_pass(cmd, program_lens_down, m_quarter, pc, 0, B::k_scene_slot, B::k_depth_slot);
    if (!m_lens_visible) {
        return;
    }
    post_pass(cmd, program_lens_streak, m_lens[0], pc, 0, B::k_quarter_slot);
    post_pass(cmd, program_lens_streak, m_lens[2], pc, 1, m_lens[0].slot);
    // Vertical then horizontal, three times, weighted up; then the wider streak's horizontal then vertical, weighted down.
    for (uint32_t i = 0; i < 3; ++i) {
        post_pass(cmd, program_lens_blur, m_lens[3], pc, 1 | 2, m_lens[2].slot);
        post_pass(cmd, program_lens_blur, m_lens[2], pc, 0 | 2, m_lens[3].slot);
    }
    for (uint32_t i = 0; i < 3; ++i) {
        post_pass(cmd, program_lens_blur, m_lens[1], pc, 0, m_lens[0].slot);
        post_pass(cmd, program_lens_blur, m_lens[0], pc, 1 | (i == 2 ? 4 : 0), m_lens[1].slot, m_lens[2].slot);
    }
}

// VolumetricLighting: apply and blur, then ISCompositeLensFlareVolumetricLighting (ISCompositeLensFlare without the first).
void Renderer::record_volumetric(VkCommandBuffer cmd, PushConstants& pc, bool volumetric, bool lens) {
    using B = Bindless;
    if (volumetric) {
        barrier(cmd, m_vl_volume.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, k_fragment, k_sampled);

        post_pass(cmd, program_vl_apply, m_vl[0], pc, 0, B::k_depth_slot);
        post_pass(cmd, program_vl_blur, m_vl[1], pc, 0, m_vl[0].slot, B::k_depth_slot);
    }
    const VkImage scene = m_samples > 1 ? m_hdr_resolved.image : m_hdr.image;
    barrier(cmd, scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, k_fragment,
            VK_ACCESS_2_NONE, k_color_out, k_color_write | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    VkRenderingAttachmentInfo out{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    out.imageView = m_samples > 1 ? m_hdr_resolved.view : m_hdr.view;
    out.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    out.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    out.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, m_target_extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &out;
    vkCmdBeginRendering(cmd, &ri);
    // The vertical half of the blur, added straight to the scene.
    fullscreen_draw(cmd, program_vl_composite, m_target_extent, pc, (volumetric ? 3u : 8u) | (lens ? 4u : 0u), m_vl[1].slot,
                    B::k_depth_slot, m_lens[0].slot);
    vkCmdEndRendering(cmd);
    barrier(cmd, scene, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, k_color_out,
            k_color_write, k_fragment, k_sampled);
}

// Rain and snow: ceil(amount) draws of an emitter's 1000 particles, the last at the fraction.
void Renderer::record_precipitation(VkCommandBuffer cmd, const SkyFrame& sky, PushConstants& pc) {
    for (uint32_t e = 0; e < 2; ++e) {
        const float amount = sky.precipitation[e].amount;
        if (amount <= 0.0f || sky.precipitation[e].texture == 0) {
            continue;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_precipitation, false));
        DrawState s;
        s.two_sided = true;
        s.z_write = false;
        apply_state(cmd, s, false);
        const uint32_t passes = std::min(static_cast<uint32_t>(std::ceil(amount)), 10u);
        for (uint32_t p = 0; p < passes; ++p) {
            pc.mode = e | (p << 8);
            vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
            vkCmdDraw(cmd, 6000, 1, 0, 0);
            ++m_stats.draw_calls;
        }
    }
    pc.mode = 0;
}

// The sky objects in the game's order (the sky range of the game).
void Renderer::record_sky(VkCommandBuffer cmd, Frame& frame, const SkyFrame& sky, PushConstants& pc, bool sky_model,
                          bool cube) {
    if (!m_sky_vertices || sky.draw_count == 0) {
        return;
    }
    VkDeviceSize zero = 0;
    auto bind_sky = [&] {
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_sky_vertices.buffer, &zero);
        vkCmdBindIndexBuffer(cmd, m_sky_indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    };
    bind_sky();
    bool model_drawn = !sky_model || m_runs_sky.empty();
    for (uint32_t i = 0; i < sky.draw_count; ++i) {
        const SkyDraw& d = sky.draws[i];
        const bool cloud = d.technique >= sky_tech_clouds && d.technique <= sky_tech_clouds_fade;
        if (cloud && !model_drawn) {
            // A weather's sky model lies behind the clouds.
            VkBuffer vb = m_meshes->vertices().buffer().buffer;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
            vkCmdBindIndexBuffer(cmd, m_meshes->indices().buffer().buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
            record_runs(cmd, frame, m_runs_sky, false, false);
            bind_sky();
            model_drawn = true;
        }
        const bool mask = d.technique == sky_tech_mask;
        const bool additive = d.technique == sky_tech_stars || d.technique == sky_tech_texture;
        const Program program = cube ? (mask ? program_cube_sky_mask : additive ? program_cube_sky_add : program_cube_sky_alpha)
                                     : (mask ? program_sky_mask : additive ? program_sky_add : program_sky_alpha);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program, false));
        DrawState s;
        s.two_sided = true;
        s.z_write = mask;
        apply_state(cmd, s, false);
        pc.mode = i;
        vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, 0, 0);
        ++m_stats.draw_calls;
    }
}

// ImageSpaceEffectHDR::Render and the distant blur's first passes (ImageSpaceEffectDepthOfField):
// everything up to the last pass, which draws into the output target.
void Renderer::record_image_space(VkCommandBuffer cmd, const SkyFrame& sky, PushConstants& pc, uint32_t scene_slot) {
    using B = Bindless;
    const ImageSpaceData& is = sky.image_space;
    if (is.world_map) {
        post_pass(cmd, program_world_map_blur, m_radial, pc, 0, scene_slot, B::k_depth_slot);
        scene_slot = B::k_radial_slot;
    }
    if (is.radial_strength > 0.0f) {
        post_pass(cmd, program_radial_blur, m_radial, pc, 0, scene_slot);
        scene_slot = B::k_radial_slot;
    }
    if (is.blur_radius > 0.0f) {
        // Downsample, the HDR blur at n = ceil(radius), and back over the scene. The chain's own quarter target and
        // bloom targets are free here; the gauss table is the bloom's (radius 7 at most).
        const uint32_t n = std::min(static_cast<uint32_t>(std::ceil(is.blur_radius)), 7u);
        post_pass(cmd, program_hdr_down4, m_quarter, pc, 0, scene_slot);
        post_pass(cmd, program_bloom_blur, m_bloom[0], pc, n, B::k_quarter_slot);
        post_pass(cmd, program_bloom_blur, m_bloom[1], pc, n | 0x100u, m_bloom[0].slot);
        PostTarget& out = scene_slot == B::k_radial_slot ? m_blurred : m_radial;
        post_pass(cmd, program_blur_mix, out, pc, 0, scene_slot, m_bloom[1].slot);
        scene_slot = out.slot;
    }
    post_pass(cmd, program_hdr_down4, m_quarter, pc, 0, scene_slot);
    post_pass(cmd, program_hdr_lum4, m_lum[0], pc, 0, B::k_quarter_slot);
    for (uint32_t i = 1; i < m_lum_count; ++i) {
        post_pass(cmd, program_hdr_lum16, m_lum[i], pc, 0, m_lum[i - 1].slot);
    }
    PostTarget& adapted = m_adapt[m_adapt_index];
    PostTarget& previous = m_adapt[m_adapt_index ^ 1];
    post_pass(cmd, program_hdr_adapt, adapted, pc, 0, m_lum[m_lum_count - 1].slot, previous.slot);
    const bool own_eye = m_far_pass || sky.ui_scene; // nothing here is the world's eye to keep: the game's chain has its own target
    m_adapt_valid = m_adapt_valid || !own_eye;
    post_pass(cmd, program_bloom_bright, m_bloom[0], pc, m_bloom_taps, B::k_quarter_slot);
    post_pass(cmd, program_bloom_blur, m_bloom[1], pc, m_bloom_taps, m_bloom[0].slot);
    post_pass(cmd, program_tonemap, m_ldr, pc, 0, m_bloom[1].slot, scene_slot, adapted.slot);
    m_adapt_index ^= own_eye ? 0 : 1;
    if (sky.ui_scene) {
        return; // the tonemap is the last pass there (capture: it writes the swap chain)
    }
    post_pass(cmd, program_ldr_down4, m_dof[0], pc, 0, B::k_ldr_slot);
    post_pass(cmd, program_dof_blur, m_dof[1], pc, 0, m_dof[0].slot, B::k_depth_slot);
    post_pass(cmd, program_dof_blur, m_dof[0], pc, 1, m_dof[1].slot, B::k_depth_slot);
}

} // namespace spud::render
