#pragma once

#include <filesystem>

namespace spud::engine {

// Writes a small synthetic import cache: worlds/tamriel.osw (9x9 hilly land
// cells with layered textures, a lake, a strip of water-only cells), test
// meshes (cubes, rotation arrows, alpha-tested foliage, blended glass,
// additive effect cubes) and generated DDS textures in several formats.
void make_test_cache(const std::filesystem::path& dir);

} // namespace spud::engine
