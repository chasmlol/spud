// Second views of a space drawn into offscreen targets the overlay samples in the same frame
// (render/view.hpp, OffscreenView): the map's local view. Kept out of renderer.cpp
// so the main frame changes in three lines only. The pass is the scene pass's opaque, terrain, alpha-tested,
// water and blended draws under the view's own camera and draw list, with main's shading; no sky.

#include "render/renderer.hpp"

#include "core/trace.hpp"

#include <cstring>

namespace spud::render {

namespace {

constexpr VkDeviceSize k_cmd_stride = sizeof(VkDrawIndexedIndirectCommand);

void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
             VkImageLayout to, VkPipelineStageFlags2 src, VkAccessFlags2 src_access,
             VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
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

} // namespace

uint32_t Renderer::create_offscreen_target(VkExtent2D extent, const char* name) {
    OffscreenTarget t;
    t.extent = extent;
    auto make = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, Lifetime lifetime,
                    const char* debug, VkImage& image, VmaAllocation& alloc, VkImageView& view) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        vk_check(m_device->create_image(info, ai, lifetime, debug, image, alloc));
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = image;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = format;
        v.subresourceRange = {aspect, 0, 1, 0, 1};
        vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &view));
    };
    // The colour is sampled after the frame that drew it, until the view draws again (a cache); the depth is
    // cleared and rewritten by every pass.
    make(m_color_format,
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         VK_IMAGE_ASPECT_COLOR_BIT, Lifetime::cache, name, t.color, t.color_alloc, t.color_view);
    make(k_depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
         VK_IMAGE_ASPECT_DEPTH_BIT, Lifetime::frame, "offscreen view depth", t.depth, t.depth_alloc, t.depth_view);
    m_offscreen.push_back(t);
    return uint32_t(m_offscreen.size() - 1);
}

void Renderer::expose_offscreen_depth(uint32_t target, uint32_t slot) {
    m_bindless->write(slot, m_offscreen[target].depth_view);
}

void Renderer::expose_offscreen_color(uint32_t target, uint32_t slot) {
    m_bindless->write(slot, m_offscreen[target].color_view);
}

VkImageView Renderer::offscreen_target_view(uint32_t target) const {
    return target < m_offscreen.size() ? m_offscreen[target].color_view : VK_NULL_HANDLE;
}

VkExtent2D Renderer::offscreen_target_extent(uint32_t target) const {
    return target < m_offscreen.size() ? m_offscreen[target].extent : VkExtent2D{};
}

