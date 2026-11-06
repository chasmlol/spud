#include "render/jobs.hpp"
#include "core/sampler.hpp"

#include "core/log.hpp"
#include "core/platform.hpp"

#include <algorithm>

namespace spud::render {

JobSystem::JobSystem(unsigned thread_count) {
    if (thread_count == 0) {
        // Half the hardware threads: with every logical core streaming, the main thread
        // shared cores (and SMT siblings, memory bandwidth and driver locks) with the workers and frames
        // stalled for milliseconds during heavy loading. Content just streams in a little more slowly.
        const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
        thread_count = std::clamp(hw / 2, 2u, 16u);
    }
    for (unsigned i = 0; i < thread_count; ++i) {
        m_threads.emplace_back([this] { worker(); });
        // Streaming must never compete with the render thread.
        platform::set_thread(m_threads.back().native_handle(), "spud worker", platform::ThreadPriority::below_normal);
    }
    log::info("jobs: {} worker threads", thread_count);
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        // Drop work that has not started; owners are shutting down.
        while (!m_queue.empty()) {
            m_queue.pop();
            m_pending.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    m_cv.notify_all();
    for (auto& t : m_threads) {
        t.join();
    }
}

void JobSystem::submit(float priority, std::function<void()> job) {
    m_pending.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(m_mutex);
        m_queue.push({priority, m_sequence++, std::move(job)});
    }
    m_cv.notify_one();
}

void JobSystem::wait_idle() {
    std::unique_lock lock(m_mutex);
    m_idle_cv.wait(lock, [this] { return m_pending.load(std::memory_order_acquire) == 0; });
}

void JobSystem::worker() {
    sampler::catch_terminate();
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop && m_queue.empty()) {
                return;
            }
            job = std::move(const_cast<Job&>(m_queue.top()));
            m_queue.pop();
        }
        try {
            job.fn();
        } catch (const std::exception& e) {
            log::error("jobs: unhandled exception: {}", e.what());
        }
        if (m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard lock(m_mutex);
            m_idle_cv.notify_all();
        }
    }
}

} // namespace spud::render
