#include "importer/legacy/install.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/platform.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace spud::legacy {

namespace {

constexpr std::array k_implicit_masters = {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm",
                                           "Dragonborn.esm"};

constexpr const char* k_default_archives =
    "Skyrim - Misc.bsa, Skyrim - Shaders.bsa, Skyrim - Interface.bsa, Skyrim - Animations.bsa, "
    "Skyrim - Meshes0.bsa, Skyrim - Meshes1.bsa, Skyrim - Sounds.bsa";
constexpr const char* k_default_archives2 =
    "Skyrim - Voices_en0.bsa, Skyrim - Textures0.bsa, Skyrim - Textures1.bsa, Skyrim - Textures2.bsa, "
    "Skyrim - Textures3.bsa, Skyrim - Textures4.bsa, Skyrim - Textures5.bsa, Skyrim - Textures6.bsa, "
    "Skyrim - Textures7.bsa, Skyrim - Textures8.bsa, Skyrim - Patch.bsa";

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::vector<std::string> read_lines(const std::filesystem::path& p) {
    std::vector<std::string> lines;
    std::ifstream in(p);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (!line.empty() && line[0] != '#') {
            lines.push_back(line);
        }
    }
    return lines;
}

// Minimal INI lookup: returns the value of `key` in `[section]`, if present.
std::optional<std::string> ini_value(const std::filesystem::path& ini, std::string_view section, std::string_view key) {
    std::ifstream in(ini);
    std::string current;
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.empty() || line[0] == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            current = path::to_lower(line.substr(1, line.size() - 2));
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos || current != path::to_lower(section)) {
            continue;
        }
        if (path::to_lower(trim(line.substr(0, eq))) == path::to_lower(key)) {
            return trim(line.substr(eq + 1));
        }
    }
    return std::nullopt;
}

std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, ',');) {
        if (item = trim(item); !item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

// Reads the TES4 header flags of a plugin: 0x1 = master, 0x200 = light.
uint32_t plugin_flags(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    char header[24]{};
    if (!in.read(header, sizeof header) || std::string_view(header, 4) != "TES4") {
        throw Error("not a valid plugin: " + p.string());
    }
    uint32_t flags;
    std::memcpy(&flags, header + 8, 4);
    return flags;
}

std::optional<std::filesystem::path> find_case_insensitive(const std::filesystem::path& dir, std::string_view name) {
    auto exact = dir / name;
    if (std::filesystem::exists(exact)) {
        return exact;
    }
    auto lower = path::to_lower(name);
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (path::to_lower(entry.path().filename().string()) == lower) {
            return entry.path();
        }
    }
    return std::nullopt;
}

#ifdef _WIN32
std::optional<std::filesystem::path> registry_install_path() {
    wchar_t buffer[MAX_PATH]{};
    DWORD size = sizeof buffer;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Bethesda Softworks\\Skyrim Special Edition",
                     L"Installed Path", RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS) {
        return std::filesystem::path(buffer);
    }
    return std::nullopt;
}
#endif

} // namespace

std::optional<std::filesystem::path> find_game_dir() {
    std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
    if (auto reg = registry_install_path()) {
        candidates.push_back(*reg);
    }
#endif
    candidates.emplace_back("C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition");
    for (char drive = 'C'; drive <= 'H'; ++drive) {
        candidates.emplace_back(std::string(1, drive) + ":/SteamLibrary/steamapps/common/Skyrim Special Edition");
    }
    for (const auto& c : candidates) {
        if (std::filesystem::exists(c / "Data" / "Skyrim.esm")) {
            return c;
        }
    }
    return std::nullopt;
}

std::filesystem::path Install::data_file(std::string_view relative) const {
    if (vfs) {
        auto key = path::to_lower(std::filesystem::path(relative).generic_string());
        if (auto it = vfs->find(key); it != vfs->end()) {
            return it->second;
        }
    }
    return data_dir / relative;
}

