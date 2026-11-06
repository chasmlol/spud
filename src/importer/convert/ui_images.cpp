#include "importer/convert/ui_images.hpp"

#include "importer/convert/ui_strings.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/esm.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "formats/dds.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <set>

namespace spud::importer {

namespace {

// The book text processing's view of an <img> tag (): "<img" then " src=" and a quote,
// the value up to the next quote; "img://" off the front; lower case with '/' as '\'.
void scan(std::string_view text, std::set<std::string>& out) {
    for (size_t i = text.find("<img"); i != std::string_view::npos; i = text.find("<img", i + 4)) {
        if (i + 10 > text.size() || (text[i + 9] != '\'' && text[i + 9] != '"')) {
            continue;
        }
        const size_t end = text.find_first_of("'\"", i + 10);
        if (end == std::string_view::npos) {
            break;
        }
        std::string lower(text.substr(i + 10, end - i - 10));
        if (lower.size() >= 6 && std::equal(lower.begin(), lower.begin() + 6, "img://", [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == b;
            })) {
            lower.erase(0, 6);
        }
        for (char& c : lower) {
            c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (!lower.empty()) {
            out.insert(lower);
        }
    }
}

// The data path of a game path ("textures\x\y.png" -> "textures/x/y.png") and the cache file the engine reads
// ("textures/x/y.dds"; ui::player::ImageRegistry::cache_texture_path).
std::string data_path(std::string_view game_path) {
    std::string p(game_path);
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.starts_with('/')) {
        p.erase(0, 1);
    }
    return p;
}

std::string cache_path(std::string_view game_path) {
    std::string p = data_path(game_path);
    const size_t slash = p.rfind('/'), dot = p.rfind('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        p.erase(dot);
    }
    return p + ".dds";
}

// The game's PNG reader (libpng 1.2.18; set-up) asks libpng for gamma correction from
// the file's gAMA to a screen gamma of 2.2, through png_set_gamma: it applies when
// |2.2 x gamma - 1| > 0.05, or the colour type has alpha (4, 6) or a palette (3). An sRGB chunk sets the file
// gamma to 0.45455 (libpng's png_handle_sRGB). libpng's 8-bit table (png_build_gamma_table, pngrtran.c) maps
// each colour sample i to (int)(pow(i / 255, 1 / (gamma x 2.2)) x 255 + 0.5); alpha is left as it is. Every
// interface PNG the game ships is 8-bit RGBA (the 119 of Skyrim - Textures5.bsa): 109 have no gAMA and no
// sRGB, 10 gAMA 0.45454, whose table is the identity, so this changes none of them.
void apply_png_gamma(const std::vector<std::byte>& bytes, std::vector<std::byte>& rgba) {
    const auto* b = reinterpret_cast<const uint8_t*>(bytes.data());
    auto be32 = [&](size_t at) {
        return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 |
               uint32_t(b[at + 3]);
    };
    uint32_t bit_depth = 8, colour_type = 0, gama = 0;
    bool srgb = false;
    for (size_t at = 8; at + 12 <= bytes.size();) {
        const uint32_t n = be32(at);
        const std::string_view type(reinterpret_cast<const char*>(b + at + 4), 4);
        if (at + 12 + size_t(n) > bytes.size() || type == "IDAT" || type == "IEND") {
            break;
        }
        if (type == "IHDR" && n >= 10) {
            bit_depth = b[at + 16];
            colour_type = b[at + 17];
        } else if (type == "gAMA" && n == 4) {
            gama = be32(at + 8);
        } else if (type == "sRGB") {
            srgb = true;
        }
        at += 12 + size_t(n);
    }
    if (!gama && !srgb) {
        return;
    }
    const float file_gamma = srgb ? 0.45455f : float(gama) / 100000.0f, screen_gamma = 2.2f;
    if (std::fabs(double(screen_gamma) * file_gamma - 1.0) <= 0.05 && !(colour_type & 4) &&
        colour_type != 3) {
        return;
    }
    if (bit_depth > 8) {
        log::warn("ui images: a 16-bit PNG with gamma {} is converted without the game's gamma step",
                  file_gamma);
        return;
    }
    const double g = 1.0 / double(file_gamma * screen_gamma);
    uint8_t table[256];
    bool identity = true;
    for (int i = 0; i < 256; ++i) {
        table[i] = uint8_t(std::pow(double(i) / 255.0, g) * 255.0 + 0.5);
        identity = identity && table[i] == i;
    }
    if (identity) {
        return;
    }
    auto* px = reinterpret_cast<uint8_t*>(rgba.data());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        px[i] = table[px[i]];
        px[i + 1] = table[px[i + 1]];
        px[i + 2] = table[px[i + 2]];
    }
}

// RGBA8 straight alpha, rows top-down, from a PNG (or any image stb reads) or an uncompressed DDS.
bool decode(const std::vector<std::byte>& bytes, uint32_t& w, uint32_t& h, std::vector<std::byte>& rgba,
            std::string& error) {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) {
        const dds::Image image = dds::parse(bytes);
        const dds::Subresource& top = image.subresource(0, 0);
        const auto* src = reinterpret_cast<const uint8_t*>(image.data.data() + top.offset);
        w = top.width;
        h = top.height;
        rgba.resize(size_t(w) * h * 4);
        auto* dst = reinterpret_cast<uint8_t*>(rgba.data());
        for (size_t i = 0; i < size_t(w) * h; ++i) {
            switch (image.format) {
            case dds::Format::rgba8:
                std::memcpy(dst + 4 * i, src + 4 * i, 4);
                break;
            case dds::Format::bgra8:
            case dds::Format::bgrx8:
                dst[4 * i] = src[4 * i + 2];
                dst[4 * i + 1] = src[4 * i + 1];
                dst[4 * i + 2] = src[4 * i];
                dst[4 * i + 3] = image.format == dds::Format::bgrx8 ? 255 : src[4 * i + 3];
                break;
            default:
                error = std::format("a {} DDS (only uncompressed ones are converted)",
                                    dds::to_string(image.format));
                return false;
            }
        }
        return true;
    }
    int iw = 0, ih = 0, channels = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()),
                                        &iw, &ih, &channels, 4);
    if (!px) {
        error = stbi_failure_reason() ? stbi_failure_reason() : "not an image";
        return false;
    }
    w = uint32_t(iw);
    h = uint32_t(ih);
    rgba.resize(size_t(w) * h * 4);
    std::memcpy(rgba.data(), px, rgba.size());
    stbi_image_free(px);
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "\x89PNG", 4) == 0) {
        apply_png_gamma(bytes, rgba);
    }
    return true;
}

