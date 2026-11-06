#pragma once

// Persistent per-reference GPU data for one space (see GpuInstanceData).
// Slots are written once from the render thread when a reference becomes
// drawable; a freed slot must not be reused until the GPU is done with it, so
// callers free through Device::defer.

#include "render/device.hpp"
#include "render/gpu_types.hpp"

#include <vector>

namespace spud::render {

class InstanceTable {
public:
    InstanceTable(Device& device, uint32_t capacity);
    ~InstanceTable();
    InstanceTable(const InstanceTable&) = delete;
    InstanceTable& operator=(const InstanceTable&) = delete;

    uint32_t allocate(); // ~0u when full
    void free(uint32_t slot);
    // New slots only: in-flight frames never read a slot before it was allocated.
    void write(uint32_t slot, const GpuInstanceData& data) { m_buffer.as<GpuInstanceData>()[slot] = data; }
    // A live slot's flags word: one aligned 32-bit store, so a frame in flight reads the old or the new value.
    uint32_t flags(uint32_t slot) const { return m_buffer.as<GpuInstanceData>()[slot].flags; }
    void set_flags(uint32_t slot, uint32_t flags) { m_buffer.as<GpuInstanceData>()[slot].flags = flags; }

    VkDeviceAddress address() const { return m_buffer.address; }
    uint32_t used() const { return m_used; }
    uint32_t capacity() const { return m_capacity; }

private:
    Device& m_device;
    Buffer m_buffer;
    uint32_t m_capacity;
    uint32_t m_next = 0;
    uint32_t m_used = 0;
    std::vector<uint32_t> m_free;
};

} // namespace spud::render
