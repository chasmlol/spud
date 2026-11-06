#include "core/parallel.hpp"

#include "core/platform.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace spud::parallel {

namespace {

class Pool {
public:
    Pool() {
        // Room for the main thread, the player's and the driver's. On the dev machine: 8 cores, 5 here.
        const unsigned cores = std::max(std::thread::hardware_concurrency() / 2, 1u);
        const unsigned count = cores > 3 ? std::min(cores - 3, 6u) : 0u;
        for (unsigned i = 0; i < count; ++i) {
            m_threads.emplace_back([this, i] { run(i); });
        }
    }
    ~Pool() {
        m_stop = true;
        m_generation.fetch_add(1);
        m_generation.notify_all();
        for (auto& t : m_threads) {
            t.join();
        }
    }

    void for_each(size_t n, const std::function<void(size_t)>& fn) {
        wait();
        if (n == 0) {
            return;
        }
        if (m_threads.empty() || n == 1) {
            for (size_t i = 0; i < n; ++i) {
                fn(i);
            }
            return;
        }
        launch(n, &fn);
        wait();
    }

    void start(size_t n, std::function<void(size_t)> fn) {
        wait();
        m_background = std::move(fn);
        if (m_threads.empty()) {
            for (size_t i = 0; i < n; ++i) {
                m_background(i);
            }
            return;
        }
        if (n != 0) {
            launch(n, &m_background);
        }
    }

    void wait() {
        if (m_running) {
            work(); // what is left, here rather than idle
            // The workers are already on it: this is at most the last item's length.
            while (m_active.load(std::memory_order_acquire) != 0) {
                std::this_thread::yield();
            }
            m_running = false;
        }
    }

private:
    void launch(size_t n, const std::function<void(size_t)>* fn) {
        m_fn = fn;
        m_count = n;
        m_next = 0;
        m_active.store(unsigned(m_threads.size()));
        m_running = true;
        m_generation.fetch_add(1);
        m_generation.notify_all();
    }

    void work() {
        for (size_t i = m_next.fetch_add(1); i < m_count; i = m_next.fetch_add(1)) {
            (*m_fn)(i);
        }
    }

    void run(unsigned index) {
        platform::set_thread(nullptr, ("spud parallel " + std::to_string(index)).c_str(), platform::ThreadPriority::highest);
        uint64_t seen = 0;
        for (;;) {
            // A short spin catches back-to-back loops without a sleep and wake.
            for (int spin = 0; spin < 2000 && m_generation.load(std::memory_order_acquire) == seen; ++spin) {
                std::this_thread::yield();
            }
            m_generation.wait(seen);
            seen = m_generation.load(std::memory_order_acquire);
            if (m_stop) {
                return;
            }
            work();
            m_active.fetch_sub(1, std::memory_order_release);
        }
    }

    std::vector<std::thread> m_threads;
    std::atomic<uint64_t> m_generation{0};
    std::atomic<size_t> m_next{0};
    std::atomic<unsigned> m_active{0};
    std::atomic<bool> m_stop{false};
    const std::function<void(size_t)>* m_fn = nullptr;
    size_t m_count = 0;
    std::function<void(size_t)> m_background;
    bool m_running = false; // the calling thread's: a loop has started and not been waited for
};

} // namespace

namespace {
Pool& pool() {
    static Pool p;
    return p;
}
} // namespace

void for_each(size_t n, const std::function<void(size_t)>& fn) { pool().for_each(n, fn); }
void start(size_t n, std::function<void(size_t)> fn) { pool().start(n, std::move(fn)); }
void wait() { pool().wait(); }

} // namespace spud::parallel
