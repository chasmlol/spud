#pragma once

// Slow-frame tracing: scoped timers on the render thread that cost one branch when off. When a frame
// takes longer than the threshold, end_frame() logs every zone it went through with its duration, so a
// rare stall can be pinned to the call that caused it. Enable with SPUD_TRACE_SLOW=<ms>.

#include <chrono>
#include <cstdint>

namespace spud::trace {

// 0 = off. Set once at startup (read from SPUD_TRACE_SLOW by default).
void set_threshold_ms(double ms);
bool enabled();

void begin_frame(uint64_t frame);
// Logs the frame's zones if it took longer than the threshold.
void end_frame();

struct Scope {
    explicit Scope(const char* name) {
        if (enabled()) {
            open(name);
        }
    }
    ~Scope() {
        if (m_index >= 0) {
            close();
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    void open(const char* name);
    void close();
    int m_index = -1;
};

} // namespace spud::trace

#define SPUD_TRACE_CAT2(a, b) a##b
#define SPUD_TRACE_CAT(a, b) SPUD_TRACE_CAT2(a, b)
#define SPUD_TRACE(name) ::spud::trace::Scope SPUD_TRACE_CAT(spud_trace_, __LINE__)(name)