void Renderer::init_offscreen_targets(VkCommandBuffer cmd) {
    for (auto& t : m_offscreen) {
        if (t.initialised) {
            continue;
        }
        // Black until a view first draws it, so a movie that shows it earlier samples something defined.
        barrier(cmd, t.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, t.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        barrier(cmd, t.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        t.initialised = true;
    }
}

VkDeviceSize Renderer::offscreen_arena_need(std::span<const OffscreenView> views) const {
    VkDeviceSize need = 0;
    for (const OffscreenView& ov : views) {
        size_t groups = 0;
        if (ov.space.draws && ov.settings.draw_meshes) {
            for (const auto& item : ov.space.draws->items()) {
                groups += m_meshes->groups(item.mesh, item.variant).size();
            }
        }
        // The frame constants, the instances and commands, and every land cell of the space at most.
        const size_t cells = ov.space.terrain ? ov.space.terrain->stats().land_cells : 0;
        need += 4096 + groups * (sizeof(GpuDrawInstance) + k_cmd_stride) + cells * 2 * sizeof(GpuFarNode) +
                ov.space.lights.size() * sizeof(GpuPointLight) + ov.local_map.size() + 16;
        if (ov.space.water) {
            need += ov.space.water->materials.size_bytes() + ov.space.water->surfaces.size_bytes() +
                    ov.space.water->placed.size_bytes() + sizeof(GpuWater) + 64;
        }
    }
    return need;
}

void Renderer::record_offscreen(VkCommandBuffer cmd, Frame& fr, const OffscreenView& ov, const FrameInput& input) {
    if (!ov.view || ov.target >= m_offscreen.size()) {
        return;
    }
    SPUD_TRACE("record offscreen");
    const OffscreenTarget& target = m_offscreen[ov.target];
    const SpaceView& space = ov.space;
    const View& view = *ov.view;
    const RenderSettings& rs = ov.settings;
    // The frame's statistics stay the main view's.
    const FrameStats main_stats = m_stats;

    m_offscreen_terrain.clear();
    if (space.terrain && rs.draw_terrain) {
        space.terrain->collect(view.frustum, view.position, rs.terrain, m_offscreen_terrain);
    }
    const VkDeviceSize frame_offset =
        write_frame_data(fr, view, space, rs, input.time, input.previous_time, target.extent, 1);
    const VkDeviceAddress lights_address = write_point_lights(fr, view, space, rs);

    VkDeviceAddress draw_instances = 0;
    if (space.draws && rs.draw_meshes) {
        build_mesh_draws(fr, space, m_runs_opaque, m_runs_alpha, m_runs_blend, draw_instances);
    } else {
        m_runs_opaque.clear();
        m_runs_alpha.clear();
        m_runs_blend.clear();
    }
    const TerrainFrame land = write_terrain(fr, m_offscreen_terrain);

    // The previous use (the overlay sampled it, last frame or earlier in flight) before it is drawn again.
    barrier(cmd, target.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    barrier(cmd, target.depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.color_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{ov.clear.r, ov.clear.g, ov.clear.b, ov.clear.a}};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = target.depth_view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = ov.keep_depth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {0.0f, 0}; // reverse-Z
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, target.extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0, 0, float(target.extent.width), float(target.extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, target.extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    VkDescriptorSet set = m_bindless->set(m_slot);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->layout(), 0, 1, &set, 0,
                            nullptr);
    PushConstants pc{};
    pc.frame = fr.arena.address + frame_offset;
    pc.draw_instances = draw_instances;
    pc.instance_table = space.instance_table;
    pc.materials = m_meshes->materials_address();
    pc.terrain_quads = space.terrain ? space.terrain->quads_address() : 0;
    pc.land_textures = space.terrain ? space.terrain->land_textures_address() : 0;
    pc.terrain_vertices = space.terrain ? space.terrain->vertex_address() : 0;
    pc.lights = lights_address;
    pc.material_anims = m_material_anims_address;
    const bool local_map = !ov.local_map.empty();
    pc.mode = local_map ? 0x4C4Du : 0u; // unlit objects (mesh.frag)
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
    if (!m_runs_opaque.empty()) {
        bind_mesh_buffers();
        record_runs(cmd, fr, m_runs_opaque, wire, flip, m_ui_pipelines.get());
    }
    if (space.terrain) {
        record_terrain(cmd, *space.terrain, land, pc, *m_ui_pipelines, wire, flip);
    }
    if (!m_runs_alpha.empty()) {
        bind_mesh_buffers();
        record_runs(cmd, fr, m_runs_alpha, wire, flip, m_ui_pipelines.get());
    }
    if (space.water && rs.draw_water) {
        // The local map's water is the LOD technique.
        DrawState state;
        state.two_sided = true;
        apply_state(cmd, state, flip);
        pc.water = write_water_data(fr, view, space, rs, true);
        draw_water_surfaces(cmd, pc, program_local_map_water, program_local_map_water, program_local_map_water,
                            *space.water, wire, m_ui_pipelines.get());
    }
    // The local map's pass has no blended draws (the capture: one blend state, off).
    if (!m_runs_blend.empty() && !local_map) {
        bind_mesh_buffers();
        record_runs(cmd, fr, m_runs_blend, wire, flip, m_ui_pipelines.get());
    }
    vkCmdEndRendering(cmd);
    barrier(cmd, target.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    if (ov.keep_depth) {
        barrier(cmd, target.depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    if (!ov.local_map.empty() && ov.local_map_target < m_offscreen.size()) {
        const OffscreenTarget& out = m_offscreen[ov.local_map_target];
        const VkDeviceSize offset = arena_alloc(fr, ov.local_map.size(), 16);
        std::memcpy(fr.arena.as<std::byte>() + offset, ov.local_map.data(), ov.local_map.size());
        pc.post = fr.arena.address + offset;
        barrier(cmd, out.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        fullscreen(cmd, program_local_map, out.color_view, out.extent, pc, 0, Bindless::k_local_map_slot);
        barrier(cmd, out.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    m_stats = main_stats;
}

} // namespace spud::render
