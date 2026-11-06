#pragma once

// Every tree of a world as an impostor: all placements resident on the GPU, sorted by cell,
// so a frame's trees are a few runs of one instanced quad.

#include "formats/osh.hpp"
#include "render/device.hpp"
#include "render/textures.hpp"
#include "render/uploader.hpp"

#include <memory>
#include <vector>

namespace spud::render {

// osh::TreeImpostor as the shaders read it.
struct GpuImpostor {
    uint32_t colour;
    uint32_t normal;
    uint32_t frames;
    float lateral;
    float centre[3];
    float radius;
};
static_assert(sizeof(GpuImpostor) == 32);

class FarTrees {
public:
    FarTrees(Device& device, Uploader& uploader, TextureManager& textures, std::shared_ptr<const osh::FarView> far);
    ~FarTrees();
    FarTrees(const FarTrees&) = delete;
    FarTrees& operator=(const FarTrees&) = delete;

    // Render thread: acquires the atlases.
    void finish_textures();
    bool ready() const { return m_ready; }
    VkDeviceAddress instances_address() const { return m_instances.address; }
    VkDeviceAddress table_address() const { return m_table.address; }

private:
    Device& m_device;
    Uploader& m_uploader;
    TextureManager& m_textures;
    std::shared_ptr<const osh::FarView> m_far;
    Buffer m_instances;
    Buffer m_table;
    std::vector<TextureId> m_acquired;
    bool m_ready = false;
};

} // namespace spud::render
