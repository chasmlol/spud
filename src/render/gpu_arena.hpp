#pragma once

// A large device buffer sub-allocated with a VMA virtual block. Used for the
// shared mesh vertex/index pools so all meshes can be drawn from one binding.
// allocate() is thread-safe (workers reserve their destination before
// uploading); free() should be deferred by the caller until the GPU is done.

#include "render/device.hpp"

#include <mutex>

namespace spud::render {

struct ArenaRange {
    VmaVirtualAllocation allocation = nullptr;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    explicit operator bool() const { return allocation != nullptr; }
};

class GpuArena {
public:
    GpuArena(Device& device, VkDeviceSize capacity, VkBufferUsageFlags usage, const char* name);
    ~GpuArena();
    GpuArena(const GpuArena&) = delete;
    GpuArena& operator=(const GpuArena&) = delete;

    ArenaRange allocate(VkDeviceSize size, VkDeviceSize alignment); // empty range if full
    void free(ArenaRange range);

    const Buffer& buffer() const { return m_buffer; }
    VkDeviceSize capacity() const { return m_capacity; }
    VkDeviceSize used() const;

private:
    Device& m_device;
    Buffer m_buffer;
    VkDeviceSize m_capacity;
    VmaVirtualBlock m_block = nullptr;
    mutable std::mutex m_mutex;
    VkDeviceSize m_used = 0;
};

} // namespace spud::render
