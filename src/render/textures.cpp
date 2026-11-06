#include "render/textures.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "formats/dds.hpp"

#include <algorithm>
#include <cstring>

namespace spud::render {

namespace {

struct VkFormatInfo {
    VkFormat format;
    VkComponentMapping swizzle;
};

constexpr VkComponentMapping k_identity = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

// UNORM everywhere: Skyrim lights in gamma space, so no sRGB decode on sampling.
VkFormatInfo vk_format(dds::Format f, TextureKind kind) {
    using S = VkComponentSwizzle;
    switch (f) {
    case dds::Format::bc1: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, k_identity};
    case dds::Format::bc2: return {VK_FORMAT_BC2_UNORM_BLOCK, k_identity};
    case dds::Format::bc3: return {VK_FORMAT_BC3_UNORM_BLOCK, k_identity};
    case dds::Format::bc4: return {VK_FORMAT_BC4_UNORM_BLOCK, {S::VK_COMPONENT_SWIZZLE_R, S::VK_COMPONENT_SWIZZLE_R,
                                                                S::VK_COMPONENT_SWIZZLE_R, S::VK_COMPONENT_SWIZZLE_ONE}};
    case dds::Format::bc4_snorm:
        return {VK_FORMAT_BC4_SNORM_BLOCK, {S::VK_COMPONENT_SWIZZLE_R, S::VK_COMPONENT_SWIZZLE_R,
                                            S::VK_COMPONENT_SWIZZLE_R, S::VK_COMPONENT_SWIZZLE_ONE}};
    case dds::Format::bc5:
        // Two-channel normal maps: z is approximated by the shader's normalise.
        return {VK_FORMAT_BC5_UNORM_BLOCK,
                {S::VK_COMPONENT_SWIZZLE_R, S::VK_COMPONENT_SWIZZLE_G,
                 kind == TextureKind::normal ? S::VK_COMPONENT_SWIZZLE_ONE : S::VK_COMPONENT_SWIZZLE_ZERO,
                 S::VK_COMPONENT_SWIZZLE_ONE}};
    case dds::Format::bc5_snorm:
        return {VK_FORMAT_BC5_SNORM_BLOCK, {S::VK_COMPONENT_SWIZZLE_R, S::VK_COMPONENT_SWIZZLE_G,
                                            S::VK_COMPONENT_SWIZZLE_ONE, S::VK_COMPONENT_SWIZZLE_ONE}};
    case dds::Format::bc6h_ufloat: return {VK_FORMAT_BC6H_UFLOAT_BLOCK, k_identity};
    case dds::Format::bc6h_sfloat: return {VK_FORMAT_BC6H_SFLOAT_BLOCK, k_identity};
    case dds::Format::bc7: return {VK_FORMAT_BC7_UNORM_BLOCK, k_identity};
    case dds::Format::rgba8: return {VK_FORMAT_R8G8B8A8_UNORM, k_identity};
    case dds::Format::bgra8: return {VK_FORMAT_B8G8R8A8_UNORM, k_identity};
    case dds::Format::bgrx8:
        return {VK_FORMAT_B8G8R8A8_UNORM, {S::VK_COMPONENT_SWIZZLE_IDENTITY, S::VK_COMPONENT_SWIZZLE_IDENTITY,
                                           S::VK_COMPONENT_SWIZZLE_IDENTITY, S::VK_COMPONENT_SWIZZLE_ONE}};
    case dds::Format::r8: return {VK_FORMAT_R8_UNORM, k_identity};
    case dds::Format::rg8: return {VK_FORMAT_R8G8_UNORM, k_identity};
    case dds::Format::l8:
        return {VK_FORMAT_R8_UNORM, {S::VK_COMPONENT_SWIZZLE_R, S::VK_COMPONENT_SWIZZLE_R, S::VK_COMPONENT_SWIZZLE_R,
                                     S::VK_COMPONENT_SWIZZLE_ONE}};
    case dds::Format::l8a8:
        return {VK_FORMAT_R8G8_UNORM, {S::VK_COMPONENT_SWIZZLE_R, S::VK_COMPONENT_SWIZZLE_R,
                                       S::VK_COMPONENT_SWIZZLE_R, S::VK_COMPONENT_SWIZZLE_G}};
    case dds::Format::a8:
        return {VK_FORMAT_R8_UNORM, {S::VK_COMPONENT_SWIZZLE_ONE, S::VK_COMPONENT_SWIZZLE_ONE,
                                     S::VK_COMPONENT_SWIZZLE_ONE, S::VK_COMPONENT_SWIZZLE_R}};
    }
    return {VK_FORMAT_R8G8B8A8_UNORM, k_identity};
}

size_t align16(size_t v) { return (v + 15) & ~size_t(15); }

} // namespace