Install open_install(const std::filesystem::path& game_dir) {
    return open_install(game_dir, InstallOverrides{});
}

Install open_install(const std::filesystem::path& game_dir, const InstallOverrides& overrides) {
    Install install;
    install.game_dir = game_dir;
    install.data_dir = game_dir / "Data";
    install.vfs = overrides.vfs;
    install.description = overrides.description.empty() ? game_dir.string() : overrides.description;
    if (!std::filesystem::exists(install.data_file("Skyrim.esm"))) {
        throw Error("no Data/Skyrim.esm under " + game_dir.string());
    }
    // A file of the (possibly merged) Data folder, matched case-insensitively.
    auto find_data = [&](std::string_view name) -> std::optional<std::filesystem::path> {
        if (install.vfs) {
            if (auto it = install.vfs->find(path::to_lower(name)); it != install.vfs->end()) {
                return it->second;
            }
            return std::nullopt;
        }
        return find_case_insensitive(install.data_dir, name);
    };

    // Load order: implicit masters, Creation Club, then Plugins.txt.
    std::vector<std::string> names;
    std::set<std::string> seen;
    auto add = [&](const std::string& name) {
        auto key = path::to_lower(name);
        if (seen.contains(key)) {
            return;
        }
        if (auto found = find_data(name)) {
            seen.insert(key);
            names.push_back(found->filename().string());
        }
    };
    for (auto name : k_implicit_masters) {
        add(name);
    }
    for (const auto& line : read_lines(game_dir / "Skyrim.ccc")) {
        add(line);
    }
    auto plugins_txt = overrides.plugins_txt.empty()
                           ? platform::local_app_data() / "Skyrim Special Edition" / "Plugins.txt"
                           : overrides.plugins_txt;
    for (const auto& line : read_lines(plugins_txt)) {
        if (line[0] == '*') {
            add(line.substr(1));
        }
    }

    for (const auto& name : names) {
        auto ext = path::to_lower(std::filesystem::path(name).extension().string());
        auto flags = plugin_flags(install.data_file(name));
        Plugin p;
        p.filename = name;
        p.light = ext == ".esl" || (flags & 0x200);
        p.master = ext == ".esm" || ext == ".esl" || (flags & 0x1);
        install.load_order.push_back(p);
    }
    // The game loads every master before any non-master, preserving relative order.
    std::stable_partition(install.load_order.begin(), install.load_order.end(),
                          [](const Plugin& p) { return p.master; });

    // Archives: the INI resource lists first, then each plugin's own archives in load order.
    auto ini = overrides.skyrim_ini.empty() ? platform::documents() / "My Games" / "Skyrim Special Edition" / "Skyrim.ini"
                                            : overrides.skyrim_ini;
    install.skyrim_ini = ini;
    auto default_ini = game_dir / "Skyrim_Default.ini";
    auto lookup = [&](const char* key, const char* fallback) {
        if (auto v = ini_value(ini, "Archive", key)) {
            return *v;
        }
        if (auto v = ini_value(default_ini, "Archive", key)) {
            return *v;
        }
        return std::string(fallback);
    };
    std::set<std::string> archive_seen;
    auto add_archive = [&](const std::string& name) {
        auto key = path::to_lower(name);
        if (archive_seen.contains(key)) {
            return;
        }
        if (auto found = find_data(name)) {
            archive_seen.insert(key);
            install.archives.push_back(*found);
        }
    };
    for (const auto& a : split_list(lookup("sResourceArchiveList", k_default_archives))) {
        add_archive(a);
    }
    for (const auto& a : split_list(lookup("sResourceArchiveList2", k_default_archives2))) {
        add_archive(a);
    }
    for (const auto& p : install.load_order) {
        auto stem = std::filesystem::path(p.filename).stem().string();
        add_archive(stem + ".bsa");
        add_archive(stem + " - Textures.bsa");
    }

    log::info("load order: {} plugins, {} archives", install.load_order.size(), install.archives.size());
    return install;
}

} // namespace spud::legacy
