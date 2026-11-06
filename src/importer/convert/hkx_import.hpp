#pragma once

// .hkx -> native: projects, characters and behaviour graphs to .osb, skeletons to .osk, clips to .osn, under
// animations/. The packfile format is; class layouts come from
// (hkx_classes.inc, generated).

#include "formats/osb.hpp"
#include "formats/osk.hpp"
#include "formats/osn.hpp"
#include "importer/incremental.hpp"
#include "importer/legacy/data_source.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

namespace spud::importer {

inline constexpr uint32_t k_hkx_converter_version = 6;

// One packfile as an object graph from its root-level container. Objects of classes the tables leave out are dropped.
// Throws ParseError on a malformed file.
osb::Graph read_hkx(std::span<const std::byte> file);

struct HkxImport {
    size_t graphs = 0;
    size_t skeletons = 0;
    size_t clips = 0;
    size_t failed = 0;
};

// Every .hkx under meshes/.
HkxImport import_hkx(const legacy::DataSource& data, AssetCache& assets, const std::filesystem::path& cache,
                     unsigned threads, bool force);

} // namespace spud::importer
