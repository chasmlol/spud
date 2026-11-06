#include "render/far_trees.hpp"

#include <algorithm>
#include <cstring>

namespace spud::render {

FarTrees::FarTrees(Device& device, Uploader& uploader, TextureManager& textures, std::shared_ptr<const osh::FarView> far)
    : m_device(device), m_uploader(uploader), m_textures(textures), m_far(std::move(far)) {
    const size_t bytes = m_far->trees.size() * sizeof(osh::TreeInstance);
    m_instances = m_device.create_buffer(std::max<size_t>(bytes, 16),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         MemoryUsage::gpu_only, Lifetime::asset, "tree impostors", true);
    if (bytes > 0) {
        UploadRequest req;
        req.data.resize(bytes);
        std::memcpy(req.data.data(), m_far->trees.data(), bytes);
        req.buffer_regions.push_back({m_instances.buffer, 0, 0, bytes});
        req.on_complete = [this] { m_ready = true; };
        m_uploader.submit(std::move(req));
    }
}

FarTrees::~FarTrees() {
    for (TextureId t : m_acquired) {
        m_textures.release(t);
    }
    m_device.destroy_buffer(m_instances);
    m_device.destroy_buffer(m_table);
}

void FarTrees::finish_textures() {
    std::vector<GpuImpostor> table;
    table.reserve(m_far->impostors.size());
    for (const osh::TreeImpostor& i : m_far->impostors) {
        GpuImpostor g{};
        g.colour = m_textures.acquire(i.colour, TextureKind::color, 0.0f);
        g.normal = m_textures.acquire(i.normal, TextureKind::normal, 0.0f);
        g.frames = i.frames;
        std::copy_n(i.centre, 3, g.centre);
        g.radius = i.radius;
        g.lateral = i.lateral;
        m_acquired.push_back(g.colour);
        m_acquired.push_back(g.normal);
        table.push_back(g);
    }
    m_table = m_device.create_buffer(std::max<size_t>(table.size(), 1) * sizeof(GpuImpostor),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryUsage::dynamic, Lifetime::asset,
                                     "tree impostor table");
    std::memcpy(m_table.mapped, table.data(), table.size() * sizeof(GpuImpostor));
}

} // namespace spud::render
