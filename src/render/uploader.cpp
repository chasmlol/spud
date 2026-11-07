#include "render/uploader.hpp"
#include "core/sampler.hpp"

#include "core/log.hpp"
#include "core/platform.hpp"

#include <algorithm>
#include <cstring>

namespace spud::render {

namespace {

size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

} // namespace

Uploader::Uploader(Device& device, size_t chunk_size, uint32_t chunk_count)
    : m_device(device), m_chunk_size(chunk_size) {
    VkDevice dev = device.device();
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device.transfer_family();
    vk_check(vkCreateCommandPool(dev, &pool_info, nullptr, &m_pool));

    VkSemaphoreTypeCreateInfo type_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue = 0;
    VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sem_info.pNext = &type_info;
    vk_check(vkCreateSemaphore(dev, &sem_info, nullptr, &m_timeline));
    device.set_name(reinterpret_cast<uint64_t>(m_timeline), VK_OBJECT_TYPE_SEMAPHORE, "upload timeline");

    m_chunks.resize(chunk_count);
    for (auto& c : m_chunks) {
        c.staging = device.create_buffer(chunk_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::upload,
                                         Lifetime::transfer, "staging chunk");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = m_pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vk_check(vkAllocateCommandBuffers(dev, &alloc, &c.cmd));
    }
    m_thread = std::thread([this] { thread_main(); });
    platform::set_thread(m_thread.native_handle(), "spud upload", platform::ThreadPriority::below_normal);
}

Uploader::~Uploader() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
    VkDevice dev = m_device.device();
    // Wait for everything submitted, then drop pending callbacks (owners are shutting down).
    const uint64_t last = m_next_value - 1;
    if (last > 0) {
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait.semaphoreCount = 1;
        wait.pSemaphores = &m_timeline;
        wait.pValues = &last;
        vkWaitSemaphores(dev, &wait, UINT64_MAX);
    }
    for (auto& c : m_chunks) {
        m_device.destroy_buffer(c.staging);
        for (auto& b : c.oversized) {
            m_device.destroy_buffer(b);
        }
    }
    vkDestroyCommandPool(dev, m_pool, nullptr);
    vkDestroySemaphore(dev, m_timeline, nullptr);
}

void Uploader::submit(UploadRequest&& request) {
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back(std::move(request));
    }
    m_cv.notify_one();
}

size_t Uploader::busy() const {
    std::lock_guard lock(m_mutex);
    return m_queue.size() + m_processing + m_submitted.size();
}

UploadStats Uploader::stats() const {
    UploadStats s;
    std::lock_guard lock(m_mutex);
    s.queued = m_queue.size() + m_processing;
    for (const auto& sub : m_submitted) {
        if (sub.value <= m_observed_value) {
            ++s.completed;
        } else {
            ++s.in_flight;
        }
    }
    s.total_bytes = m_total_bytes;
    s.bytes_last_poll = m_bytes_last_poll;
    return s;
}

void Uploader::poll(size_t max_callbacks, size_t max_bytes) {
    uint64_t value = 0;
    vk_check(vkGetSemaphoreCounterValue(m_device.device(), m_timeline, &value));

    // Pop completed entries under the lock, run callbacks outside it.
    std::vector<Submitted> done;
    size_t bytes = 0;
    {
        std::lock_guard lock(m_mutex);
        while (!m_submitted.empty() && m_submitted.front().value <= value && done.size() < max_callbacks &&
               (done.empty() || bytes + m_submitted.front().bytes <= max_bytes)) {
            bytes += m_submitted.front().bytes;
            done.push_back(std::move(m_submitted.front()));
            m_submitted.pop_front();
        }
    }
    for (auto& d : done) {
        // Only claim visibility of what we actually consumed: the graphics
        // queue waits on the highest value whose callbacks have run.
        m_observed_value = std::max(m_observed_value, d.value);
        if (d.on_complete) {
            d.on_complete();
        }
    }
    m_total_bytes += bytes;
    m_bytes_last_poll = bytes;
}

