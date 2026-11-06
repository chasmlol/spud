#include "importer/legacy/mo2.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/platform.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <unordered_map>

namespace spud::legacy {

namespace {

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

// QSettings INI values: "@ByteArray(...)" wraps raw bytes, backslashes are escaped, and a value may be quoted.
std::string qsettings_value(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        v = v.substr(1, v.size() - 2);
    }
    if (v.starts_with("@ByteArray(") && v.ends_with(")")) {
        v = v.substr(11, v.size() - 12);
    }
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            out.push_back(v[++i]);
        } else {
            out.push_back(v[i]);
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> read_ini(const std::filesystem::path& p) {
    std::unordered_map<std::string, std::string> out; // "section/key" (lowercase) -> value
    std::ifstream in(p);
    std::string section;
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = path::to_lower(line.substr(1, line.size() - 2));
            continue;
        }
        auto eq = line.find('=');
        if (eq != std::string::npos) {
            out[section + "/" + path::to_lower(trim(line.substr(0, eq)))] = qsettings_value(line.substr(eq + 1));
        }
    }
    return out;
}

std::filesystem::path configurable(const std::unordered_map<std::string, std::string>& ini, const std::string& key,
                                   const std::string& default_dir, const std::filesystem::path& base) {
    std::string v = "%BASE_DIR%/" + default_dir;
    if (auto it = ini.find("settings/" + key); it != ini.end() && !it->second.empty()) {
        v = it->second;
    }
    if (auto at = v.find("%BASE_DIR%"); at != std::string::npos) {
        v.replace(at, 10, base.generic_string());
    }
    return std::filesystem::path(v);
}

// Adds every file under `root` to the merged view (later calls win). MO2 links whole mod folders; files are
// matched case-insensitively, like Windows.
void overlay(std::unordered_map<std::string, std::filesystem::path>& vfs, const std::filesystem::path& root) {
    if (!std::filesystem::is_directory(root)) {
        return;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_directory() && path::to_lower(it->path().filename().string()) == ".git") {
            it.disable_recursion_pending(); // MO2's default skip_directories
            continue;
        }
        if (!it->is_regular_file()) {
            continue;
        }
        auto rel = path::to_lower(std::filesystem::relative(it->path(), root).generic_string());
        vfs[rel] = it->path();
    }
}

} // namespace

Mo2Instance open_mo2_instance(const std::filesystem::path& dir) {
    Mo2Instance m;
    m.ini = dir / "ModOrganizer.ini";
    if (!std::filesystem::exists(m.ini)) {
        throw Error("no ModOrganizer.ini in " + dir.string());
    }
    auto ini = read_ini(m.ini);
    m.base_dir = dir;
    if (auto it = ini.find("settings/base_directory"); it != ini.end() && !it->second.empty()) {
        m.base_dir = it->second;
    }
    m.game_dir = ini["general/gamepath"];
    m.game_name = ini["general/gamename"];
    m.selected_profile = ini["general/selected_profile"];
    m.mods_dir = configurable(ini, "mod_directory", "mods", m.base_dir);
    m.profiles_dir = configurable(ini, "profiles_directory", "profiles", m.base_dir);
    m.overwrite_dir = configurable(ini, "overwrite_directory", "overwrite", m.base_dir);
    if (m.game_dir.empty()) {
        throw Error(m.ini.string() + " has no gamePath");
    }
    return m;
}

std::vector<std::filesystem::path> find_mo2_instances() {
    std::vector<std::filesystem::path> out;
    auto root = platform::local_app_data() / "ModOrganizer";
    if (std::filesystem::is_directory(root)) {
        for (const auto& e : std::filesystem::directory_iterator(root)) {
            if (e.is_directory() && std::filesystem::exists(e.path() / "ModOrganizer.ini")) {
                out.push_back(e.path());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

Mo2Profile read_mo2_profile(const Mo2Instance& instance, const std::string& name) {
    Mo2Profile p;
    p.name = name.empty() ? instance.selected_profile : name;
    p.dir = instance.profiles_dir / p.name;
    if (!std::filesystem::is_directory(p.dir)) {
        throw Error("no MO2 profile '" + p.name + "' in " + instance.profiles_dir.string());
    }
    auto settings = read_ini(p.dir / "settings.ini");
    p.local_settings = path::to_lower(settings["general/localsettings"]) == "true";
    p.local_saves = path::to_lower(settings["general/localsaves"]) == "true";

    // modlist.txt: highest priority first; '+' enabled, '-' disabled, '*' foreign (always enabled), no prefix
    // enabled; '#' comments; a repeated name keeps its first line; names of missing folders are skipped
    // (foreign entries name game content, not folders).
    std::ifstream in(p.dir / "modlist.txt");
    std::vector<Mo2Mod> listed;
    std::set<std::string> seen;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        Mo2Mod m;
        m.enabled = line[0] != '-';
        m.foreign = line[0] == '*';
        m.name = trim(line[0] == '-' || line[0] == '+' || line[0] == '*' ? line.substr(1) : line);
        if (m.name.empty() || !seen.insert(m.name).second || path::to_lower(m.name) == "overwrite") {
            continue;
        }
        m.dir = instance.mods_dir / m.name;
        if (!m.foreign && !std::filesystem::is_directory(m.dir)) {
            log::warn("MO2 profile {}: mod '{}' has no folder; skipped", p.name, m.name);
            continue;
        }
        listed.push_back(std::move(m));
    }
    // Mod folders the list doesn't name get a priority above every listed mod (MO2 adds new mods on top).
    std::vector<Mo2Mod> unlisted;
    if (std::filesystem::is_directory(instance.mods_dir)) {
        for (const auto& e : std::filesystem::directory_iterator(instance.mods_dir)) {
            auto n = e.path().filename().string();
            if (e.is_directory() && !seen.contains(n)) {
                unlisted.push_back({n, e.path(), false, false, 0});
            }
        }
    }
    std::sort(unlisted.begin(), unlisted.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    std::reverse(listed.begin(), listed.end());
    p.mods = std::move(listed);
    p.mods.insert(p.mods.end(), unlisted.begin(), unlisted.end());
    for (size_t i = 0; i < p.mods.size(); ++i) {
        p.mods[i].priority = static_cast<int>(i);
    }
    return p;
}

Install open_mo2_install(const Mo2Instance& instance, const Mo2Profile& profile) {
    auto vfs = std::make_shared<std::unordered_map<std::string, std::filesystem::path>>();
    overlay(*vfs, instance.game_dir / "Data");
    size_t mods = 0;
    for (const auto& m : profile.mods) {
        if (m.enabled && !m.foreign) {
            overlay(*vfs, m.dir);
            ++mods;
        }
    }
    overlay(*vfs, instance.overwrite_dir);

    InstallOverrides o;
    o.vfs = vfs;
    o.plugins_txt = profile.dir / "plugins.txt";
    if (profile.local_settings) {
        o.skyrim_ini = profile.dir / "skyrim.ini";
    }
    o.description = "MO2 " + instance.base_dir.filename().string() + " / " + profile.name;
    log::info("MO2 profile {}: {} enabled mods over {}, {} files in the merged Data folder", profile.name, mods,
              (instance.game_dir / "Data").string(), vfs->size());
    return open_install(instance.game_dir, o);
}

} // namespace spud::legacy