TextureManager::TextureManager(Device& device, Bindless& bindless, Uploader& uploader, JobSystem& jobs,
                               std::filesystem::path root)
    : m_device(device), m_bindless(bindless), m_uploader(uploader), m_jobs(jobs), m_root(std::move(root)) {
    m_entries.reserve(Bindless::k_texture_slots);

    const uint32_t white = 0xFFFFFFFF;
    const uint32_t black = 0xFF000000;
    const uint32_t grey = 0xFF808080;
    const uint32_t flat = 0xFFFF8080; // (128, 128, 255) in RGBA little-endian
    const uint32_t facegen_detail = 0x40404040;
    uint32_t checker[8 * 8];
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            checker[y * 8 + x] = ((x ^ y) & 1) ? 0xFFFF00FF : 0xFF200020;
        }
    }
    m_white = create_builtin(1, 1, &white);
    m_black = create_builtin(1, 1, &black);
    m_grey = create_builtin(1, 1, &grey);
    m_flat_normal = create_builtin(1, 1, &flat);
    m_facegen_detail = create_builtin(1, 1, &facegen_detail);
    m_missing = create_builtin(8, 8, checker);
    const uint32_t black_faces[6] = {black, black, black, black, black, black};
    m_black_cube = create_builtin(1, 1, black_faces, true);
    m_bindless.fill_all(m_entries[m_grey].view);
    for (TextureId id :
         {m_white, m_black, m_grey, m_flat_normal, m_missing, m_black_cube, m_facegen_detail}) {
        m_bindless.write(id, m_entries[id].view);
    }
}

TextureManager::~TextureManager() {
    // The renderer has drained the GPU and the job system before this runs.
    VkDevice dev = m_device.device();
    for (auto& e : m_entries) {
        if (e.state == State::external) {
            continue; // the owner destroys it
        }
        if (e.view) {
            vkDestroyImageView(dev, e.view, nullptr);
        }
        if (e.image) {
            vmaDestroyImage(m_device.allocator(), e.image, e.allocation);
        }
    }
    if (m_pool) {
        vmaDestroyPool(m_device.allocator(), m_pool);
    }
}

