#pragma once

// The images the game's interface names through img://: every texture a book's text
// puts in an <img src='img://...'> (the drop caps, maps, drawings), from the winning BOOK records' DESC, into
// the cache as RGBA8 DDS at the path the engine looks them up by (textures/.../name.dds,
// ui::player::ImageRegistry), one level as the game's texture has (GFx's InitTexture gives a
// 4-byte image one mip level). PNGs are decoded as the game's loader decodes them (libpng 1.2.18
// through, to 8-bit RGB or RGBA, with its gamma step); DDS files are decoded to RGBA8 the same
// way.

#include <filesystem>
#include <string>
#include <vector>

namespace spud::legacy {
class DataSource;
class EsmDatabase;
} // namespace spud::legacy

namespace spud::importer {

// The texture paths (as the game's text processing leaves them: lower case, '\' separators, no "img://") of
// every <img> in the books' texts, sorted and unique.
std::vector<std::string> interface_image_paths(const legacy::EsmDatabase& db, const legacy::DataSource& data);

// Converts them into the cache; existing outputs are kept unless `force`. Returns the number written or kept.
size_t write_interface_images(const legacy::EsmDatabase& db, const legacy::DataSource& data,
                              const std::string& cache_dir, bool force);

// GFx's own image for an img:// URL the engine's loader has nothing for (the game
// falls back to): 57 x 36 pixels, 19 x 12 cells of 3 x 3, each cell's colour named by one character
// of a pattern the game holds ( in 1.7.104). The pattern is read from the user's executable
// (never kept in the repo) and the image written to <cache>/interface/gfx_default_image.dds (RGBA8, straight
// alpha). Returns false, with a warning, when the game or the pattern is not found.
inline constexpr const char* k_gfx_default_image = "interface/gfx_default_image.dds";
bool write_gfx_default_image(const std::filesystem::path& game_dir, const std::string& cache_dir);

} // namespace spud::importer
