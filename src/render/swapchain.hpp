#pragma once

#include "render/device.hpp"

#include <vector>

namespace spud::render {

enum class PresentMode {
    vsync,     // FIFO
    no_vsync,  // IMMEDIATE, else MAILBOX, else FIFO
    mailbox,   // MAILBOX, else FIFO
};

class Swapchain {
public:
    Swapchain(Device& device, uint32_t width, uint32_t height, PresentMode mode);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    void recreate(uint32_t width, uint32_t height, PresentMode mode);

    VkSwapchainKHR handle() const { return m_swapchain; }
    VkFormat format() const { return m_format; }
    VkExtent2D extent() const { return m_extent; }
    uint32_t image_count() const { return static_cast<uint32_t>(m_images.size()); }
    uint32_t min_image_count() const { return m_min_images; }
    VkImage image(uint32_t i) const { return m_images[i]; }
    VkImageView view(uint32_t i) const { return m_views[i]; }
    VkSemaphore present_semaphore(uint32_t i) const { return m_present_semaphores[i]; }
    VkPresentModeKHR present_mode() const { return m_present_mode; }
    const char* present_mode_name() const;

private:
    void create(uint32_t width, uint32_t height, PresentMode mode, VkSwapchainKHR old);
    void destroy_views();

    Device& m_device;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR m_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D m_extent{};
    VkPresentModeKHR m_present_mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t m_min_images = 2;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;
    std::vector<VkSemaphore> m_present_semaphores; // one per image: reuse is safe once it is re-acquired
};

} // namespace spud::render
