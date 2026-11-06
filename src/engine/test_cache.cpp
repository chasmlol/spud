#include "engine/test_cache.hpp"

#include "engine/rotation.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/units.hpp"
#include "formats/dds.hpp"
#include "formats/osm.hpp"
#include "formats/osw.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <numbers>
#include <random>

namespace spud::engine {

namespace {

struct Rgba {
    uint8_t r, g, b, a;
};

using Pixels = std::vector<Rgba>;

float hash2(int x, int y, int seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
                 static_cast<uint32_t>(seed) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFFFF) / float(0xFFFFFF);
}

// Tileable value noise on a `period` grid.
float value_noise(float x, float y, int period, int seed) {
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const float tx = x - x0, ty = y - y0;
    auto at = [&](int i, int j) {
        return hash2(((i % period) + period) % period, ((j % period) + period) % period, seed);
    };
    const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * sx;
    const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * sx;
    return a + (b - a) * sy;
}

float fbm(float u, float v, int size, int seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    int period = 8;
    for (int o = 0; o < 4; ++o) {
        sum += amp * value_noise(u * period / size, v * period / size, period, seed + o);
        norm += amp;
        amp *= 0.5f;
        period *= 2;
    }
    return sum / norm;
}

uint8_t to_u8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

Pixels make_pixels(int size, const std::function<Rgba(int, int)>& fn) {
    Pixels p(size_t(size) * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            p[size_t(y) * size + x] = fn(x, y);
        }
    }
    return p;
}

std::vector<Pixels> mip_chain(Pixels base, int size) {
    std::vector<Pixels> levels{std::move(base)};
    while (size > 1) {
        const int half = size / 2;
        const Pixels& src = levels.back();
        Pixels dst(size_t(half) * half);
        for (int y = 0; y < half; ++y) {
            for (int x = 0; x < half; ++x) {
                int acc[4] = {0, 0, 0, 0};
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const Rgba& s = src[size_t(y * 2 + dy) * size + (x * 2 + dx)];
                        acc[0] += s.r;
                        acc[1] += s.g;
                        acc[2] += s.b;
                        acc[3] += s.a;
                    }
                }
                dst[size_t(y) * half + x] = {uint8_t((acc[0] + 2) / 4), uint8_t((acc[1] + 2) / 4),
                                             uint8_t((acc[2] + 2) / 4), uint8_t((acc[3] + 2) / 4)};
            }
        }
        levels.push_back(std::move(dst));
        size = half;
    }
    return levels;
}

uint16_t pack565(const float c[3]) {
    const auto r = static_cast<uint16_t>(std::clamp(c[0], 0.0f, 255.0f) * 31.0f / 255.0f + 0.5f);
    const auto g = static_cast<uint16_t>(std::clamp(c[1], 0.0f, 255.0f) * 63.0f / 255.0f + 0.5f);
    const auto b = static_cast<uint16_t>(std::clamp(c[2], 0.0f, 255.0f) * 31.0f / 255.0f + 0.5f);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void unpack565(uint16_t v, float c[3]) {
    c[0] = float((v >> 11) & 31) * 255.0f / 31.0f;
    c[1] = float((v >> 5) & 63) * 255.0f / 63.0f;
    c[2] = float(v & 31) * 255.0f / 31.0f;
}

// Simple BC1 colour block: bounding-box endpoints, 4-colour mode, nearest palette entry.
void encode_bc1_block(const Rgba* block, uint8_t* out) {
    float lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        const float c[3] = {float(block[i].r), float(block[i].g), float(block[i].b)};
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], c[k]);
            hi[k] = std::max(hi[k], c[k]);
        }
    }
    uint16_t c0 = pack565(hi), c1 = pack565(lo);
    if (c0 < c1) {
        std::swap(c0, c1);
    }
    uint32_t indices = 0;
    if (c0 != c1) {
        float p[4][3];
        unpack565(c0, p[0]);
        unpack565(c1, p[1]);
        for (int k = 0; k < 3; ++k) {
            p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
            p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
        }
        for (int i = 0; i < 16; ++i) {
            const float c[3] = {float(block[i].r), float(block[i].g), float(block[i].b)};
            int best = 0;
            float best_d = 1e30f;
            for (int j = 0; j < 4; ++j) {
                const float d = (c[0] - p[j][0]) * (c[0] - p[j][0]) + (c[1] - p[j][1]) * (c[1] - p[j][1]) +
                                (c[2] - p[j][2]) * (c[2] - p[j][2]);
                if (d < best_d) {
                    best_d = d;
                    best = j;
                }
            }
            indices |= uint32_t(best) << (2 * i);
        }
    }
    std::memcpy(out, &c0, 2);
    std::memcpy(out + 2, &c1, 2);
    std::memcpy(out + 4, &indices, 4);
}

