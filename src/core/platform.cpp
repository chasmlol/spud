#include "core/platform.hpp"

#include "core/sampler.hpp"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace spud::platform {

std::optional<std::string> env(const char* name) {
    if (const char* v = std::getenv(name); v && *v) {
        return std::string(v);
    }
    return std::nullopt;
}

#ifdef _WIN32

namespace {

std::filesystem::path known_folder(const KNOWNFOLDERID& id) {
    PWSTR raw = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) {
        result = raw;
    }
    CoTaskMemFree(raw);
    return result;
}

} // namespace

std::filesystem::path local_app_data() { return known_folder(FOLDERID_LocalAppData); }

std::filesystem::path documents() { return known_folder(FOLDERID_Documents); }

#endif

std::filesystem::path default_cache_dir() { return local_app_data() / "spud" / "cache" / "skyrim"; }

void set_thread(void* native_handle, const char* name, ThreadPriority priority) {
    if (!native_handle) {
        sampler::catch_terminate();
    }
#ifdef _WIN32
    HANDLE h = native_handle ? static_cast<HANDLE>(native_handle) : GetCurrentThread();
    int p = THREAD_PRIORITY_NORMAL;
    switch (priority) {
    case ThreadPriority::below_normal: p = THREAD_PRIORITY_BELOW_NORMAL; break;
    case ThreadPriority::normal: p = THREAD_PRIORITY_NORMAL; break;
    case ThreadPriority::above_normal: p = THREAD_PRIORITY_ABOVE_NORMAL; break;
    case ThreadPriority::highest: p = THREAD_PRIORITY_HIGHEST; break;
    }
    SetThreadPriority(h, p);
    if (name) {
        const std::filesystem::path wide(name);
        SetThreadDescription(h, wide.c_str());
    }
#else
    (void)native_handle;
    (void)name;
    (void)priority;
#endif
}

} // namespace spud::platform
