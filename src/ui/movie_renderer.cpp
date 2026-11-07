#include "ui/movie_renderer.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "ui/player/movie.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace spud::ui {

// Mirrors FilterPush in shaders/uimovie_filter.glsl (scalar layout).
struct FilterPush {
    float rect[4];
    float to_clip[2];
    int32_t mode;
    int32_t flags;
    float offset[2];
    float shadow[2];
    float color[4];
    float strength;
    float full_size;
    int32_t m;
    float alpha;
    float mul[4];
    float add[4];
    float matrix[5][4];
};
static_assert(sizeof(FilterPush) == 192);

// One filter pass of a layer: `push` into atlas image `out`, reading images `a` and `b`.
struct FilterStep {
    uint32_t out, a, b;
    FilterPush push;
};

namespace {

using render::vk_check;

enum FilterMode : int32_t { mode_blur_h, mode_blur_v, mode_blur_alpha_h, mode_blur_alpha_v, mode_shadow,
                            mode_color_matrix, mode_composite };

// One box-blur pass of width `size` pixels, as Ruffle documents Flash Player's (render/wgpu filters/blur.rs): the
// centre and m taps each side at full weight, one more each side at a weight quantised to 1/255.
void box_kernel(float size, FilterPush& p) {
    p.full_size = std::clamp(size, 1.0f, 255.0f); // width 1 and under: no blur (the game's text shadows have blur 0)
    const float radius = (p.full_size - 1.0f) * 0.5f;
    const float m = std::max(0.0f, std::ceil(radius) - 1.0f);
    p.m = int32_t(m);
    p.alpha = std::floor((radius - m) * 255.0f) / 255.0f;
}
using player::DrawItem;
using player::Matrix2D;

constexpr uint32_t k_max_sets = 4096;

// Mirrors MovieDraw in shaders/uimovie_common.glsl (scalar layout).
struct GpuDraw {
    float xform[4];
    float xform_t[4];
    float mul[4];
    float add[4];
    float color[4];
    float fill[4];
    float fill_t[4];
    float rect[4];
    float inner[4];
    float inner_t[4];
    float s9_bounds[4];
    float s9_grid[4];
    uint64_t vertices;
    uint64_t stops;
    uint32_t stop_count;
    uint32_t sampler;
    uint32_t flags;
    uint32_t pad;
};
static_assert(sizeof(GpuDraw) == 224);

// Mirrors MoviePush.
struct GpuPush {
    VkDeviceAddress draws;
    float to_clip[2];
    uint32_t draw;
    uint32_t pad;
};
static_assert(sizeof(GpuPush) == 24);

enum Paint : uint32_t { paint_solid, paint_linear, paint_radial, paint_focal, paint_bitmap };

VkShaderModule load_module(VkDevice dev, const std::filesystem::path& file) {
    const auto code = read_file(file);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    vk_check(vkCreateShaderModule(dev, &info, nullptr, &m));
    return m;
}

void unpack(uint32_t c, float out[4]) {
    out[0] = float(c & 255) / 255.0f;
    out[1] = float((c >> 8) & 255) / 255.0f;
    out[2] = float((c >> 16) & 255) / 255.0f;
    out[3] = float(c >> 24) / 255.0f;
}

void put(float out[4], float a, float b, float c, float d) {
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags2 src, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst,
                   VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst;
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

bool has_depth(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

} // namespace

// ---- setup ---------------------------------------------------------------------------------------

MovieRenderer::MovieRenderer(render::Device& device, render::Uploader& uploader, VkFormat color_format,
                             VkExtent2D extent, const std::filesystem::path& shader_dir,
                             const std::filesystem::path& pipeline_cache)
    : m_device(device), m_uploader(uploader), m_color_format(color_format) {
    VkDevice dev = device.device();
    // Stencil only is all masks need; take the smallest format that supports the sample count.
    for (VkFormat f : {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        VkImageFormatProperties props{};
        if (vkGetPhysicalDeviceImageFormatProperties(device.physical(), f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0,
                                                     &props) == VK_SUCCESS &&
            (props.sampleCounts & m_samples)) {
            m_stencil_format = f;
            break;
        }
    }
    if (m_stencil_format == VK_FORMAT_UNDEFINED) {
        throw Error("ui: no multisampled stencil format");
    }

    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.maxLod = 0.0f;
    for (uint32_t i = 0; i < 4; ++i) {
        const bool repeat = i & 1, smooth = i & 2;
        s.magFilter = s.minFilter = smooth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        s.addressModeU = s.addressModeV = s.addressModeW =
            repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vk_check(vkCreateSampler(dev, &s, nullptr, &m_samplers[i]));
    }

    VkDescriptorSetLayoutBinding tex{};
    tex.binding = 0;
    tex.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    tex.descriptorCount = 1;
    tex.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &tex;
    vk_check(vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_texture_layout));
    VkDescriptorSetLayoutBinding smp{};
    smp.binding = 0;
    smp.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    smp.descriptorCount = 4;
    smp.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    smp.pImmutableSamplers = m_samplers.data();
    li.pBindings = &smp;
    vk_check(vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_sampler_layout));

    const std::array<VkDescriptorSetLayout, 2> sets = {m_texture_layout, m_sampler_layout};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GpuPush)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = uint32_t(sets.size());
    pl.pSetLayouts = sets.data();
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    vk_check(vkCreatePipelineLayout(dev, &pl, nullptr, &m_layout));
    const std::array<VkDescriptorSetLayout, 3> filter_sets = {m_texture_layout, m_texture_layout, m_sampler_layout};
    VkPushConstantRange filter_push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FilterPush)};
    pl.setLayoutCount = uint32_t(filter_sets.size());
    pl.pSetLayouts = filter_sets.data();
    pl.pPushConstantRanges = &filter_push;
    vk_check(vkCreatePipelineLayout(dev, &pl, nullptr, &m_filter_layout));

    const std::array<VkDescriptorPoolSize, 2> sizes = {{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, k_max_sets + 8},
                                                        {VK_DESCRIPTOR_TYPE_SAMPLER, 4}}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = k_max_sets + 9;
    pool.poolSizeCount = uint32_t(sizes.size());
    pool.pPoolSizes = sizes.data();
    vk_check(vkCreateDescriptorPool(dev, &pool, nullptr, &m_pool));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_sampler_layout;
    vk_check(vkAllocateDescriptorSets(dev, &ai, &m_sampler_set));

    // A transparent texel for draws without a bitmap (the fragment shader always declares one).
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vk_check(m_device.create_image(info, alloc, render::Lifetime::asset, "ui movie blank", m_blank.image,
                                   m_blank.allocation));
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = m_blank.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(dev, &view, nullptr, &m_blank.view));
    m_device.immediate([&](VkCommandBuffer cmd) {
        image_barrier(cmd, m_blank.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue clear{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, m_blank.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        image_barrier(cmd, m_blank.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    ai.pSetLayouts = &m_texture_layout;
    vk_check(vkAllocateDescriptorSets(dev, &ai, &m_blank.set));
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, m_blank.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_blank.set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    m_blank.ready = true;

    for (auto& f : m_frames) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        vk_check(vkCreateQueryPool(dev, &qi, nullptr, &f.queries));
        vkResetQueryPool(dev, f.queries, 0, 2);
        // Room for 4096 draws a frame up front (the journal draws about 170); more grows the buffer.
        f.draws = device.create_buffer(4096 * sizeof(GpuDraw), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       render::MemoryUsage::dynamic, render::Lifetime::frame, "ui movie draws");
    }
    m_timestamp_period_ns = device.properties().limits.timestampPeriod;
    create_pipelines(shader_dir, pipeline_cache);
    if (extent.width && extent.height) {
        ensure_targets(extent);
    }
}

MovieRenderer::~MovieRenderer() {
    VkDevice dev = m_device.device();
    m_device.wait_idle();
    m_device.flush_all_garbage();
    destroy_targets();
    m_device.flush_all_garbage();
    for (auto& t : m_textures) {
        if (t.image) {
            vkDestroyImageView(dev, t.view, nullptr);
            vmaDestroyImage(m_device.allocator(), t.image, t.allocation);
        }
    }
    vkDestroyImageView(dev, m_blank.view, nullptr);
    vmaDestroyImage(m_device.allocator(), m_blank.image, m_blank.allocation);
    for (auto& [def, g] : m_movies) {
        m_device.destroy_buffer(g.vertices);
        m_device.destroy_buffer(g.indices);
        m_device.destroy_buffer(g.stops);
    }
    for (auto& f : m_frames) {
        m_device.destroy_buffer(f.draws);
        if (f.vertices) {
            m_device.destroy_buffer(f.vertices);
        }
        vkDestroyQueryPool(dev, f.queries, nullptr);
    }
    vkDestroyDescriptorPool(dev, m_pool, nullptr);
    for (auto p : m_pipelines) {
        vkDestroyPipeline(dev, p, nullptr);
    }
    vkDestroyPipelineLayout(dev, m_layout, nullptr);
    vkDestroyPipelineLayout(dev, m_filter_layout, nullptr);
    vkDestroyDescriptorSetLayout(dev, m_texture_layout, nullptr);
    vkDestroyDescriptorSetLayout(dev, m_sampler_layout, nullptr);
    for (auto s : m_samplers) {
        vkDestroySampler(dev, s, nullptr);
    }
}

void MovieRenderer::create_pipelines(const std::filesystem::path& shader_dir, const std::filesystem::path& cache_file) {
    VkDevice dev = m_device.device();
    std::vector<std::byte> cache_data;
    std::error_code ec;
    if (!cache_file.empty() && std::filesystem::exists(cache_file, ec)) {
        try {
            cache_data = read_file(cache_file);
        } catch (const std::exception&) {
            cache_data.clear();
        }
    }
    VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cache_info.initialDataSize = cache_data.size();
    cache_info.pInitialData = cache_data.data();
    VkPipelineCache cache = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(dev, &cache_info, nullptr, &cache) != VK_SUCCESS) {
        cache_info.initialDataSize = 0;
        vk_check(vkCreatePipelineCache(dev, &cache_info, nullptr, &cache));
    }
    const VkShaderModule vert = load_module(dev, shader_dir / "uimovie.vert.spv");
    const VkShaderModule frag = load_module(dev, shader_dir / "uimovie.frag.spv");
    const VkShaderModule cvert = load_module(dev, shader_dir / "uimovie_composite.vert.spv");
    const VkShaderModule cfrag = load_module(dev, shader_dir / "uimovie_composite.frag.spv");

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = m_samples;
    VkPipelineMultisampleStateCreateInfo ms1{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms1.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState premul{};
    premul.blendEnable = VK_TRUE;
    premul.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    premul.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    premul.colorBlendOp = VK_BLEND_OP_ADD;
    premul.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    premul.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    premul.alphaBlendOp = VK_BLEND_OP_ADD;
    premul.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                            VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState no_color{};
    VkPipelineColorBlendStateCreateInfo cb_content{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb_content.attachmentCount = 1;
    cb_content.pAttachments = &premul;
    VkPipelineColorBlendStateCreateInfo cb_mask = cb_content;
    cb_mask.pAttachments = &no_color;

    // Masks: geometry raises the stencil from the current level to the next (or back down), never twice
    // where its triangles overlap, because the test asks for the level it starts from.
    auto stencil = [](VkStencilOp pass, uint32_t write) {
        VkStencilOpState s{};
        s.failOp = VK_STENCIL_OP_KEEP;
        s.passOp = pass;
        s.depthFailOp = VK_STENCIL_OP_KEEP;
        s.compareOp = VK_COMPARE_OP_EQUAL;
        s.compareMask = 0xFF;
        s.writeMask = write;
        return s;
    };
    VkPipelineDepthStencilStateCreateInfo ds_content{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds_content.stencilTestEnable = VK_TRUE;
    ds_content.front = ds_content.back = stencil(VK_STENCIL_OP_KEEP, 0);
    VkPipelineDepthStencilStateCreateInfo ds_add = ds_content;
    ds_add.front = ds_add.back = stencil(VK_STENCIL_OP_INCREMENT_AND_CLAMP, 0xFF);
    VkPipelineDepthStencilStateCreateInfo ds_remove = ds_content;
    ds_remove.front = ds_remove.back = stencil(VK_STENCIL_OP_DECREMENT_AND_CLAMP, 0xFF);
    VkPipelineDepthStencilStateCreateInfo ds_none{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

    const std::array<VkDynamicState, 3> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                               VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(dyn.size());
    dy.pDynamicStates = dyn.data();
    VkPipelineDynamicStateCreateInfo dy2 = dy;
    dy2.dynamicStateCount = 2;

    VkPipelineRenderingCreateInfo layer_rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    layer_rendering.colorAttachmentCount = 1;
    layer_rendering.pColorAttachmentFormats = &m_color_format;
    layer_rendering.stencilAttachmentFormat = m_stencil_format;
    VkPipelineRenderingCreateInfo target_rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    target_rendering.colorAttachmentCount = 1;
    target_rendering.pColorAttachmentFormats = &m_color_format;

    std::array<std::array<VkPipelineShaderStageCreateInfo, 2>, pipe_filter> stages{};
    std::array<VkGraphicsPipelineCreateInfo, pipe_filter> infos{};
    for (uint32_t i = 0; i < pipe_filter; ++i) {
        const bool composite = i == pipe_composite;
        stages[i][0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i][0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[i][0].module = composite ? cvert : vert;
        stages[i][0].pName = "main";
        stages[i][1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[i][1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[i][1].module = composite ? cfrag : frag;
        stages[i][1].pName = "main";
        auto& p = infos[i];
        p = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        p.pNext = composite ? &target_rendering : &layer_rendering;
        p.stageCount = 2;
        p.pStages = stages[i].data();
        p.pVertexInputState = &vi;
        p.pInputAssemblyState = &ia;
        p.pViewportState = &vp;
        p.pRasterizationState = &rs;
        p.pMultisampleState = composite ? &ms1 : &ms;
        p.pDepthStencilState = i == pipe_content ? &ds_content : i == pipe_mask_add ? &ds_add
                               : i == pipe_mask_remove ? &ds_remove : &ds_none;
        p.pColorBlendState = i == pipe_content || composite ? &cb_content : &cb_mask;
        p.pDynamicState = composite ? &dy2 : &dy;
        p.layout = m_layout;
    }
    vk_check(vkCreateGraphicsPipelines(dev, cache, pipe_filter, infos.data(), nullptr, m_pipelines.data()));

    // Filter passes (single-sample, no blending) and layer composites (into the multisampled layers, under the
    // stencil, one blend state per blend mode).
    const VkShaderModule fvert = load_module(dev, shader_dir / "uimovie_filter.vert.spv");
    const VkShaderModule ffrag = load_module(dev, shader_dir / "uimovie_filter.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> fstages{};
    fstages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    fstages[0].module = fvert;
    fstages[0].pName = "main";
    fstages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fstages[1].module = ffrag;
    fstages[1].pName = "main";
    VkPipelineColorBlendAttachmentState replace{};
    replace.colorWriteMask = premul.colorWriteMask;
    VkPipelineColorBlendStateCreateInfo cb_replace = cb_content;
    cb_replace.pAttachments = &replace;
    constexpr uint32_t k_extra = k_pipeline_count - pipe_filter;
    std::array<VkPipelineColorBlendAttachmentState, k_extra> blends{};
    std::array<VkPipelineColorBlendStateCreateInfo, k_extra> cbs{};
    std::array<VkGraphicsPipelineCreateInfo, k_extra> extra{};
    for (uint32_t i = 0; i < k_extra; ++i) {
        const uint32_t pipe = pipe_filter + i;
        VkPipelineColorBlendAttachmentState b = premul; // normal, layer
        switch (pipe) {
        case pipe_layer_add:
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case pipe_layer_multiply: // src * dst + dst * (1 - src.a), premultiplied
            b.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            break;
        case pipe_layer_screen: // src + dst * (1 - src)
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            break;
        case pipe_layer_lighten:
            b.colorBlendOp = VK_BLEND_OP_MAX;
            b.srcColorBlendFactor = b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case pipe_layer_darken:
            b.colorBlendOp = VK_BLEND_OP_MIN;
            b.srcColorBlendFactor = b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case pipe_layer_subtract: // dst - src
            b.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
            b.srcColorBlendFactor = b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        default:
            break;
        }
        blends[i] = b;
        cbs[i] = cb_content;
        cbs[i].pAttachments = &blends[i];
        auto& info = extra[i];
        info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        const bool filter = pipe == pipe_filter;
        info.pNext = filter ? &target_rendering : &layer_rendering;
        info.stageCount = 2;
        info.pStages = fstages.data();
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = filter ? &ms1 : &ms;
        info.pDepthStencilState = filter ? &ds_none : &ds_content;
        info.pColorBlendState = filter ? &cb_replace : &cbs[i];
        info.pDynamicState = filter ? &dy2 : &dy;
        info.layout = m_filter_layout;
    }
    vk_check(vkCreateGraphicsPipelines(dev, cache, k_extra, extra.data(), nullptr, m_pipelines.data() + pipe_filter));
    for (auto m : {vert, frag, cvert, cfrag, fvert, ffrag}) {
        vkDestroyShaderModule(dev, m, nullptr);
    }
    size_t size = 0;
    if (!cache_file.empty() && vkGetPipelineCacheData(dev, cache, &size, nullptr) == VK_SUCCESS && size > 0) {
        std::vector<std::byte> data(size);
        if (vkGetPipelineCacheData(dev, cache, &size, data.data()) == VK_SUCCESS) {
            try {
                write_file(cache_file, std::span(data.data(), size));
            } catch (const std::exception& e) {
                log::warn("ui: cannot write pipeline cache {}: {}", cache_file.string(), e.what());
            }
        }
    }
    vkDestroyPipelineCache(dev, cache, nullptr);
}

// ---- targets -------------------------------------------------------------------------------------

void MovieRenderer::destroy_targets() {
    destroy_pages();
    if (!m_msaa) {
        return;
    }
    m_device.defer([dev = m_device.device(), alloc = m_device.allocator(), pool = m_pool, images = m_atlas,
                    allocs = m_atlas_alloc, views = m_atlas_view, sets = m_atlas_set] {
        for (size_t i = 0; i < images.size(); ++i) {
            vkDestroyImageView(dev, views[i], nullptr);
            vmaDestroyImage(alloc, images[i], allocs[i]);
            vkFreeDescriptorSets(dev, pool, 1, &sets[i]);
        }
    });
    m_device.defer([dev = m_device.device(), alloc = m_device.allocator(), pool = m_pool, msaa = m_msaa,
                    msaa_alloc = m_msaa_alloc, msaa_view = m_msaa_view, stencil = m_stencil,
                    stencil_alloc = m_stencil_alloc, stencil_view = m_stencil_view, resolved = m_resolved,
                    resolved_alloc = m_resolved_alloc, resolved_view = m_resolved_view, set = m_resolved_set] {
        vkDestroyImageView(dev, msaa_view, nullptr);
        vkDestroyImageView(dev, stencil_view, nullptr);
        vkDestroyImageView(dev, resolved_view, nullptr);
        vmaDestroyImage(alloc, msaa, msaa_alloc);
        vmaDestroyImage(alloc, stencil, stencil_alloc);
        vmaDestroyImage(alloc, resolved, resolved_alloc);
        vkFreeDescriptorSets(dev, pool, 1, &set);
    });
    m_msaa = m_stencil = m_resolved = VK_NULL_HANDLE;
    m_extent = {};
}

void MovieRenderer::ensure_targets(VkExtent2D extent) {
    if (m_msaa && extent.width == m_extent.width && extent.height == m_extent.height) {
        return;
    }
    destroy_targets();
    m_extent = extent;
    VkDevice dev = m_device.device();
    m_atlas_extent = {extent.width + 2, extent.height * 2}; // the gap pack() adds: a text field as wide as the screen
    auto make = [&](VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                    const char* name, VkImage& image, VmaAllocation& allocation, VkImageView& view,
                    VkExtent2D size = {}) {
        if (!size.width) {
            size = extent;
        }
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {size.width, size.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = samples;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        vk_check(m_device.create_image(info, alloc, render::Lifetime::frame, name, image, allocation));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        vk_check(vkCreateImageView(dev, &vi, nullptr, &view));
    };
    make(m_color_format, m_samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
         VK_IMAGE_ASPECT_COLOR_BIT, "ui movie layer", m_msaa, m_msaa_alloc, m_msaa_view);
    make(m_stencil_format, m_samples,
         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
         has_depth(m_stencil_format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_STENCIL_BIT,
         "ui movie stencil", m_stencil, m_stencil_alloc, m_stencil_view);
    make(m_color_format, VK_SAMPLE_COUNT_1_BIT,
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         VK_IMAGE_ASPECT_COLOR_BIT, "ui movie resolved", m_resolved, m_resolved_alloc, m_resolved_view);
    for (size_t i = 0; i < m_atlas.size(); ++i) {
        make(m_color_format, VK_SAMPLE_COUNT_1_BIT,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
             VK_IMAGE_ASPECT_COLOR_BIT, "ui movie layer image", m_atlas[i], m_atlas_alloc[i], m_atlas_view[i],
             m_atlas_extent);
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_texture_layout;
    vk_check(vkAllocateDescriptorSets(dev, &ai, &m_resolved_set));
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, m_resolved_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_resolved_set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    for (size_t i = 0; i < m_atlas.size(); ++i) {
        vk_check(vkAllocateDescriptorSets(dev, &ai, &m_atlas_set[i]));
        VkDescriptorImageInfo ai_info{VK_NULL_HANDLE, m_atlas_view[i], VK_IMAGE_LAYOUT_GENERAL};
        w.dstSet = m_atlas_set[i];
        w.pImageInfo = &ai_info;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }
    create_pages(extent.height);
}

// ---- the Book Menu's page targets ------------------------------------------------------------------

void MovieRenderer::create_pages(uint32_t size) {
    // Render targets 105..108: the screen's height square, made
    // with the other screen targets, before any book opens, as the game makes them.
    m_page_size = size;
    for (Page& p : m_pages) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = m_color_format;
        info.extent = {size, size, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        vk_check(m_device.create_image(info, alloc, render::Lifetime::frame, "ui book page", p.image, p.allocation));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = p.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = m_color_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_check(vkCreateImageView(m_device.device(), &vi, nullptr, &p.view));
    }
    // Sampled before any page is drawn (a book whose pages are not in yet): transparent, readable.
    m_device.immediate([&](VkCommandBuffer cmd) {
        for (const Page& p : m_pages) {
            image_barrier(cmd, p.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                          VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue clear{};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            image_barrier(cmd, p.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    });
    ++m_page_generation;
}

void MovieRenderer::destroy_pages() {
    for (Page& p : m_pages) {
        if (!p.image) {
            continue;
        }
        m_device.defer([dev = m_device.device(), alloc = m_device.allocator(), p] {
            vkDestroyImageView(dev, p.view, nullptr);
            vmaDestroyImage(alloc, p.image, p.allocation);
        });
        p = Page{};
    }
    m_page_size = 0;
}

void MovieRenderer::set_page_paper(VkImageView view) {
    if (view == m_paper_view) {
        return;
    }
    m_paper_view = view;
    m_paper_texture = 0;
    if (!view) {
        return;
    }
    // A slot of its own per view (a descriptor set in use by a frame in flight is never rewritten).
    auto it = m_external.find(view);
    if (it == m_external.end()) {
        const uint32_t slot = uint32_t(m_textures.size());
        m_textures.emplace_back();
        Texture& t = m_textures.back();
        t.view = view; // owned by the renderer's texture manager
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_texture_layout;
        vk_check(vkAllocateDescriptorSets(m_device.device(), &ai, &t.set));
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = t.set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(m_device.device(), 1, &w, 0, nullptr);
        t.ready = true;
        it = m_external.emplace(view, slot).first;
    }
    m_paper_texture = it->second + 1;
}

bool MovieRenderer::read_page(uint32_t page, std::vector<uint8_t>& rgba) {
    if (page >= k_pages || !m_pages[page].image || !m_page_size) {
        return false;
    }
    const VkDeviceSize bytes = VkDeviceSize(m_page_size) * m_page_size * 4;
    render::Buffer buf = m_device.create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, render::MemoryUsage::readback,
                                                render::Lifetime::transfer, "ui page read-back");
    const VkImage image = m_pages[page].image;
    // On the frames' queue, after every frame submitted so far: the page is in SHADER_READ_ONLY between frames.
    m_device.immediate([&](VkCommandBuffer cmd) {
        image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_page_size, m_page_size, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1, &region);
        image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    });
    vmaInvalidateAllocation(m_device.allocator(), buf.allocation, 0, VK_WHOLE_SIZE);
    rgba.resize(size_t(bytes));
    std::memcpy(rgba.data(), buf.mapped, size_t(bytes));
    if (m_color_format == VK_FORMAT_B8G8R8A8_UNORM || m_color_format == VK_FORMAT_B8G8R8A8_SRGB) {
        for (size_t i = 0; i < rgba.size(); i += 4) {
            std::swap(rgba[i], rgba[i + 2]);
        }
    }
    m_device.destroy_buffer(buf);
    return true;
}

void MovieRenderer::record_pages(VkCommandBuffer cmd, VkDeviceAddress draws) {
    // The game, per page: the paper copied into the target (ISCopy), ShowPageAtOffset(i)
    // (the menu did it when it drew the page's list), the Book movie displayed over it. Here the paper and the page
    // draw into the multisampled layer, which is resolved and composited into the page target.
    const VkPipelineStageFlags2 color_out = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags2 frag = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    const VkAccessFlags2 color_rw = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    const VkImageAspectFlags stencil_aspect =
        has_depth(m_stencil_format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    const VkPipelineStageFlags2 tests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags2 stencil_rw =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (!m_page_size || m_page_size > m_extent.width || m_page_size > m_extent.height) {
        return;
    }
    const VkExtent2D size{m_page_size, m_page_size};
    const VkRect2D area{{0, 0}, size};
    const VkViewport viewport{0, 0, float(size.width), float(size.height), 0.0f, 1.0f};
    for (uint32_t i = 0; i < k_pages; ++i) {
        if (m_page_ops[i].empty()) {
            continue;
        }
        image_barrier(cmd, m_msaa, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_out, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      color_out, color_rw);
        image_barrier(cmd, m_resolved, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, frag | color_out | VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, color_out, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        image_barrier(cmd, m_stencil, stencil_aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, tests,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, tests, stencil_rw);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = m_msaa_view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = m_resolved_view;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingAttachmentInfo st{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        st.imageView = m_stencil_view;
        st.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        st.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        st.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        st.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = area;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pStencilAttachment = &st;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &area);
        record_ops(cmd, m_page_ops[i], draws, size);
        vkCmdEndRendering(cmd);
        image_barrier(cmd, m_resolved, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_out, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, frag,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        // The previous frame's mesh pass may still sample the page: order after any fragment read.
        image_barrier(cmd, m_pages[i].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, frag, VK_ACCESS_2_NONE, color_out,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo out{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        out.imageView = m_pages[i].view;
        out.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        out.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        out.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        out.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo ro{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ro.renderArea = area;
        ro.layerCount = 1;
        ro.colorAttachmentCount = 1;
        ro.pColorAttachments = &out;
        vkCmdBeginRendering(cmd, &ro);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &area);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines[pipe_composite]);
        const std::array<VkDescriptorSet, 2> sets = {m_resolved_set, m_sampler_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 2, sets.data(), 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        image_barrier(cmd, m_pages[i].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_out, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      frag, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
}


bool MovieRenderer::pack(int w, int h, VkRect2D& out) {
    // Shelves, left to right and top to bottom; a 2-pixel gap keeps filter taps out of the neighbours.
    const int aw = int(m_atlas_extent.width), ah = int(m_atlas_extent.height);
    w += 2;
    h += 2;
    if (w > aw || h > ah) {
        return false;
    }
    if (m_pack_x + w > aw) {
        m_pack_x = 0;
        m_pack_y += m_pack_row;
        m_pack_row = 0;
    }
    if (m_pack_y + h > ah) {
        return false;
    }
    out = {{m_pack_x, m_pack_y}, {uint32_t(w - 2), uint32_t(h - 2)}};
    m_pack_x += w;
    m_pack_row = std::max(m_pack_row, h);
    return true;
}

// ---- movies and bitmaps --------------------------------------------------------------------------

uint32_t MovieRenderer::create_texture(uint32_t width, uint32_t height, std::vector<std::byte> rgba) {
    const uint32_t slot = uint32_t(m_textures.size());
    m_textures.emplace_back();
    Texture& t = m_textures.back();
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    m_device.set_shared(info);
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vk_check(m_device.create_image(info, alloc, render::Lifetime::asset, "ui movie bitmap", t.image, t.allocation));
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = t.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(m_device.device(), &view, nullptr, &t.view));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_texture_layout;
    vk_check(vkAllocateDescriptorSets(m_device.device(), &ai, &t.set));
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = t.set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(m_device.device(), 1, &w, 0, nullptr);

    // Through the uploader's staging ring on the transfer queue; drawn from the frame after it lands.
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    render::UploadRequest req;
    req.data = std::move(rgba);
    req.image = t.image;
    req.image_range = view.subresourceRange;
    req.image_regions.push_back(region);
    req.on_complete = [this, slot] { m_textures[slot].ready = true; };
    m_uploader.submit(std::move(req));
    return slot;
}

uint32_t MovieRenderer::bitmap_texture(const player::MovieDef& def, uint32_t character) {
    const osu::Movie& mv = def.data();
    if (character >= mv.characters.size() || mv.characters[character].kind != osu::CharKind::bitmap) {
        return ~0u;
    }
    MovieGpu& gm = m_movies[&def];
    auto it = gm.bitmaps.find(character);
    if (it == gm.bitmaps.end()) {
        const osu::Bitmap& bm = mv.bitmaps[mv.characters[character].index];
        const auto* src = mv.bitmap_data.data() + bm.offset;
        std::vector<std::byte> rgba(src, src + size_t(bm.width) * bm.height * 4);
        it = gm.bitmaps.emplace(character, create_texture(bm.width, bm.height, std::move(rgba))).first;
    }
    return it->second;
}

MovieRenderer::MovieGpu& MovieRenderer::movie_gpu(const player::MovieDef& def) {
    if (auto it = m_movies.find(&def); it != m_movies.end()) {
        return it->second;
    }
    const osu::Movie& m = def.data();
    MovieGpu& g = m_movies[&def];
    auto upload = [&](const void* data, size_t bytes, VkBufferUsageFlags usage, const char* name) {
        render::Buffer b;
        if (bytes == 0) {
            return b;
        }
        b = m_device.create_buffer(bytes, usage, render::MemoryUsage::dynamic, render::Lifetime::asset, name);
        std::memcpy(b.mapped, data, bytes);
        return b;
    };
    g.vertices = upload(m.vertices.data(), m.vertices.size() * sizeof(osu::Vertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        "ui movie vertices");
    g.indices = upload(m.indices.data(), m.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       "ui movie indices");
    g.stops = upload(m.gradient_stops.data(), m.gradient_stops.size() * sizeof(osu::GradientStop),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "ui movie gradients");
    m_stats.movies = uint32_t(m_movies.size());
    return g;
}

void MovieRenderer::prepare(const player::MovieDef& def) { movie_gpu(def); }

// ---- building a frame ----------------------------------------------------------------------------

void MovieRenderer::build(const render::OverlayTarget& target) {
    poll_engine_images();
    m_ops.clear();
    m_passes.clear();
    m_draw_data.clear();
    m_dyn_vertices.clear();
    m_dyn_draws.clear();
    m_pack_x = m_pack_y = m_pack_row = 0;
    uint32_t draw_count = 0;
    auto add_draw = [&](const GpuDraw& d) {
        const size_t at = m_draw_data.size();
        m_draw_data.resize(at + sizeof(GpuDraw));
        std::memcpy(m_draw_data.data() + at, &d, sizeof(GpuDraw));
        return draw_count++;
    };
    // Where ops go: the main pass or an open layer pass, each with its own stencil level (a layer starts with a
    // clear stencil). Layers draw at their place on screen; only their filters and composites see the atlas.
    struct Open {
        int pass;            // -1: main
        uint32_t level = 0;
        int in_place = 0;    // layers drawn in place inside this one (no atlas space)
        std::vector<player::ColorXform> cx{player::ColorXform{}};
    };
    std::vector<Open> open;
    // The screen's ops, or a page's (MovieLayer::page).
    std::vector<Op>* main_ops = &m_ops;
    auto ops = [&]() -> std::vector<Op>& { return open.back().pass < 0 ? *main_ops : m_passes[size_t(open.back().pass)].ops; };
    m_scene_marks.clear();
    for (auto& p : m_page_ops) {
        p.clear();
    }
    for (const MovieLayer& layer : m_layers) {
        if (layer.scene >= 0) {
            m_scene_marks.push_back({m_ops.size(), layer.scene});
            continue;
        }
        if (!layer.list) {
            continue;
        }
        main_ops = &m_ops;
        if (layer.page >= 0) {
            if (layer.page >= int(k_pages) || !m_pages[size_t(layer.page)].view) {
                continue;
            }
            main_ops = &m_page_ops[size_t(layer.page)];
            if (m_paper_texture && main_ops->empty()) {
                // The game copies the paper over the whole target before the movie draws on it.
                GpuDraw d{};
                const float size = float(m_page_size);
                put(d.xform, 1, 0, 0, 1);
                put(d.mul, 1, 1, 1, 1);
                put(d.fill, 1.0f / size, 0, 0, 1.0f / size);
                put(d.rect, 0, 0, size, size);
                d.xform_t[2] = 1;
                d.xform_t[3] = float(paint_bitmap);
                d.sampler = 2u;
                Op op{};
                op.pipeline = pipe_content;
                op.texture = m_paper_texture;
                op.draw = add_draw(d);
                main_ops->push_back(op);
            }
        }
        open.clear();
        open.push_back({-1});
        if (layer.background >> 24) {
            // GFx clears the movie's viewport (the whole target) with the background colour at its alpha.
            GpuDraw d{};
            put(d.xform, 1, 0, 0, 1);
            put(d.mul, 1, 1, 1, 1);
            put(d.fill, 1, 0, 0, 1);
            put(d.rect, 0, 0, float(target.extent.width), float(target.extent.height));
            unpack(layer.background, d.color);
            d.xform_t[2] = 1;
            Op op{};
            op.pipeline = pipe_content;
            op.draw = add_draw(d);
            main_ops->push_back(op);
        }
        const float stage_scale = std::sqrt(std::abs(layer.stage_to_pixels.a * layer.stage_to_pixels.d));
        enum class Mode { content, mask_add, mask_remove } mode = Mode::content;
        for (const DrawItem& item : layer.list->items) {
            Open& top = open.back();
            switch (item.kind) {
            case DrawItem::Kind::mask_begin: mode = Mode::mask_add; continue;
            case DrawItem::Kind::mask_content: mode = Mode::content; ++top.level; continue;
            case DrawItem::Kind::mask_end: mode = Mode::mask_remove; continue;
            case DrawItem::Kind::mask_done: mode = Mode::content; top.level = top.level ? top.level - 1 : 0; continue;
            case DrawItem::Kind::layer_begin: {
                // The layer's pixels: its content's bounds on the target, grown by what its filters reach.
                float x0, y0, x1, y1;
                layer.stage_to_pixels.apply(item.bounds[0], item.bounds[1], x0, y0);
                layer.stage_to_pixels.apply(item.bounds[2], item.bounds[3], x1, y1);
                float margin = 1;
                for (uint32_t i = 0; i < item.filter_count; ++i) {
                    const osu::Filter& f = item.filters[i];
                    const float blur = std::max(f.blur_x, f.blur_y) * stage_scale;
                    margin += (std::ceil(blur * 0.5f) + 1.0f) * float(std::max<uint8_t>(f.passes, 1)) +
                              std::abs(f.distance) * stage_scale;
                }
                const int px0 = std::max(0, int(std::floor(std::min(x0, x1) - margin)));
                const int py0 = std::max(0, int(std::floor(std::min(y0, y1) - margin)));
                const int px1 = std::min(int(target.extent.width), int(std::ceil(std::max(x0, x1) + margin)));
                const int py1 = std::min(int(target.extent.height), int(std::ceil(std::max(y0, y1) + margin)));
                VkRect2D region{};
                if (px1 <= px0 || py1 <= py0 || !pack(px1 - px0, py1 - py0, region)) {
                    if (!(px1 <= px0 || py1 <= py0) && !m_warned_atlas) {
                        log::warn("ui: the layer atlas is full; a layer draws without its filters ({}x{} at {},{}; packed to {},{})",
                                  px1 - px0, py1 - py0, px0, py0, m_pack_x, m_pack_y);
                        m_warned_atlas = true;
                    }
                    ++top.in_place;
                    top.cx.push_back(top.cx.back() * item.cxform);
                    continue;
                }
                LayerPass pass;
                pass.region = region;
                pass.origin[0] = px0;
                pass.origin[1] = py0;
                pass.filters = item.filters;
                pass.filter_count = item.filter_count;
                pass.filter_values = item.filter_values;
                pass.scale = stage_scale;
                pass.blend = item.blend;
                pass.cx = top.cx.back() * item.cxform;
                m_passes.push_back(std::move(pass));
                open.push_back(Open{int(m_passes.size()) - 1});
                continue;
            }
            case DrawItem::Kind::layer_end: {
                if (top.in_place > 0) {
                    --top.in_place;
                    top.cx.pop_back();
                    continue;
                }
                if (open.size() < 2) {
                    continue;
                }
                const int index = top.pass;
                open.pop_back();
                const LayerPass& pass = m_passes[size_t(index)];
                Op op{};
                switch (pass.blend) {
                case osu::Blend::add: op.pipeline = pipe_layer_add; break;
                case osu::Blend::multiply: op.pipeline = pipe_layer_multiply; break;
                case osu::Blend::screen: op.pipeline = pipe_layer_screen; break;
                case osu::Blend::lighten: op.pipeline = pipe_layer_lighten; break;
                case osu::Blend::darken: op.pipeline = pipe_layer_darken; break;
                case osu::Blend::subtract: op.pipeline = pipe_layer_subtract; break;
                default: op.pipeline = pipe_layer_normal; break;
                }
                op.stencil = open.back().level;
                op.layer = index;
                op.rect[0] = float(pass.origin[0]);
                op.rect[1] = float(pass.origin[1]);
                op.rect[2] = float(pass.origin[0] + int(pass.region.extent.width));
                op.rect[3] = float(pass.origin[1] + int(pass.region.extent.height));
                ops().push_back(op);
                continue;
            }
            default: break;
            }
            GpuDraw d{};
            const Matrix2D m = layer.stage_to_pixels * item.transform;
            put(d.xform, m.a, m.b, m.c, m.d);
            const player::ColorXform cx = top.cx.back() * item.cxform;
            put(d.mul, cx.mul[0], cx.mul[1], cx.mul[2], cx.mul[3]);
            put(d.add, cx.add[0], cx.add[1], cx.add[2], cx.add[3]);
            put(d.fill, 1, 0, 0, 1);
            uint32_t paint = paint_solid, vertex_mode = 0, texture = 0;
            Op op{};
            op.pipeline = mode == Mode::content ? pipe_content : mode == Mode::mask_add ? pipe_mask_add : pipe_mask_remove;
            op.stencil = top.level;
            uint32_t dyn_first = ~0u;
            if (item.kind == DrawItem::Kind::rect) {
                vertex_mode = 1;
                put(d.rect, item.rect[0], item.rect[1], item.rect[2], item.rect[3]);
                unpack(item.color, d.color);
            } else if (item.kind == DrawItem::Kind::bitmap || item.kind == DrawItem::Kind::image) {
                // An inline text image or an engine image (img://): the texture over the rectangle, UV 0..1 across
                // it, smoothed and clamped.
                const float w = item.rect[2] - item.rect[0], h = item.rect[3] - item.rect[1];
                const bool engine = item.kind == DrawItem::Kind::image;
                if ((!engine && !item.movie) || w <= 0 || h <= 0) {
                    continue;
                }
                const uint32_t texture_slot =
                    engine ? engine_image_texture(item.index) : bitmap_texture(*item.movie, item.index);
                if (texture_slot == ~0u || !m_textures[texture_slot].ready) {
                    continue; // still uploading: appears on a later frame
                }
                vertex_mode = 1;
                put(d.rect, item.rect[0], item.rect[1], item.rect[2], item.rect[3]);
                if (mode == Mode::content) {
                    put(d.fill, 1.0f / w, 0, 0, 1.0f / h);
                    d.fill_t[0] = -item.rect[0] / w;
                    d.fill_t[1] = -item.rect[1] / h;
                    paint = paint_bitmap;
                    texture = texture_slot + 1;
                    d.sampler = 2u;
                    d.flags |= engine ? engine_image_flags(item.index) : 0u;
                }
            } else if (item.kind == DrawItem::Kind::triangles) {
                // Its vertices go into this frame's vertex buffer; the address is filled in once it exists.
                if (item.count == 0 || item.first + item.count > layer.list->vertices.size()) {
                    continue;
                }
                dyn_first = uint32_t(m_dyn_vertices.size());
                m_dyn_vertices.insert(m_dyn_vertices.end(), layer.list->vertices.begin() + item.first,
                                      layer.list->vertices.begin() + item.first + item.count);
                op.vertex_count = item.count;
                unpack(item.color, d.color);
            } else {
                if (!item.movie) {
                    continue;
                }
                const osu::Movie& mv = item.movie->data();
                const MovieGpu& g = movie_gpu(*item.movie);
                d.vertices = g.vertices.address;
                op.movie = &g;
                if (item.kind == DrawItem::Kind::glyph) {
                    const osu::Glyph& gl = mv.glyphs[item.index];
                    if (gl.index_count == 0) {
                        continue;
                    }
                    op.first_index = gl.first_index;
                    op.index_count = gl.index_count;
                    op.vertex_offset = gl.first_vertex;
                    unpack(item.color, d.color);
                } else {
                    const osu::Mesh& mesh = mv.meshes[item.index];
                    if (mesh.index_count == 0) {
                        continue;
                    }
                    op.first_index = mesh.first_index;
                    op.index_count = mesh.index_count;
                    op.vertex_offset = mesh.first_vertex;
                    const osu::Matrix& f = mesh.fill_matrix;
                    put(d.fill, f.a, f.b, f.c, f.d);
                    d.fill_t[0] = f.tx;
                    d.fill_t[1] = f.ty;
                    if (mode != Mode::content) {
                        paint = paint_solid; // mask geometry: only coverage counts
                    } else if (mesh.fill == osu::Fill::solid) {
                        unpack(mesh.color, d.color);
                    } else if (mesh.fill == osu::Fill::bitmap) {
                        const uint32_t ch = mesh.bitmap;
                        if (ch == osu::k_none || ch >= mv.characters.size() ||
                            mv.characters[ch].kind != osu::CharKind::bitmap) {
                            continue;
                        }
                        const uint32_t texture_slot = bitmap_texture(*item.movie, ch);
                        if (!m_textures[texture_slot].ready) {
                            continue; // still uploading: appears on a later frame
                        }
                        paint = paint_bitmap;
                        texture = texture_slot + 1;
                        d.sampler = ((mesh.flags & osu::mesh_bitmap_repeat) ? 1u : 0u) |
                                    ((mesh.flags & osu::mesh_bitmap_smooth) ? 2u : 0u);
                    } else if (mesh.gradient != osu::k_none) {
                        const osu::Gradient& gr = mv.gradients[mesh.gradient];
                        paint = mesh.fill == osu::Fill::linear ? paint_linear
                                : mesh.fill == osu::Fill::radial ? paint_radial
                                                                 : paint_focal;
                        d.stops = g.stops.address + uint64_t(gr.first_stop) * sizeof(osu::GradientStop);
                        d.stop_count = gr.stop_count;
                        d.fill_t[2] = float(uint32_t(gr.spread));
                        d.fill_t[3] = gr.focal;
                        d.flags |= gr.linear_rgb ? 2u : 0u;
                    }
                }
            }
            if (item.scale9) {
                d.flags |= 1u;
                put(d.inner, item.inner.a, item.inner.b, item.inner.c, item.inner.d);
                put(d.inner_t, item.inner.tx, item.inner.ty, item.s9_scale[0], item.s9_scale[1]);
                std::copy(item.s9_bounds, item.s9_bounds + 4, d.s9_bounds);
                std::copy(item.s9_grid, item.s9_grid + 4, d.s9_grid);
            }
            d.xform_t[0] = m.tx;
            d.xform_t[1] = m.ty;
            d.xform_t[2] = float(vertex_mode);
            d.xform_t[3] = float(paint);
            op.texture = texture;
            op.draw = add_draw(d);
            if (dyn_first != ~0u) {
                m_dyn_draws.push_back({op.draw, dyn_first});
            }
            ops().push_back(op);
        }
        // Unbalanced layers (a broken list) still finish.
        while (open.size() > 1) {
            open.pop_back();
        }
    }
}

// ---- recording -----------------------------------------------------------------------------------

void MovieRenderer::record_ops(VkCommandBuffer cmd, std::span<const Op> list, VkDeviceAddress draws,
                               VkExtent2D extent) {
    const std::array<VkDescriptorSet, 2> base_sets = {m_blank.set, m_sampler_set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 2, base_sets.data(), 0, nullptr);
    GpuPush push{};
    push.draws = draws;
    push.to_clip[0] = 2.0f / float(extent.width);
    push.to_clip[1] = 2.0f / float(extent.height);
    uint32_t bound_pipe = UINT32_MAX, bound_stencil = UINT32_MAX, bound_tex = 0;
    const MovieGpu* bound_movie = nullptr;
    bool sets_movie = true; // which pipeline layout's sets are bound
    for (const Op& op : list) {
        if (op.pipeline != bound_pipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines[op.pipeline]);
            bound_pipe = op.pipeline;
        }
        if (op.stencil != bound_stencil) {
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, op.stencil);
            bound_stencil = op.stencil;
        }
        if (op.layer >= 0) {
            // A finished layer: its atlas image at its region, with its colour transform.
            const LayerPass& pass = m_passes[size_t(op.layer)];
            const std::array<VkDescriptorSet, 3> sets = {m_atlas_set[pass.result], m_blank.set, m_sampler_set};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_filter_layout, 0, 3, sets.data(), 0,
                                    nullptr);
            sets_movie = false;
            FilterPush fp{};
            std::copy(op.rect, op.rect + 4, fp.rect);
            fp.to_clip[0] = push.to_clip[0];
            fp.to_clip[1] = push.to_clip[1];
            fp.mode = mode_composite;
            fp.offset[0] = float(pass.region.offset.x) - op.rect[0];
            fp.offset[1] = float(pass.region.offset.y) - op.rect[1];
            std::copy(pass.cx.mul, pass.cx.mul + 4, fp.mul);
            std::copy(pass.cx.add, pass.cx.add + 4, fp.add);
            vkCmdPushConstants(cmd, m_filter_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(fp), &fp);
            vkCmdDraw(cmd, 6, 1, 0, 0);
            continue;
        }
        if (!sets_movie) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 2, base_sets.data(), 0, nullptr);
            sets_movie = true;
            bound_tex = 0;
        }
        if (op.texture != bound_tex) {
            const VkDescriptorSet set = op.texture ? m_textures[op.texture - 1].set : m_blank.set;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &set, 0, nullptr);
            bound_tex = op.texture;
        }
        push.draw = op.draw;
        vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);
        if (!op.movie) {
            vkCmdDraw(cmd, op.vertex_count ? op.vertex_count : 6, 1, 0, 0);
            continue;
        }
        if (op.movie != bound_movie) {
            vkCmdBindIndexBuffer(cmd, op.movie->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            bound_movie = op.movie;
        }
        vkCmdDrawIndexed(cmd, op.index_count, 1, op.first_index, int32_t(op.vertex_offset), 0);
    }
}

namespace {

void pass_barrier(VkCommandBuffer cmd) {
    // Layer passes write and read the shared targets and the atlas images in turn, drawing, copying and sampling.
    constexpr VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
                                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = stages;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = stages;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                      VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

void MovieRenderer::filter_pass(VkCommandBuffer cmd, uint32_t out, const VkRect2D& region, const FilterPush& push,
                                uint32_t in_a, uint32_t in_b) {
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_atlas_view[out];
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = region;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport viewport{0, 0, float(m_atlas_extent.width), float(m_atlas_extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &region);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines[pipe_filter]);
    const std::array<VkDescriptorSet, 3> sets = {m_atlas_set[in_a], m_atlas_set[in_b], m_sampler_set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_filter_layout, 0, 3, sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, m_filter_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void MovieRenderer::draw_layers(VkCommandBuffer cmd, std::span<LayerPass* const> batch, VkDeviceAddress draws) {
    // The content, multisampled with a clear stencil and resolved, then copied into atlas image 0 at each region. The main
    // pass's targets serve as scratch: it clears them after the layers.
    int x0 = INT_MAX, y0 = INT_MAX, x1 = 0, y1 = 0;
    for (const LayerPass* pass : batch) {
        x0 = std::min(x0, pass->scratch[0]);
        y0 = std::min(y0, pass->scratch[1]);
        x1 = std::max(x1, pass->scratch[0] + int(pass->region.extent.width));
        y1 = std::max(y1, pass->scratch[1] + int(pass->region.extent.height));
    }
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_msaa_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    color.resolveImageView = m_resolved_view;
    color.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderingAttachmentInfo st{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    st.imageView = m_stencil_view;
    st.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    st.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    st.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    st.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pStencilAttachment = &st;
    vkCmdBeginRendering(cmd, &ri);
    m_layer_copies.clear();
    for (const LayerPass* pass : batch) {
        // A whole number of pixels away from its place on screen: the same samples cover the same shapes.
        const VkRect2D area{{pass->scratch[0], pass->scratch[1]}, pass->region.extent};
        const VkViewport viewport{float(area.offset.x - pass->origin[0]), float(area.offset.y - pass->origin[1]),
                                  float(m_extent.width), float(m_extent.height), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &area);
        record_ops(cmd, pass->ops, draws, m_extent);
        VkImageCopy2& copy = m_layer_copies.emplace_back(VkImageCopy2{VK_STRUCTURE_TYPE_IMAGE_COPY_2});
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.srcOffset = {area.offset.x, area.offset.y, 0};
        copy.dstSubresource = copy.srcSubresource;
        copy.dstOffset = {pass->region.offset.x, pass->region.offset.y, 0};
        copy.extent = {area.extent.width, area.extent.height, 1};
    }
    vkCmdEndRendering(cmd);
    pass_barrier(cmd);
    VkCopyImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};
    ci.srcImage = m_resolved;
    ci.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ci.dstImage = m_atlas[0];
    ci.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ci.regionCount = uint32_t(m_layer_copies.size());
    ci.pRegions = m_layer_copies.data();
    vkCmdCopyImage2(cmd, &ci);
    pass_barrier(cmd);
}

void MovieRenderer::record_layers(VkCommandBuffer cmd, VkDeviceAddress draws) {
    // Layers in waves, inner ones before those that hold them. A wave's layers go through each stage together, with one
    // barrier a stage instead of a few each: "X DISCOVERED" is a layer a letter, and paid 0.3 ms in barriers alone.
    std::vector<uint32_t> depth(m_passes.size(), 0);
    for (size_t i = m_passes.size(); i-- > 0;) { // a layer opens before those inside it
        for (const Op& op : m_passes[i].ops) {
            if (op.layer >= 0) {
                depth[i] = std::max(depth[i], depth[size_t(op.layer)] + 1);
            }
        }
    }
    const uint32_t waves = m_passes.empty() ? 0 : *std::max_element(depth.begin(), depth.end()) + 1;
    std::vector<LayerPass*> wave, batch;
    for (uint32_t w = 0; w < waves; ++w) {
        wave.clear();
        for (size_t i = 0; i < m_passes.size(); ++i) {
            if (depth[i] == w) {
                wave.push_back(&m_passes[i]);
            }
        }
        // They share the scratch targets side by side, laid out as the atlas packed them, as much as they hold at once.
        // A layer that holds others stays at its place on screen, where its composites expect to be.
        batch.clear();
        int top = 0, bottom = 0;
        auto flush = [&] {
            for (LayerPass* pass : batch) {
                pass->scratch[0] = pass->region.offset.x;
                pass->scratch[1] = pass->region.offset.y - top;
            }
            if (!batch.empty()) {
                draw_layers(cmd, batch, draws);
                batch.clear();
            }
        };
        for (LayerPass* pass : wave) {
            if (w > 0) {
                pass->scratch[0] = pass->origin[0];
                pass->scratch[1] = pass->origin[1];
                draw_layers(cmd, std::span<LayerPass* const>(&pass, 1), draws);
                continue;
            }
            const int y = pass->region.offset.y, end = y + int(pass->region.extent.height);
            if (!batch.empty() && std::max(bottom, end) - std::min(top, y) > int(m_extent.height)) {
                flush();
            }
            top = batch.empty() ? y : std::min(top, y);
            bottom = batch.empty() ? end : std::max(bottom, end);
            batch.push_back(pass);
        }
        flush();
        std::vector<std::vector<FilterStep>> steps(wave.size());
        size_t stages = 0;
        for (size_t i = 0; i < wave.size(); ++i) {
            plan_filters(*wave[i], steps[i]);
            stages = std::max(stages, steps[i].size());
        }
        for (size_t stage = 0; stage < stages; ++stage) {
            for (size_t i = 0; i < wave.size(); ++i) {
                if (stage < steps[i].size()) {
                    const FilterStep& s = steps[i][stage];
                    filter_pass(cmd, s.out, wave[i]->region, s.push, s.a, s.b);
                }
            }
            pass_barrier(cmd);
        }
    }
}

void MovieRenderer::plan_filters(LayerPass& pass, std::vector<FilterStep>& steps) {
    // Its filters, in order, each reading the last result; image 0 holds the content, 1 and 2 ping-pong.
    uint32_t cur = 0;
    auto others = [](uint32_t c, uint32_t& a, uint32_t& b) {
        a = (c + 1) % 3;
        b = (c + 2) % 3;
    };
    FilterPush base{};
    base.rect[0] = float(pass.region.offset.x);
    base.rect[1] = float(pass.region.offset.y);
    base.rect[2] = float(pass.region.offset.x + int(pass.region.extent.width));
    base.rect[3] = float(pass.region.offset.y + int(pass.region.extent.height));
    base.to_clip[0] = 2.0f / float(m_atlas_extent.width);
    base.to_clip[1] = 2.0f / float(m_atlas_extent.height);
    for (uint32_t fi = 0; fi < pass.filter_count; ++fi) {
        const osu::Filter& f = pass.filters[fi];
        const int passes = std::max<int>(f.passes, 1);
        uint32_t a, b;
        others(cur, a, b);
        switch (f.kind) {
        case osu::FilterKind::blur: {
            uint32_t src = cur;
            for (int i = 0; i < passes; ++i) {
                FilterPush h = base;
                h.mode = mode_blur_h;
                box_kernel(f.blur_x * pass.scale, h);
                steps.push_back({a, src, src, h});
                FilterPush v = base;
                v.mode = mode_blur_v;
                box_kernel(f.blur_y * pass.scale, v);
                steps.push_back({b, a, a, v});
                src = b;
            }
            cur = b;
            break;
        }
        case osu::FilterKind::drop_shadow:
        case osu::FilterKind::glow: {
            // The object's alpha blurred (a -> b per pass), then the shadow combined with the object into `a`.
            uint32_t src = cur;
            for (int i = 0; i < passes; ++i) {
                FilterPush h = base;
                h.mode = mode_blur_alpha_h;
                box_kernel(f.blur_x * pass.scale, h);
                steps.push_back({a, src, src, h});
                FilterPush v = base;
                v.mode = mode_blur_alpha_v;
                box_kernel(f.blur_y * pass.scale, v);
                steps.push_back({b, a, a, v});
                src = b;
            }
            FilterPush c = base;
            c.mode = mode_shadow;
            c.flags = ((f.flags & osu::filter_inner) ? 1 : 0) | ((f.flags & osu::filter_knockout) ? 2 : 0) |
                      ((f.flags & osu::filter_composite) ? 4 : 0);
            const float distance = f.kind == osu::FilterKind::glow ? 0.0f : f.distance * pass.scale;
            c.shadow[0] = std::cos(f.angle) * distance;
            c.shadow[1] = std::sin(f.angle) * distance;
            unpack(f.color, c.color);
            c.strength = f.strength;
            steps.push_back({a, cur, b, c});
            cur = a;
            break;
        }
        case osu::FilterKind::color_matrix: {
            FilterPush c = base;
            c.mode = mode_color_matrix;
            const float* v = pass.filter_values + f.first_value;
            // Flash's 4x5 matrix rows (r, g, b, a) with offsets in 0..255.
            for (int r = 0; r < 4; ++r) {
                for (int k = 0; k < 4; ++k) {
                    c.matrix[r][k] = f.value_count >= 20 ? v[r * 5 + k] : (r == k ? 1.0f : 0.0f);
                }
                c.matrix[4][r] = f.value_count >= 20 ? v[r * 5 + 4] / 255.0f : 0.0f;
            }
            steps.push_back({a, cur, cur, c});
            cur = a;
            break;
        }
        default:
            if (!m_warned_filter) {
                log::warn("ui: a {} filter is drawn without its effect", int(f.kind));
                m_warned_filter = true;
            }
            break;
        }
    }
    pass.result = cur;
}

void MovieRenderer::record_main(VkCommandBuffer cmd, const render::OverlayTarget& target, size_t first, size_t end,
                                bool clear, VkDeviceAddress draws) {
    const VkImageAspectFlags stencil_aspect =
        has_depth(m_stencil_format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    const VkPipelineStageFlags2 tests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags2 stencil_rw =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkAccessFlags2 color_rw = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    const VkPipelineStageFlags2 color_out = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags2 frag = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    const VkPipelineStageFlags2 copy_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    // After layer passes these are discarded too; the source masks order the main pass after their last use.
    image_barrier(cmd, m_msaa, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_out, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  color_out, color_rw);
    image_barrier(cmd, m_resolved, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, frag | color_out | copy_stage,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, color_out, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    image_barrier(cmd, m_stencil, stencil_aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  tests, stencil_rw);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_msaa_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    color.resolveImageView = m_resolved_view;
    color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderingAttachmentInfo st{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    st.imageView = m_stencil_view;
    st.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    st.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    st.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    st.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, target.extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pStencilAttachment = &st;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport viewport{0, 0, float(target.extent.width), float(target.extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, target.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    record_ops(cmd, std::span<const Op>(m_ops).subspan(first, end - first), draws, target.extent);
    vkCmdEndRendering(cmd);

    image_barrier(cmd, m_resolved, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_out, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, frag,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // Over the scene (or black when there is none).
    VkRenderingAttachmentInfo out{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    out.imageView = target.view;
    out.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    out.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    out.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    out.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo ro{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ro.renderArea = {{0, 0}, target.extent};
    ro.layerCount = 1;
    ro.colorAttachmentCount = 1;
    ro.pColorAttachments = &out;
    vkCmdBeginRendering(cmd, &ro);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines[pipe_composite]);
    const std::array<VkDescriptorSet, 2> composite_sets = {m_resolved_set, m_sampler_set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 2, composite_sets.data(), 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

}

void MovieRenderer::record(VkCommandBuffer cmd, const render::OverlayTarget& target) {
    const uint32_t slot = target.slot % render::k_frames_in_flight;
    FrameData& frame = m_frames[slot];
    if (frame.queries_written) {
        uint64_t ts[2];
        if (vkGetQueryPoolResults(m_device.device(), frame.queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            m_stats.gpu_ms = float(double(ts[1] - ts[0]) * m_timestamp_period_ns / 1e6);
        }
        frame.queries_written = false;
    }
    vkCmdResetQueryPool(cmd, frame.queries, 0, 2);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, frame.queries, 0);

    ensure_targets(target.extent);
    build(target);
    // This slot's previous frame has completed (the renderer waited on it), so its buffer can be rewritten.
    if (m_draw_data.size() > frame.draws.size) {
        if (frame.draws) {
            m_device.defer_destroy(frame.draws);
        }
        frame.draws = m_device.create_buffer(std::max<VkDeviceSize>(m_draw_data.size() * 2, 64 * 1024),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, render::MemoryUsage::dynamic,
                                             render::Lifetime::frame, "ui movie draws");
    }
    if (!m_dyn_vertices.empty()) {
        const VkDeviceSize bytes = m_dyn_vertices.size() * sizeof(osu::Vertex);
        if (bytes > frame.vertices.size) {
            if (frame.vertices) {
                m_device.defer_destroy(frame.vertices);
            }
            frame.vertices = m_device.create_buffer(std::max<VkDeviceSize>(bytes * 2, 64 * 1024),
                                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, render::MemoryUsage::dynamic,
                                                    render::Lifetime::frame, "ui movie drawn vertices");
        }
        std::memcpy(frame.vertices.mapped, m_dyn_vertices.data(), bytes);
        for (const auto& [draw, first] : m_dyn_draws) {
            auto* d = reinterpret_cast<GpuDraw*>(m_draw_data.data() + size_t(draw) * sizeof(GpuDraw));
            d->vertices = frame.vertices.address + VkDeviceAddress(first) * sizeof(osu::Vertex);
        }
    }
    if (!m_draw_data.empty()) {
        std::memcpy(frame.draws.mapped, m_draw_data.data(), m_draw_data.size());
    }

    const VkImageAspectFlags stencil_aspect =
        has_depth(m_stencil_format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    const VkPipelineStageFlags2 tests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags2 stencil_rw =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkAccessFlags2 color_rw = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    const VkPipelineStageFlags2 color_out = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags2 frag = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

    // Layer passes first, inner layers before the layers that hold them.
    const VkPipelineStageFlags2 copy_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    if (!m_passes.empty()) {
        image_barrier(cmd, m_msaa, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_out, VK_ACCESS_2_NONE, color_out, color_rw);
        image_barrier(cmd, m_stencil, stencil_aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, tests, VK_ACCESS_2_NONE, tests, stencil_rw);
        image_barrier(cmd, m_resolved, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      frag | color_out, VK_ACCESS_2_NONE, color_out | copy_stage,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        for (VkImage image : m_atlas) {
            image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          color_out | frag, VK_ACCESS_2_NONE, color_out | frag | copy_stage,
                          color_rw | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        record_layers(cmd, frame.draws.address);
    }

    // The pages first: the 3D book drawn among the menus samples this frame's pages.
    record_pages(cmd, frame.draws.address);
    // The menus bottom to top, each UI 3D scene at its place: the ops below a scene are composited, the scene is
    // drawn over them, and the ops above it go into a fresh layer composited over that.
    bool drawn = !target.clear;
    size_t at = 0;
    for (const SceneMark& mark : m_scene_marks) {
        if (mark.at > at) {
            record_main(cmd, target, at, mark.at, !drawn, frame.draws.address);
            drawn = true;
        }
        at = mark.at;
        if (target.scenes && mark.scene >= 0 && uint32_t(mark.scene) < target.scene_count) {
            target.scenes->record_scene(cmd, uint32_t(mark.scene), !drawn);
            drawn = true;
        }
    }
    if (at < m_ops.size() || !drawn) {
        record_main(cmd, target, at, m_ops.size(), !drawn, frame.draws.address);
    }

    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, frame.queries, 1);
    frame.queries_written = true;
    size_t draws = m_ops.size();
    for (const auto& p : m_passes) {
        draws += p.ops.size();
    }
    m_stats.draws = uint32_t(draws);
    m_stats.layers = uint32_t(m_passes.size());
    uint32_t pending = 0;
    for (const auto& t : m_textures) {
        pending += !t.ready;
    }
    m_stats.bitmaps_pending = pending;
}

} // namespace spud::ui
