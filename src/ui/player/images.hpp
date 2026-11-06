#pragma once

// Images the engine hands movies by name: `img://NAME` as an inline <img src> or as a loadMovie /
// MovieClipLoader.loadClip URL. Skyrim's GFx image loader (
// LoadImage) strips "img://" and looks NAME up, case kept, among the images the engine registered
// with it, each counted by its users: a texture the engine loaded, a render target, or a file
// the loader reads by path. Every image it returns is 64 x 64 to GFx, whatever the texture's own size
//
// The registry holds the names; the renderer (ui/movie_renderer) holds the pixels. Ids stay valid for the
// registry's life (a name released to zero users keeps its id for when it comes back).

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud::ui::player {

enum class ImageSource : uint8_t {
    texture,       // a file of the cache (`path`), loaded by the renderer
    render_target, // an image the engine renders, registered with the renderer under the image's name
};

struct EngineImage {
    std::string name; // as registered: the img:// URL without its scheme
    ImageSource source = ImageSource::texture;
    std::string path; // texture: its file in the cache ("textures/interface/books/.../t_letter.dds")
    uint32_t users = 0;
    float width = 64.0f, height = 64.0f; // its size to GFx
};

class ImageRegistry {
public:
    static constexpr uint32_t k_none = 0xFFFFFFFFu;
    // GFx's size of every image the engine's loader returns.
    static constexpr float k_size = 64.0f;
    // What GFx shows for an img:// URL nothing is registered under: its own default image, 57 x 36
    // (the game falls back to when the loader returns nothing; measured in
    // the game, run images1: <IMG SRC="img://...nothere.png" WIDTH="57" HEIGHT="36">), as an inline image and
    // as a loaded movie alike. Its pixels are the importer's (importer/convert/ui_images.cpp, from the user's
    // the game).
    static constexpr float k_missing_width = 57.0f, k_missing_height = 36.0f;
    static constexpr const char* k_default_image_path = "interface/gfx_default_image.dds";

    // A name with one more user. A texture registers only if the cache has its file (as the game's loader
    // keeps an image only once it has read it); returns its id, or k_none.
    uint32_t add_texture(std::string_view name, std::string_view cache_path);
    uint32_t add_render_target(std::string_view name);
    // One user fewer.
    void release(std::string_view name);

    // img://NAME (or NAME): the registered image with users, or k_none.
    uint32_t resolve(std::string_view url) const;
    // GFx's default image (its file in the cache, relative to the cache root), once at startup;
    // default_image() is its id, or k_none without the file.
    void set_default_image(std::string_view cache_path);
    uint32_t default_image() const { return m_default; }
    // What GFx shows for img://NAME: the registered image, else its default image (k_none without one).
    uint32_t resolve_or_default(std::string_view url) const {
        const uint32_t id = resolve(url);
        return id != k_none ? id : m_default;
    }
    const EngineImage* get(uint32_t id) const { return id < m_images.size() ? &m_images[id] : nullptr; }
    size_t size() const { return m_images.size(); }

    // The textures the cache holds (relative paths, forward slashes), indexed once at startup so registering
    // never touches the disk.
    void set_cache_textures(std::unordered_set<std::string> paths) { m_cache_textures = std::move(paths); }
    bool cache_has(std::string_view path) const { return m_cache_textures.contains(std::string(path)); }

    // The cache file of a texture path as the game's books name them ("textures\interface\books\x.png"):
    // lower case, forward slashes, the extension .dds (the importer converts the game's PNGs,
    //).
    static std::string cache_texture_path(std::string_view game_path);
    // The part of a URL after "img://" (GFx's loader finds the scheme case-insensitively), or "" if it has
    // none.
    static std::string_view name_of(std::string_view url);

private:
    uint32_t add(std::string_view name, ImageSource source, std::string_view path);

    std::vector<EngineImage> m_images;
    std::unordered_map<std::string, uint32_t> m_by_name;
    std::unordered_set<std::string> m_cache_textures;
    uint32_t m_default = k_none;
};

} // namespace spud::ui::player
