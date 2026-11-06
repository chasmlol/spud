#include "formats/dds.hpp"

#include "core/binary.hpp"

#include <algorithm>
#include <bit>
#include <format>

namespace spud::dds {

namespace {

constexpr uint32_t k_magic = 0x20534444; // "DDS "

constexpr uint32_t ddsd_mipmapcount = 0x20000;
constexpr uint32_t ddsd_depth = 0x800000;

constexpr uint32_t ddpf_alphapixels = 0x1;
constexpr uint32_t ddpf_alpha = 0x2;
constexpr uint32_t ddpf_fourcc = 0x4;
constexpr uint32_t ddpf_rgb = 0x40;
constexpr uint32_t ddpf_luminance = 0x20000;

constexpr uint32_t ddscaps_texture = 0x1000;
constexpr uint32_t ddscaps_mipmap = 0x400000;
constexpr uint32_t ddscaps_complex = 0x8;
constexpr uint32_t ddscaps2_cubemap = 0x200;
constexpr uint32_t ddscaps2_cubemap_all_faces = 0xFC00;
constexpr uint32_t ddscaps2_volume = 0x200000;

constexpr uint32_t dx10_misc_texturecube = 0x4;
constexpr uint32_t dx10_dimension_texture3d = 4;

constexpr uint32_t make_fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

struct PixelFormat {
    uint32_t size;
    uint32_t flags;
    uint32_t fourcc;
    uint32_t bit_count;
    uint32_t r_mask;
    uint32_t g_mask;
    uint32_t b_mask;
    uint32_t a_mask;
};
static_assert(sizeof(PixelFormat) == 32);

struct Header {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitch_or_linear_size;
    uint32_t depth;
    uint32_t mip_map_count;
    uint32_t reserved1[11];
    PixelFormat pf;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};
static_assert(sizeof(Header) == 124);

struct HeaderDx10 {
    uint32_t dxgi_format;
    uint32_t resource_dimension;
    uint32_t misc_flag;
    uint32_t array_size;
    uint32_t misc_flags2;
};
static_assert(sizeof(HeaderDx10) == 20);

// How the source texels must be turned into the output format.
enum class Conversion { none, masks };

struct SourceLayout {
    Format format = Format::rgba8;
    bool srgb = false;
    Conversion conversion = Conversion::none;
    uint32_t src_bytes = 0; // per texel, for mask conversion
    PixelFormat masks{};    // for mask conversion
    bool luminance = false; // masks: replicate R into RGB
};

bool from_dxgi(uint32_t dxgi, SourceLayout& out) {
    switch (dxgi) {
    case 70: case 71: out.format = Format::bc1; return true;
    case 72: out.format = Format::bc1; out.srgb = true; return true;
    case 73: case 74: out.format = Format::bc2; return true;
    case 75: out.format = Format::bc2; out.srgb = true; return true;
    case 76: case 77: out.format = Format::bc3; return true;
    case 78: out.format = Format::bc3; out.srgb = true; return true;
    case 79: case 80: out.format = Format::bc4; return true;
    case 81: out.format = Format::bc4_snorm; return true;
    case 82: case 83: out.format = Format::bc5; return true;
    case 84: out.format = Format::bc5_snorm; return true;
    case 94: case 95: out.format = Format::bc6h_ufloat; return true;
    case 96: out.format = Format::bc6h_sfloat; return true;
    case 97: case 98: out.format = Format::bc7; return true;
    case 99: out.format = Format::bc7; out.srgb = true; return true;
    case 27: case 28: out.format = Format::rgba8; return true;
    case 29: out.format = Format::rgba8; out.srgb = true; return true;
    case 87: case 90: out.format = Format::bgra8; return true;
    case 91: out.format = Format::bgra8; out.srgb = true; return true;
    case 88: case 92: out.format = Format::bgrx8; return true;
    case 93: out.format = Format::bgrx8; out.srgb = true; return true;
    case 60: case 61: out.format = Format::r8; return true;
    case 48: case 49: out.format = Format::rg8; return true;
    case 65: out.format = Format::a8; return true;
    case 85: // B5G6R5
        out.format = Format::rgba8;
        out.conversion = Conversion::masks;
        out.src_bytes = 2;
        out.masks = {32, ddpf_rgb, 0, 16, 0xF800, 0x07E0, 0x001F, 0};
        return true;
    case 86: // B5G5R5A1
        out.format = Format::rgba8;
        out.conversion = Conversion::masks;
        out.src_bytes = 2;
        out.masks = {32, ddpf_rgb | ddpf_alphapixels, 0, 16, 0x7C00, 0x03E0, 0x001F, 0x8000};
        return true;
    case 115: // B4G4R4A4
        out.format = Format::rgba8;
        out.conversion = Conversion::masks;
        out.src_bytes = 2;
        out.masks = {32, ddpf_rgb | ddpf_alphapixels, 0, 16, 0x0F00, 0x00F0, 0x000F, 0xF000};
        return true;
    default:
        return false;
    }
}

SourceLayout layout_from_legacy(const PixelFormat& pf) {
    SourceLayout out;
    if (pf.flags & ddpf_fourcc) {
        switch (pf.fourcc) {
        case make_fourcc('D', 'X', 'T', '1'): out.format = Format::bc1; return out;
        case make_fourcc('D', 'X', 'T', '2'):
        case make_fourcc('D', 'X', 'T', '3'): out.format = Format::bc2; return out;
        case make_fourcc('D', 'X', 'T', '4'):
        case make_fourcc('D', 'X', 'T', '5'): out.format = Format::bc3; return out;
        case make_fourcc('A', 'T', 'I', '1'):
        case make_fourcc('B', 'C', '4', 'U'): out.format = Format::bc4; return out;
        case make_fourcc('B', 'C', '4', 'S'): out.format = Format::bc4_snorm; return out;
        case make_fourcc('A', 'T', 'I', '2'):
        case make_fourcc('B', 'C', '5', 'U'): out.format = Format::bc5; return out;
        case make_fourcc('B', 'C', '5', 'S'): out.format = Format::bc5_snorm; return out;
        default:
            // D3DFMT numeric codes stored in the fourcc field.
            if (pf.fourcc == 21) { // A8R8G8B8
                out.format = Format::bgra8;
                return out;
            }
            if (pf.fourcc == 22) { // X8R8G8B8
                out.format = Format::bgrx8;
                return out;
            }
            if (pf.fourcc == 50) { // L8
                out.format = Format::l8;
                return out;
            }
            if (pf.fourcc == 51) { // A8L8
                out.format = Format::l8a8;
                return out;
            }
            throw ParseError(std::format("unsupported DDS fourcc 0x{:08x}", pf.fourcc));
        }
    }

    const bool has_alpha = (pf.flags & ddpf_alphapixels) != 0;
    if (pf.flags & ddpf_rgb) {
        if (pf.bit_count == 32) {
            if (pf.r_mask == 0x00FF0000 && pf.g_mask == 0x0000FF00 && pf.b_mask == 0x000000FF) {
                out.format = (has_alpha && pf.a_mask == 0xFF000000) ? Format::bgra8 : Format::bgrx8;
                return out;
            }
            if (pf.r_mask == 0x000000FF && pf.g_mask == 0x0000FF00 && pf.b_mask == 0x00FF0000 && has_alpha &&
                pf.a_mask == 0xFF000000) {
                out.format = Format::rgba8;
                return out;
            }
        }
        if (pf.bit_count != 16 && pf.bit_count != 24 && pf.bit_count != 32) {
            throw ParseError(std::format("unsupported DDS RGB bit count {}", pf.bit_count));
        }
        out.format = Format::rgba8;
        out.conversion = Conversion::masks;
        out.src_bytes = pf.bit_count / 8;
        out.masks = pf;
        if (!has_alpha) {
            out.masks.a_mask = 0;
        }
        return out;
    }
    if (pf.flags & ddpf_luminance) {
        if (pf.bit_count == 8 && pf.r_mask == 0xFF) {
            out.format = Format::l8;
            return out;
        }
        if (pf.bit_count == 16 && has_alpha && pf.r_mask == 0x00FF && pf.a_mask == 0xFF00) {
            out.format = Format::l8a8;
            return out;
        }
        if (pf.bit_count != 8 && pf.bit_count != 16) {
            throw ParseError(std::format("unsupported DDS luminance bit count {}", pf.bit_count));
        }
        out.format = Format::rgba8;
        out.conversion = Conversion::masks;
        out.src_bytes = pf.bit_count / 8;
        out.masks = pf;
        out.luminance = true;
        if (!has_alpha) {
            out.masks.a_mask = 0;
        }
        return out;
    }
    if ((pf.flags & ddpf_alpha) && pf.bit_count == 8) {
        out.format = Format::a8;
        return out;
    }
    throw ParseError(std::format("unsupported DDS pixel format (flags 0x{:x}, {} bits)", pf.flags, pf.bit_count));
}

// Extracts the channel selected by `mask` and rescales it to 8 bits.
uint8_t extract_channel(uint32_t texel, uint32_t mask, uint8_t fallback) {
    if (mask == 0) {
        return fallback;
    }
    const int shift = std::countr_zero(mask);
    const uint32_t max = mask >> shift;
    const uint32_t value = (texel & mask) >> shift;
    return static_cast<uint8_t>((value * 255 + max / 2) / max);
}

void convert_masks(const SourceLayout& layout, std::span<const std::byte> src, uint32_t width, uint32_t height,
                   std::byte* dst) {
    const size_t count = static_cast<size_t>(width) * height;
    const auto* s = reinterpret_cast<const uint8_t*>(src.data());
    auto* d = reinterpret_cast<uint8_t*>(dst);
    for (size_t i = 0; i < count; ++i) {
        uint32_t texel = 0;
        for (uint32_t b = 0; b < layout.src_bytes; ++b) {
            texel |= static_cast<uint32_t>(s[i * layout.src_bytes + b]) << (8 * b);
        }
        const uint8_t r = extract_channel(texel, layout.masks.r_mask, 0);
        if (layout.luminance) {
            d[i * 4 + 0] = r;
            d[i * 4 + 1] = r;
            d[i * 4 + 2] = r;
        } else {
            d[i * 4 + 0] = r;
            d[i * 4 + 1] = extract_channel(texel, layout.masks.g_mask, 0);
            d[i * 4 + 2] = extract_channel(texel, layout.masks.b_mask, 0);
        }
        d[i * 4 + 3] = extract_channel(texel, layout.masks.a_mask, 255);
    }
}

} // namespace

std::string_view to_string(Format format) {
    switch (format) {
    case Format::bc1: return "BC1";
    case Format::bc2: return "BC2";
    case Format::bc3: return "BC3";
    case Format::bc4: return "BC4";
    case Format::bc4_snorm: return "BC4_SNORM";
    case Format::bc5: return "BC5";
    case Format::bc5_snorm: return "BC5_SNORM";
    case Format::bc6h_ufloat: return "BC6H_UF";
    case Format::bc6h_sfloat: return "BC6H_SF";
    case Format::bc7: return "BC7";
    case Format::rgba8: return "RGBA8";
    case Format::bgra8: return "BGRA8";
    case Format::bgrx8: return "BGRX8";
    case Format::r8: return "R8";
    case Format::rg8: return "RG8";
    case Format::l8: return "L8";
    case Format::l8a8: return "L8A8";
    case Format::a8: return "A8";
    }
    return "?";
}

bool is_block_compressed(Format format) { return format <= Format::bc7; }

uint32_t block_or_texel_bytes(Format format) {
    switch (format) {
    case Format::bc1:
    case Format::bc4:
    case Format::bc4_snorm:
        return 8;
    case Format::bc2:
    case Format::bc3:
    case Format::bc5:
    case Format::bc5_snorm:
    case Format::bc6h_ufloat:
    case Format::bc6h_sfloat:
    case Format::bc7:
        return 16;
    case Format::rgba8:
    case Format::bgra8:
    case Format::bgrx8:
        return 4;
    case Format::rg8:
    case Format::l8a8:
        return 2;
    case Format::r8:
    case Format::l8:
    case Format::a8:
        return 1;
    }
    return 4;
}

size_t level_size(Format format, uint32_t width, uint32_t height) {
    if (is_block_compressed(format)) {
        const size_t bw = std::max(1u, (width + 3) / 4);
        const size_t bh = std::max(1u, (height + 3) / 4);
        return bw * bh * block_or_texel_bytes(format);
    }
    return static_cast<size_t>(width) * height * block_or_texel_bytes(format);
}

Image parse(std::span<const std::byte> file) {
    BinaryReader r(file);
    if (r.read<uint32_t>() != k_magic) {
        throw ParseError("not a DDS file");
    }
    const auto h = r.read<Header>();
    if (h.size != sizeof(Header)) {
        throw ParseError(std::format("bad DDS header size {}", h.size));
    }
    if (h.width == 0 || h.height == 0 || h.width > 16384 || h.height > 16384) {
        throw ParseError(std::format("bad DDS dimensions {}x{}", h.width, h.height));
    }

    SourceLayout layout;
    Image image;
    image.width = h.width;
    image.height = h.height;

    const bool dx10 = (h.pf.flags & ddpf_fourcc) && h.pf.fourcc == make_fourcc('D', 'X', '1', '0');
    if (dx10) {
        const auto x = r.read<HeaderDx10>();
        if (!from_dxgi(x.dxgi_format, layout)) {
            throw ParseError(std::format("unsupported DXGI format {}", x.dxgi_format));
        }
        if (x.resource_dimension == dx10_dimension_texture3d) {
            throw ParseError("volume DDS textures are not supported");
        }
        image.cubemap = (x.misc_flag & dx10_misc_texturecube) != 0;
        image.layer_count = std::max(1u, x.array_size) * (image.cubemap ? 6 : 1);
    } else {
        if ((h.caps2 & ddscaps2_volume) || ((h.flags & ddsd_depth) && h.depth > 1)) {
            throw ParseError("volume DDS textures are not supported");
        }
        layout = layout_from_legacy(h.pf);
        if (h.caps2 & ddscaps2_cubemap) {
            if ((h.caps2 & ddscaps2_cubemap_all_faces) != ddscaps2_cubemap_all_faces) {
                throw ParseError("partial DDS cubemaps are not supported");
            }
            image.cubemap = true;
            image.layer_count = 6;
        }
    }
    image.format = layout.format;
    image.srgb = layout.srgb;

    uint32_t mips = 1;
    if ((h.flags & ddsd_mipmapcount) || (h.caps & ddscaps_mipmap)) {
        mips = std::max(1u, h.mip_map_count);
    }
    // A full chain for the largest dimension is the most a file can hold.
    const uint32_t max_mips = 1 + static_cast<uint32_t>(std::bit_width(std::max(h.width, h.height)) - 1);
    mips = std::min(mips, max_mips);
    (void)ddscaps_texture;
    (void)ddscaps_complex;

    // Source sizes per level (may differ from output for converted formats).
    auto src_level_size = [&](uint32_t w, uint32_t hgt) -> size_t {
        if (layout.conversion == Conversion::masks) {
            return static_cast<size_t>(w) * hgt * layout.src_bytes;
        }
        return level_size(layout.format, w, hgt);
    };
    auto src_layer_size = [&](uint32_t mip_count) {
        size_t total = 0;
        for (uint32_t m = 0; m < mip_count; ++m) {
            total += src_level_size(std::max(1u, h.width >> m), std::max(1u, h.height >> m));
        }
        return total;
    };

    // Clamp truncated mip chains (seen in some mod textures) instead of rejecting.
    const size_t available = r.remaining();
    while (mips > 1 && src_layer_size(mips) * image.layer_count > available) {
        --mips;
    }
    if (src_layer_size(mips) * image.layer_count > available) {
        throw ParseError(std::format("truncated DDS data ({} bytes, need {})", available,
                                     src_layer_size(mips) * image.layer_count));
    }
    image.mip_count = mips;

    size_t out_total = 0;
    for (uint32_t layer = 0; layer < image.layer_count; ++layer) {
        for (uint32_t m = 0; m < mips; ++m) {
            const uint32_t w = std::max(1u, h.width >> m);
            const uint32_t hh = std::max(1u, h.height >> m);
            const size_t size = level_size(image.format, w, hh);
            image.subresources.push_back({out_total, size, w, hh});
            out_total += size;
        }
    }
    image.data.resize(out_total);

    const size_t layer_stride = src_layer_size(max_mips < mips ? mips : std::min(mips, max_mips));
    (void)layer_stride;
    // Each layer in the file stores the file's full declared chain; skip the levels we clamped away.
    uint32_t declared = 1;
    if ((h.flags & ddsd_mipmapcount) || (h.caps & ddscaps_mipmap)) {
        declared = std::min(std::max(1u, h.mip_map_count), max_mips);
    }
    const size_t src_stride = image.layer_count > 1 ? src_layer_size(declared) : 0;
    const size_t base = r.tell();
    for (uint32_t layer = 0; layer < image.layer_count; ++layer) {
        size_t src = base + layer * src_stride;
        if (image.layer_count > 1 && src + src_layer_size(mips) > file.size()) {
            throw ParseError("truncated DDS cubemap/array data");
        }
        for (uint32_t m = 0; m < mips; ++m) {
            const auto& sub = image.subresource(layer, m);
            const size_t src_size = src_level_size(sub.width, sub.height);
            auto bytes = file.subspan(src, src_size);
            if (layout.conversion == Conversion::masks) {
                convert_masks(layout, bytes, sub.width, sub.height, image.data.data() + sub.offset);
            } else {
                std::memcpy(image.data.data() + sub.offset, bytes.data(), src_size);
            }
            src += src_size;
        }
    }
    return image;
}

std::vector<std::byte> write(const Image& image) {
    BinaryWriter w;
    w.write(k_magic);
    Header h{};
    h.size = sizeof(Header);
    h.flags = 0x1 | 0x2 | 0x4 | 0x1000; // caps, height, width, pixelformat
    if (image.mip_count > 1) {
        h.flags |= ddsd_mipmapcount;
    }
    h.width = image.width;
    h.height = image.height;
    h.mip_map_count = image.mip_count;
    h.pf.size = sizeof(PixelFormat);
    h.caps = ddscaps_texture | (image.mip_count > 1 ? (ddscaps_mipmap | ddscaps_complex) : 0);

    bool need_dx10 = false;
    uint32_t dxgi = 0;
    switch (image.format) {
    case Format::bc1: h.pf.flags = ddpf_fourcc; h.pf.fourcc = make_fourcc('D', 'X', 'T', '1'); break;
    case Format::bc2: h.pf.flags = ddpf_fourcc; h.pf.fourcc = make_fourcc('D', 'X', 'T', '3'); break;
    case Format::bc3: h.pf.flags = ddpf_fourcc; h.pf.fourcc = make_fourcc('D', 'X', 'T', '5'); break;
    case Format::rgba8:
        h.pf = {32, ddpf_rgb | ddpf_alphapixels, 0, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000};
        break;
    case Format::bgra8:
        h.pf = {32, ddpf_rgb | ddpf_alphapixels, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000};
        break;
    case Format::bgrx8: h.pf = {32, ddpf_rgb, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0}; break;
    case Format::l8: h.pf = {32, ddpf_luminance, 0, 8, 0xFF, 0, 0, 0}; break;
    case Format::l8a8: h.pf = {32, ddpf_luminance | ddpf_alphapixels, 0, 16, 0x00FF, 0, 0, 0xFF00}; break;
    case Format::a8: h.pf = {32, ddpf_alpha, 0, 8, 0, 0, 0, 0xFF}; break;
    case Format::bc4: need_dx10 = true; dxgi = 80; break;
    case Format::bc4_snorm: need_dx10 = true; dxgi = 81; break;
    case Format::bc5: need_dx10 = true; dxgi = 83; break;
    case Format::bc5_snorm: need_dx10 = true; dxgi = 84; break;
    case Format::bc6h_ufloat: need_dx10 = true; dxgi = 95; break;
    case Format::bc6h_sfloat: need_dx10 = true; dxgi = 96; break;
    case Format::bc7: need_dx10 = true; dxgi = 98; break;
    case Format::r8: need_dx10 = true; dxgi = 61; break;
    case Format::rg8: need_dx10 = true; dxgi = 49; break;
    }
    if (image.cubemap) {
        h.caps |= ddscaps_complex;
        h.caps2 = ddscaps2_cubemap | ddscaps2_cubemap_all_faces;
    }
    if (need_dx10 || (image.layer_count > 1 && !image.cubemap)) {
        if (!need_dx10) {
            throw Error("DDS writer: texture arrays need a DX10 format");
        }
        h.pf = {32, ddpf_fourcc, make_fourcc('D', 'X', '1', '0'), 0, 0, 0, 0, 0};
        w.write(h);
        HeaderDx10 x{};
        x.dxgi_format = dxgi;
        x.resource_dimension = 3;
        x.misc_flag = image.cubemap ? dx10_misc_texturecube : 0;
        x.array_size = image.cubemap ? image.layer_count / 6 : image.layer_count;
        w.write(x);
    } else {
        w.write(h);
    }
    w.write_bytes(image.data);
    return w.take();
}

} // namespace spud::dds
