#pragma once

// Small priority thread pool for streaming work (file IO, parsing, vertex
// generation). Lower priority values run first; ties run in submit order.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace spud::render {

class JobSystem {
public:
    explicit JobSystem(unsigned thread_count = 0); // 0 = hardware threads - 1
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void submit(float priority, std::function<void()> job);

    // Queued + running jobs.
    size_t pending() const { return m_pending.load(std::memory_order_acquire); }
    unsigned thread_count() const { return static_cast<unsigned>(m_threads.size()); }
    // Blocks until every submitted job has finished (shutdown / tests).
    void wait_idle();

private:
    struct Job {
        float priority;
        uint64_t sequence;
        std::function<void()> fn;
        bool operator<(const Job& o) const {
            // std::priority_queue pops the largest element: invert.
            if (priority != o.priority) {
                return priority > o.priority;
            }
            return sequence > o.sequence;
        }
    };

    void worker();

    std::vector<std::thread> m_threads;
    std::priority_queue<Job> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_idle_cv;
    std::atomic<size_t> m_pending{0};
    uint64_t m_sequence = 0;
    bool m_stop = false;
};

} // namespace spud::render
