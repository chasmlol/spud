#pragma once

// CPU access to cached textures for the far-view bakes: block decoding, a sampled image, and writing mip chains
// back as block-compressed DDS. Only the importer reads pixels on the CPU; the engine keeps textures on the GPU.

#include "formats/dds.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::importer::far {

// One decoded mip level, RGBA8.
struct Texels {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;

    explicit operator bool() const { return !rgba.empty(); }
    const uint8_t* at(uint32_t x, uint32_t y) const { return rgba.data() + (size_t(y) * width + x) * 4; }
    // Bilinear with wrapped or clamped addressing, 0..1 per channel; u, v in texture coordinates.
    std::array<float, 4> sample(float u, float v, bool wrap) const;
};

// The level of `image` nearest to `want` texels across, never larger than the image; empty for formats the bakes
// don't read (BC6H, BC7: nothing in the base game uses them).
Texels decode_level(const dds::Image& image, uint32_t want);

// Decoded levels of cached textures, shared by the bake's threads. Missing or unreadable textures give an empty
// Texels once and are logged once.
class TexelCache {
public:
    explicit TexelCache(std::filesystem::path root) : m_root(std::move(root)) {}
    std::shared_ptr<const Texels> get(const std::string& path, uint32_t want);

private:
    std::filesystem::path m_root;
    std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<const Texels>> m_levels;
};

// A float image being built: four channels per texel.
struct Canvas {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> texels; // rgba, 0..1

    Canvas() = default;
    Canvas(uint32_t w, uint32_t h) : width(w), height(h), texels(size_t(w) * h * 4, 0.0f) {}
    float* at(uint32_t x, uint32_t y) { return texels.data() + (size_t(y) * width + x) * 4; }
    const float* at(uint32_t x, uint32_t y) const { return texels.data() + (size_t(y) * width + x) * 4; }
};

// Half-size box filter; with `coverage` the rgb average is weighted by alpha, so transparent texels don't bleed
// their colour into the next level.
Canvas downsample(const Canvas& src, bool coverage);

enum class Encoding { bc1, bc3, bc5 };

// A full mip chain down to 4 texels, block compressed: `threads` workers share the blocks. BC5 stores r and g.
dds::Image encode(const Canvas& top, Encoding encoding, bool coverage, unsigned threads);

} // namespace spud::importer::far
