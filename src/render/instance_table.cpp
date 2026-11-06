#include "render/instance_table.hpp"

namespace spud::render {

InstanceTable::InstanceTable(Device& device, uint32_t capacity) : m_device(device), m_capacity(capacity) {
    m_buffer = device.create_buffer(VkDeviceSize(capacity) * sizeof(GpuInstanceData),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryUsage::dynamic, Lifetime::asset, "instance table");
}

InstanceTable::~InstanceTable() { m_device.destroy_buffer(m_buffer); }

uint32_t InstanceTable::allocate() {
    uint32_t slot;
    if (!m_free.empty()) {
        slot = m_free.back();
        m_free.pop_back();
    } else if (m_next < m_capacity) {
        slot = m_next++;
    } else {
        return ~0u;
    }
    ++m_used;
    return slot;
}

void InstanceTable::free(uint32_t slot) {
    m_free.push_back(slot);
    --m_used;
}

} // namespace spud::render
