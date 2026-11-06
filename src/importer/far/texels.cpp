#include "importer/far/texels.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <thread>

#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

namespace spud::importer::far {

namespace {

void decode_color_block(const uint8_t* b, uint8_t out[16][4], bool opaque_only) {
    const uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
    uint8_t p[4][4];
    auto expand = [](uint16_t c, uint8_t* o) {
        o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
        o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
        o[2] = uint8_t((c & 31) * 255 / 31);
        o[3] = 255;
    };
    expand(c0, p[0]);
    expand(c1, p[1]);
    if (c0 > c1 || opaque_only) {
        for (int i = 0; i < 3; ++i) {
            p[2][i] = uint8_t((2 * p[0][i] + p[1][i]) / 3);
            p[3][i] = uint8_t((p[0][i] + 2 * p[1][i]) / 3);
        }
        p[2][3] = p[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) {
            p[2][i] = uint8_t((p[0][i] + p[1][i]) / 2);
            p[3][i] = 0;
        }
        p[2][3] = 255;
        p[3][3] = 0;
    }
    const uint32_t bits = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; ++i) {
        std::memcpy(out[i], p[(bits >> (2 * i)) & 3], 4);
    }
}

void decode_alpha_block(const uint8_t* b, uint8_t out[16]) {
    uint8_t p[8];
    p[0] = b[0];
    p[1] = b[1];
    if (p[0] > p[1]) {
        for (int i = 1; i < 7; ++i) {
            p[1 + i] = uint8_t(((7 - i) * p[0] + i * p[1]) / 7);
        }
    } else {
        for (int i = 1; i < 5; ++i) {
            p[1 + i] = uint8_t(((5 - i) * p[0] + i * p[1]) / 5);
        }
        p[6] = 0;
        p[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= uint64_t(b[2 + i]) << (8 * i);
    }
    for (int i = 0; i < 16; ++i) {
        out[i] = p[(bits >> (3 * i)) & 7];
    }
}

} // namespace

std::array<float, 4> Texels::sample(float u, float v, bool wrap) const {
    const float x = u * float(width) - 0.5f, y = v * float(height) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    auto index = [wrap](int i, uint32_t n) {
        const int m = int(n);
        return uint32_t(wrap ? ((i % m) + m) % m : std::clamp(i, 0, m - 1));
    };
    const uint32_t x0 = index(int(fx), width), x1 = index(int(fx) + 1, width);
    const uint32_t y0 = index(int(fy), height), y1 = index(int(fy) + 1, height);
    const uint8_t *a = at(x0, y0), *b = at(x1, y0), *c = at(x0, y1), *d = at(x1, y1);
    std::array<float, 4> out;
    for (int i = 0; i < 4; ++i) {
        const float top = float(a[i]) + (float(b[i]) - float(a[i])) * tx;
        const float bottom = float(c[i]) + (float(d[i]) - float(c[i])) * tx;
        out[i] = (top + (bottom - top) * ty) * (1.0f / 255.0f);
    }
    return out;
}

Texels decode_level(const dds::Image& image, uint32_t want) {
    uint32_t mip = 0;
    while (mip + 1 < image.mip_count && std::max(1u, image.width >> (mip + 1)) >= want) {
        ++mip;
    }
    const dds::Subresource& s = image.subresource(0, mip);
    const auto* src = reinterpret_cast<const uint8_t*>(image.data.data()) + s.offset;
    Texels out;
    out.width = s.width;
    out.height = s.height;
    out.rgba.assign(size_t(s.width) * s.height * 4, 255);
    const uint32_t bw = (s.width + 3) / 4, bh = (s.height + 3) / 4;
    auto place = [&](uint32_t bx, uint32_t by, const uint8_t px[16][4]) {
        for (uint32_t i = 0; i < 16; ++i) {
            const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
            if (x < s.width && y < s.height) {
                std::memcpy(out.rgba.data() + (size_t(y) * s.width + x) * 4, px[i], 4);
            }
        }
    };
    uint8_t px[16][4];
    uint8_t alpha[16], alpha2[16];
    switch (image.format) {
    case dds::Format::bc1:
    case dds::Format::bc2:
    case dds::Format::bc3:
    case dds::Format::bc4:
    case dds::Format::bc5:
        for (uint32_t by = 0; by < bh; ++by) {
            for (uint32_t bx = 0; bx < bw; ++bx) {
                const uint8_t* b = src + (size_t(by) * bw + bx) * dds::block_or_texel_bytes(image.format);
                switch (image.format) {
                case dds::Format::bc1:
                    decode_color_block(b, px, false);
                    break;
                case dds::Format::bc2:
                    decode_color_block(b + 8, px, true);
                    for (int i = 0; i < 16; ++i) {
                        px[i][3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                    }
                    break;
                case dds::Format::bc3:
                    decode_color_block(b + 8, px, true);
                    decode_alpha_block(b, alpha);
                    for (int i = 0; i < 16; ++i) {
                        px[i][3] = alpha[i];
                    }
                    break;
                case dds::Format::bc4:
                    decode_alpha_block(b, alpha);
                    for (int i = 0; i < 16; ++i) {
                        px[i][0] = px[i][1] = px[i][2] = alpha[i];
                        px[i][3] = 255;
                    }
                    break;
                default:
                    decode_alpha_block(b, alpha);
                    decode_alpha_block(b + 8, alpha2);
                    for (int i = 0; i < 16; ++i) {
                        px[i][0] = alpha[i];
                        px[i][1] = alpha2[i];
                        px[i][2] = 255;
                        px[i][3] = 255;
                    }
                    break;
                }
                place(bx, by, px);
            }
        }
        return out;
    case dds::Format::rgba8:
        std::memcpy(out.rgba.data(), src, out.rgba.size());
        return out;
    case dds::Format::bgra8:
    case dds::Format::bgrx8:
        for (size_t i = 0; i < size_t(s.width) * s.height; ++i) {
            out.rgba[i * 4 + 0] = src[i * 4 + 2];
            out.rgba[i * 4 + 1] = src[i * 4 + 1];
            out.rgba[i * 4 + 2] = src[i * 4 + 0];
            out.rgba[i * 4 + 3] = image.format == dds::Format::bgrx8 ? 255 : src[i * 4 + 3];
        }
        return out;
    case dds::Format::r8:
    case dds::Format::l8:
        for (size_t i = 0; i < size_t(s.width) * s.height; ++i) {
            out.rgba[i * 4 + 0] = out.rgba[i * 4 + 1] = out.rgba[i * 4 + 2] = src[i];
        }
        return out;
    case dds::Format::rg8:
        for (size_t i = 0; i < size_t(s.width) * s.height; ++i) {
            out.rgba[i * 4 + 0] = src[i * 2];
            out.rgba[i * 4 + 1] = src[i * 2 + 1];
        }
        return out;
    case dds::Format::l8a8:
        for (size_t i = 0; i < size_t(s.width) * s.height; ++i) {
            out.rgba[i * 4 + 0] = out.rgba[i * 4 + 1] = out.rgba[i * 4 + 2] = src[i * 2];
            out.rgba[i * 4 + 3] = src[i * 2 + 1];
        }
        return out;
    case dds::Format::a8:
        for (size_t i = 0; i < size_t(s.width) * s.height; ++i) {
            out.rgba[i * 4 + 3] = src[i];
        }
        return out;
    default:
        return {};
    }
}

std::shared_ptr<const Texels> TexelCache::get(const std::string& path, uint32_t want) {
    const std::string key = std::format("{}|{}", path, want);
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_levels.find(key); it != m_levels.end()) {
            return it->second;
        }
    }
    auto texels = std::make_shared<Texels>();
    try {
        const auto bytes = read_file(m_root / path);
        *texels = decode_level(dds::parse(bytes), want);
        if (!*texels) {
            log::warn("far view: {} has a format the bakes don't read", path);
        }
    } catch (const std::exception& e) {
        log::warn("far view: {}: {}", path, e.what());
    }
    std::lock_guard lock(m_mutex);
    return m_levels.emplace(key, std::move(texels)).first->second;
}

