#include "core/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace spud::log {

namespace {

std::atomic<Level> g_level{Level::info};
std::mutex g_mutex;
std::ofstream g_file;
std::function<void(Level, std::string_view)> g_listener;

const char* level_name(Level level) {
    switch (level) {
    case Level::debug:
        return "debug";
    case Level::info:
        return "info";
    case Level::warn:
        return "warn";
    case Level::error:
        return "error";
    }
    return "?";
}

} // namespace

void set_level(Level level) { g_level = level; }

Level level() { return g_level; }

void set_file(const std::filesystem::path& path) {
    std::lock_guard lock(g_mutex);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    g_file = std::ofstream(path, std::ios::trunc);
}

void set_listener(std::function<void(Level, std::string_view)> listener) {
    std::lock_guard lock(g_mutex);
    g_listener = std::move(listener);
}

void write(Level level, std::string_view message) {
    if (level < g_level) {
        return;
    }
    auto line = std::format("[{}] {}\n", level_name(level), message);
    std::lock_guard lock(g_mutex);
    std::fputs(line.c_str(), stderr);
    if (g_file) {
        g_file << line;
        g_file.flush();
    }
    if (g_listener) {
        g_listener(level, message);
    }
}

} // namespace spud::log