void Uploader::record_request(VkCommandBuffer cmd, VkBuffer staging, size_t offset, const UploadRequest& r) {
    for (const auto& br : r.buffer_regions) {
        if (br.size == 0) {
            continue; // an empty copy is invalid usage, and what a driver does with it is anyone's guess
        }
        VkBufferCopy copy{offset + br.src_offset, br.dst_offset, br.size};
        vkCmdCopyBuffer(cmd, staging, br.dst, 1, &copy);
    }
    if (r.image != VK_NULL_HANDLE) {
        VkImageMemoryBarrier2 pre{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        pre.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        pre.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        pre.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        pre.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        pre.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pre.image = r.image;
        pre.subresourceRange = r.image_range;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &pre;
        vkCmdPipelineBarrier2(cmd, &dep);

        std::vector<VkBufferImageCopy> regions = r.image_regions;
        for (auto& reg : regions) {
            reg.bufferOffset += offset;
        }
        vkCmdCopyBufferToImage(cmd, staging, r.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(regions.size()), regions.data());

        // The graphics queue's semaphore wait provides the dependency to shader reads.
        VkImageMemoryBarrier2 post = pre;
        post.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        post.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        post.dstAccessMask = VK_ACCESS_2_NONE;
        post.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        post.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dep.pImageMemoryBarriers = &post;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

void Uploader::thread_main() {
    sampler::catch_terminate();
    VkDevice dev = m_device.device();
    uint32_t chunk_index = 0;
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop) {
                return;
            }
        }

        Chunk& chunk = m_chunks[chunk_index];
        chunk_index = (chunk_index + 1) % static_cast<uint32_t>(m_chunks.size());
        if (chunk.value > 0) {
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wait.semaphoreCount = 1;
            wait.pSemaphores = &m_timeline;
            wait.pValues = &chunk.value;
            vk_check(vkWaitSemaphores(dev, &wait, UINT64_MAX));
        }
        for (auto& b : chunk.oversized) {
            m_device.destroy_buffer(b);
        }
        chunk.oversized.clear();

        // Take as many requests as fit in this chunk (an oversized one goes alone).
        std::vector<UploadRequest> batch;
        size_t used = 0;
        {
            std::lock_guard lock(m_mutex);
            while (!m_queue.empty()) {
                const size_t need = align_up(m_queue.front().data.size(), 16);
                if (!batch.empty() && used + need > m_chunk_size) {
                    break;
                }
                used += need;
                batch.push_back(std::move(m_queue.front()));
                m_queue.pop_front();
                if (used >= m_chunk_size) {
                    break;
                }
            }
            m_processing = batch.size();
        }
        if (batch.empty()) {
            continue;
        }

        vk_check(vkResetCommandBuffer(chunk.cmd, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(chunk.cmd, &begin));
        size_t offset = 0;
        auto* base = chunk.staging.as<std::byte>();
        for (auto& r : batch) {
            if (r.data.size() > m_chunk_size) {
                auto big = m_device.create_buffer(r.data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                  MemoryUsage::upload, Lifetime::transfer, "oversized staging");
                std::memcpy(big.mapped, r.data.data(), r.data.size());
                record_request(chunk.cmd, big.buffer, 0, r);
                chunk.oversized.push_back(big);
            } else {
                std::memcpy(base + offset, r.data.data(), r.data.size());
                record_request(chunk.cmd, chunk.staging.buffer, offset, r);
                offset += align_up(r.data.size(), 16);
            }
        }
        vk_check(vkEndCommandBuffer(chunk.cmd));

        const uint64_t value = m_next_value++;
        VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cmd_info.commandBuffer = chunk.cmd;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = m_timeline;
        signal.value = value;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_info;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        if (m_device.transfer_shares_graphics_queue()) {
            std::lock_guard qlock(m_device.queue_mutex());
            vk_check(vkQueueSubmit2(m_device.transfer_queue(), 1, &submit, VK_NULL_HANDLE));
        } else {
            std::lock_guard qlock(m_device.transfer_mutex());
            vk_check(vkQueueSubmit2(m_device.transfer_queue(), 1, &submit, VK_NULL_HANDLE));
        }
        chunk.value = value;

        std::lock_guard lock(m_mutex);
        for (auto& r : batch) {
            m_submitted.push_back({value, r.data.size(), std::move(r.on_complete)});
        }
        m_processing = 0;
    }
}

} // namespace spud::render
