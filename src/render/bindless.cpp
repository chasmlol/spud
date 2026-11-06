#include "render/bindless.hpp"

#include <algorithm>

namespace spud::render {

Bindless::Bindless(Device& device) : m_device(device) {
    VkDevice dev = device.device();
    const float max_aniso = std::min(16.0f, device.properties().limits.maxSamplerAnisotropy);
    for (uint32_t i = 0; i < k_sampler_count; ++i) {
        if (i == sampler_shadow) {
            VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            info.magFilter = VK_FILTER_LINEAR;
            info.minFilter = VK_FILTER_LINEAR;
            info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.compareEnable = VK_TRUE;
            info.compareOp = VK_COMPARE_OP_LESS;
            info.maxLod = 0.0f;
            vk_check(vkCreateSampler(dev, &info, nullptr, &m_samplers[i]));
            continue;
        }
        const bool wrap_u = i == sampler_wrap_clamp || i == sampler_wrap_wrap || i == sampler_wrap_isotropic;
        const bool wrap_v = i == sampler_clamp_wrap || i == sampler_wrap_wrap || i == sampler_wrap_isotropic;
        const bool anisotropic = max_aniso > 1.0f && i != sampler_wrap_isotropic;
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU = wrap_u ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = wrap_v ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy = anisotropic ? max_aniso : 1.0f;
        info.maxLod = VK_LOD_CLAMP_NONE;
        // The game's textures, sampled through D3D11 on the reference machine, come out 0.25 mip levels sharper
        // than Vulkan's at the same sampler state (16x anisotropic, LOD bias 0).
        info.mipLodBias = k_texture_lod_bias;
        vk_check(vkCreateSampler(dev, &info, nullptr, &m_samplers[i]));
    }

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = k_max_textures;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = k_sampler_count;
    bindings[1].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[1].pImmutableSamplers = m_samplers.data();

    VkDescriptorBindingFlags flags[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flag_info.bindingCount = 2;
    flag_info.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.pNext = &flag_info;
    layout_info.bindingCount = 2;
    layout_info.pBindings = bindings;
    vk_check(vkCreateDescriptorSetLayout(dev, &layout_info, nullptr, &m_layout));

    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, k_max_textures * k_frames_in_flight},
        {VK_DESCRIPTOR_TYPE_SAMPLER, k_sampler_count * k_frames_in_flight},
    };
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = k_frames_in_flight;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes;
    vk_check(vkCreateDescriptorPool(dev, &pool_info, nullptr, &m_pool));

    std::array<VkDescriptorSetLayout, k_frames_in_flight> layouts;
    layouts.fill(m_layout);
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = m_pool;
    alloc.descriptorSetCount = k_frames_in_flight;
    alloc.pSetLayouts = layouts.data();
    vk_check(vkAllocateDescriptorSets(dev, &alloc, m_sets.data()));
}

Bindless::~Bindless() {
    VkDevice dev = m_device.device();
    vkDestroyDescriptorPool(dev, m_pool, nullptr);
    vkDestroyDescriptorSetLayout(dev, m_layout, nullptr);
    for (auto s : m_samplers) {
        vkDestroySampler(dev, s, nullptr);
    }
}

void Bindless::fill_all(VkImageView view) {
    std::vector<VkDescriptorImageInfo> infos(k_max_textures,
                                             {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    for (auto set : m_sets) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = 0;
        w.descriptorCount = k_max_textures;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = infos.data();
        vkUpdateDescriptorSets(m_device.device(), 1, &w, 0, nullptr);
    }
}

void Bindless::begin_frame(uint32_t frame) {
    m_frame = frame;
    auto& pending = m_pending[frame];
    if (pending.empty()) {
        return;
    }
    // Only a slot's latest queued view is written. An earlier one may already be destroyed: a resize destroys
    // the render targets' views and writes the new ones before this set's turn comes round.
    std::stable_sort(pending.begin(), pending.end(),
                     [](const Pending& a, const Pending& b) { return a.slot < b.slot; });
    std::vector<VkDescriptorImageInfo> infos;
    std::vector<VkWriteDescriptorSet> writes;
    infos.reserve(pending.size());
    writes.reserve(pending.size());
    for (size_t i = 0; i < pending.size(); ++i) {
        if (i + 1 < pending.size() && pending[i + 1].slot == pending[i].slot) {
            continue;
        }
        infos.push_back({VK_NULL_HANDLE, pending[i].view, pending[i].layout});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = m_sets[frame];
        w.dstBinding = 0;
        w.dstArrayElement = pending[i].slot;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes.push_back(w);
    }
    for (size_t i = 0; i < writes.size(); ++i) {
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device.device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    pending.clear();
}

void Bindless::write(uint32_t slot, VkImageView view, VkImageLayout layout) {
    VkDescriptorImageInfo info{VK_NULL_HANDLE, view, layout};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_sets[m_frame];
    w.dstBinding = 0;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(m_device.device(), 1, &w, 0, nullptr);
    for (uint32_t f = 0; f < k_frames_in_flight; ++f) {
        if (f != m_frame) {
            m_pending[f].push_back({slot, view, layout});
        }
    }
}

} // namespace spud::render
