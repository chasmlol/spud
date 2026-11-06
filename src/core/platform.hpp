#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace spud::platform {

// %LOCALAPPDATA% on Windows.
std::filesystem::path local_app_data();

// "My Documents".
std::filesystem::path documents();

// Default import cache for Skyrim content: %LOCALAPPDATA%/spud/cache/skyrim
std::filesystem::path default_cache_dir();

std::optional<std::string> env(const char* name);

enum class ThreadPriority { below_normal, normal, above_normal, highest };

// Names a thread (shown in debuggers and profilers) and sets its scheduling priority.
// `native_handle` is std::thread::native_handle(); null means the calling thread.
void set_thread(void* native_handle, const char* name, ThreadPriority priority);

} // namespace spud::platform