void encode_bc3_alpha_block(const Rgba* block, uint8_t* out) {
    uint8_t a0 = 0, a1 = 255;
    for (int i = 0; i < 16; ++i) {
        a0 = std::max(a0, block[i].a);
        a1 = std::min(a1, block[i].a);
    }
    out[0] = a0;
    out[1] = a1;
    uint64_t bits = 0;
    if (a0 != a1) {
        float pal[8];
        pal[0] = a0;
        pal[1] = a1;
        for (int j = 1; j < 7; ++j) {
            pal[j + 1] = ((7 - j) * a0 + j * a1) / 7.0f;
        }
        for (int i = 0; i < 16; ++i) {
            int best = 0;
            float best_d = 1e30f;
            for (int j = 0; j < 8; ++j) {
                const float d = std::abs(block[i].a - pal[j]);
                if (d < best_d) {
                    best_d = d;
                    best = j;
                }
            }
            bits |= uint64_t(best) << (3 * i);
        }
    }
    for (int k = 0; k < 6; ++k) {
        out[2 + k] = static_cast<uint8_t>(bits >> (8 * k));
    }
}

void write_texture(const std::filesystem::path& file, dds::Format format, const Pixels& base, int size) {
    const auto levels = mip_chain(base, size);
    dds::Image img;
    img.format = format;
    img.width = img.height = static_cast<uint32_t>(size);
    img.mip_count = static_cast<uint32_t>(levels.size());
    std::vector<std::byte> data;
    for (size_t m = 0; m < levels.size(); ++m) {
        const int s = std::max(1, size >> m);
        const Pixels& px = levels[m];
        std::vector<uint8_t> out;
        if (format == dds::Format::bc1 || format == dds::Format::bc3) {
            const int blocks = std::max(1, (s + 3) / 4);
            for (int by = 0; by < blocks; ++by) {
                for (int bx = 0; bx < blocks; ++bx) {
                    Rgba blk[16];
                    for (int i = 0; i < 16; ++i) {
                        const int x = std::min(bx * 4 + i % 4, s - 1), y = std::min(by * 4 + i / 4, s - 1);
                        blk[i] = px[size_t(y) * s + x];
                    }
                    uint8_t enc[16];
                    if (format == dds::Format::bc3) {
                        encode_bc3_alpha_block(blk, enc);
                        encode_bc1_block(blk, enc + 8);
                        out.insert(out.end(), enc, enc + 16);
                    } else {
                        encode_bc1_block(blk, enc);
                        out.insert(out.end(), enc, enc + 8);
                    }
                }
            }
        } else {
            for (const auto& p : px) {
                switch (format) {
                case dds::Format::rgba8: out.insert(out.end(), {p.r, p.g, p.b, p.a}); break;
                case dds::Format::bgra8: out.insert(out.end(), {p.b, p.g, p.r, p.a}); break;
                case dds::Format::l8: out.push_back(p.r); break;
                default: throw Error("test cache: unsupported generated format");
                }
            }
        }
        img.subresources.push_back({data.size(), out.size(), uint32_t(s), uint32_t(s)});
        const auto* b = reinterpret_cast<const std::byte*>(out.data());
        data.insert(data.end(), b, b + out.size());
    }
    img.data = std::move(data);
    write_file(file, dds::write(img));
}