void TextureManager::reserve(uint64_t bytes) {
    // All sampled 2D optimal images share a memory type on desktop GPUs; the game it with a typical one.
    VkImageCreateInfo probe{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    probe.imageType = VK_IMAGE_TYPE_2D;
    probe.format = VK_FORMAT_BC7_UNORM_BLOCK;
    probe.extent = {1024, 1024, 1};
    probe.mipLevels = 11;
    probe.arrayLayers = 1;
    probe.samples = VK_SAMPLE_COUNT_1_BIT;
    probe.tiling = VK_IMAGE_TILING_OPTIMAL;
    probe.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    m_device.set_shared(probe);
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    uint32_t type = 0;
    vk_check(vmaFindMemoryTypeIndexForImageInfo(m_device.allocator(), &probe, &ai, &type));
    constexpr VkDeviceSize k_block = 512ull << 20;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = k_block;
    pi.minBlockCount = static_cast<size_t>((bytes + k_block - 1) / k_block);
    pi.priority = 0.5f;
    vk_check(vmaCreatePool(m_device.allocator(), &pi, &m_pool));
    const VkPhysicalDeviceMemoryProperties* mem = nullptr;
    vmaGetMemoryProperties(m_device.allocator(), &mem);
    const auto& mt = mem->memoryTypes[type];
    log::info("textures: {} MiB reserved in {} blocks (memory type {}, flags 0x{:x}, heap {} flags 0x{:x})",
              (pi.minBlockCount * k_block) >> 20, pi.minBlockCount, type, mt.propertyFlags, mt.heapIndex,
              mem->memoryHeaps[mt.heapIndex].flags);
}

TextureId TextureManager::create_builtin(uint32_t w, uint32_t h, const uint32_t* rgba, bool cube) {
    const TextureId id = allocate_slot();
    Entry& e = m_entries[id];
    e.state = State::builtin;
    e.refs = 1;

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = cube ? 6 : 1;
    info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vk_check(m_device.create_image(info, alloc, Lifetime::asset, "builtin texture", e.image, e.allocation));
    const uint32_t layers = info.arrayLayers;

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = e.image;
    view.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vk_check(vkCreateImageView(m_device.device(), &view, nullptr, &e.view));

    const size_t size = size_t(w) * h * 4 * layers;
    auto staging = m_device.create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::upload, Lifetime::transfer, "builtin staging");
    std::memcpy(staging.mapped, rgba, size);
    m_device.immediate([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = e.image;
        b.subresourceRange = view.subresourceRange;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
        region.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    m_device.destroy_buffer(staging);
    return id;
}

TextureId TextureManager::allocate_slot() {
    if (!m_free_slots.empty()) {
        const TextureId id = m_free_slots.back();
        m_free_slots.pop_back();
        m_entries[id] = Entry{};
        return id;
    }
    if (m_entries.size() >= Bindless::k_texture_slots) {
        return k_invalid_slot;
    }
    m_entries.emplace_back();
    return static_cast<TextureId>(m_entries.size() - 1);
}

TextureId TextureManager::register_view(VkImageView view) {
    const TextureId id = allocate_slot();
    if (id == k_invalid_slot) {
        return m_missing;
    }
    Entry& e = m_entries[id];
    e.state = State::external;
    e.refs = 1;
    e.view = view;
    m_bindless.write(id, view);
    return id;
}

void TextureManager::unregister_view(TextureId id) {
    if (id >= m_entries.size() || m_entries[id].state != State::external) {
        return;
    }
    m_bindless.write(id, fallback_view(TextureKind::color));
    m_entries[id] = Entry{};
    m_free_slots.push_back(id);
}

VkImageView TextureManager::fallback_view(TextureKind kind) const {
    switch (kind) {
    case TextureKind::normal: return m_entries[m_flat_normal].view;
    case TextureKind::cube: return m_entries[m_black_cube].view;
    default: return m_entries[m_grey].view;
    }
}

void TextureManager::provide(std::string path, std::vector<std::byte> file) {
    std::lock_guard lock(m_provided_mutex);
    m_provided.emplace(std::move(path), std::move(file));
}

TextureId TextureManager::acquire(std::string_view path, TextureKind kind, float priority) {
    if (path.empty()) {
        return kind == TextureKind::normal ? m_flat_normal : kind == TextureKind::cube ? m_black_cube : m_white;
    }
    // A cube view and a 2D view of one file are different descriptors.
    std::string key = kind == TextureKind::cube ? "cube:" + std::string(path) : std::string(path);
    if (auto it = m_by_path.find(key); it != m_by_path.end()) {
        Entry& e = m_entries[it->second];
        if (e.refs++ == 0 && e.in_lru) {
            m_lru.erase(e.lru);
            e.in_lru = false;
        }
        if (e.state == State::queued && priority < e.priority) {
            e.priority = priority;
            m_queue.push({priority, m_sequence++, it->second});
        }
        return it->second;
    }
    const TextureId id = allocate_slot();
    if (id == k_invalid_slot) {
        log::warn("textures: all {} bindless slots in use, {} uses a fallback", Bindless::k_max_textures, path);
        return kind == TextureKind::normal ? m_flat_normal : kind == TextureKind::cube ? m_black_cube : m_missing;
    }
    Entry& e = m_entries[id];
    e.path = std::move(key);
    e.kind = kind;
    e.state = State::queued;
    e.refs = 1;
    e.priority = priority;
    m_by_path.emplace(e.path, id);
    m_bindless.write(id, fallback_view(kind));
    m_queue.push({priority, m_sequence++, id});
    ++m_queued;
    return id;
}

void TextureManager::release(TextureId id) {
    Entry& e = m_entries[id];
    if (e.state == State::builtin || e.refs == 0) {
        return;
    }
    if (--e.refs == 0 && (e.state == State::ready || e.state == State::missing)) {
        touch_lru(id);
    }
}

void TextureManager::touch_lru(TextureId id) {
    Entry& e = m_entries[id];
    if (e.in_lru) {
        m_lru.erase(e.lru);
    }
    m_lru.push_back(id);
    e.lru = std::prev(m_lru.end());
    e.in_lru = true;
}

void TextureManager::update() {
    {
        std::vector<Failure> failures;
        {
            std::lock_guard lock(m_fail_mutex);
            failures.swap(m_failures);
        }
        for (const auto& f : failures) {
            fail_load(f.id, f.error, f.not_found);
        }
    }

    // Bounded in flight: workers never get far ahead of the uploader, so
    // staging memory stays small and priorities stay fresh.
    while (m_in_flight < m_max_in_flight && !m_queue.empty()) {
        const QueueItem item = m_queue.top();
        m_queue.pop();
        Entry& e = m_entries[item.id];
        if (e.state != State::queued || item.priority != e.priority) {
            continue; // stale duplicate
        }
        --m_queued;
        if (e.refs == 0) {
            // Nobody wants it any more: drop it before doing any work.
            m_by_path.erase(e.path);
            e = Entry{};
            m_free_slots.push_back(item.id);
            continue;
        }
        e.state = State::loading;
        ++m_in_flight;
        m_jobs.submit(item.priority, [this, id = item.id, path = e.path, kind = e.kind] { load_job(id, path, kind); });
    }
    evict_over_budget();
}

void TextureManager::load_job(TextureId id, std::string path, TextureKind kind) {
    try {
        if (kind == TextureKind::cube && path.starts_with("cube:")) {
            path.erase(0, 5);
        }
        const auto full = m_root / path;
        std::vector<std::byte> file;
        {
            std::lock_guard lock(m_provided_mutex);
            if (const auto it = m_provided.find(path); it != m_provided.end()) {
                file = std::move(it->second);
                m_provided.erase(it);
            }
        }
        std::error_code ec;
        if (file.empty() && !std::filesystem::exists(full, ec)) {
            std::lock_guard lock(m_fail_mutex);
            m_failures.push_back({id, "file not found", true});
            return;
        }
        if (file.empty()) {
            file = read_file(full);
        }
        const auto image = dds::parse(file);
        const auto fmt = vk_format(image.format, kind);

        // Cube maps load all six faces for TextureKind::cube; otherwise only layer 0 of arrays and cubes.
        const bool cube = kind == TextureKind::cube;
        if (cube && !image.cubemap) {
            throw Error("not a cube map");
        }
        const uint32_t layers = cube ? 6 : 1;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        info.format = fmt.format;
        info.extent = {image.width, image.height, 1};
        info.mipLevels = image.mip_count;
        info.arrayLayers = layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        m_device.set_shared(info);
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        // Block-compressed textures (nearly all game data) go into the reserved pool. Uncompressed
        // ones sample far slower from pool blocks on NVIDIA (measured ~75x), so they use VMA's
        // default heaps; they are rare in Skyrim data.
        alloc.pool = dds::is_block_compressed(image.format) ? m_pool : nullptr;
        VkImage vk_image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VmaAllocationInfo alloc_info{};
        if (m_device.create_image(info, alloc, Lifetime::asset, nullptr, vk_image, allocation, &alloc_info) !=
            VK_SUCCESS) {
            // Incompatible memory type (unusual format) or pool exhausted: default heap.
            alloc.pool = nullptr;
            vk_check(m_device.create_image(info, alloc, Lifetime::asset, nullptr, vk_image, allocation, &alloc_info));
        }

        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = vk_image;
        view_info.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = fmt.format;
        view_info.components = fmt.swizzle;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.mip_count, 0, layers};
        VkImageView view = VK_NULL_HANDLE;
        vk_check(vkCreateImageView(m_device.device(), &view_info, nullptr, &view));

        UploadRequest req;
        size_t total = 0;
        for (uint32_t l = 0; l < layers; ++l) {
            for (uint32_t m = 0; m < image.mip_count; ++m) {
                total += align16(image.subresource(l, m).size);
            }
        }
        req.data.resize(total);
        size_t offset = 0;
        for (uint32_t l = 0; l < layers; ++l) {
            for (uint32_t m = 0; m < image.mip_count; ++m) {
                const auto& sub = image.subresource(l, m);
                std::memcpy(req.data.data() + offset, image.data.data() + sub.offset, sub.size);
                VkBufferImageCopy region{};
                region.bufferOffset = offset;
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, l, 1};
                region.imageExtent = {sub.width, sub.height, 1};
                req.image_regions.push_back(region);
                offset += align16(sub.size);
            }
        }
        req.image = vk_image;
        req.image_range = view_info.subresourceRange;
        const uint64_t bytes = alloc_info.size;
        req.on_complete = [this, id, vk_image, allocation, view, bytes] {
            finish_load(id, vk_image, allocation, view, bytes);
        };
        m_uploader.submit(std::move(req));
    } catch (const std::exception& ex) {
        std::lock_guard lock(m_fail_mutex);
        m_failures.push_back({id, ex.what(), false});
    }
}

