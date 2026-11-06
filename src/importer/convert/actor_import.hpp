#pragma once

// What actors are drawn with: every armour addon, head part, weapon and FaceGen model as a skinned .osm under
// skinned/, every race's skeleton as.osk beside them, and the textures they use.

#include "importer/incremental.hpp"
#include "importer/legacy/data_source.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace spud::importer {

inline constexpr uint32_t k_skinned_converter_version = 11;

struct ActorImport {
    size_t models = 0;
    size_t failed = 0;
    size_t skeletons = 0;
    std::vector<std::string> textures; // normalised cache paths, sorted, unique
};

ActorImport import_actors(const legacy::DataSource& data, AssetCache& assets, const std::filesystem::path& cache,
                          unsigned threads, bool force);

} // namespace spud::importer
