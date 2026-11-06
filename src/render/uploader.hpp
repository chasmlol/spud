#pragma once

// Asynchronous GPU uploads on the transfer queue.
//
// Any thread submits an UploadRequest (CPU bytes + destination regions). A
// dedicated upload thread copies the bytes into a chunked staging ring,
// records the copies (and image layout transitions) on the transfer queue and
// signals a timeline semaphore. The render thread polls that semaphore once a
// frame, runs the completion callbacks of finished uploads under a per-frame
// budget (that is where resources become visible to draws) and makes its next
// graphics submit wait on the timeline value it has observed, which is
// already signalled and therefore never stalls.

#include "render/device.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace spud::render {

struct UploadRequest {
    std::vector<std::byte> data;

    struct BufferRegion {
        VkBuffer dst;
        VkDeviceSize dst_offset;
        size_t src_offset; // into data
        size_t size;
    };
    std::vector<BufferRegion> buffer_regions;

    // Optional image destination: regions' bufferOffset are relative to `data`.
    VkImage image = VK_NULL_HANDLE;
    VkImageSubresourceRange image_range{};
    std::vector<VkBufferImageCopy> image_regions;

    // Runs on the render thread (from poll) once the copy has completed on the GPU.
    std::function<void()> on_complete;
};

struct UploadStats {
    size_t queued = 0;       // waiting for the upload thread
    size_t in_flight = 0;    // submitted, not yet completed
    size_t completed = 0;    // completed, callbacks not yet run
    uint64_t total_bytes = 0;
    uint64_t bytes_last_poll = 0; // bytes whose completions ran in the last poll()
};

class Uploader {
public:
    explicit Uploader(Device& device, size_t chunk_size = 32ull << 20, uint32_t chunk_count = 8);
    ~Uploader();
    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;

    // Thread-safe.
    void submit(UploadRequest&& request);

    // Render thread: runs completion callbacks for finished uploads, at most
    // `max_callbacks` and roughly `max_bytes` per call.
    void poll(size_t max_callbacks, size_t max_bytes);

    // The graphics submit must wait on this semaphore at wait_value() (already reached).
    VkSemaphore timeline() const { return m_timeline; }
    uint64_t wait_value() const { return m_observed_value; }

    // Requests not yet fully processed (queued, in flight or awaiting their callback).
    size_t busy() const;
    UploadStats stats() const;

private:
    struct Chunk {
        Buffer staging;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint64_t value = 0;              // timeline value of the last batch that used it
        std::vector<Buffer> oversized;   // dedicated staging for requests larger than a chunk
    };
    struct Submitted {
        uint64_t value;
        size_t bytes;
        std::function<void()> on_complete;
    };

    void thread_main();
    void record_request(VkCommandBuffer cmd, VkBuffer staging, size_t offset, const UploadRequest& r);

    Device& m_device;
    size_t m_chunk_size;
    std::vector<Chunk> m_chunks;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    uint64_t m_next_value = 1;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<UploadRequest> m_queue;
    std::deque<Submitted> m_submitted; // ordered by value
    size_t m_processing = 0;           // taken by the upload thread, not yet in m_submitted
    bool m_stop = false;
    std::thread m_thread;

    uint64_t m_observed_value = 0; // render thread
    uint64_t m_total_bytes = 0;
    uint64_t m_bytes_last_poll = 0;
};

} // namespace spud::render