void TextureManager::finish_load(TextureId id, VkImage image, VmaAllocation allocation, VkImageView view,
                                 uint64_t bytes) {
    Entry& e = m_entries[id];
    --m_in_flight;
    e.state = State::ready;
    e.image = image;
    e.allocation = allocation;
    e.view = view;
    e.bytes = bytes;
    m_gpu_bytes += bytes;
    m_bindless.write(id, view);
    if (e.refs == 0) {
        touch_lru(id);
    }
}

void TextureManager::fail_load(TextureId id, const std::string& error, bool not_found) {
    Entry& e = m_entries[id];
    --m_in_flight;
    e.state = State::missing;
    if (m_logged_missing.insert(e.path).second) {
        if (not_found) {
            log::warn("textures: missing {}", e.path);
        } else {
            log::warn("textures: cannot load {}: {}", e.path, error);
        }
    }
    m_bindless.write(id, e.kind == TextureKind::normal ? m_entries[m_flat_normal].view
                         : e.kind == TextureKind::cube ? m_entries[m_black_cube].view
                                                       : m_entries[m_missing].view);
    if (e.refs == 0) {
        touch_lru(id);
    }
}

void TextureManager::evict_over_budget() {
    // Small per-frame cap keeps eviction cost flat.
    int budget = 64;
    auto it = m_lru.begin();
    while (m_gpu_bytes > m_budget && it != m_lru.end() && budget-- > 0) {
        const TextureId id = *it;
        Entry& e = m_entries[id];
        if (e.state != State::ready) {
            ++it; // keep "missing" entries so we don't retry them
            continue;
        }
        it = m_lru.erase(it);
        m_gpu_bytes -= e.bytes;
        m_bindless.write(id, fallback_view(e.kind));
        m_device.defer([dev = m_device.device(), alloc = m_device.allocator(), image = e.image,
                        allocation = e.allocation, view = e.view] {
            vkDestroyImageView(dev, view, nullptr);
            vmaDestroyImage(alloc, image, allocation);
        });
        m_by_path.erase(e.path);
        e = Entry{};
        m_free_slots.push_back(id);
    }
}

TextureStats TextureManager::stats() const {
    TextureStats s;
    for (const auto& e : m_entries) {
        switch (e.state) {
        case State::free:
        case State::builtin:
            continue;
        case State::queued:
        case State::loading:
            ++s.loading;
            break;
        case State::ready:
            ++s.ready;
            break;
        case State::missing:
            ++s.missing;
            break;
        }
        ++s.live;
        if (e.refs > 0) {
            ++s.referenced;
        }
    }
    s.gpu_bytes = m_gpu_bytes;
    return s;
}

} // namespace spud::render
