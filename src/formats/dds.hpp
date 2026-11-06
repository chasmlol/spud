#pragma once

// DDS texture container (generic GPU format, kept as-is by the importer).
// Parses the legacy header and the DX10 extension into a GPU-ready image:
// block-compressed data is passed through, odd uncompressed layouts (24-bit,
// 16-bit, unusual masks) are expanded to RGBA8 on the CPU.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace spud::dds {

enum class Format : uint8_t {
    bc1,
    bc2,
    bc3,
    bc4,
    bc4_snorm,
    bc5,
    bc5_snorm,
    bc6h_ufloat,
    bc6h_sfloat,
    bc7,
    rgba8,
    bgra8,
    bgrx8,  // alpha channel must read as 1
    r8,
    rg8,
    l8,     // luminance: sample as (l, l, l, 1)
    l8a8,   // stored as two channels (l, a): sample as (l, l, l, a)
    a8,     // alpha only: sample as (1, 1, 1, a)
};

std::string_view to_string(Format format);
bool is_block_compressed(Format format);
// Bytes per 4x4 block for compressed formats, bytes per texel otherwise.
uint32_t block_or_texel_bytes(Format format);
size_t level_size(Format format, uint32_t width, uint32_t height);

struct Subresource {
    size_t offset; // into Image::data
    size_t size;
    uint32_t width;
    uint32_t height;
};

struct Image {
    Format format = Format::rgba8;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_count = 1;
    uint32_t layer_count = 1; // 6 for cubemaps
    bool cubemap = false;
    bool srgb = false; // what the file claims; spud samples everything as UNORM anyway
    // Tightly packed, layer-major: subresource(layer, mip) = subresources[layer * mip_count + mip].
    std::vector<std::byte> data;
    std::vector<Subresource> subresources;

    const Subresource& subresource(uint32_t layer, uint32_t mip) const {
        return subresources[layer * mip_count + mip];
    }
};

// Throws ParseError for malformed or unsupported files. Truncated mip chains
// are clamped to the levels that are fully present.
Image parse(std::span<const std::byte> file);

// Writes a DDS file (legacy header for BC1/2/3 and 32-bit RGBA/BGRA, DX10 header
// otherwise). Used by tools and tests that generate textures.
std::vector<std::byte> write(const Image& image);

} // namespace spud::dds
