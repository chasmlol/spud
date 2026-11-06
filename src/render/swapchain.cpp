#include "render/swapchain.hpp"

#include "core/log.hpp"

#include <algorithm>

namespace spud::render {

Swapchain::Swapchain(Device& device, uint32_t width, uint32_t height, PresentMode mode) : m_device(device) {
    create(width, height, mode, VK_NULL_HANDLE);
}

Swapchain::~Swapchain() {
    destroy_views();
    if (m_swapchain) {
        vkDestroySwapchainKHR(m_device.device(), m_swapchain, nullptr);
    }
}

const char* Swapchain::present_mode_name() const {
    switch (m_present_mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
    default: return "other";
    }
}

void Swapchain::destroy_views() {
    VkDevice dev = m_device.device();
    for (auto v : m_views) {
        vkDestroyImageView(dev, v, nullptr);
    }
    for (auto s : m_present_semaphores) {
        vkDestroySemaphore(dev, s, nullptr);
    }
    m_views.clear();
    m_present_semaphores.clear();
    m_images.clear();
}

void Swapchain::recreate(uint32_t width, uint32_t height, PresentMode mode) {
    vkDeviceWaitIdle(m_device.device());
    destroy_views();
    VkSwapchainKHR old = m_swapchain;
    create(width, height, mode, old);
    vkDestroySwapchainKHR(m_device.device(), old, nullptr);
}

void Swapchain::create(uint32_t width, uint32_t height, PresentMode mode, VkSwapchainKHR old) {
    VkPhysicalDevice pd = m_device.physical();
    VkSurfaceKHR surface = m_device.surface();
    VkSurfaceCapabilitiesKHR caps;
    vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &count, formats.data());
    // UNORM on purpose: shading is in gamma space and must not be re-encoded.
    m_format = VK_FORMAT_UNDEFINED;
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        for (const auto& f : formats) {
            if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                m_format = f.format;
                m_color_space = f.colorSpace;
                break;
            }
        }
        if (m_format != VK_FORMAT_UNDEFINED) {
            break;
        }
    }
    if (m_format == VK_FORMAT_UNDEFINED) {
        throw Error("surface supports no UNORM BGRA8/RGBA8 format");
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &count, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    m_present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (mode == PresentMode::no_vsync) {
        m_present_mode = has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                         : has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
                                                            : VK_PRESENT_MODE_FIFO_KHR;
    } else if (mode == PresentMode::mailbox && has(VK_PRESENT_MODE_MAILBOX_KHR)) {
        m_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        m_extent = caps.currentExtent;
    } else {
        m_extent = {std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
                    std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height)};
    }
    // One spare image so acquire never waits on the presentation engine without vsync.
    uint32_t images = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount > 0) {
        images = std::min(images, caps.maxImageCount);
    }
    m_min_images = caps.minImageCount;

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = images;
    info.imageFormat = m_format;
    info.imageColorSpace = m_color_space;
    info.imageExtent = m_extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = m_present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;
    vk_check(vkCreateSwapchainKHR(m_device.device(), &info, nullptr, &m_swapchain));

    vkGetSwapchainImagesKHR(m_device.device(), m_swapchain, &count, nullptr);
    m_images.resize(count);
    vkGetSwapchainImagesKHR(m_device.device(), m_swapchain, &count, m_images.data());
    for (auto image : m_images) {
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = image;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = m_format;
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        vk_check(vkCreateImageView(m_device.device(), &v, nullptr, &view));
        m_views.push_back(view);
        VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore sem;
        vk_check(vkCreateSemaphore(m_device.device(), &s, nullptr, &sem));
        m_present_semaphores.push_back(sem);
    }
    log::info("swapchain: {}x{}, {} images, present mode {}", m_extent.width, m_extent.height, count,
              present_mode_name());
}

} // namespace spud::render