// Legacy 24-bit RGB DDS with a full mip chain, to exercise the expansion path.
void write_rgb24(const std::filesystem::path& file, const Pixels& px, int size) {
    const auto levels = mip_chain(px, size);
    std::vector<std::byte> out(4 + 124);
    auto put = [&](size_t off, uint32_t v) { std::memcpy(out.data() + off, &v, 4); };
    put(0, 0x20534444);
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000); // caps, height, width, pixelformat, mipmapcount
    put(12, uint32_t(size));
    put(16, uint32_t(size));
    put(28, uint32_t(levels.size()));
    put(4 + 72, 32);        // pf.size
    put(4 + 76, 0x40);      // DDPF_RGB
    put(4 + 84, 24);        // bit count
    put(4 + 88, 0xFF0000);  // R
    put(4 + 92, 0x00FF00);  // G
    put(4 + 96, 0x0000FF);  // B
    put(4 + 104, 0x1000 | 0x400000 | 0x8); // texture, mipmap, complex
    for (const auto& level : levels) {
        for (const auto& p : level) {
            out.push_back(std::byte(p.b));
            out.push_back(std::byte(p.g));
            out.push_back(std::byte(p.r));
        }
    }
    write_file(file, out);
}

// Normal map from a height function (tangent space, +Y = up the image / +v).
Pixels normal_map(int size, const std::function<float(int, int)>& height, float strength) {
    return make_pixels(size, [&](int x, int y) {
        const float dx = height((x + 1) % size, y) - height((x + size - 1) % size, y);
        const float dy = height(x, (y + 1) % size) - height(x, (y + size - 1) % size);
        glm::vec3 n = glm::normalize(glm::vec3(-dx * strength, -dy * strength, 1.0f));
        return Rgba{to_u8(n.x * 0.5f + 0.5f), to_u8(n.y * 0.5f + 0.5f), to_u8(n.z * 0.5f + 0.5f), 255};
    });
}

// `t` is the u direction. Skyrim's frame: normal-map x follows the bitangent B
// (= u here) and y follows the tangent T (= +v, which grows down the image).
osm::Vertex vtx(glm::vec3 p, glm::vec2 uv, glm::vec3 n, glm::vec3 t, glm::u8vec4 c = {255, 255, 255, 255}) {
    osm::Vertex v{};
    v.position[0] = p.x;
    v.position[1] = p.y;
    v.position[2] = p.z;
    v.uv[0] = uv.x;
    v.uv[1] = uv.y;
    auto un = [](float f) { return static_cast<uint8_t>(std::clamp(std::lround((f * 0.5f + 0.5f) * 255.0f), 0l, 255l)); };
    const glm::vec3 tv = -glm::cross(n, t);
    v.bitangent_x = t.x;
    v.normal[0] = un(n.x);
    v.normal[1] = un(n.y);
    v.normal[2] = un(n.z);
    v.normal[3] = un(t.y);
    v.tangent[0] = un(tv.x);
    v.tangent[1] = un(tv.y);
    v.tangent[2] = un(tv.z);
    v.tangent[3] = un(t.z);
    v.color[0] = c.r;
    v.color[1] = c.g;
    v.color[2] = c.b;
    v.color[3] = c.a;
    return v;
}

// Axis-aligned box as 6 quads, CCW from outside.
void add_box(std::vector<osm::Vertex>& v, std::vector<uint32_t>& idx, glm::vec3 lo, glm::vec3 hi,
             glm::u8vec4 color = {255, 255, 255, 255}, float uv_scale = 1.0f) {
    struct Face {
        glm::vec3 n, u, w; // normal, tangent (u), bitangent (v)
    };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}}, {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}},
    };
    const glm::vec3 c = (lo + hi) * 0.5f, h = (hi - lo) * 0.5f;
    for (const auto& f : faces) {
        const uint32_t base = static_cast<uint32_t>(v.size());
        const glm::vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& k : corners) {
            const glm::vec3 p = c + (f.n + f.u * k.x + f.w * k.y) * h;
            // v grows downwards in the image: top of the face is v = 0.
            v.push_back(vtx(p, glm::vec2(k.x * 0.5f + 0.5f, 0.5f - k.y * 0.5f) * uv_scale, f.n, f.u, color));
        }
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

osm::Material lit_material(const std::string& diffuse, const std::string& normal, uint32_t extra_flags = 0) {
    osm::Material m;
    m.params.flags |= extra_flags;
    m.textures[osm::texture_diffuse] = diffuse;
    m.textures[osm::texture_normal] = normal;
    return m;
}

