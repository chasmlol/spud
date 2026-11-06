#pragma once

// Common Vulkan + VMA includes and error helpers for the renderer.

#include "core/error.hpp"

#include <vma/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <format>
#include <source_location>
#include <string_view>

namespace spud::render {

std::string_view vk_result_name(VkResult result);

// Called once when the GPU is lost, before the error is thrown: says what the driver knows of why (Device).
inline void (*g_device_lost_report)() = nullptr;

inline void vk_check(VkResult result, std::source_location loc = std::source_location::current()) {
    if (result != VK_SUCCESS) {
        if (result == VK_ERROR_DEVICE_LOST && g_device_lost_report) {
            g_device_lost_report();
        }
        throw Error(std::format("Vulkan error {} at {}:{}", vk_result_name(result), loc.file_name(), loc.line()));
    }
}

inline constexpr uint32_t k_frames_in_flight = 2;

} // namespace spud::render
