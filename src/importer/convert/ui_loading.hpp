#pragma once

// The load order's loading screens -> <cache>/interface/loadingscreens.osl (docs/formats/osl.md): every LSCR
// record with its conditions, and the starting state the conditions read. The models
// the screens show (their NNAM statics) become .osm files with their textures, the camera paths (MOD2) .ose node
// animations, next to the other converted meshes.

#include "formats/osl.hpp"

#include <string>
#include <vector>

namespace spud::legacy {
class DataSource;
class EsmDatabase;
} // namespace spud::legacy

namespace spud::importer {

struct LoadingImport {
    osl::LoadingScreens table;
    std::vector<std::string> models;       // normalised NIF paths of the screens' models
    std::vector<std::string> camera_paths; // normalised NIF paths of the camera paths
    std::vector<std::string> warnings;
};

LoadingImport import_loading_screens(const legacy::EsmDatabase& db, const legacy::DataSource& data);

// Writes the table, converts the models (with textures) and camera paths; existing outputs are kept unless
// `force`. Returns the number of screens.
size_t write_loading_screens(const legacy::EsmDatabase& db, const legacy::DataSource& data,
                             const std::string& cache_dir, bool force);

} // namespace spud::importer
