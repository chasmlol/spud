#pragma once

// Mod Organizer 2: the game as MO2 starts it from a profile. MO2 doesn't copy mods into Data/; its virtual file
// system (usvfs) overlays every enabled mod's folder on the game's Data folder, in priority order, with the
// overwrite folder on top, and substitutes the profile's plugins.txt (and, with profile-local settings, its INI
// files). open_mo2_install() builds the same merged view and load order. Rules and their evidence (MO2's own
// source), "Mod Organizer 2".

#include "importer/legacy/install.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace spud::legacy {

struct Mo2Instance {
    std::filesystem::path ini;      // ModOrganizer.ini
    std::filesystem::path base_dir; // [Settings] base_directory, default: the folder holding ModOrganizer.ini
    std::filesystem::path game_dir; // [General] gamePath
    std::filesystem::path mods_dir;
    std::filesystem::path profiles_dir;
    std::filesystem::path overwrite_dir;
    std::string game_name;          // [General] gameName
    std::string selected_profile;   // [General] selected_profile
};

struct Mo2Mod {
    std::string name;
    std::filesystem::path dir;
    bool enabled = false;
    bool foreign = false; // '*': an unmanaged entry (DLC, Creation Club) whose files are in the game's Data
    int priority = 0;     // 0 = lowest; the overwrite folder is above every mod
};

struct Mo2Profile {
    std::string name;
    std::filesystem::path dir;
    std::vector<Mo2Mod> mods;  // priority order, lowest first (modlist.txt lists them highest first)
    bool local_settings = false; // settings.ini [General] LocalSettings: the profile has its own Skyrim.ini
    bool local_saves = false;
};

// Reads an instance: a folder with ModOrganizer.ini (portable), or %LOCALAPPDATA%/ModOrganizer/<instance>.
Mo2Instance open_mo2_instance(const std::filesystem::path& dir);
// Instances registered for this user (%LOCALAPPDATA%/ModOrganizer/*/ModOrganizer.ini).
std::vector<std::filesystem::path> find_mo2_instances();
Mo2Profile read_mo2_profile(const Mo2Instance& instance, const std::string& profile);

// The install as the game sees it when MO2 starts it with `profile`.
Install open_mo2_install(const Mo2Instance& instance, const Mo2Profile& profile);

} // namespace spud::legacy