// The cell colours of GFx's default image (one case per character), as the 32-bit values it writes
// into its ARGB_8888 image: in memory their bytes are the texture's R, G, B, A (InitTexture makes
// format 1 a DXGI_FORMAT_R8G8B8A8_UNORM texture of the image's own bytes; the game's screenshot shows 'b' red
// and 'r' blue, run images3). Any other character leaves its cell transparent.
uint32_t gfx_default_cell(char c) {
    switch (c) {
    case '#':
        return 0xFF000000u;
    case 'a':
        return 0x80FF00FFu;
    case 'b':
        return 0x802020FFu;
    case 'c':
        return 0x8000FFFFu;
    case 'g':
        return 0x8020FF20u;
    case 'r':
        return 0x80FF2020u;
    case 'y':
        return 0x80FFFF00u;
    default:
        return 0;
    }
}

constexpr uint32_t k_default_w = 57, k_default_h = 36, k_default_cell = 3;
constexpr size_t k_default_cells = (k_default_w / k_default_cell) * (k_default_h / k_default_cell); // 19 x 12

// The pattern in the game's bytes: the one run of exactly 228 cell characters ending in a NUL and not
// preceded by another ( in 1.7.104,.data; found by its shape so another build's place works too).
std::optional<std::string> find_default_pattern(const std::vector<std::byte>& exe) {
    auto cell = [](uint8_t c) {
        return c == '#' || c == 'a' || c == 'b' || c == 'c' || c == 'g' || c == 'r' || c == 'y';
    };
    const auto* b = reinterpret_cast<const uint8_t*>(exe.data());
    std::optional<std::string> found;
    size_t run = 0;
    for (size_t i = 0; i < exe.size(); ++i) {
        if (cell(b[i])) {
            ++run;
            continue;
        }
        if (b[i] == 0 && run == k_default_cells) {
            if (found) {
                return std::nullopt; // two candidates: not this pattern
            }
            found.emplace(reinterpret_cast<const char*>(b + i - run), run);
        }
        run = 0;
    }
    return found;
}

} // namespace