void write_mesh(const std::filesystem::path& file, std::vector<osm::Vertex> v, std::vector<uint32_t> idx,
                osm::Material mat) {
    osm::Mesh mesh;
    mesh.vertices = std::move(v);
    mesh.indices = std::move(idx);
    osm::Submesh sm{};
    sm.index_count = static_cast<uint32_t>(mesh.indices.size());
    sm.vertex_count = static_cast<uint32_t>(mesh.vertices.size());
    mesh.submeshes.push_back(sm);
    mesh.materials.push_back(std::move(mat));
    osm::compute_bounds(mesh);
    write_file(file, osm::write(mesh));
}

float terrain_height(float x, float y) {
    float h = 25.0f * std::sin(x / 90.0f) * std::cos(y / 110.0f) + 10.0f * std::sin(x / 37.0f + 1.0f) * std::sin(y / 41.0f);
    h += 55.0f * std::exp(-((x - 90) * (x - 90) + (y - 70) * (y - 70)) / (2 * 60.0f * 60.0f));
    h -= 32.0f * std::exp(-((x + 120) * (x + 120) + (y + 110) * (y + 110)) / (2 * 75.0f * 75.0f));
    return h + 6.0f;
}

} // namespace

void make_test_cache(const std::filesystem::path& dir) {
    log::info("test cache: writing to {}", dir.string());
    const int ts = 256;

    // ---- Textures ------------------------------------------------------------------
    auto tex = [&](const char* name) { return dir / "textures" / "test" / name; };
    write_texture(tex("grass.dds"), dds::Format::bc1, make_pixels(ts, [&](int x, int y) {
                      const float n = fbm(float(x), float(y), ts, 1);
                      return Rgba{to_u8(0.22f + 0.18f * n), to_u8(0.40f + 0.25f * n), to_u8(0.12f + 0.08f * n), 255};
                  }), ts);
    write_texture(tex("dirt.dds"), dds::Format::bgra8, make_pixels(ts, [&](int x, int y) {
                      const float n = fbm(float(x), float(y), ts, 7);
                      return Rgba{to_u8(0.42f + 0.2f * n), to_u8(0.31f + 0.15f * n), to_u8(0.20f + 0.1f * n), 255};
                  }), ts);
    write_rgb24(tex("rock.dds"), make_pixels(ts, [&](int x, int y) {
                    const float n = fbm(float(x), float(y), ts, 13);
                    const float crack = std::abs(std::sin((x + 3 * fbm(float(y), float(x), ts, 3) * 40) * 0.09f));
                    const float v = 0.35f + 0.3f * n - (crack < 0.06f ? 0.15f : 0.0f);
                    return Rgba{to_u8(v), to_u8(v * 0.97f), to_u8(v * 0.93f), 255};
                }), ts);
    write_texture(tex("snow.dds"), dds::Format::bc3, make_pixels(ts, [&](int x, int y) {
                      const float n = fbm(float(x), float(y), ts, 21);
                      return Rgba{to_u8(0.86f + 0.1f * n), to_u8(0.88f + 0.1f * n), to_u8(0.93f + 0.07f * n), 255};
                  }), ts);
    write_texture(tex("default.dds"), dds::Format::l8, make_pixels(ts, [&](int x, int y) {
                      const float n = fbm(float(x), float(y), ts, 31);
                      const uint8_t l = to_u8(0.45f + 0.25f * n);
                      return Rgba{l, l, l, 255};
                  }), ts);
    write_texture(tex("terrain_n.dds"), dds::Format::rgba8,
                  normal_map(ts, [&](int x, int y) { return fbm(float(x), float(y), ts, 5); }, 6.0f), ts);
    write_texture(tex("rock_n.dds"), dds::Format::rgba8,
                  normal_map(ts, [&](int x, int y) { return fbm(float(x), float(y), ts, 13); }, 10.0f), ts);
    // Bricks: 8 rows of 4 bricks, mortar lines.
    auto brick_height = [&](int x, int y) {
        const int row = y / 32;
        const int bx = (x + (row % 2) * 32) % 64;
        const int by = y % 32;
        return (bx < 3 || by < 3) ? 0.0f : 1.0f;
    };
    write_texture(tex("brick.dds"), dds::Format::bc1, make_pixels(ts, [&](int x, int y) {
                      const float n = fbm(float(x), float(y), ts, 41);
                      if (brick_height(x, y) == 0) {
                          return Rgba{to_u8(0.7f), to_u8(0.68f), to_u8(0.62f), 255};
                      }
                      const float tint = hash2((x + (y / 32 % 2) * 32) / 64, y / 32, 9);
                      return Rgba{to_u8(0.55f + 0.15f * tint + 0.1f * n), to_u8(0.25f + 0.08f * n),
                                  to_u8(0.18f + 0.05f * n), 255};
                  }), ts);
    write_texture(tex("brick_n.dds"), dds::Format::bgra8,
                  normal_map(ts, [&](int x, int y) { return brick_height(x, y); }, 3.0f), ts);
    // Leaves: alpha-tested blobs.
    write_texture(tex("leaves.dds"), dds::Format::rgba8, make_pixels(ts, [&](int x, int y) {
                      const float n = fbm(float(x), float(y), ts, 51);
                      const float cx = x - ts / 2.0f, cy = y - ts / 2.0f;
                      const float r = std::sqrt(cx * cx + cy * cy) / (ts / 2.0f);
                      const bool on = n > 0.45f + r * 0.25f;
                      return Rgba{to_u8(0.18f + 0.2f * n), to_u8(0.45f + 0.3f * n), to_u8(0.12f), uint8_t(on ? 255 : 0)};
                  }), ts);
    write_texture(tex("glass.dds"), dds::Format::bc3, make_pixels(ts, [&](int x, int y) {
                      const bool frame = x < 12 || y < 12 || x >= ts - 12 || y >= ts - 12;
                      return frame ? Rgba{60, 50, 40, 255} : Rgba{150, 200, 230, 90};
                  }), ts);
    write_texture(tex("glow.dds"), dds::Format::bc1, make_pixels(ts, [&](int x, int y) {
                      const float cx = x - ts / 2.0f, cy = y - ts / 2.0f;
                      const float r = std::sqrt(cx * cx + cy * cy) / (ts / 2.0f);
                      const float v = std::clamp(1.0f - r, 0.0f, 1.0f);
                      return Rgba{to_u8(1.0f * v), to_u8(0.55f * v), to_u8(0.15f * v), 255};
                  }), ts);

    // ---- Meshes --------------------------------------------------------------------
    auto mesh = [&](const char* name) { return dir / "meshes" / "test" / name; };
    {
        std::vector<osm::Vertex> v;
        std::vector<uint32_t> i;
        add_box(v, i, {-0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 1.0f});
        write_mesh(mesh("cube.osm"), v, i, lit_material("textures/test/brick.dds", "textures/test/brick_n.dds"));
    }
    {
        std::vector<osm::Vertex> v;
        std::vector<uint32_t> i;
        add_box(v, i, {-0.6f, -0.6f, 0.0f}, {0.6f, 0.6f, 5.0f}, {255, 255, 255, 255}, 1.0f);
        write_mesh(mesh("pillar.osm"), v, i, lit_material("textures/test/rock.dds", "textures/test/rock_n.dds"));
    }
    {
        // Rotation probe: shaft along +X (red), fin up +Z (blue), stub towards +Y (green).
        std::vector<osm::Vertex> v;
        std::vector<uint32_t> i;
        add_box(v, i, {0.0f, -0.15f, -0.15f}, {3.0f, 0.15f, 0.15f}, {230, 40, 40, 255});
        add_box(v, i, {2.4f, -0.05f, 0.15f}, {2.9f, 0.05f, 1.2f}, {40, 60, 230, 255});
        add_box(v, i, {0.0f, 0.15f, -0.1f}, {0.2f, 1.2f, 0.1f}, {40, 200, 40, 255});
        write_mesh(mesh("arrow.osm"), v, i, lit_material("", "", osm::material_vertex_colors));
    }
    {
        // Two crossed quads, two-sided, alpha tested.
        std::vector<osm::Vertex> v;
        std::vector<uint32_t> i;
        for (int q = 0; q < 2; ++q) {
            const glm::vec3 axis = q == 0 ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const glm::vec3 n = q == 0 ? glm::vec3(0, -1, 0) : glm::vec3(1, 0, 0);
            const uint32_t b = static_cast<uint32_t>(v.size());
            v.push_back(vtx(-axis * 1.5f, {0, 1}, n, axis));
            v.push_back(vtx(axis * 1.5f, {1, 1}, n, axis));
            v.push_back(vtx(axis * 1.5f + glm::vec3(0, 0, 3), {1, 0}, n, axis));
            v.push_back(vtx(-axis * 1.5f + glm::vec3(0, 0, 3), {0, 0}, n, axis));
            i.insert(i.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
        }
        auto m = lit_material("textures/test/leaves.dds", "", osm::material_alpha_test | osm::material_two_sided);
        write_mesh(mesh("bush.osm"), v, i, m);
    }
    {
        std::vector<osm::Vertex> v;
        std::vector<uint32_t> i;
        const glm::vec3 n(0, -1, 0), t(1, 0, 0);
        v.push_back(vtx({-1.5f, 0, 0}, {0, 1}, n, t));
        v.push_back(vtx({1.5f, 0, 0}, {1, 1}, n, t));
        v.push_back(vtx({1.5f, 0, 2.5f}, {1, 0}, n, t));
        v.push_back(vtx({-1.5f, 0, 2.5f}, {0, 0}, n, t));
        i = {0, 1, 2, 0, 2, 3};
        auto m = lit_material("textures/test/glass.dds", "", osm::material_alpha_blend | osm::material_two_sided);
        m.params.flags &= ~osm::material_z_write;
        write_mesh(mesh("glass.osm"), v, i, m);
    }
    {
        std::vector<osm::Vertex> v;
        std::vector<uint32_t> i;
        add_box(v, i, {-0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 1.0f});
        osm::Material m;
        m.params.model = osm::ShaderModel::effect;
        m.params.flags = osm::material_alpha_blend | osm::material_z_test;
        m.params.blend_src = 0; // ONE
        m.params.blend_dst = 0; // ONE: additive
        m.params.base_color[0] = 1.0f;
        m.params.base_color[1] = 0.9f;
        m.params.base_color[2] = 0.7f;
        m.params.base_color_scale = 1.5f;
        m.textures[0] = "textures/test/glow.dds";
        write_mesh(mesh("glow.osm"), v, i, m);
    }
    {
        std::vector<osm::Vertex> v;
        std::vector<uint32_t> i;
        add_box(v, i, {-0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 1.0f});
        write_mesh(mesh("missing_texture.osm"), v, i,
                   lit_material("textures/test/this_texture_does_not_exist.dds", ""));
    }

    // ---- World ---------------------------------------------------------------------
    osw::World w;
    w.edid = "tamriel";
    w.name = "Synthetic Tamriel";
    w.flags = osw::world_has_water;
    w.cell_size = units::k_cell_size_m;
    w.default_land_height = -20.0f;
    w.default_water_height = 0.0f;
    w.default_land_texture = "textures/test/default.dds";
    w.meshes = {"meshes/test/cube.osm",  "meshes/test/pillar.osm", "meshes/test/arrow.osm",
                "meshes/test/bush.osm",  "meshes/test/glass.osm",  "meshes/test/glow.osm",
                "meshes/test/missing_texture.osm", "meshes/test/does_not_exist.osm"};
    enum { m_cube, m_pillar, m_arrow, m_bush, m_glass, m_glow, m_missing_tex, m_missing_mesh };
    w.land_textures = {
        {0x100, "textures/test/grass.dds", "textures/test/terrain_n.dds", 0},
        {0x101, "textures/test/dirt.dds", "textures/test/terrain_n.dds", 0},
        {0x102, "textures/test/rock.dds", "textures/test/rock_n.dds", 0},
        {0x103, "textures/test/snow.dds", "", 0},
    };
    const float cs = w.cell_size;
    const int lo = -4, hi = 4;
    w.cell_min[0] = w.cell_min[1] = lo;
    w.cell_max[0] = hi;
    w.cell_max[1] = hi + 1;

    struct PendingRef {
        osw::Ref ref;
        int cx, cy;
    };
    std::vector<PendingRef> refs;
    auto place = [&](uint32_t mesh_index, glm::vec3 pos, glm::vec3 euler, float scale, uint32_t flags) {
        osw::Ref r{};
        r.form_id = 0x00100000u + static_cast<uint32_t>(refs.size());
        r.base_form_id = 0x00200000u + mesh_index;
        r.mesh = mesh_index;
        r.flags = flags;
        r.position[0] = pos.x;
        r.position[1] = pos.y;
        r.position[2] = pos.z;
        r.scale = scale;
        r.source_euler[0] = euler.x;
        r.source_euler[1] = euler.y;
        r.source_euler[2] = euler.z;
        const glm::quat q = glm::quat_cast(euler_rotation(RotationMode::xyz_neg, r.source_euler));
        r.rotation[0] = q.x;
        r.rotation[1] = q.y;
        r.rotation[2] = q.z;
        r.rotation[3] = q.w;
        refs.push_back({r, static_cast<int>(std::floor(pos.x / cs)), static_cast<int>(std::floor(pos.y / cs))});
    };

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const float extent = (hi - lo + 1) * cs;
    const float origin = lo * cs;
    for (int n = 0; n < 420; ++n) {
        const float x = origin + 4.0f + uni(rng) * (extent - 8.0f);
        const float y = origin + 4.0f + uni(rng) * (extent - 8.0f);
        const float z = terrain_height(x, y);
        if (z < 0.5f) {
            continue; // keep the lake clear
        }
        const float pick = uni(rng);
        const float yaw = uni(rng) * 2.0f * std::numbers::pi_v<float>;
        const float tilt = (uni(rng) - 0.5f) * 0.5f;
        if (pick < 0.45f) {
            const uint32_t flags = (n % 9 == 0) ? osw::ref_initially_disabled : 0;
            place(m_cube, {x, y, z - 0.2f}, {tilt, tilt * 0.5f, yaw}, 0.6f + uni(rng) * 3.0f, flags);
        } else if (pick < 0.75f) {
            place(m_bush, {x, y, z - 0.1f}, {0, 0, yaw}, 0.7f + uni(rng) * 1.2f, 0);
        } else if (pick < 0.88f) {
            place(m_pillar, {x, y, z - 0.5f}, {tilt * 0.3f, 0, yaw}, 0.5f + uni(rng), 0);
        } else if (pick < 0.94f) {
            place(m_glass, {x, y, z}, {0, 0, yaw}, 1.0f, 0);
        } else {
            place(m_glow, {x, y, z + 0.5f}, {0, 0, yaw}, 0.5f + uni(rng), 0);
        }
    }
    // Rotation probes in a row near the centre: identity, then 90 degrees about z, x and y.
    const glm::vec3 probe_angles[] = {{0, 0, 0}, {0, 0, 1.5708f}, {1.5708f, 0, 0}, {0, 1.5708f, 0}, {0.6f, 0.4f, 1.0f}};
    for (int k = 0; k < 5; ++k) {
        const float x = -20.0f + k * 8.0f, y = 10.0f;
        place(m_arrow, {x, y, terrain_height(x, y) + 2.5f}, probe_angles[k], 1.0f, 0);
    }
    place(m_missing_tex, {6.0f, -8.0f, terrain_height(6, -8) - 0.1f}, {0, 0, 0.3f}, 2.0f, 0);
    place(m_missing_mesh, {10.0f, -8.0f, terrain_height(10, -8)}, {0, 0, 0}, 1.0f, 0);

    // Cells (sorted by y, x) with land, plus a water-only strip to the north.
    for (int cy = lo; cy <= hi + 1; ++cy) {
        for (int cx = lo; cx <= hi; ++cx) {
            osw::Cell cell{};
            cell.x = cx;
            cell.y = cy;
            cell.form_id = 0x00300000u + static_cast<uint32_t>((cy - lo) * 16 + (cx - lo));
            cell.water_height = 0.0f;
            cell.land = osw::k_none;
            float min_h = 1e30f;
            if (cy <= hi) {
                osw::Land land{};
                for (int r = 0; r < osw::k_land_verts; ++r) {
                    for (int c = 0; c < osw::k_land_verts; ++c) {
                        const float x = (cx + c / 32.0f) * cs, y = (cy + r / 32.0f) * cs;
                        const float h = terrain_height(x, y);
                        const int vi = r * osw::k_land_verts + c;
                        land.heights[vi] = h;
                        min_h = std::min(min_h, h);
                        const float e = cs / 32.0f;
                        const glm::vec3 n = glm::normalize(glm::vec3(terrain_height(x - e, y) - terrain_height(x + e, y),
                                                                     terrain_height(x, y - e) - terrain_height(x, y + e),
                                                                     2.0f * e));
                        land.normals[vi * 3 + 0] = static_cast<int8_t>(std::lround(n.x * 127));
                        land.normals[vi * 3 + 1] = static_cast<int8_t>(std::lround(n.y * 127));
                        land.normals[vi * 3 + 2] = static_cast<int8_t>(std::lround(n.z * 127));
                        const float shade = std::clamp(0.85f + h / 200.0f, 0.7f, 1.0f);
                        land.colors[vi * 3 + 0] = to_u8(shade);
                        land.colors[vi * 3 + 1] = to_u8(shade);
                        land.colors[vi * 3 + 2] = to_u8(shade * (cx == 0 && cy == 0 ? 0.8f : 1.0f));
                    }
                }
                for (int q = 0; q < 4; ++q) {
                    auto& quad = land.quadrants[q];
                    // East column uses the world default texture as its base.
                    quad.base_texture = cx == hi ? osw::k_no_texture : 0;
                    quad.first_layer = static_cast<uint32_t>(w.land_layers.size());
                    const int r0 = q >= 2 ? 16 : 0, c0 = (q & 1) ? 16 : 0;
                    for (uint16_t t = 1; t <= 3; ++t) {
                        osw::LandLayer layer{};
                        layer.texture = t;
                        bool any = false;
                        for (int r = 0; r < osw::k_quad_verts; ++r) {
                            for (int c = 0; c < osw::k_quad_verts; ++c) {
                                const int vi = (r0 + r) * osw::k_land_verts + (c0 + c);
                                const float x = (cx + (c0 + c) / 32.0f) * cs, y = (cy + (r0 + r) / 32.0f) * cs;
                                const float h = land.heights[vi];
                                const float nz = land.normals[vi * 3 + 2] / 127.0f;
                                float o = 0;
                                if (t == 1) {
                                    o = std::clamp((fbm(x * 0.8f, y * 0.8f, 256, 77) - 0.5f) * 4.0f, 0.0f, 1.0f);
                                } else if (t == 2) {
                                    o = std::clamp((0.93f - nz) * 8.0f, 0.0f, 1.0f);
                                } else {
                                    o = std::clamp((h - 42.0f) / 8.0f, 0.0f, 1.0f);
                                }
                                layer.opacity[r * osw::k_quad_verts + c] = to_u8(o);
                                any = any || o > 0;
                            }
                        }
                        if (any) {
                            w.land_layers.push_back(layer);
                            ++quad.layer_count;
                        }
                    }
                }
                cell.land = static_cast<uint32_t>(w.lands.size());
                w.lands.push_back(land);
                cell.flags |= osw::cell_has_land;
            }
            if (cy > hi || min_h < 0.0f) {
                cell.flags |= osw::cell_has_water;
            }
            cell.first_ref = static_cast<uint32_t>(w.refs.size());
            for (const auto& pr : refs) {
                if (pr.cx == cx && pr.cy == cy) {
                    w.refs.push_back(pr.ref);
                }
            }
            cell.ref_count = static_cast<uint32_t>(w.refs.size()) - cell.first_ref;
            w.cells.push_back(cell);
        }
    }
    write_file(dir / "worlds" / "tamriel.osw", osw::write(w));
    log::info("test cache: {} cells, {} refs, {} meshes, {} land layers", w.cells.size(), w.refs.size(),
              w.meshes.size(), w.land_layers.size());
}

} // namespace spud::engine
