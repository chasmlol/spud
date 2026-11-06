#pragma once

// Where a finished frame goes. The scene is always rendered into the
// renderer's offscreen colour target; the output blits it to a swapchain image,
// draws the UI over it and presents.
//
// Every frame is presented, headless ones included: headless runs present to a
// window that is never shown. A queue that never presents makes the NVIDIA
// driver run ~1.5 ms of housekeeping inside every 1024th vkQueueSubmit
// which play never sees.

#include "render/device.hpp"
#include "render/swapchain.hpp"

#include <array>
#include <memory>
#include <vector>

struct ImDrawData;
struct SDL_Window;

namespace spud::render {

class Output {
public:
    virtual ~Output() = default;

    virtual const char* present_mode_name() const = 0;
    // Size the renderer uses when the render size follows the output.
    virtual VkExtent2D extent() const = 0;

    // Before recording frame `slot`. False: skip this frame (e.g. out-of-date or minimised swapchain).
    virtual bool acquire(uint32_t slot) = 0;
    // After the scene pass. `scene` is in TRANSFER_SRC_OPTIMAL when `has_scene`; without a scene the
    // output only clears (bench "world none" measures exactly clear + present).
    virtual void record(VkCommandBuffer cmd, VkImage scene, VkExtent2D scene_extent, bool has_scene,
                        ImDrawData* ui) = 0;
    virtual void add_waits(uint32_t slot, std::vector<VkSemaphoreSubmitInfo>& waits) = 0;
    virtual void add_signals(std::vector<VkSemaphoreSubmitInfo>& signals) = 0;
    // Right after the queue submit, with the queue mutex held.
    virtual void present() = 0;

    virtual void on_resize() {}
    virtual void set_present_mode(PresentMode) {}
    virtual bool has_ui() const { return false; }
    // True when the output size changed since the last call (renderer re-sizes its targets).
    virtual bool take_resized() { return false; }
};

class SwapchainOutput final : public Output {
public:
    SwapchainOutput(Device& device, SDL_Window* window, PresentMode mode, bool imgui);
    ~SwapchainOutput() override;

    const char* present_mode_name() const override { return m_swapchain->present_mode_name(); }
    VkExtent2D extent() const override { return m_swapchain->extent(); }
    bool acquire(uint32_t slot) override;
    void record(VkCommandBuffer cmd, VkImage scene, VkExtent2D scene_extent, bool has_scene, ImDrawData* ui) override;
    void add_waits(uint32_t slot, std::vector<VkSemaphoreSubmitInfo>& waits) override;
    void add_signals(std::vector<VkSemaphoreSubmitInfo>& signals) override;
    void present() override;
    void on_resize() override { m_dirty = true; }
    void set_present_mode(PresentMode mode) override;
    bool has_ui() const override { return m_imgui; }
    bool take_resized() override;

private:
    void recreate();
    void init_imgui();

    Device& m_device;
    SDL_Window* m_window;
    PresentMode m_mode;
    std::unique_ptr<Swapchain> m_swapchain;
    std::array<VkSemaphore, k_frames_in_flight> m_acquire{};
    uint32_t m_image = 0;
    bool m_dirty = false;
    bool m_resized = false;
    bool m_imgui = false;
    VkFormat m_imgui_format = VK_FORMAT_UNDEFINED;
};

} // namespace spud::render