Canvas downsample(const Canvas& src, bool coverage) {
    Canvas dst(std::max(1u, src.width / 2), std::max(1u, src.height / 2));
    for (uint32_t y = 0; y < dst.height; ++y) {
        for (uint32_t x = 0; x < dst.width; ++x) {
            float sum[4] = {0, 0, 0, 0};
            float weight = 0;
            for (uint32_t j = 0; j < 2; ++j) {
                for (uint32_t i = 0; i < 2; ++i) {
                    const float* t = src.at(std::min(x * 2 + i, src.width - 1), std::min(y * 2 + j, src.height - 1));
                    const float w = coverage ? t[3] : 1.0f;
                    sum[0] += t[0] * w;
                    sum[1] += t[1] * w;
                    sum[2] += t[2] * w;
                    sum[3] += t[3];
                    weight += w;
                }
            }
            float* d = dst.at(x, y);
            const float inv = weight > 0 ? 1.0f / weight : 0.0f;
            d[0] = sum[0] * inv;
            d[1] = sum[1] * inv;
            d[2] = sum[2] * inv;
            d[3] = sum[3] * 0.25f;
        }
    }
    return dst;
}

dds::Image encode(const Canvas& top, Encoding encoding, bool coverage, unsigned threads) {
    dds::Image image;
    image.format = encoding == Encoding::bc1   ? dds::Format::bc1
                   : encoding == Encoding::bc3 ? dds::Format::bc3
                                               : dds::Format::bc5;
    image.width = top.width;
    image.height = top.height;
    const uint32_t block_bytes = dds::block_or_texel_bytes(image.format);
    uint32_t mips = 1;
    for (uint32_t w = top.width, h = top.height; std::min(w, h) > 4; w /= 2, h /= 2) {
        ++mips;
    }
    std::vector<Canvas> levels;
    levels.reserve(mips);
    const Canvas* level = &top;
    size_t total = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t bw = (level->width + 3) / 4, bh = (level->height + 3) / 4;
        image.subresources.push_back({total, size_t(bw) * bh * block_bytes, level->width, level->height});
        total += size_t(bw) * bh * block_bytes;
        if (m + 1 < mips) {
            levels.push_back(downsample(*level, coverage));
            level = &levels.back();
        }
    }
    image.mip_count = mips;
    image.data.resize(total);
    for (uint32_t m = 0; m < mips; ++m) {
        const Canvas& src = m == 0 ? top : levels[m - 1];
        const dds::Subresource& sub = image.subresources[m];
        auto* out = reinterpret_cast<uint8_t*>(image.data.data()) + sub.offset;
        const uint32_t bw = (src.width + 3) / 4, bh = (src.height + 3) / 4;
        std::atomic<uint32_t> next{0};
        auto work = [&] {
            for (uint32_t by = next++; by < bh; by = next++) {
                for (uint32_t bx = 0; bx < bw; ++bx) {
                    uint8_t px[16 * 4];
                    uint8_t rg[16 * 2];
                    for (uint32_t i = 0; i < 16; ++i) {
                        const float* t = src.at(std::min(bx * 4 + (i & 3), src.width - 1),
                                                std::min(by * 4 + (i >> 2), src.height - 1));
                        for (int c = 0; c < 4; ++c) {
                            px[i * 4 + c] = uint8_t(std::clamp(t[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                        }
                        rg[i * 2] = px[i * 4];
                        rg[i * 2 + 1] = px[i * 4 + 1];
                    }
                    uint8_t* block = out + (size_t(by) * bw + bx) * block_bytes;
                    if (encoding == Encoding::bc5) {
                        stb_compress_bc5_block(block, rg);
                    } else {
                        stb_compress_dxt_block(block, px, encoding == Encoding::bc3 ? 1 : 0, STB_DXT_HIGHQUAL);
                    }
                }
            }
        };
        if (bh > 64 && threads > 1) {
            std::vector<std::jthread> pool;
            for (unsigned t = 0; t < threads; ++t) {
                pool.emplace_back(work);
            }
        } else {
            work();
        }
    }
    return image;
}

} // namespace spud::importer::far
