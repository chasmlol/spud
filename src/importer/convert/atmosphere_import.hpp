#pragma once

// Climates, weathers, image spaces and their modifiers, volumetric lighting, precipitation, the sky's meshes and game
// settings -> .osa (docs/formats/osa.md).

#include "formats/osa.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/esm.hpp"

#include <set>
#include <string>

namespace spud::importer {

struct AtmosphereImport {
    osa::Atmosphere atmosphere;
    std::set<std::string> textures; // cache paths the sky reads (clouds, sun, moons, stars, precipitation)
    std::set<std::string> meshes;   // models drawn as ordinary meshes (weather sky models: aurora, ...)
};

AtmosphereImport import_atmosphere(const legacy::EsmDatabase& db, const legacy::DataSource& data);

} // namespace spud::importer
