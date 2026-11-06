// The land's macro maps: what the full landscape shader computes, averaged over a texel.

#include "importer/far/far_view.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "formats/dds.hpp"
#include "importer/far/texels.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <thread>

namespace spud::importer::far {

namespace {

constexpr uint32_t k_colour_per_cell = 128;
constexpr uint32_t k_normal_per_cell = 64;
// A land texture repeats every 4/3 grid steps, about 2.4 m: a level this many texels
// across averages it over what one macro texel covers.
constexpr uint32_t k_land_texels = 8;
// The LOD landscape shader darkens its map by a noise factor of about 0.55, so a map that keeps
// the full land's brightness is that much brighter.
constexpr float k_map_gain = 1.82f;

struct Slot {
    std::shared_ptr<const Texels> diffuse;
    std::shared_ptr<const Texels> normal;
};

// Bilinear in the quadrant's 17 x 17 grid: the weight byte of slot `layer` (see docs/formats/osw.md).
float layer_weight(const osw::LandLayer& layer, int i, int j, float fx, float fy) {
    const auto w = [&](int r, int c) { return float(layer.opacity[r * osw::k_quad_verts + c]); };
    const float top = w(i, j) + (w(i, j + 1) - w(i, j)) * fx;
    const float bottom = w(i + 1, j) + (w(i + 1, j + 1) - w(i + 1, j)) * fx;
    return (top + (bottom - top) * fy) * (1.0f / 255.0f);
}

template <typename F>
float grid_lerp(int row, int col, float fx, float fy, F value) {
    const float top = value(row, col) + (value(row, col + 1) - value(row, col)) * fx;
    const float bottom = value(row + 1, col) + (value(row + 1, col + 1) - value(row + 1, col)) * fx;
    return top + (bottom - top) * fy;
}

// One land cell at `per_cell` texels; `out(x, y, albedo, normal)` receives each.
template <typename Out>
void bake_cell(const osw::World& w, const osw::Land& land, const std::vector<Slot>& slots, uint32_t per_cell, Out out) {
    const uint32_t default_slot = uint32_t(slots.size()) - 1;
    auto slot_of = [&](uint32_t texture) { return texture < slots.size() - 1 ? texture : default_slot; };
    for (uint32_t y = 0; y < per_cell; ++y) {
        for (uint32_t x = 0; x < per_cell; ++x) {
            // Vertex coordinates of the cell's 33 x 33 grid; the row and column of the quadrant it falls in.
            const float gx = (float(x) + 0.5f) * 32.0f / float(per_cell);
            const float gy = (float(y) + 0.5f) * 32.0f / float(per_cell);
            const int col = std::min(int(gx), 31), row = std::min(int(gy), 31);
            const float fx = gx - float(col), fy = gy - float(row);
            const int q = (row >= 16 ? 2 : 0) + (col >= 16 ? 1 : 0);
            const int qi = row - (q >= 2 ? 16 : 0), qj = col - ((q & 1) ? 16 : 0);
            const osw::LandQuadrant& quad = land.quadrants[q];
            // The game's UVs run 0.75 per grid step from the quadrant's corner.
            const float u = (float(qj) + fx) * 0.75f, v = (float(qi) + fy) * 0.75f;
            glm::vec3 albedo(0.0f), nts(0.0f);
            float base = 1.0f;
            const uint32_t count = std::min<uint32_t>(quad.layer_count, 5);
            auto add = [&](uint32_t texture, float weight) {
                const Slot& s = slots[slot_of(texture)];
                if (weight <= 0.0f) {
                    return;
                }
                if (s.diffuse && *s.diffuse) {
                    const auto d = s.diffuse->sample(u, v, true);
                    albedo += glm::vec3(d[0], d[1], d[2]) * weight;
                }
                if (s.normal && *s.normal) {
                    const auto n = s.normal->sample(u, v, true);
                    nts += (glm::vec3(n[0], n[1], n[2]) * 2.0f - 1.0f) * weight;
                } else {
                    nts += glm::vec3(0, 0, 1) * weight;
                }
            };
            for (uint32_t l = 0; l < count; ++l) {
                if (quad.first_layer + l >= w.land_layers.size()) {
                    continue;
                }
                const osw::LandLayer& layer = w.land_layers[quad.first_layer + l];
                const float weight = layer_weight(layer, qi, qj, fx, fy);
                base -= weight;
                add(layer.texture, weight);
            }
            add(quad.base_texture, std::clamp(base, 0.0f, 1.0f));
            glm::vec3 tint;
            for (int c = 0; c < 3; ++c) {
                tint[c] = grid_lerp(row, col, fx, fy, [&](int r, int k) {
                    return float(land.colors[(r * osw::k_land_verts + k) * 3 + c]);
                }) * (1.0f / 255.0f);
            }
            glm::vec3 vn;
            for (int c = 0; c < 3; ++c) {
                vn[c] = grid_lerp(row, col, fx, fy, [&](int r, int k) {
                    return float(land.normals[(r * osw::k_land_verts + k) * 3 + c]);
                });
            }
            const float length = glm::length(vn);
            vn = length > 0 ? vn / length : glm::vec3(0, 0, 1);
            // Land's frame: T along +x, B along +y, made orthogonal to the record's normal.
            const glm::vec3 t = glm::normalize(glm::vec3(1, 0, 0) - vn * vn.x);
            const glm::vec3 b = glm::cross(vn, t);
            const glm::vec3 n = nts.x * t + nts.y * b + nts.z * vn;
            const float nl = glm::length(n);
            out(x, y, albedo * tint, nl > 0 ? n / nl : vn);
        }
    }
}

void write_dds(const std::filesystem::path& cache, const std::string& path, const dds::Image& image) {
    write_file(cache / path, dds::write(image));
    log::info("far view: {} {} x {}, {} MiB", path, image.width, image.height, image.data.size() >> 20);
}

} // namespace

void bake_land(const osw::World& w, TexelCache& texels, const std::filesystem::path& cache, osh::FarView& view,
               unsigned threads) {
    int32_t lo[2] = {INT32_MAX, INT32_MAX}, hi[2] = {INT32_MIN, INT32_MIN};
    std::vector<const osw::Cell*> cells;
    for (const auto& c : w.cells) {
        if (c.land < w.lands.size()) {
            lo[0] = std::min(lo[0], c.x);
            lo[1] = std::min(lo[1], c.y);
            hi[0] = std::max(hi[0], c.x);
            hi[1] = std::max(hi[1], c.y);
            cells.push_back(&c);
        }
    }
    if (cells.empty()) {
        return;
    }
    // The last slot stands for k_no_texture: the world's default land texture.
    std::vector<Slot> slots;
    slots.reserve(w.land_textures.size() + 1);
    for (const auto& t : w.land_textures) {
        slots.push_back({texels.get(t.diffuse, k_land_texels), texels.get(t.normal, k_land_texels)});
    }
    {
        std::string normal = w.default_land_texture;
        if (normal.size() > 4) {
            normal.insert(normal.size() - 4, "_n");
        }
        slots.push_back(w.default_land_texture.empty()
                            ? Slot{}
                            : Slot{texels.get(w.default_land_texture, k_land_texels), texels.get(normal, k_land_texels)});
    }
    view.cell_min[0] = lo[0];
    view.cell_min[1] = lo[1];
    view.cell_max[0] = hi[0];
    view.cell_max[1] = hi[1];
    const uint32_t cw = uint32_t(hi[0] - lo[0] + 1), ch = uint32_t(hi[1] - lo[1] + 1);
    const std::string edid = path::to_lower(w.edid);
    Canvas colour(cw * k_colour_per_cell, ch * k_colour_per_cell);
    Canvas normal(cw * k_normal_per_cell, ch * k_normal_per_cell);
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i = next++; i < cells.size(); i = next++) {
            const osw::Cell& c = *cells[i];
            const osw::Land& land = w.lands[c.land];
            const uint32_t ox = uint32_t(c.x - lo[0]) * k_colour_per_cell, oy = uint32_t(c.y - lo[1]) * k_colour_per_cell;
            bake_cell(w, land, slots, k_colour_per_cell, [&](uint32_t x, uint32_t y, const glm::vec3& a, const glm::vec3&) {
                float* t = colour.at(ox + x, oy + y);
                t[0] = a.r * k_map_gain;
                t[1] = a.g * k_map_gain;
                t[2] = a.b * k_map_gain;
                t[3] = 1.0f;
            });
            const uint32_t nx = uint32_t(c.x - lo[0]) * k_normal_per_cell, ny = uint32_t(c.y - lo[1]) * k_normal_per_cell;
            bake_cell(w, land, slots, k_normal_per_cell, [&](uint32_t x, uint32_t y, const glm::vec3&, const glm::vec3& n) {
                float* t = normal.at(nx + x, ny + y);
                t[0] = n.x * 0.5f + 0.5f;
                t[1] = n.y * 0.5f + 0.5f;
                t[2] = 0.0f;
                t[3] = 1.0f;
            });
        }
    };
    {
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back(work);
        }
    }
    view.colour_per_cell = k_colour_per_cell;
    view.normal_per_cell = k_normal_per_cell;
    view.colour = std::format("far/{}/land.dds", edid);
    view.normal = std::format("far/{}/land_n.dds", edid);
    write_dds(cache, view.colour, encode(colour, Encoding::bc1, true, threads));
    write_dds(cache, view.normal, encode(normal, Encoding::bc5, true, threads));
}

} // namespace spud::importer::far
