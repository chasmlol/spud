#pragma once

// Something composited over the finished scene: the game UI (src/ui). The renderer
// records it last in the frame, after the pre-UI capture, so `screenshot` never
// contains it. FrameInput::overlay is null while nothing is open, and then the frame
// has no UI pass at all.

#include "render/vk.hpp"

namespace spud::render {

// Draws the frame's UI 3D scenes (FrameInput::ui_scenes) for the overlay: scene `index` over `target`'s current
// content, its own depth cleared. Recorded outside any rendering pass; the target is in COLOR_ATTACHMENT_OPTIMAL
// before and after. `clear`: nothing has been drawn into the target yet this frame (no world, nothing composited):
// clear it to black first.
class SceneRecorder {
public:
    virtual ~SceneRecorder() = default;
    virtual void record_scene(VkCommandBuffer cmd, uint32_t index, bool clear) = 0;
};

struct OverlayTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t slot = 0;  // frame-in-flight slot being recorded (its previous use has completed)
    bool clear = false; // no scene this frame: the overlay must clear the target itself
    SceneRecorder* scenes = nullptr; // the frame's UI 3D scenes, null when there are none
    uint32_t scene_count = 0;
};

class Overlay {
public:
    virtual ~Overlay() = default;
    // `target` is in COLOR_ATTACHMENT_OPTIMAL, outside any rendering pass. The overlay begins and
    // ends its own pass(es) and leaves the target in COLOR_ATTACHMENT_OPTIMAL.
    virtual void record(VkCommandBuffer cmd, const OverlayTarget& target) = 0;
};

} // namespace spud::render
