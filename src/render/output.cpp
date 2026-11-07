#include "render/output.hpp"

#include "core/log.hpp"
#include "core/platform.hpp"
#include "core/sampler.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <utility>

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

void clear_image(VkCommandBuffer cmd, VkImage image) {
    VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
}

} // namespace

// ---- swapchain ---------------------------------------------------------------------

SwapchainOutput::SwapchainOutput(Device& device, SDL_Window* window, PresentMode mode, bool imgui)
    : m_device(device), m_window(window), m_mode(mode) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    m_swapchain = std::make_unique<Swapchain>(device, static_cast<uint32_t>(std::max(w, 1)),
                                              static_cast<uint32_t>(std::max(h, 1)), mode);
    for (auto& s : m_acquire) {
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk_check(vkCreateSemaphore(device.device(), &info, nullptr, &s));
    }
    if (imgui) {
        init_imgui();
    }
    m_present = std::thread([this] { present_thread(); });
    platform::set_thread(m_present.native_handle(), "spud present", platform::ThreadPriority::highest);
}

SwapchainOutput::~SwapchainOutput() {
    {
        std::unique_lock lock(m_present_mutex);
        m_present_cv.wait(lock, [this] { return !m_present_busy; });
        m_present_stop = true;
    }
    m_present_cv.notify_all();
    m_present.join();
    m_device.wait_idle();
    if (m_imgui) {
        ImGui_ImplVulkan_Shutdown();
    }
    for (auto s : m_acquire) {
        vkDestroySemaphore(m_device.device(), s, nullptr);
    }
    m_swapchain.reset();
}

void SwapchainOutput::init_imgui() {
    m_imgui_format = m_swapchain->format();
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = m_device.instance();
    ii.PhysicalDevice = m_device.physical();
    ii.Device = m_device.device();
    ii.QueueFamily = m_device.queue_family();
    ii.Queue = m_device.queue();
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = std::max(2u, m_swapchain->min_image_count());
    ii.ImageCount = std::max(ii.MinImageCount, m_swapchain->image_count());
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &m_imgui_format;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        throw Error("ImGui_ImplVulkan_Init failed");
    }
    std::lock_guard lock(m_device.queue_mutex());
    ImGui_ImplVulkan_CreateFontsTexture();
    m_imgui = true;
}

void SwapchainOutput::set_present_mode(PresentMode mode) {
    if (mode != m_mode) {
        m_mode = mode;
        m_dirty = true;
    }
}

bool SwapchainOutput::take_resized() {
    const bool r = m_resized;
    m_resized = false;
    return r;
}

