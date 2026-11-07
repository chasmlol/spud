#pragma once

// Trees whose branches move in the wind: a model whose root is a BSTreeNode with branch bones gets a skinned copy of
// what it draws (<model>.sway.osm) and its node tree (<model>.sway.osk) beside the plain model, for the engine to pose
// (docs/formats/osm.md, osk.md).

#include "formats/osk.hpp"
#include "formats/osm.hpp"
#include "importer/incremental.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/nif.hpp"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace spud::importer {

inline constexpr uint32_t k_tree_sway_converter_version = 1;

// Whether the model's root is a BSTreeNode with branch bones.
bool has_branch_bones(const legacy::NifFile& nif);

struct TreeSwayModel {
    osm::Mesh mesh;
    osk::Skeleton skeleton;
    std::vector<std::string> textures;
};
// nullopt unless has_branch_bones.
std::optional<TreeSwayModel> import_tree_sway(const legacy::NifFile& nif, std::string_view source_path);

// The cache paths beside a model's .osm.
std::string tree_sway_mesh_path(std::string_view mesh);     // <model>.sway.osm
std::string tree_sway_skeleton_path(std::string_view mesh); // <model>.sway.osk

// Converts `src` (a data path, "meshes/...nif") if it's such a tree, unless its outputs are current.
void convert_tree_sway(const legacy::DataSource& data, AssetCache& assets, const std::filesystem::path& cache,
                       const std::string& src, bool force, std::set<std::string>& textures);

} // namespace spud::importer
