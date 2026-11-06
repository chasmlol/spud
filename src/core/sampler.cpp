#include "core/sampler.hpp"

#include "core/log.hpp"

#include <exception>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#include <dbghelp.h>
#endif

namespace spud::sampler {

#ifdef _WIN32
namespace {

constexpr uint32_t k_max_depth = 48;
constexpr size_t k_max_words = size_t(1) << 26; // 512 MiB of addresses at most
constexpr size_t k_stack_copy = 256 * 1024;

std::atomic<bool> g_stop{false};
std::atomic<uint32_t> g_tag{0};
std::thread g_thread;
std::string g_file;
HANDLE g_target = nullptr;
uint64_t g_stack_top = 0; // where the sampled thread's stack ends

// Walks a copy of a stack by the unwind tables. `shift` takes an address on the real stack to the copy, which runs from
// `low` to `high`: saved frame pointers come back as real addresses.
uint32_t unwind(CONTEXT context, uint64_t* out, uint64_t low, uint64_t high, uint64_t shift) {
    uint32_t n = 0;
    while (n < k_max_depth && context.Rip) {
        if (context.Rbp + shift >= low && context.Rbp + shift < high) {
            context.Rbp += shift;
        }
        if (context.Rsp < low || context.Rsp + 8 > high) {
            break;
        }
        out[n++] = context.Rip;
        DWORD64 image = 0;
        RUNTIME_FUNCTION* fn = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
        if (!fn) {
            // A leaf function: its return address is on top of the stack.
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handler = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, fn, &context, &handler, &frame, nullptr);
    }
    return n;
}

// A walk reads a stack that was copied mid-call: a bad address ends that sample, not the run.
uint32_t guarded_unwind(const CONTEXT& context, uint64_t* out, uint64_t low, uint64_t high, uint64_t shift) {
    __try {
        return unwind(context, out, low, high, shift);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void run() {
    timeBeginPeriod(1);
    std::vector<uint64_t> words;
    words.reserve(size_t(1) << 22);
    uint64_t stack[k_max_depth];
    std::vector<std::byte> copy(k_stack_copy);
    while (!g_stop.load(std::memory_order_relaxed) && words.size() + k_max_depth + 2 < k_max_words) {
        Sleep(1);
        if (SuspendThread(g_target) == DWORD(-1)) {
            break;
        }
        // Only the copy happens with the thread stopped. The unwinder takes locks, and a stopped thread may hold them.
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        const bool got = GetThreadContext(g_target, &context) != 0;
        size_t copied = 0;
        if (got) {
            const uint64_t room = g_stack_top > context.Rsp ? g_stack_top - context.Rsp : 0;
            copied = size_t(std::min<uint64_t>(room, k_stack_copy));
            std::memcpy(copy.data(), reinterpret_cast<const void*>(context.Rsp), copied);
        }
        ResumeThread(g_target);
        uint32_t depth = 0;
        if (got && copied) {
            const uint64_t low = reinterpret_cast<uint64_t>(copy.data());
            const uint64_t shift = low - context.Rsp;
            context.Rsp = low;
            depth = guarded_unwind(context, stack, low, low + copied, shift);
        }
        words.push_back(uint64_t(depth) | uint64_t(g_tag.load(std::memory_order_relaxed)) << 32);
        words.insert(words.end(), stack, stack + depth);
    }
    timeEndPeriod(1);
    if (FILE* f = std::fopen(g_file.c_str(), "wb")) {
        const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        std::fwrite(&base, sizeof(base), 1, f);
        std::fwrite(words.data(), sizeof(uint64_t), words.size(), f);
        std::fclose(f);
    }
}

} // namespace

void start() {
    const char* file = std::getenv("SPUD_SAMPLE");
    if (!file || !*file || g_thread.joinable()) {
        return;
    }
    g_file = file;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    g_stack_top = high;
    g_thread = std::thread(run);
    std::atexit(stop);
}

void stop() {
    if (g_thread.joinable()) {
        g_stop = true;
        g_thread.join();
    }
}

void set_tag(uint32_t tag) { g_tag.store(tag, std::memory_order_relaxed); }

namespace {
LONG WINAPI write_dump(EXCEPTION_POINTERS* info) {
    char dir[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const std::string folder = std::string(dir) + "/spud/crash";
    CreateDirectoryA(folder.c_str(), nullptr);
    const std::string file = folder + "/spud-" + std::to_string(GetCurrentProcessId()) + ".dmp";
    const HANDLE out = CreateFileA(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION e{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), out, MiniDumpWithThreadInfo, &e, nullptr, nullptr);
        CloseHandle(out);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void catch_crashes() {
    SetUnhandledExceptionFilter(write_dump);
    catch_terminate();
}

// An exception nothing caught ends in a fast fail, which passes no filter: say what it was, and leave a dump from here. The
// handler is per thread on this CRT, so every thread sets it (platform::set_thread).
void catch_terminate() {
    std::set_terminate([] {
        try {
            if (const std::exception_ptr e = std::current_exception()) {
                std::rethrow_exception(e);
            }
            log::error("terminate: no exception in flight");
        } catch (const std::exception& e) {
            log::error("terminate: {}", e.what());
        } catch (...) {
            log::error("terminate: an exception of an unknown type");
        }
        RaiseException(0xE0000001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    });
}

#else
void start() {}
void stop() {}
void set_tag(uint32_t) {}
void catch_crashes() {}
void catch_terminate() {}
#endif

} // namespace spud::sampler
