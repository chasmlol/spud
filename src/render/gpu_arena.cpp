#include "render/gpu_arena.hpp"

namespace spud::render {

GpuArena::GpuArena(Device& device, VkDeviceSize capacity, VkBufferUsageFlags usage, const char* name)
    : m_device(device), m_capacity(capacity) {
    m_buffer = device.create_buffer(capacity, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::gpu_only, Lifetime::asset, name,
                                    true);
    VmaVirtualBlockCreateInfo info{};
    info.size = capacity;
    vk_check(vmaCreateVirtualBlock(&info, &m_block));
}

GpuArena::~GpuArena() {
    vmaClearVirtualBlock(m_block);
    vmaDestroyVirtualBlock(m_block);
    m_device.destroy_buffer(m_buffer);
}

ArenaRange GpuArena::allocate(VkDeviceSize size, VkDeviceSize alignment) {
    // VMA aligns to powers of two only. Other alignments (36-byte vertices, so the offset is a whole vertex index)
    // take a little extra space and round the offset up inside it.
    const bool pow2 = alignment != 0 && (alignment & (alignment - 1)) == 0;
    VmaVirtualAllocationCreateInfo info{};
    info.size = pow2 ? size : size + alignment;
    info.alignment = pow2 ? alignment : 4;
    ArenaRange range;
    std::lock_guard lock(m_mutex);
    if (vmaVirtualAllocate(m_block, &info, &range.allocation, &range.offset) != VK_SUCCESS) {
        return {};
    }
    if (!pow2) {
        range.offset = (range.offset + alignment - 1) / alignment * alignment;
    }
    range.size = size;
    m_used += size;
    return range;
}

void GpuArena::free(ArenaRange range) {
    if (!range) {
        return;
    }
    std::lock_guard lock(m_mutex);
    vmaVirtualFree(m_block, range.allocation);
    m_used -= range.size;
}

VkDeviceSize GpuArena::used() const {
    std::lock_guard lock(m_mutex);
    return m_used;
}

} // namespace spud::render
