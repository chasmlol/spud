// The renderer's hook for the game UI (render/overlay.hpp): one pass over the finished scene
// target, after the pre-UI capture and before the output. Kept out of renderer.cpp so the UI
// touches the core frame in one place only.

#include "render/renderer.hpp"

namespace spud::render {

namespace {

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src,
             VkAccessFlags2 src_access, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

// The UI 3D scenes, drawn when the overlay reaches them: over the colour target as it stands, with the depth target
// cleared first, as the game clears depth and draws into the frame.
// Only the mesh path: no terrain, sky or water.
class Renderer::UiSceneRecorder final : public SceneRecorder {
public:
    UiSceneRecorder(Renderer& r, Frame& frame, const RenderSettings& rs) : m_r(r), m_frame(frame), m_rs(rs) {}

    void record_scene(VkCommandBuffer cmd, uint32_t index, bool clear) override {
        if (index >= m_r.m_ui_runs.size()) {
            return;
        }
        const UiSceneRuns& runs = m_r.m_ui_runs[index];
        const VkExtent2D extent = m_r.m_target_extent;
        // The overlay's last pass wrote the colour target; an earlier scene or the world wrote the depth.
        barrier(cmd, m_r.m_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
        VkImageMemoryBarrier2 db{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        db.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        db.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        db.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        db.dstAccessMask =
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // its contents are cleared
        db.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        db.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.image = m_r.m_ui_depth.image;
        db.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &db;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = m_r.m_color_view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = m_r.m_ui_depth.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {0.0f, 0}; // reverse-Z: 0 is infinitely far
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        if (!runs.opaque.empty() || !runs.alpha.empty() || !runs.blend.empty()) {
            const VkViewport vp{0, 0, float(extent.width), float(extent.height), 0.0f, 1.0f};
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            VkDescriptorSet set = m_r.m_bindless->set(m_r.m_slot);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_r.m_pipelines->layout(), 0, 1, &set, 0,
                                    nullptr);
            PushConstants pc{};
            pc.frame = m_frame.arena.address + runs.frame_offset;
            pc.draw_instances = runs.draw_instances;
            pc.instance_table = runs.instance_table;
            pc.materials = m_r.m_meshes->materials_address();
            pc.lights = runs.lights;
            pc.material_anims = runs.material_anims;
            vkCmdPushConstants(cmd, m_r.m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
            vkCmdSetDepthBias(cmd, 0, 0, 0);
            VkBuffer vb = m_r.m_meshes->vertices().buffer().buffer;
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
            vkCmdBindIndexBuffer(cmd, m_r.m_meshes->indices().buffer().buffer, 0, VK_INDEX_TYPE_UINT32);
            m_r.record_runs(cmd, m_frame, runs.opaque, m_rs.wireframe, m_rs.flip_winding, m_r.m_ui_pipelines.get());
            m_r.record_runs(cmd, m_frame, runs.alpha, m_rs.wireframe, m_rs.flip_winding, m_r.m_ui_pipelines.get());
            m_r.record_runs(cmd, m_frame, runs.blend, m_rs.wireframe, m_rs.flip_winding, m_r.m_ui_pipelines.get());
        }
        vkCmdEndRendering(cmd);
        // The overlay's next pass draws over it.
        barrier(cmd, m_r.m_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    }

private:
    Renderer& m_r;
    Frame& m_frame;
    const RenderSettings& m_rs;
};

bool Renderer::record_overlay(VkCommandBuffer cmd, const FrameInput& input, Frame& frame, bool has_scene) {
    Overlay* overlay = input.overlay;
    const bool capture = m_capture_overlay_requested;
    if (!overlay && !capture) {
        return false;
    }
    const VkExtent2D extent = m_target_extent;
    // With a scene the target is in TRANSFER_SRC (the pre-UI capture may have read it); without one
    // it is undefined. The previous frame in flight may still be blitting it either way.
    barrier(cmd, m_color, has_scene ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    if (overlay) {
        OverlayTarget target;
        target.image = m_color;
        target.view = m_color_view;
        target.format = m_color_format;
        target.extent = extent;
        target.slot = m_slot;
        target.clear = !has_scene;
        UiSceneRecorder scenes(*this, frame, input.settings);
        if (!input.ui_scenes.empty()) {
            target.scenes = &scenes;
            target.scene_count = static_cast<uint32_t>(input.ui_scenes.size());
        }
        overlay->record(cmd, target);
    } else if (!has_scene) {
        // A UI capture with nothing open and no world: the same black the output would show.
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = m_color_view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdEndRendering(cmd);
    }
    barrier(cmd, m_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

    if (capture) {
        const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * 4;
        if (!m_capture_buffer || m_capture_buffer.size < bytes) {
            m_device->destroy_buffer(m_capture_buffer);
            m_capture_buffer = m_device->create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::readback,
                                                       Lifetime::transfer, "capture");
        }
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, m_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_capture_buffer.buffer, 1, &region);
        m_capture_overlay_requested = false;
        m_capture_frame = m_frame_number + 1;
        m_capture_slot = m_slot;
        m_capture_extent = extent;
    }
    return true;
}

} // namespace spud::render