void SwapchainOutput::recreate() {
    finish_present();
    m_dirty = false;
    if (m_next_ready) {
        // That image goes with the swapchain, and its semaphore stays signalled for a wait that won't come.
        m_next_ready = false;
        m_device.wait_idle();
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (VkSemaphore& s : m_acquire) {
            vkDestroySemaphore(m_device.device(), s, nullptr);
            vk_check(vkCreateSemaphore(m_device.device(), &info, nullptr, &s));
        }
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
    if (w <= 0 || h <= 0) {
        return;
    }
    const VkExtent2D before = m_swapchain->extent();
    m_swapchain->recreate(static_cast<uint32_t>(w), static_cast<uint32_t>(h), m_mode);
    if (m_imgui) {
        ImGui_ImplVulkan_SetMinImageCount(std::max(2u, m_swapchain->min_image_count()));
    }
    const VkExtent2D after = m_swapchain->extent();
    if (before.width != after.width || before.height != after.height) {
        m_resized = true;
    }
}

bool SwapchainOutput::take_image() {
    const VkResult r = vkAcquireNextImageKHR(m_device.device(), m_swapchain->handle(), UINT64_MAX, m_acquire[m_semaphore],
                                             VK_NULL_HANDLE, &m_image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate();
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR) {
        m_dirty = true;
    } else if (r != VK_SUCCESS) {
        vk_check(r);
    }
    return true;
}

bool SwapchainOutput::acquire(uint32_t) {
    if (m_dirty) {
        recreate();
    }
    const VkExtent2D e = m_swapchain->extent();
    if (e.width == 0 || e.height == 0) {
        return false;
    }
    bool coming;
    {
        std::lock_guard lock(m_present_mutex);
        coming = m_present_busy || m_next_ready;
    }
    // The image the last frame's present goes on to take comes at the blit. Without one, as at the start, it's taken here.
    m_have_image = coming || take_image();
    return m_have_image;
}

void SwapchainOutput::finish_present() {
    std::unique_lock lock(m_present_mutex);
    m_present_cv.wait(lock, [this] { return !m_present_busy; });
    if (m_present_error) {
        std::rethrow_exception(std::exchange(m_present_error, nullptr));
    }
}

void SwapchainOutput::wait_for_image() {
    const bool coming = m_have_image;
    finish_present();
    if (m_next_ready) {
        m_next_ready = false;
        m_image = m_next_image;
    } else if (coming && m_dirty) {
        // It couldn't take one: the swapchain went out of date. Once more, with a new one.
        recreate();
        const VkExtent2D e = m_swapchain->extent();
        m_have_image = e.width != 0 && e.height != 0 && take_image();
    }
}

void SwapchainOutput::record(VkCommandBuffer cmd, VkImage scene, VkExtent2D scene_extent, bool has_scene,
                             ImDrawData* ui) {
    wait_for_image();
    if (!m_have_image) {
        return; // nothing to show it on: the frame's work still runs
    }
    VkImage swap = m_swapchain->image(m_image);
    const VkExtent2D se = m_swapchain->extent();
    barrier(cmd, swap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (has_scene) {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(scene_extent.width), int32_t(scene_extent.height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int32_t(se.width), int32_t(se.height), 1};
        const bool same = scene_extent.width == se.width && scene_extent.height == se.height;
        vkCmdBlitImage(cmd, scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    } else {
        clear_image(cmd, swap);
    }
    if (m_imgui && ui && ui->CmdListsCount > 0) {
        barrier(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = m_swapchain->view(m_image);
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, se};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        vkCmdBeginRendering(cmd, &ri);
        ImGui_ImplVulkan_RenderDrawData(ui, cmd);
        vkCmdEndRendering(cmd);
        barrier(cmd, swap, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    } else {
        barrier(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE);
    }
}

void SwapchainOutput::add_waits(uint32_t, std::vector<VkSemaphoreSubmitInfo>& waits) {
    if (!m_have_image) {
        return;
    }
    VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    w.semaphore = m_acquire[m_semaphore];
    w.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    waits.push_back(w);
}

void SwapchainOutput::add_signals(std::vector<VkSemaphoreSubmitInfo>& signals) {
    if (!m_have_image) {
        return;
    }
    VkSemaphoreSubmitInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    s.semaphore = m_swapchain->present_semaphore(m_image);
    s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals.push_back(s);
}

void SwapchainOutput::present(VkFence done) {
    if (!m_have_image) {
        return;
    }
    {
        std::lock_guard lock(m_present_mutex);
        m_present_image = m_image;
        m_present_fence = done;
        m_semaphore = (m_semaphore + 1) % uint32_t(m_acquire.size());
        m_next_semaphore = m_semaphore;
        m_present_busy = true;
    }
    m_present_cv.notify_all();
}

void SwapchainOutput::present_thread() {
    sampler::catch_terminate();
    for (;;) {
        uint32_t image = 0, semaphore = 0;
        VkFence done = VK_NULL_HANDLE;
        {
            std::unique_lock lock(m_present_mutex);
            m_present_cv.wait(lock, [this] { return m_present_busy || m_present_stop; });
            if (m_present_stop) {
                return;
            }
            image = m_present_image;
            done = m_present_fence;
            semaphore = m_next_semaphore;
        }
        uint32_t next = 0;
        bool ready = false;
        std::exception_ptr error;
        try {
            // The driver holds a lock through a present until the frame is drawn, and the main thread's recording stops at
            // it, so the GPU runs out of work. Presented once it's drawn, the call is short.
            vk_check(vkWaitForFences(m_device.device(), 1, &done, VK_TRUE, UINT64_MAX));
            VkSwapchainKHR sc = m_swapchain->handle();
            VkSemaphore sem = m_swapchain->present_semaphore(image);
            VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &sem;
            present.swapchainCount = 1;
            present.pSwapchains = &sc;
            present.pImageIndices = &image;
            VkResult r = VK_SUCCESS;
            {
                std::lock_guard queue(m_device.queue_mutex());
                r = vkQueuePresentKHR(m_device.queue(), &present);
            }
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
                m_dirty = true;
            } else if (r != VK_SUCCESS) {
                vk_check(r);
            }
            if (r != VK_ERROR_OUT_OF_DATE_KHR) {
                r = vkAcquireNextImageKHR(m_device.device(), sc, UINT64_MAX, m_acquire[semaphore], VK_NULL_HANDLE, &next);
                ready = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
                if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) {
                    m_dirty = true;
                } else if (!ready) {
                    vk_check(r);
                }
            }
        } catch (...) {
            error = std::current_exception();
        }
        {
            std::lock_guard lock(m_present_mutex);
            m_next_image = next;
            m_next_ready = ready;
            m_present_error = error;
            m_present_busy = false;
        }
        m_present_cv.notify_all();
    }
}

} // namespace spud::render