std::vector<std::string> interface_image_paths(const legacy::EsmDatabase& db,
                                               const legacy::DataSource& data) {
    const GameStrings strings(db, data, "english");
    std::set<std::string> paths;
    for (const legacy::RecordEntry* e : db.of_type(legacy::record_type("BOOK"))) {
        const legacy::Record rec = db.read(*e);
        scan(strings.text(rec, legacy::record_type("DESC")), paths);
    }
    return {paths.begin(), paths.end()};
}

size_t write_interface_images(const legacy::EsmDatabase& db, const legacy::DataSource& data,
                              const std::string& cache_dir, bool force) {
    const std::filesystem::path cache(cache_dir);
    size_t written = 0, kept = 0, missing = 0, failed = 0;
    for (const std::string& game_path : interface_image_paths(db, data)) {
        const auto out = cache / cache_path(game_path);
        std::error_code ec;
        if (!force && std::filesystem::exists(out, ec)) {
            ++kept;
            continue;
        }
        const auto bytes = data.read(data_path(game_path));
        if (!bytes) {
            log::debug("ui images: {} is not in the data", game_path);
            ++missing;
            continue;
        }
        uint32_t w = 0, h = 0;
        std::vector<std::byte> rgba;
        std::string error;
        try {
            if (!decode(*bytes, w, h, rgba, error)) {
                log::warn("ui images: {}: {}", game_path, error);
                ++failed;
                continue;
            }
        } catch (const std::exception& ex) {
            log::warn("ui images: {}: {}", game_path, ex.what());
            ++failed;
            continue;
        }
        dds::Image image;
        image.format = dds::Format::rgba8;
        image.width = w;
        image.height = h;
        image.data = std::move(rgba);
        image.subresources.push_back({0, image.data.size(), w, h});
        write_file(out, dds::write(image));
        ++written;
    }
    log::info("ui images: {} converted, {} kept, {} not in the data, {} failed", written, kept, missing,
              failed);
    return written + kept;
}

bool write_gfx_default_image(const std::filesystem::path& game_dir, const std::string& cache_dir) {
    const std::filesystem::path exe = game_dir / "SkyrimSE.exe";
    std::optional<std::string> pattern;
    try {
        pattern = find_default_pattern(read_file(exe));
    } catch (const std::exception& e) {
        log::warn("ui images: GFx's default image: {}", e.what());
        return false;
    }
    if (!pattern) {
        log::warn("ui images: GFx's default image: its pattern is not in {}", exe.string());
        return false;
    }
    // Row by row, 19 cells a row, each 3 x 3 pixels.
    dds::Image image;
    image.format = dds::Format::rgba8;
    image.width = k_default_w;
    image.height = k_default_h;
    image.data.resize(size_t(k_default_w) * k_default_h * 4);
    auto* px = reinterpret_cast<uint8_t*>(image.data.data());
    const uint32_t columns = k_default_w / k_default_cell;
    for (uint32_t y = 0; y < k_default_h; ++y) {
        for (uint32_t x = 0; x < k_default_w; ++x) {
            const uint32_t v =
                gfx_default_cell((*pattern)[(y / k_default_cell) * columns + x / k_default_cell]);
            uint8_t* p = px + (size_t(y) * k_default_w + x) * 4;
            p[0] = uint8_t(v);
            p[1] = uint8_t(v >> 8);
            p[2] = uint8_t(v >> 16);
            p[3] = uint8_t(v >> 24);
        }
    }
    image.subresources.push_back({0, image.data.size(), k_default_w, k_default_h});
    write_file(std::filesystem::path(cache_dir) / k_gfx_default_image, dds::write(image));
    log::info("ui images: GFx's default image ({} x {}) from {}", k_default_w, k_default_h, exe.string());
    return true;
}

} // namespace spud::importer
