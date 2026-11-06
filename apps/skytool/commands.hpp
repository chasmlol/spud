#pragma once

#include "importer/legacy/install.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spud::forms {
class Database;
}

namespace skytool {

using Args = std::span<const std::string>;

// Options shared by every command, parsed before the command name.
struct Globals {
    std::optional<std::filesystem::path> game_dir; // --game <dir>
    std::optional<std::filesystem::path> cache_dir; // --cache <dir>
    std::optional<std::string> mo2;                 // --mo2 <instance folder | instance name | auto>
    std::optional<std::string> mo2_profile;         // --profile <name> (default: the instance's selected one)
};

const Globals& globals();

// Opens the Skyrim install from --game or auto-detection; throws if not found.
spud::legacy::Install open_install();
std::filesystem::path cache_dir();

// Returns the value following `flag` in args ("--out x"), if any.
std::optional<std::string> option(Args args, std::string_view flag);
bool has_flag(Args args, std::string_view flag);
// Positional arguments: everything that is not a --flag or a flag's value.
std::vector<std::string> positionals(Args args, std::initializer_list<std::string_view> flags_with_values = {});

int cmd_bsa(Args args);
int cmd_script(Args args);
int cmd_esm(Args args);
int cmd_nif(Args args);
int cmd_import(Args args);
int cmd_osb(Args args);
int cmd_far(Args args);
int cmd_conditions(Args args);
int cmd_records(Args args);
int cmd_strings(Args args);
int cmd_mo2(Args args);
int records_gamedump(const spud::forms::Database& db, Args args);

// A Mod Organizer 2 instance folder from --mo2: a folder, an instance name, or "auto" (the first one found).
std::filesystem::path mo2_instance_dir(const std::string& spec);

// sLanguage of the install (its Skyrim.ini, then Skyrim_Default.ini).
std::string install_language(const spud::legacy::Install& install);
int cmd_ui(Args args);
int cmd_fx(Args args);
int cmd_audio(Args args);

} // namespace skytool
