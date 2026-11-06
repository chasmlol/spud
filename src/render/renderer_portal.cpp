// The view through an open doorway (render/view.hpp, PortalInput, "The view through
// a door"). The far space is a whole frame of its own, the same code as the main frame, from the camera carried
// through the door: shadows, fog, water, particles, its image space, everything a space has. Its view clips at the far
// doorway's plane, so nothing on the near side of that door draws, and its depth stays that of a plain projection for
// everything that reads it.
//
// The main frame then starts from that scene inside the doorway, so its eye adaptation, bloom and lens flare see what
// is seen through the door, with the doorway's own depth: what stands in front of the doorway draws over it, what is
// behind fails against it. At the end the doorway takes the far frame's finished image, by coverage, so the main
// image's edges stay antialiased.

#include "render/renderer.hpp"

#include "core/trace.hpp"

#include <algorithm>
#include <cstring>

namespace spud::render {

namespace {

constexpr uint32_t k_clip_vertices = 6; // the doorway's two triangles

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

// One render pass to the next on the same attachments.
void attachment_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    m.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    m.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

void Renderer::prepare_portal(const FrameInput& input) {
    m_portal.ready = false;
    const PortalInput* p = input.portal;
    if (!p || !p->view || !input.view) {
        return;
    }
    SPUD_TRACE("prepare portal");
    const View& main = *input.view;
    const VkExtent2D extent = m_target_extent;

    // The doorway on screen. A corner behind the camera (it is about to be walked through) takes the whole target.
    bool whole = false;
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (int i = 0; i < 4; ++i) {
        m_portal.clip[i] = main.view_proj * glm::vec4(p->quad[i] - main.position, 1.0f);
        const glm::vec4& c = m_portal.clip[i];
        if (c.w <= main.near_plane) {
            whole = true;
            continue;
        }
        const glm::vec2 ndc = glm::vec2(c) / c.w;
        const glm::vec2 px((ndc.x * 0.5f + 0.5f) * float(extent.width), (ndc.y * 0.5f + 0.5f) * float(extent.height));
        lo = glm::min(lo, px);
        hi = glm::max(hi, px);
    }
    int32_t x0 = 0, y0 = 0, x1 = int32_t(extent.width), y1 = int32_t(extent.height);
    if (!whole) {
        x0 = std::clamp(int32_t(std::floor(lo.x)) - 1, 0, int32_t(extent.width));
        y0 = std::clamp(int32_t(std::floor(lo.y)) - 1, 0, int32_t(extent.height));
        x1 = std::clamp(int32_t(std::ceil(hi.x)) + 1, 0, int32_t(extent.width));
        y1 = std::clamp(int32_t(std::ceil(hi.y)) + 1, 0, int32_t(extent.height));
    }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    m_portal.scissor = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};

    // The doorway for the final composite, in clip x, y, w: a linear map of view space, so a corner behind the camera
    // holds. Four edge planes through the eye, then the doorway's plane, its normal away from the eye.
    glm::vec3 corner[4], middle(0.0f);
    for (int i = 0; i < 4; ++i) {
        corner[i] = glm::vec3(m_portal.clip[i].x, m_portal.clip[i].y, m_portal.clip[i].w);
        middle += corner[i];
    }
    for (int i = 0; i < 4; ++i) {
        const glm::vec3 edge = glm::cross(corner[i], corner[(i + 1) & 3]);
        m_portal.doorway[i] = glm::vec4(glm::dot(edge, middle) < 0.0f ? -edge : edge, 0.0f);
    }
    glm::vec3 normal = glm::cross(corner[1] - corner[0], corner[2] - corner[0]);
    if (glm::dot(normal, corner[0]) < 0.0f) {
        normal = -normal;
    }
    m_portal.doorway[4] = glm::vec4(normal, glm::dot(normal, corner[0]));
    m_portal.ready = true;
}

bool Renderer::record_portal(VkCommandBuffer cmd, Frame& fr, const PushConstants& base, const glm::vec3& clear) {
    if (!m_portal.ready || m_far_pass) {
        return false;
    }
    SPUD_TRACE("record portal");
    const VkExtent2D extent = m_target_extent;
    const VkDeviceSize clip_offset = arena_alloc(fr, k_clip_vertices * sizeof(glm::vec4), 16);
    auto* v = reinterpret_cast<glm::vec4*>(fr.arena.as<std::byte>() + clip_offset);
    // Nearer than the near plane the doorway would clip away and the wall behind it show, walking through. Its pipeline
    // clamps depth instead of clipping: that part stands at the near plane, the rest where it is.
    const int tri[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) {
        v[i] = m_portal.clip[tri[i]];
    }
    const VkDeviceAddress clip_positions = fr.arena.address + clip_offset;

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_hdr.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{clear.r, clear.g, clear.b, 1.0f}};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_depth_target.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0, 0, float(extent.width), float(extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &m_portal.scissor);

    // The far scene, for the main frame's eye and bloom; its composite's 1 / 1.2 follows, so it goes in that much higher.
    PushConstants pc = base;
    pc.mode = 1;
    pc.pass[0] = Bindless::k_portal_scene_slot;
    pc.pass[3] = extent.width | (extent.height << 16);
    vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_portal_copy, false));
    DrawState fullscreen;
    fullscreen.two_sided = true;
    fullscreen.z_test = false;
    fullscreen.z_write = false;
    apply_state(cmd, fullscreen, false);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    // The doorway's depth, colour untouched.
    PushConstants depth_pc = base;
    std::memcpy(depth_pc.pass, &clip_positions, sizeof(clip_positions));
    vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(depth_pc), &depth_pc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_portal_depth, false));
    DrawState s;
    s.two_sided = true;
    s.z_test = true; // compare ALWAYS in the pipeline
    s.z_write = true;
    apply_state(cmd, s, false);
    vkCmdDraw(cmd, k_clip_vertices, 1, 0, 0);
    vkCmdEndRendering(cmd);
    attachment_barrier(cmd);
    return true;
}

void Renderer::record_portal_stamp(VkCommandBuffer cmd, const PushConstants& base, uint32_t scene_slot,
                                   uint32_t adapted_slot) {
    if (!m_portal.ready || m_far_pass) {
        return;
    }
    SPUD_TRACE("portal stamp");
    const VkExtent2D extent = m_target_extent;
    const bool msaa = m_samples > 1;
    constexpr auto attachment = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr auto fragment = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    constexpr auto depth_tests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    if (msaa) {
        image_barrier(cmd, m_depth_target.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depth_tests,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, fragment, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    VkRenderingAttachmentInfo out{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    out.imageView = m_color_view;
    out.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    out.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    out.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &out;
    image_barrier(cmd, m_color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, attachment, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, attachment,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0, 0, float(extent.width), float(extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &m_portal.scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_portal_stamp, false));
    DrawState s;
    s.two_sided = true;
    s.z_test = false;
    s.z_write = false;
    apply_state(cmd, s, false);
    PushConstants pc = base;
    pc.mode = (msaa ? 1u : 0u) | (adapted_slot << 1);
    pc.pass[0] = Bindless::k_portal_image_slot;
    pc.pass[1] = msaa ? Bindless::k_scene_depth_slot : Bindless::k_depth_slot;
    pc.pass[2] = scene_slot;
    pc.pass[3] = extent.width | (extent.height << 16);
    vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

} // namespace spud::render
