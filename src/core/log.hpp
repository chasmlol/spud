#pragma once

#include <filesystem>
#include <format>
#include <functional>
#include <string_view>
#include <utility>

namespace spud::log {

enum class Level { debug, info, warn, error };

void set_level(Level level);
Level level();
// Also append every message to `path` (in addition to stderr).
void set_file(const std::filesystem::path& path);
// Also hand every message to `listener`, on the calling thread (null: stop).
void set_listener(std::function<void(Level, std::string_view)> listener);
void write(Level level, std::string_view message);

template <typename... Args>
void debug(std::format_string<Args...> f, Args&&... args) {
    if (level() <= Level::debug) {
        write(Level::debug, std::format(f, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void info(std::format_string<Args...> f, Args&&... args) {
    write(Level::info, std::format(f, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::format_string<Args...> f, Args&&... args) {
    write(Level::warn, std::format(f, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> f, Args&&... args) {
    write(Level::error, std::format(f, std::forward<Args>(args)...));
}

} // namespace spud::log
