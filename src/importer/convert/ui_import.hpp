#pragma once

// Skyrim's interface -> native UI data (docs/formats/osu.md): every movie in Interface/ becomes an .osu movie,
// fontconfig.txt a font map, translate_<language>.txt a string table. All output goes to the user's cache.

#include "formats/osu.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spud::legacy {
class DataSource;
}

namespace spud::importer {

struct MovieImport {
    osu::Movie movie;
    std::vector<std::string> warnings;
};

// A SWF or GFX file's bytes -> movie. Throws ParseError for files that aren't movies.
MovieImport import_movie(std::span<const std::byte> swf);

// translate_<language>.txt (UTF-16 LE, "$KEY<tab>text" lines).
osu::Strings import_translations(std::span<const std::byte> text);

// fontconfig.txt: fontlib "Interface\x.swf" lines and map "$Name" = "Font" Style lines.
osu::FontMap import_font_config(std::span<const std::byte> text);

struct InterfaceSummary {
    size_t movies = 0, movies_failed = 0, warnings = 0, tables = 0, models = 0;
};

// The models the UI 3D scenes show that no worldspace references: the Mist Menu's logos
// and the Stats Menu's skydome, star and line
// (StatsMenu::StatsMenu, table; the vampire skydome for the beast trees). Each is converted
// into <cache>/<data path>.osm with its textures and a scene graph <cache>/<data path>.ose (docs/formats/ose.md,
// version 3). The game prefixes its model paths with "meshes\\".
inline constexpr std::string_view k_scene_models[] = {
    "meshes/interface/logo/logo.nif",          "meshes/interface/logo/logo01ae.nif",
    "meshes/interface/intperkskydome.nif",     "meshes/interface/intperkstars01.nif",
    "meshes/interface/intperkline01.nif",      "meshes/dlc01/interface/intvampireperkskydome.nif",
    // The Lockpicking Menu's slot 7.
    "meshes/interface/lockpicking/lockpickshiv01.nif", "meshes/interface/lockpicking/lockpick01.nif",
    "meshes/interface/lockpicking/lockpickskeletonkey01.nif",
};

// The world map's land and objects: every worldspace's level 32 terrain and level 16 object blocks with the world-map
// bit set and the overlay's textures in their materials, and the cloud bank. The count of
// blocks converted.
size_t convert_world_map(const legacy::DataSource& data, const std::string& cache_dir, bool force);

// One model as a UI 3D scene reads it: .osm, .ose scene graph and textures into the cache. `model` is a data path
// with or without "meshes/". A model converted before is kept unless `force` or its scene graph predates version 3.
// False when the model is missing or fails to convert (logged).
bool convert_scene_model(const legacy::DataSource& data, const std::string& cache_dir, std::string_view model,
                         bool force);

// Converts everything under interface/ that the data source resolves into <cache>/interface/.
InterfaceSummary import_interface(const legacy::DataSource& data, const std::string& cache_dir, bool force);

} // namespace spud::importer
