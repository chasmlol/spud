#include "core/trace.hpp"

#include "core/log.hpp"
#include "core/platform.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <format>
#include <string>
#include <thread>

namespace spud::trace {

namespace {

using Clock = std::chrono::steady_clock;

struct Zone {
    const char* name;
    int depth;
    Clock::time_point start;
    Clock::time_point end;
};

constexpr int k_max_zones = 256;

double g_threshold_ms = [] {
    const auto v = platform::env("SPUD_TRACE_SLOW");
    return v ? std::atof(v->c_str()) : 0.0;
}();

// Only the thread that calls begin_frame records; zones on other threads are ignored.
std::atomic<std::thread::id> g_owner;
std::array<Zone, k_max_zones> g_zones;
int g_count = 0;
int g_depth = 0;
uint64_t g_frame = 0;
Clock::time_point g_frame_start;

double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

} // namespace

void set_threshold_ms(double ms) { g_threshold_ms = ms; }

bool enabled() { return g_threshold_ms > 0 && std::this_thread::get_id() == g_owner.load(std::memory_order_relaxed); }

void begin_frame(uint64_t frame) {
    if (g_threshold_ms <= 0) {
        return;
    }
    g_owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
    g_count = 0;
    g_depth = 0;
    g_frame = frame;
    g_frame_start = Clock::now();
}

void end_frame() {
    if (!enabled()) {
        return;
    }
    const auto now = Clock::now();
    const double total = ms(g_frame_start, now);
    if (total < g_threshold_ms) {
        return;
    }
    std::string out = std::format("trace: frame {} took {:.3f} ms:", g_frame, total);
    for (int i = 0; i < g_count; ++i) {
        const Zone& z = g_zones[i];
        out += std::format("\n  {:{}}{} +{:.3f} {:.3f} ms", "", z.depth * 2, z.name, ms(g_frame_start, z.start),
                           ms(z.start, z.end));
    }
    log::warn("{}", out);
}

void Scope::open(const char* name) {
    if (g_count >= k_max_zones) {
        return;
    }
    m_index = g_count++;
    g_zones[m_index] = {name, g_depth++, Clock::now(), {}};
}

void Scope::close() {
    g_zones[m_index].end = Clock::now();
    --g_depth;
}

} // namespace spud::trace
