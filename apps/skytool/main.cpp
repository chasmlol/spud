// skytool — inspect Skyrim data and import it into spud formats.

#include "commands.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/platform.hpp"
#include "importer/legacy/mo2.hpp"
#include "importer/legacy/strings.hpp"

#include <cstdio>
#include <exception>

namespace skytool {

namespace {

Globals g_globals;

struct Command {
    const char* name;
    int (*run)(Args);
    const char* help;
};

constexpr Command k_commands[] = {
    {"bsa", cmd_bsa, "bsa list <archive> [prefix] | bsa extract <archive> <path> <out> | bsa find <path>"},
    {"esm", cmd_esm, "esm ... (inspect plugins)"},
    {"nif", cmd_nif, "nif ... (inspect meshes)"},
    {"audio", cmd_audio, "audio decode|encode|lip <file> <out> (convert one xWMA / FUZ file), audio rng ..."},
    {"import", cmd_import, "import [--world <edid>] (convert the install into the spud cache)"},
    {"osb", cmd_osb, "osb <file.osb> [class] (print a behaviour file's objects)"},
    {"conditions", cmd_conditions, "conditions <in.csv> <out.csv> [hour weather [worldspace]] (condition functions from the records)"},
    {"far", cmd_far, "far [--world <edid>]... (generate the far view from the cache: land maps, impostors, proxies)"},
    {"records", cmd_records, "records check|import|show|list|stats|dump ... (the forms database)"},
    {"strings", cmd_strings, "strings <plugin> ... (localized string tables)"},
    {"mo2", cmd_mo2, "mo2 [instances|profile|files|find <path>] (Mod Organizer 2: what the game sees)"},
    {"script", cmd_script, "script import [OUT.oss] (compiled Papyrus into the cache, docs/formats/oss.md)"},
    {"ui", cmd_ui, "ui convert|dump|check|import ... (interface movies, docs/formats/osu.md)"},
    {"fx", cmd_fx, "fx convert|dump|sim|check ... (particle effects, docs/formats/ose.md)"},
};

void usage() {
    std::puts("usage: skytool [--game <dir>] [--mo2 <instance|auto> [--profile <name>]] [--cache <dir>] [-v] <command> [args]\n"
              "  --mo2 runs every command on the game as Mod Organizer 2 starts it with that profile\n");
    for (const auto& c : k_commands) {
        std::printf("  %s\n", c.help);
    }
}

} // namespace

const Globals& globals() { return g_globals; }

std::filesystem::path mo2_instance_dir(const std::string& spec) {
    if (std::filesystem::exists(std::filesystem::path(spec) / "ModOrganizer.ini")) {
        return spec;
    }
    if (spec == "auto") {
        // Instances that manage this game; MO2 names an instance after its game by default, so prefer that one.
        std::vector<std::filesystem::path> managing;
        for (const auto& dir : spud::legacy::find_mo2_instances()) {
            try {
                auto m = spud::legacy::open_mo2_instance(dir);
                if (std::filesystem::exists(m.game_dir / "SkyrimSE.exe")) {
                    if (dir.filename().string() == m.game_name) {
                        return dir;
                    }
                    managing.push_back(dir);
                }
            } catch (const std::exception&) {
            }
        }
        if (!managing.empty()) {
            return managing.front();
        }
    }
    for (const auto& dir : spud::legacy::find_mo2_instances()) {
        if (spud::path::to_lower(dir.filename().string()) == spud::path::to_lower(spec)) {
            return dir;
        }
    }
    throw spud::Error("no Mod Organizer 2 instance '" + spec + "' (a folder with ModOrganizer.ini, or an instance name)");
}

std::string install_language(const spud::legacy::Install& install) {
    return spud::legacy::read_language(install.skyrim_ini, install.game_dir / "Skyrim_Default.ini");
}

spud::legacy::Install open_install() {
    if (g_globals.mo2) {
        auto instance = spud::legacy::open_mo2_instance(mo2_instance_dir(*g_globals.mo2));
        auto profile = spud::legacy::read_mo2_profile(instance, g_globals.mo2_profile.value_or(""));
        return spud::legacy::open_mo2_install(instance, profile);
    }
    auto dir = g_globals.game_dir ? g_globals.game_dir : spud::legacy::find_game_dir();
    if (!dir) {
        throw spud::Error("Skyrim Special Edition not found; pass --game <dir>");
    }
    return spud::legacy::open_install(*dir);
}

std::filesystem::path cache_dir() { return g_globals.cache_dir.value_or(spud::platform::default_cache_dir()); }

std::optional<std::string> option(Args args, std::string_view flag) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag) {
            return args[i + 1];
        }
    }
    return std::nullopt;
}

bool has_flag(Args args, std::string_view flag) {
    for (const auto& a : args) {
        if (a == flag) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> positionals(Args args, std::initializer_list<std::string_view> flags_with_values) {
    std::vector<std::string> out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i].starts_with("--")) {
            for (auto f : flags_with_values) {
                if (args[i] == f) {
                    ++i;
                    break;
                }
            }
            continue;
        }
        out.push_back(args[i]);
    }
    return out;
}

} // namespace skytool

int main(int argc, char** argv) {
    using namespace skytool;
    std::vector<std::string> args(argv + 1, argv + argc);
    size_t i = 0;
    for (; i < args.size() && args[i].starts_with("-"); ++i) {
        if (args[i] == "--game" && i + 1 < args.size()) {
            g_globals.game_dir = args[++i];
        } else if (args[i] == "--cache" && i + 1 < args.size()) {
            g_globals.cache_dir = args[++i];
        } else if (args[i] == "--mo2" && i + 1 < args.size()) {
            g_globals.mo2 = args[++i];
        } else if (args[i] == "--profile" && i + 1 < args.size()) {
            g_globals.mo2_profile = args[++i];
        } else if (args[i] == "-v") {
            spud::log::set_level(spud::log::Level::debug);
        } else {
            break;
        }
    }
    if (i >= args.size()) {
        usage();
        return 1;
    }
    for (const auto& c : k_commands) {
        if (args[i] == c.name) {
            try {
                return c.run(Args(args).subspan(i + 1));
            } catch (const std::exception& e) {
                spud::log::error("{}", e.what());
                return 1;
            }
        }
    }
    usage();
    return 1;
}
