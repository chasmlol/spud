#include "commands.hpp"

#include "core/error.hpp"
#include "core/path.hpp"
#include "importer/legacy/mo2.hpp"

#include <cstdio>
#include <map>

namespace skytool {

namespace {

using namespace spud;

legacy::Mo2Instance instance_from_globals() {
    return legacy::open_mo2_instance(mo2_instance_dir(globals().mo2.value_or("auto")));
}

} // namespace

int cmd_mo2(Args args) {
    auto pos = positionals(args, {"--prefix"});
    if (has_flag(args, "--help")) {
        std::puts("mo2 instances                 Mod Organizer 2 instances of this user, their game and profiles\n"
                  "mo2 profile                   the profile's mods by priority, the load order and archives the game gets\n"
                  "mo2 files [--prefix P]        per mod, how many files of the merged Data folder it provides\n"
                  "mo2 find <path>               which mod a Data file comes from, and every mod that has one\n"
                  "  (use the global --mo2 <instance|auto> and --profile <name>; default: auto, the selected profile)");
        return 0;
    }
    auto sub = pos.empty() ? std::string("profile") : pos[0];
    if (sub == "instances") {
        for (const auto& dir : legacy::find_mo2_instances()) {
            legacy::Mo2Instance m;
            try {
                m = legacy::open_mo2_instance(dir);
            } catch (const std::exception& e) {
                std::printf("%s\n  (%s)\n", dir.string().c_str(), e.what());
                continue;
            }
            std::printf("%s\n  game: %s\n  mods: %s\n  selected profile: %s\n  profiles:", dir.string().c_str(),
                        m.game_dir.string().c_str(), m.mods_dir.string().c_str(), m.selected_profile.c_str());
            if (std::filesystem::is_directory(m.profiles_dir)) {
                for (const auto& p : std::filesystem::directory_iterator(m.profiles_dir)) {
                    std::printf(" [%s]", p.path().filename().string().c_str());
                }
            }
            std::puts("");
        }
        return 0;
    }
    auto instance = instance_from_globals();
    auto profile = legacy::read_mo2_profile(instance, globals().mo2_profile.value_or(""));
    if (sub == "profile") {
        std::printf("instance %s, profile %s (local settings %s)\nmods, highest priority first:\n",
                    instance.base_dir.string().c_str(), profile.name.c_str(), profile.local_settings ? "yes" : "no");
        std::printf("  [top] overwrite\n");
        for (auto it = profile.mods.rbegin(); it != profile.mods.rend(); ++it) {
            std::printf("  %4d %s %s\n", it->priority, it->foreign ? "*" : it->enabled ? "+" : "-", it->name.c_str());
        }
        std::printf("  [base] %s\n", (instance.game_dir / "Data").string().c_str());
        auto install = legacy::open_mo2_install(instance, profile);
        std::printf("\nload order (%zu plugins):\n", install.load_order.size());
        for (const auto& p : install.load_order) {
            std::printf("  %s%s%s  <- %s\n", p.filename.c_str(), p.master ? " [master]" : "", p.light ? " [light]" : "",
                        install.data_file(p.filename).string().c_str());
        }
        std::printf("\narchives (%zu, increasing priority):\n", install.archives.size());
        for (const auto& a : install.archives) {
            std::printf("  %s\n", a.string().c_str());
        }
        std::printf("\nSkyrim.ini: %s, sLanguage %s\n", install.skyrim_ini.string().c_str(),
                    install_language(install).c_str());
        return 0;
    }
    auto install = legacy::open_mo2_install(instance, profile);
    if (sub == "files") {
        auto prefix = path::to_lower(option(args, "--prefix").value_or(""));
        std::map<std::string, size_t> by_origin;
        for (const auto& [rel, real] : *install.vfs) {
            if (!rel.starts_with(prefix)) {
                continue;
            }
            auto origin = std::filesystem::relative(real, instance.mods_dir).generic_string();
            auto slash = origin.find('/');
            if (origin.starts_with("..")) {
                origin = real.string().starts_with(instance.overwrite_dir.string()) ? "[overwrite]" : "[game Data]";
            } else {
                origin = origin.substr(0, slash);
            }
            ++by_origin[origin];
        }
        for (const auto& [o, n] : by_origin) {
            std::printf("%8zu  %s\n", n, o.c_str());
        }
        return 0;
    }
    if (sub == "find" && pos.size() >= 2) {
        auto rel = path::to_lower(std::filesystem::path(pos[1]).generic_string());
        auto it = install.vfs->find(rel);
        std::printf("%s -> %s\n", rel.c_str(), it == install.vfs->end() ? "(not in the merged Data folder)" : it->second.string().c_str());
        auto has = [&](const std::filesystem::path& root) {
            return std::filesystem::exists(root / std::filesystem::path(pos[1]));
        };
        if (has(instance.game_dir / "Data")) {
            std::printf("  present in game Data\n");
        }
        for (const auto& m : profile.mods) {
            if (!m.foreign && has(m.dir)) {
                std::printf("  present in mod %d %s (%s)\n", m.priority, m.name.c_str(), m.enabled ? "enabled" : "disabled");
            }
        }
        if (has(instance.overwrite_dir)) {
            std::printf("  present in overwrite\n");
        }
        return 0;
    }
    std::puts("unknown mo2 command; see 'skytool mo2 --help'");
    return 1;
}

} // namespace skytool
