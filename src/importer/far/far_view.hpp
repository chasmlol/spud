#pragma once

// Generates each worldspace's far view from what the import already wrote to the cache: only native formats are read
// here, so it can run again without touching the install.

#include "formats/osh.hpp"
#include "formats/osw.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace spud::importer::far {

class TexelCache;

struct FarViewOptions {
    std::filesystem::path cache_dir;
    std::vector<std::string> worlds; // EditorIDs; empty: every worldspace with land
    unsigned threads = 0;
};

// The land's macro maps (far_land.cpp): fills view's map fields and writes the two DDS files.
void bake_land(const osw::World& world, TexelCache& texels, const std::filesystem::path& cache, osh::FarView& view,
               unsigned threads);

// Tree impostors and placements (far_trees.cpp).
void bake_trees(const osw::World& world, TexelCache& texels, const std::filesystem::path& cache, osh::FarView& view,
                unsigned threads);

// Region proxies (far_proxies.cpp).
void bake_proxies(const osw::World& world, TexelCache& texels, const std::filesystem::path& cache, osh::FarView& view,
                  unsigned threads);

void build_far_views(const FarViewOptions& options);

} // namespace spud::importer::far
