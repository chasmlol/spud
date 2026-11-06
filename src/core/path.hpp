#pragma once

#include <string>
#include <string_view>

namespace spud::path {

std::string to_lower(std::string_view s);

// Normalises a path as written in game data ("Textures\\Foo.DDS",
// "data\\textures\\foo.dds", "/meshes//a.nif", "x\\build\\pc\\data\\textures\\a.dds")
// to the canonical form used everywhere in spud: lowercase, forward slashes,
// relative to the data folder ("textures/foo.dds").
std::string normalize(std::string_view p);

// Ensures a normalised path begins with `root` + "/" (e.g. "textures").
// Game data frequently omits the root folder in texture and mesh paths.
std::string with_root(std::string_view normalized, std::string_view root);

// "meshes/a/b.nif" + ".osm" -> "meshes/a/b.osm"
std::string replace_extension(std::string_view p, std::string_view extension);

} // namespace spud::path
