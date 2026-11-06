#pragma once

// Vulkan instance + physical/logical device + VMA, and the plumbing every
// other render object needs: buffers, deferred destruction, debug names.

#include "render/vk.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace spud::render {

enum class MemoryUsage {
    gpu_only,     // device local, filled through the uploader
    upload,       // host-visible staging, mapped, sequential writes
    dynamic,      // host-visible, mapped; prefers device-local (ReBAR) for per-frame data the GPU reads
    readback,     // host-visible, cached, for GPU -> CPU copies
};

// What a GPU resource holds from one frame to the next (docs/RENDERING.md, "Image clarity").
// The final image is never built from earlier frames. Effects may keep their own cross-frame state as
// `simulation` or `cache` resources, listed in the allowlist in device.cpp; an unlisted one throws.
enum class Lifetime {
    asset,      // content: meshes, textures, terrain, material and instance tables
    frame,      // fully rewritten every frame before it is read: render targets, per-frame arenas
    transfer,   // staging uploads and readbacks
    simulation, // effect state that evolves over time (water ripples, volumetric accumulation, exposure)
    cache,      // reused results (e.g. static shadow maps)
};

const char* to_string(Lifetime lifetime);

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;

    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
    template <typename T>
    T* as() const {
        return static_cast<T*>(mapped);
    }
};

struct DeviceOptions {
    bool validation = false;
    std::vector<const char*> instance_extensions; // e.g. from SDL for the surface
    bool need_present = false;
};

class Device {
public:
    // `make_surface` (optional) is called after instance creation so device
    // selection can require presentation support.
    Device(const DeviceOptions& options, const std::function<VkSurfaceKHR(VkInstance)>& make_surface);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physical() const { return m_physical; }
    VkDevice device() const { return m_device; }
    VkQueue queue() const { return m_queue; }
    void report_lost();
    bool coarse_foliage() const { return m_coarse_foliage; }
    // A mark in the command stream for report_lost to find.
    void checkpoint(VkCommandBuffer cmd, uintptr_t id) const {
        if (m_set_checkpoint) {
            m_set_checkpoint(cmd, reinterpret_cast<const void*>(id));
        }
    }
    uint32_t queue_family() const { return m_queue_family; }
    // Async upload queue. May be the graphics queue itself, then submits are serialised by queue_mutex().
    VkQueue transfer_queue() const { return m_transfer_queue; }
    uint32_t transfer_family() const { return m_transfer_family; }
    bool transfer_shares_graphics_queue() const { return m_transfer_queue == m_queue; }
    // VK_KHR_pipeline_executable_properties enabled (SPUD_PIPELINE_STATS=1): pipelines log their statistics.
    bool pipeline_stats() const { return m_pipeline_stats; }
    std::mutex& queue_mutex() { return m_queue_mutex; }
    VkSurfaceKHR surface() const { return m_surface; }
    VmaAllocator allocator() const { return m_allocator; }
    const VkPhysicalDeviceProperties& properties() const { return m_properties; }
    const std::string& name() const { return m_name; }

    // Thread-safe (VMA and vkCreateBuffer are).
    // `shared`: also accessed by the transfer queue (concurrent sharing if its family differs).
    Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage memory, Lifetime lifetime,
                         const char* debug_name = nullptr, bool shared = false);
    // The only place images are allocated, so every image declares its lifetime. Returns the VMA
    // result so callers can fall back (e.g. from a pool to the default heaps).
    VkResult create_image(const VkImageCreateInfo& info, const VmaAllocationCreateInfo& alloc, Lifetime lifetime,
                          const char* debug_name, VkImage& image, VmaAllocation& allocation,
                          VmaAllocationInfo* alloc_info = nullptr);
    // Fills `info` sharing fields for a resource written by the transfer queue and read by graphics.
    void set_shared(VkImageCreateInfo& info) const;
    void destroy_buffer(Buffer& buffer);

    // Defers destruction until the GPU has finished every frame submitted so far.
    // Render thread only.
    void defer(std::function<void()> fn);
    void defer_destroy(Buffer buffer);
    // Called by the renderer: `serial` is the frame being recorded, `completed` the newest finished one.
    void set_frame_serials(uint64_t recording, uint64_t completed);
    uint64_t recording_serial() const { return m_recording_serial; }
    void collect_garbage();
    void flush_all_garbage(); // device must be idle

    // Records and submits a one-off command buffer and waits for it. Init/shutdown only.
    void immediate(const std::function<void(VkCommandBuffer)>& record);

    void set_name(uint64_t handle, VkObjectType type, const char* name) const;

    // Device-local memory: total budget and usage reported by VMA.
    void memory_budget(uint64_t& used, uint64_t& budget) const;

    // vkAllocateMemory calls made by VMA (new blocks / dedicated allocations).
    struct MemoryEvents {
        std::atomic<uint64_t> allocations{0};
        std::atomic<uint64_t> frees{0};
        std::atomic<uint64_t> allocated_bytes{0};
    };
    const MemoryEvents& memory_events() const { return m_memory_events; }

private:
    void create_instance(const DeviceOptions& options);
    void pick_physical_device(bool need_present);
    void create_device();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    bool m_checkpoints = false;
    bool m_coarse_foliage = false;
    PFN_vkCmdSetCheckpointNV m_set_checkpoint = nullptr;
    uint32_t m_queue_family = 0;
    VkQueue m_transfer_queue = VK_NULL_HANDLE;
    uint32_t m_transfer_family = 0;
    uint32_t m_transfer_index = 0;
    uint32_t m_families[2] = {0, 0};
    std::mutex m_queue_mutex;
    VmaAllocator m_allocator = nullptr;
    VkPhysicalDeviceProperties m_properties{};
    std::string m_name;
    bool m_debug_utils = false;
    bool m_pipeline_stats = false;
    PFN_vkSetDebugUtilsObjectNameEXT m_set_name = nullptr;

    VkCommandPool m_immediate_pool = VK_NULL_HANDLE;
    MemoryEvents m_memory_events;

    struct Deferred {
        uint64_t serial;
        std::function<void()> fn;
    };
    std::vector<Deferred> m_deferred;
    uint64_t m_recording_serial = 1;
    uint64_t m_completed_serial = 0;
};

} // namespace spud::render
