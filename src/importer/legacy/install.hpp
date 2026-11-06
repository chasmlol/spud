#pragma once

// Locating a Skyrim SE/AE installation, its load order and its archives —
// the same files and rules the original game uses at startup.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::legacy {

struct Plugin {
    std::string filename; // as on disk, e.g. "Skyrim.esm"
    bool master = false;  // ESM flag (or .esm/.esl extension)
    bool light = false;   // ESL flag (or .esl extension)
};

// The merged Data folder of a mod manager: lowercase relative path ("meshes/a.nif") -> the real file.
using VirtualData = std::unordered_map<std::string, std::filesystem::path>;

struct Install {
    std::filesystem::path game_dir; // contains the game
    std::filesystem::path data_dir; // game_dir/Data
    std::vector<Plugin> load_order; // active plugins, masters first, in load order
    std::vector<std::filesystem::path> archives; // The game in increasing priority
    // Set when the game runs through Mod Organizer 2 (mo2.hpp): what the game sees as Data/. Null: Data/ as is.
    std::shared_ptr<const VirtualData> vfs;
    std::filesystem::path skyrim_ini; // the Skyrim.ini the game reads
    std::string description;          // where this install came from, for logs

    // The real file behind Data/<relative> (case-insensitive under a VirtualData).
    std::filesystem::path data_file(std::string_view relative) const;
};

// What differs when a mod manager starts the game (mo2.hpp); empty members mean the game's own defaults.
struct InstallOverrides {
    std::shared_ptr<const VirtualData> vfs;
    std::filesystem::path plugins_txt; // default: %LOCALAPPDATA%/Skyrim Special Edition/Plugins.txt
    std::filesystem::path skyrim_ini;  // default: Documents/My Games/Skyrim Special Edition/Skyrim.ini
    std::string description;
};

// Finds the game via the registry, then common Steam paths.
std::optional<std::filesystem::path> find_game_dir();

// Builds the active load order and archive list for a game directory:
// implicit masters (Skyrim, Update, DLC), Skyrim.ccc Creation Club content,
// then active entries of %LOCALAPPDATA%/Skyrim Special Edition/Plugins.txt.
Install open_install(const std::filesystem::path& game_dir);
Install open_install(const std::filesystem::path& game_dir, const InstallOverrides& overrides);

} // namespace spud::legacy
