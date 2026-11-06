#include "render/terrain.hpp"

#include "core/units.hpp"

#include "core/log.hpp"
#include "render/light_grid.hpp"
#include "render/shadows.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace spud::render {

namespace {

constexpr float k_skirt_depth = 3.0f;
constexpr size_t k_max_near_in_flight = 64;
// The heightfield steps in the game's: land heights are whole multiples of 8 units.
constexpr float k_height_step = 8.0f * units::k_skyrim_unit_m;
constexpr int k_grid = 16;                       // quads per far node edge
constexpr int k_grid_vertices = (k_grid + 1) * (k_grid + 1);
// Vertex-lit fog needs vertices however flat the land: a node also waits until it is this many of its own sizes away.
constexpr float k_min_range_sizes = 1.5f;
constexpr float k_morph_begin = 0.8f;            // of the parent's range
constexpr size_t k_heights_per_upload = 4u << 20;
constexpr float k_replan_cells = 0.125f; // camera movement (cells) before the full-res ring is re-planned

constexpr int k_quad_edge = 17;
constexpr uint32_t k_quad_vertices = k_quad_edge * k_quad_edge + 2 * k_quad_edge;
constexpr uint32_t k_cell_vertices = 4 * k_quad_vertices;

// Skyrim's quadrant triangle split: quad (row r, col c) with corners SW, SE, NW, NE.
// Odd (r xor c): (NW, SW, SE), (SE, NE, NW); even: (NE, NW, SW), (SW, SE, NE). Both counter-clockwise from above.
template <typename F>
void for_each_land_triangle(int n, F f) {
    for (int r = 0; r + 1 < n; ++r) {
        for (int c = 0; c + 1 < n; ++c) {
            const int sw = r * n + c, se = sw + 1, nw = sw + n, ne = nw + 1;
            if ((r ^ c) & 1) {
                f(nw, sw, se);
                f(se, ne, nw);
            } else {
                f(ne, nw, sw);
                f(sw, se, ne);
            }
        }
    }
}

// The game's tangent frame of one full-resolution quadrant (7.3): per-triangle dP/du accumulated per vertex, made
// orthogonal to the record normal; B = N x T. Model space, units.
struct LandFrame {
    glm::vec3 t[17 * 17];
    glm::vec3 b[17 * 17];
};

void land_frame(const osw::Land& land, int q, LandFrame& out) {
    constexpr int n = 17;
    const int r0 = q >= 2 ? 16 : 0;
    const int c0 = (q & 1) ? 16 : 0;
    glm::vec3 p[n * n];
    glm::vec2 uv[n * n];
    glm::vec3 nrm[n * n];
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            const int vi = (r0 + i) * osw::k_land_verts + (c0 + j);
            // Heights are whole multiples of 8 units; undo the metre conversion exactly.
            const float h = std::round(land.heights[vi] / units::k_skyrim_unit_m / 8.0f) * 8.0f;
            p[i * n + j] = glm::vec3(float(j << 7), float(i << 7), h);
            uv[i * n + j] = glm::vec2(float(j), float(i)) * 0.75f;
            const glm::vec3 v(float(land.normals[vi * 3 + 0]), float(land.normals[vi * 3 + 1]),
                              float(land.normals[vi * 3 + 2]));
            glm::vec3 nv = v * 0.0078740157f;
            const float len = glm::length(nv);
            nrm[i * n + j] = len > 1e-6f ? nv / len : glm::vec3(0.0f);
        }
    }
    glm::vec3 acc[n * n] = {};
    for_each_land_triangle(n, [&](int i0, int i1, int i2) {
        const glm::vec3 dp1 = p[i1] - p[i0], dp2 = p[i2] - p[i0];
        const float du1 = uv[i1].x - uv[i0].x, dv1 = uv[i1].y - uv[i0].y;
        const float du2 = uv[i2].x - uv[i0].x, dv2 = uv[i2].y - uv[i0].y;
        const glm::vec3 t = (dp1 * dv2 - dp2 * dv1) / (dv2 * du1 - dv1 * du2);
        acc[i0] += t;
        acc[i1] += t;
        acc[i2] += t;
    });
    for (int k = 0; k < n * n; ++k) {
        const glm::vec3& nv = nrm[k];
        const glm::vec3 t1 = acc[k] - nv * glm::dot(acc[k], nv);
        const glm::vec3 b1 = glm::cross(nv, t1);
        const float lt = glm::length(t1), lb = glm::length(b1);
        out.t[k] = lt > 1e-6f ? t1 / lt : glm::vec3(0.0f);
        out.b[k] = lb > 1e-6f ? b1 / lb : glm::vec3(0.0f);
    }
}

uint8_t pack_land(float v) { return static_cast<uint8_t>(int32_t((v + 1.0f) * 127.5f)); }

// Writes the vertices of one cell in the game's land vertex layout (7.3).
void build_cell(const osw::World& world, const osw::Land& land, uint32_t land_index, int32_t cx, int32_t cy,
                TerrainVertex* out) {
    constexpr int step = 1;
    constexpr int n = k_quad_edge;
    const float cs = world.cell_size;
    const float skirt = k_skirt_depth;
    LandFrame frame;
    for (int q = 0; q < 4; ++q) {
        const int r0 = q >= 2 ? 16 : 0;
        const int c0 = (q & 1) ? 16 : 0;
        const auto& quad = land.quadrants[q];
        const uint32_t layer_count = std::min<uint32_t>(quad.layer_count, 5);
        land_frame(land, q, frame);
        TerrainVertex* qv = out + q * k_quad_vertices;
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                const int row = r0 + i * step;
                const int col = c0 + j * step;
                const int vi = row * osw::k_land_verts + col;
                const int fi = (i * step) * 17 + (j * step);
                TerrainVertex& v = qv[i * n + j];
                v.position[0] = (static_cast<float>(cx) + static_cast<float>(col) / 32.0f) * cs;
                v.position[1] = (static_cast<float>(cy) + static_cast<float>(row) / 32.0f) * cs;
                v.position[2] = land.heights[vi];
                const glm::vec3& t = frame.t[fi];
                const glm::vec3& b = frame.b[fi];
                v.tangent_x = (t.x + 1.0f) * 0.5f; // stored like this by the game, read undecoded (7.3)
                for (int c = 0; c < 3; ++c) {
                    v.normal[c] = static_cast<uint8_t>(land.normals[vi * 3 + c] + 128);
                }
                v.normal[3] = pack_land(t.y);
                v.binormal[0] = pack_land(b.x);
                v.binormal[1] = pack_land(b.y);
                v.binormal[2] = pack_land(b.z);
                v.binormal[3] = pack_land(t.z);
                v.color[0] = land.colors[vi * 3 + 0];
                v.color[1] = land.colors[vi * 3 + 1];
                v.color[2] = land.colors[vi * 3 + 2];
                v.color[3] = 255;
                const int oi = (i * step) * osw::k_quad_verts + (j * step);
                int sum = 0;
                for (uint32_t l = 0; l < 5; ++l) {
                    const uint8_t w = l < layer_count && quad.first_layer + l < world.land_layers.size()
                                          ? world.land_layers[quad.first_layer + l].opacity[oi]
                                          : 0;
                    v.weight[l + 1] = w;
                    sum += w;
                }
                v.weight[0] = static_cast<uint8_t>(std::clamp(255 - sum, 0, 255));
                v.grid[0] = static_cast<uint8_t>(j * step);
                v.grid[1] = static_cast<uint8_t>(i * step);
                v.quad = land_index * 4 + static_cast<uint32_t>(q);
            }
        }
        // Skirts: south/north edge (row 0 or n-1) then west/east edge (col 0 or n-1).
        TerrainVertex* sa = qv + n * n;
        TerrainVertex* sb = sa + n;
        const int ei = q < 2 ? 0 : n - 1;
        const int ej = (q & 1) ? n - 1 : 0;
        for (int k = 0; k < n; ++k) {
            sa[k] = qv[ei * n + k];
            sa[k].position[2] -= skirt;
            sb[k] = qv[k * n + ej];
            sb[k].position[2] -= skirt;
        }
    }
}

void append_near_template(std::vector<uint32_t>& idx) {
    constexpr uint32_t n = k_quad_edge;
    for (uint32_t q = 0; q < 4; ++q) {
        const uint32_t base = q * k_quad_vertices;
        auto g = [&](uint32_t i, uint32_t j) { return base + i * n + j; };
        for_each_land_triangle(int(n), [&](int a, int b, int c) {
            idx.insert(idx.end(), {base + uint32_t(a), base + uint32_t(b), base + uint32_t(c)});
        });
        const uint32_t sa = base + n * n;
        const uint32_t sb = sa + n;
        const uint32_t ei = q < 2 ? 0 : n - 1;
        const uint32_t ej = (q & 1) ? n - 1 : 0;
        for (uint32_t k = 0; k + 1 < n; ++k) {
            // Edge A runs along +X; edge B along +Y. Wind each skirt to face outwards.
            const uint32_t ea0 = g(ei, k), ea1 = g(ei, k + 1), s0 = sa + k, s1 = sa + k + 1;
            if (q < 2) { // south, faces -Y
                idx.insert(idx.end(), {s0, s1, ea1, s0, ea1, ea0});
            } else { // north, faces +Y
                idx.insert(idx.end(), {s1, s0, ea0, s1, ea0, ea1});
            }
            const uint32_t eb0 = g(k, ej), eb1 = g(k + 1, ej), t0 = sb + k, t1 = sb + k + 1;
            if ((q & 1) == 0) { // west, faces -X
                idx.insert(idx.end(), {t1, t0, eb0, t1, eb0, eb1});
            } else { // east, faces +X
                idx.insert(idx.end(), {t0, t1, eb1, t0, eb1, eb0});
            }
        }
    }
}

// The shared far node: the grid, every quad split south-west to north-east so that a child morphs exactly onto its
// parent's surface, then the skirts' vertices after the grid's: south, north, west, east (terrain_far.glsl).
void append_far_template(std::vector<uint32_t>& idx) {
    constexpr uint32_t n = k_grid + 1;
    auto g = [](uint32_t row, uint32_t col) { return row * n + col; };
    for (uint32_t r = 0; r + 1 < n; ++r) {
        for (uint32_t c = 0; c + 1 < n; ++c) {
            const uint32_t sw = g(r, c), se = sw + 1, nw = sw + n, ne = nw + 1;
            idx.insert(idx.end(), {sw, se, ne, sw, ne, nw});
        }
    }
    const uint32_t south = k_grid_vertices, north = south + n, west = north + n, east = west + n;
    for (uint32_t k = 0; k + 1 < n; ++k) {
        idx.insert(idx.end(), {south + k, south + k + 1, g(0, k + 1), south + k, g(0, k + 1), g(0, k)});
        idx.insert(idx.end(), {north + k + 1, north + k, g(n - 1, k), north + k + 1, g(n - 1, k), g(n - 1, k + 1)});
        idx.insert(idx.end(), {west + k + 1, west + k, g(k, 0), west + k + 1, g(k, 0), g(k + 1, 0)});
        idx.insert(idx.end(), {east + k, east + k + 1, g(k + 1, n - 1), east + k, g(k + 1, n - 1), g(k, n - 1)});
    }
}

} // namespace

Terrain::Terrain(Device& device, Uploader& uploader, JobSystem& jobs, TextureManager& textures,
                 std::shared_ptr<const osw::World> world, std::shared_ptr<const osh::FarView> far)
    : m_device(device), m_uploader(uploader), m_jobs(jobs), m_textures(textures), m_world(std::move(world)),
      m_far(std::move(far)) {
    const auto& w = *m_world;
    m_cell_size = w.cell_size;

    // Land cells and the lookup grid.
    int32_t lo[2] = {INT32_MAX, INT32_MAX}, hi[2] = {INT32_MIN, INT32_MIN};
    for (const auto& c : w.cells) {
        if (c.land == osw::k_none || c.land >= w.lands.size()) {
            continue;
        }
        lo[0] = std::min(lo[0], c.x);
        lo[1] = std::min(lo[1], c.y);
        hi[0] = std::max(hi[0], c.x);
        hi[1] = std::max(hi[1], c.y);
        LandCell lc{};
        lc.x = c.x;
        lc.y = c.y;
        lc.land = c.land;
        m_cells.push_back(lc);
    }
    if (!m_cells.empty()) {
        m_grid_min[0] = lo[0];
        m_grid_min[1] = lo[1];
        m_grid_size[0] = hi[0] - lo[0] + 1;
        m_grid_size[1] = hi[1] - lo[1] + 1;
        m_grid.assign(size_t(m_grid_size[0]) * m_grid_size[1], -1);
        for (uint32_t i = 0; i < m_cells.size(); ++i) {
            const int gx = m_cells[i].x - m_grid_min[0];
            const int gy = m_cells[i].y - m_grid_min[1];
            m_grid[size_t(gy) * m_grid_size[0] + gx] = static_cast<int32_t>(i);
        }
    }

    const uint64_t cells = m_cells.size();
    const int ring = 2 * k_max_full_res_radius + 3;
    m_near_capacity = static_cast<uint32_t>(std::min<uint64_t>(cells, uint64_t(ring) * ring));
    const uint64_t total = uint64_t(m_near_capacity) * k_cell_vertices;
    for (uint32_t s = m_near_capacity; s > 0; --s) {
        m_free_slots.push_back(s - 1);
    }
    m_vertices = device.create_buffer(std::max<uint64_t>(total, 1) * sizeof(TerrainVertex),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      MemoryUsage::gpu_only, Lifetime::asset, "terrain vertices", true);
    log::info("terrain: {} land cells, {} MiB vertices ({} full-res slots)", cells,
              (total * sizeof(TerrainVertex)) >> 20, m_near_capacity);

    build_index_templates();
    build_tables();
    build_heightfield();
}

Terrain::~Terrain() {
    for (auto t : m_acquired) {
        m_textures.release(t);
    }
    m_device.destroy_buffer(m_vertices);
    m_device.destroy_buffer(m_heights);
    m_device.destroy_buffer(m_indices);
    m_device.destroy_buffer(m_quads);
    m_device.destroy_buffer(m_land_textures);
}

void Terrain::build_index_templates() {
    std::vector<uint32_t> idx;
    append_near_template(idx);
    m_near_index_count = static_cast<uint32_t>(idx.size());
    append_far_template(idx);
    m_far_index_count = static_cast<uint32_t>(idx.size()) - m_near_index_count;
    const size_t bytes = idx.size() * sizeof(uint32_t);
    m_indices = m_device.create_buffer(bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       MemoryUsage::gpu_only, Lifetime::asset, "terrain indices", true);
    UploadRequest req;
    req.data.resize(bytes);
    std::memcpy(req.data.data(), idx.data(), bytes);
    req.buffer_regions.push_back({m_indices.buffer, 0, 0, bytes});
    ++m_pending;
    req.on_complete = [this] { --m_pending; };
    m_uploader.submit(std::move(req));
}

void Terrain::finish_textures() {
    const auto& w = *m_world;
    if (m_far) {
        m_far_colour = m_textures.acquire(m_far->colour, TextureKind::color, 0.0f);
        m_far_normal = m_textures.acquire(m_far->normal, TextureKind::normal, 0.0f);
        m_far_noise = m_textures.acquire("textures/terrain/noise.dds", TextureKind::color, 0.0f);
        m_acquired.insert(m_acquired.end(), {m_far_colour, m_far_normal, m_far_noise});
    } else {
        m_far_colour = m_far_noise = m_textures.white();
        m_far_normal = m_textures.flat_normal();
    }
    // Land textures; the last entry is the world default for k_no_texture.
    std::vector<GpuLandTexture> ltex;
    ltex.reserve(w.land_textures.size() + 1);
    for (const auto& t : w.land_textures) {
        GpuLandTexture g{};
        g.diffuse = m_textures.acquire(t.diffuse, TextureKind::color, 0.0f);
        g.normal = m_textures.acquire(t.normal, TextureKind::normal, 0.0f);
        g.specular_power = float(t.specular_exponent);
        g.is_snow = (t.flags & osw::land_texture_snow) ? 1.0f : 0.0f;
        m_acquired.push_back(g.diffuse);
        m_acquired.push_back(g.normal);
        ltex.push_back(g);
    }
    {
        GpuLandTexture g{};
        g.diffuse = w.default_land_texture.empty() ? m_textures.white()
                                                   : m_textures.acquire(w.default_land_texture, TextureKind::color, 0.0f);
        // The engine's default land texture set: the normal map beside the diffuse (sDefaultLandNormalTexture).
        std::string normal = w.default_land_texture;
        if (normal.size() > 4) {
            normal.insert(normal.size() - 4, "_n");
        }
        g.normal = w.default_land_texture.empty() ? m_textures.flat_normal()
                                                  : m_textures.acquire(normal, TextureKind::normal, 0.0f);
        g.specular_power = 30.0f;
        if (!w.default_land_texture.empty()) {
            m_acquired.push_back(g.diffuse);
            m_acquired.push_back(g.normal);
        }
        ltex.push_back(g);
    }
    m_land_textures = m_device.create_buffer(ltex.size() * sizeof(GpuLandTexture), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                             MemoryUsage::dynamic, Lifetime::asset, "land textures");
    std::memcpy(m_land_textures.mapped, ltex.data(), ltex.size() * sizeof(GpuLandTexture));

}

void Terrain::build_tables() {
    const auto& w = *m_world;
    // finish_textures() puts the world default for k_no_texture after the land textures.
    const uint32_t default_index = static_cast<uint32_t>(w.land_textures.size());
    auto resolve = [&](uint32_t t) { return t < w.land_textures.size() ? t : default_index; };
    std::vector<GpuTerrainQuad> quads(std::max<size_t>(w.lands.size() * 4, 1));
    // Point lights per quadrant geometry, tested on its own bound.
    const LightGrid light_grid(w);
    std::vector<const osw::Cell*> cell_of_land(w.lands.size(), nullptr);
    for (const auto& c : w.cells) {
        if (c.land < w.lands.size()) {
            cell_of_land[c.land] = &c;
        }
    }
    bool logged = false;
    for (size_t l = 0; l < w.lands.size(); ++l) {
        for (int q = 0; q < 4; ++q) {
            const auto& src = w.lands[l].quadrants[q];
            auto& dst = quads[l * 4 + q];
            for (auto& word : dst.lights) {
                word = 0xFFFFFFFFu;
            }
            if (const osw::Cell* c = cell_of_land[l]; c && !w.lights.empty()) {
                const int r0 = (q / 2) * 16, c0 = (q % 2) * 16;
                const float step = w.cell_size / 32.0f;
                glm::vec3 lo(1e30f), hi(-1e30f);
                for (int r = r0; r <= r0 + 16; ++r) {
                    for (int k = c0; k <= c0 + 16; ++k) {
                        const glm::vec3 p(c->x * w.cell_size + k * step, c->y * w.cell_size + r * step,
                                          w.lands[l].heights[r * osw::k_land_verts + k]);
                        lo = glm::min(lo, p);
                        hi = glm::max(hi, p);
                    }
                }
                const glm::vec3 centre = (lo + hi) * 0.5f;
                light_grid.list(centre, glm::length(hi - centre), true, dst.lights);
            }
            dst.base = src.base_texture == osw::k_no_texture ? default_index : resolve(src.base_texture);
            dst.count = std::min<uint32_t>(src.layer_count, 5);
            if (src.layer_count > 5 && !logged) {
                log::warn("terrain: quadrant with {} layer slots, only 5 are drawn", src.layer_count);
                logged = true;
            }
            for (uint32_t i = 0; i < 8; ++i) {
                dst.layers[i] = default_index;
                if (i < dst.count && src.first_layer + i < w.land_layers.size()) {
                    dst.layers[i] = resolve(w.land_layers[src.first_layer + i].texture);
                }
            }
        }
    }
    const size_t bytes = quads.size() * sizeof(GpuTerrainQuad);
    m_quads = m_device.create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     MemoryUsage::gpu_only, Lifetime::asset, "terrain quads", true);
    UploadRequest req;
    req.data.resize(bytes);
    std::memcpy(req.data.data(), quads.data(), bytes);
    req.buffer_regions.push_back({m_quads.buffer, 0, 0, bytes});
    ++m_pending;
    req.on_complete = [this] { --m_pending; };
    m_uploader.submit(std::move(req));
}

void Terrain::build_heightfield() {
    if (m_cells.empty()) {
        return;
    }
    const int32_t levels = [&] {
        int32_t l = 0;
        while ((1 << l) < std::max(m_grid_size[0], m_grid_size[1])) {
            ++l;
        }
        return l + 1;
    }();
    m_height_width = m_grid_size[0] * 32 + 1;
    m_height_rows = m_grid_size[1] * 32 + 1;
    const size_t bytes = (size_t(m_height_width) * m_height_rows * sizeof(uint16_t) + 3) & ~size_t(3);
    m_heights = m_device.create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       MemoryUsage::gpu_only, Lifetime::asset, "terrain heights", true);
    m_levels.resize(levels);
    ++m_pending;
    m_jobs.submit(1.0f, [this, world = m_world] {
        const int32_t width = m_height_width, rows = m_height_rows;
        // Whole steps above the lowest land.
        int32_t lowest = INT32_MAX;
        for (const LandCell& c : m_cells) {
            for (float h : world->lands[c.land].heights) {
                lowest = std::min(lowest, int32_t(std::lround(h / k_height_step)));
            }
        }
        m_height_bias = float(lowest) * k_height_step;
        auto samples = std::make_shared<std::vector<uint16_t>>(size_t(width) * rows + 2, uint16_t(0));
        uint16_t* field = samples->data();
        for (int32_t l = 0; l < int32_t(m_levels.size()); ++l) {
            Level& level = m_levels[l];
            level.width = (m_grid_size[0] + (1 << l) - 1) >> l;
            level.height = (m_grid_size[1] + (1 << l) - 1) >> l;
            level.nodes.assign(size_t(level.width) * level.height, Node{});
        }
        for (LandCell& c : m_cells) {
            const auto& land = world->lands[c.land];
            const int32_t gx = c.x - m_grid_min[0], gy = c.y - m_grid_min[1];
            float zmin = 1e30f, zmax = -1e30f;
            for (int r = 0; r < osw::k_land_verts; ++r) {
                for (int k = 0; k < osw::k_land_verts; ++k) {
                    const float h = land.heights[r * osw::k_land_verts + k];
                    field[size_t(gy * 32 + r) * width + gx * 32 + k] =
                        static_cast<uint16_t>(std::lround(h / k_height_step) - lowest);
                    zmin = std::min(zmin, h);
                    zmax = std::max(zmax, h);
                }
            }
            c.lo = {c.x * m_cell_size, c.y * m_cell_size, zmin - k_skirt_depth};
            c.hi = {(c.x + 1) * m_cell_size, (c.y + 1) * m_cell_size, zmax};
            Node& n = m_levels[0].at(gx, gy);
            n.zmin = zmin;
            n.zmax = zmax;
            n.land = true;
            n.full = true;
        }
        auto height = [&](int32_t col, int32_t row) { return float(field[size_t(row) * width + col]) * k_height_step; };
        for (int32_t l = 0; l < int32_t(m_levels.size()); ++l) {
            Level& level = m_levels[l];
            const int32_t step = 2 << l, half = step / 2, span = 32 << l;
            for (int32_t y = 0; y < level.height; ++y) {
                for (int32_t x = 0; x < level.width; ++x) {
                    Node& n = level.at(x, y);
                    if (l > 0) {
                        const Level& below = m_levels[l - 1];
                        n.full = true;
                        for (int32_t j = 0; j < 2; ++j) {
                            for (int32_t i = 0; i < 2; ++i) {
                                const int32_t cx = x * 2 + i, cy = y * 2 + j;
                                if (cx >= below.width || cy >= below.height || !below.at(cx, cy).full) {
                                    n.full = false;
                                }
                                if (cx < below.width && cy < below.height && below.at(cx, cy).land) {
                                    const Node& child = below.at(cx, cy);
                                    n.land = true;
                                    n.zmin = std::min(n.zmin, child.zmin);
                                    n.zmax = std::max(n.zmax, child.zmax);
                                    n.error = std::max(n.error, child.error);
                                }
                            }
                        }
                    }
                    if (!n.full) {
                        continue; // never drawn as one piece, so its grid has no error to know
                    }
                    // What collapsing the finer grid onto this one moves: the vertices between its own.
                    const int32_t c0 = x * span, r0 = y * span;
                    for (int32_t r = 0; r <= span; r += half) {
                        for (int32_t c = 0; c <= span; c += half) {
                            const bool odd_c = (c / half) & 1, odd_r = (r / half) & 1;
                            if (!odd_c && !odd_r) {
                                continue;
                            }
                            const int32_t col = c0 + c, row = r0 + r;
                            const float h = height(col, row);
                            const float t = odd_c && odd_r ? (height(col - half, row - half) + height(col + half, row + half)) * 0.5f
                                            : odd_c        ? (height(col - half, row) + height(col + half, row)) * 0.5f
                                                           : (height(col, row - half) + height(col, row + half)) * 0.5f;
                            n.error = std::max(n.error, std::abs(h - t));
                        }
                    }
                }
            }
        }
        const size_t total = (size_t(width) * rows * sizeof(uint16_t) + 3) & ~size_t(3);
        size_t chunks = 0;
        for (size_t at = 0; at < total; at += k_heights_per_upload) {
            ++chunks;
        }
        auto left = std::make_shared<size_t>(chunks);
        for (size_t at = 0; at < total; at += k_heights_per_upload) {
            const size_t n = std::min(k_heights_per_upload, total - at);
            UploadRequest req;
            req.data.resize(n);
            std::memcpy(req.data.data(), reinterpret_cast<const std::byte*>(field) + at, n);
            req.buffer_regions.push_back({m_heights.buffer, at, 0, n});
            req.on_complete = [this, left] {
                if (--*left == 0) {
                    m_heights_ready = true;
                    --m_pending;
                }
            };
            m_uploader.submit(std::move(req));
        }
    });
}

void Terrain::request_near(uint32_t cell, float priority) {
    LandCell& c = m_cells[cell];
    if (c.near == NearState::abandoned) {
        c.near = NearState::pending; // the upload in flight becomes useful again
        return;
    }
    if (c.near != NearState::none) {
        return;
    }
    if (m_free_slots.empty() || m_near_in_flight >= k_max_near_in_flight) {
        m_near_wanted_unscheduled = true;
        return;
    }
    const uint32_t slot = m_free_slots.back();
    m_free_slots.pop_back();
    c.near = NearState::pending;
    c.near_slot = slot;
    m_near_cells.push_back(cell);
    ++m_near_in_flight;
    const VkDeviceSize offset = VkDeviceSize(slot) * k_cell_vertices * sizeof(TerrainVertex);
    m_jobs.submit(priority, [this, cell, offset, world = m_world, x = c.x, y = c.y, land = c.land] {
        UploadRequest req;
        req.data.resize(k_cell_vertices * sizeof(TerrainVertex));
        build_cell(*world, world->lands[land], land, x, y, reinterpret_cast<TerrainVertex*>(req.data.data()));
        req.buffer_regions.push_back({m_vertices.buffer, offset, 0, req.data.size()});
        req.on_complete = [this, cell] {
            --m_near_in_flight;
            LandCell& lc = m_cells[cell];
            if (lc.near == NearState::pending) {
                lc.near = NearState::ready;
            } else if (lc.near == NearState::abandoned) {
                m_free_slots.push_back(lc.near_slot);
                lc.near_slot = ~0u;
                lc.near = NearState::none;
            }
        };
        m_uploader.submit(std::move(req));
    });
}

void Terrain::release_near(uint32_t cell) {
    LandCell& c = m_cells[cell];
    if (c.near == NearState::pending) {
        c.near = NearState::abandoned;
    } else if (c.near == NearState::ready) {
        c.near = NearState::none;
        // Frames in flight may still draw from the slot.
        m_device.defer([this, slot = c.near_slot] { m_free_slots.push_back(slot); });
        c.near_slot = ~0u;
    }
}

void Terrain::update(const glm::vec3& camera, const TerrainSettings& settings) {
    if (m_cells.empty()) {
        return;
    }
    const float radius = std::clamp(settings.full_res_radius, 0.0f, float(k_max_full_res_radius));
    const int32_t cx = static_cast<int32_t>(std::floor(camera.x / m_cell_size));
    const int32_t cy = static_cast<int32_t>(std::floor(camera.y / m_cell_size));
    const glm::vec2 cam(camera.x / m_cell_size, camera.y / m_cell_size);
    // Re-plan on small moves too, so the ring is always measured from close to where the camera is now.
    if (cx == m_last_cam_cell[0] && cy == m_last_cam_cell[1] && radius == m_last_radius &&
        !m_near_wanted_unscheduled && glm::length(cam - m_plan_pos) <= k_replan_cells) {
        return;
    }
    m_last_cam_cell[0] = cx;
    m_last_cam_cell[1] = cy;
    m_last_radius = radius;
    m_plan_pos = cam;
    m_near_wanted_unscheduled = false;
    auto dist = [&](const LandCell& c) { return glm::length(glm::vec2(c.x + 0.5f, c.y + 0.5f) - cam); };

    // Release cells that left the ring (with one cell of hysteresis).
    std::erase_if(m_near_cells, [&](uint32_t ci) {
        LandCell& c = m_cells[ci];
        if (c.near != NearState::none && dist(c) > radius + 1.5f) {
            release_near(ci);
        }
        return c.near == NearState::none;
    });

    // Request the ring, nearest first.
    m_scratch.clear();
    const int r = static_cast<int>(std::ceil(radius)) + 1;
    for (int y = cy - r; y <= cy + r; ++y) {
        for (int x = cx - r; x <= cx + r; ++x) {
            const int gx = x - m_grid_min[0], gy = y - m_grid_min[1];
            if (gx < 0 || gy < 0 || gx >= m_grid_size[0] || gy >= m_grid_size[1]) {
                continue;
            }
            const int32_t ci = m_grid[size_t(gy) * m_grid_size[0] + gx];
            if (ci < 0) {
                continue;
            }
            const float d = dist(m_cells[ci]);
            // A superset of the ring collect() draws from the current camera, which may be up to
            // k_replan_cells from where this plan was made.
            if (d <= radius + 0.75f + k_replan_cells) {
                m_scratch.emplace_back(d, static_cast<uint32_t>(ci));
            }
        }
    }
    std::sort(m_scratch.begin(), m_scratch.end());
    for (const auto& [d, ci] : m_scratch) {
        request_near(ci, d);
    }
}

void Terrain::collect_cascade(const ShadowCascade& cascade, const glm::vec3& camera, const TerrainSettings& settings,
                              TerrainDraws& out) const {
    out.clear();
    if (!m_heights_ready) {
        return;
    }
    const float ring = std::clamp(settings.full_res_radius, 0.0f, float(k_max_full_res_radius));
    const glm::vec2 cam(camera.x / m_cell_size, camera.y / m_cell_size);
    for (const LandCell& c : m_cells) {
        const glm::vec3 centre = (c.lo + c.hi) * 0.5f - camera;
        if (!cascade.sphere(centre, glm::length(c.hi - c.lo) * 0.5f)) {
            continue;
        }
        // As collect(): what is drawn depends on where the camera is, not on what the hysteresis has kept.
        if (c.near == NearState::ready && glm::length(glm::vec2(c.x + 0.5f, c.y + 0.5f) - cam) <= ring + 0.75f) {
            out.near.push_back(c.near_slot * k_cell_vertices);
        } else {
            add_far_node(out, 0, c.x - m_grid_min[0], c.y - m_grid_min[1], 0.0f);
        }
    }
}

float Terrain::node_range(int level, const Node& node, float range_scale) const {
    return std::max(node.error * range_scale, k_min_range_sizes * m_cell_size * float(1 << level));
}

void Terrain::add_far_node(TerrainDraws& out, int level, int32_t x, int32_t y, float parent_range) const {
    const Node& n = m_levels[level].at(x, y);
    GpuFarNode g{};
    g.column = (x << level) * 32;
    g.row = (y << level) * 32;
    g.step = 2u << level;
    g.skirt = k_skirt_depth + 2.0f * n.error + 0.01f * m_cell_size * float(1 << level);
    g.morph_start = k_morph_begin * parent_range;
    g.morph_scale = parent_range > 0.0f ? 1.0f / ((1.0f - k_morph_begin) * parent_range) : 0.0f;
    out.far.push_back(g);
}

void Terrain::far_node(FarWalk& walk, int level, int32_t x, int32_t y, float parent_range) {
    const Level& lv = m_levels[level];
    if (x >= lv.width || y >= lv.height) {
        return;
    }
    const Node& n = lv.at(x, y);
    if (!n.land) {
        return;
    }
    const int32_t cells = 1 << level;
    const glm::vec2 cell_lo(float(m_grid_min[0] + (x << level)), float(m_grid_min[1] + (y << level)));
    const glm::vec3 lo(cell_lo * m_cell_size, n.zmin - 2.0f * n.error);
    const glm::vec3 hi((cell_lo + float(cells)) * m_cell_size, n.zmax);
    if (walk.frustum.aabb(lo - walk.camera, hi - walk.camera) == Cull::outside) {
        return;
    }
    if (level == 0) {
        const int32_t ci = m_grid[size_t(y) * m_grid_size[0] + x];
        const LandCell& c = m_cells[ci];
        if (!(c.near == NearState::ready &&
              glm::length(glm::vec2(c.x + 0.5f, c.y + 0.5f) - walk.camera_cells) <= walk.ring + 0.75f)) {
            add_far_node(walk.out, 0, x, y, parent_range);
        }
        return;
    }
    const glm::vec3 nearest = glm::clamp(walk.camera, lo, hi);
    const float distance = glm::length(nearest - walk.camera);
    const float range = node_range(level, n, walk.range_scale);
    const glm::vec2 to_rect =
        glm::max(glm::max(cell_lo - walk.camera_cells, walk.camera_cells - (cell_lo + float(cells))), glm::vec2(0.0f));
    const bool ring = glm::length(to_rect) <= walk.ring + 1.5f;
    if (!ring && n.full && distance >= range) {
        add_far_node(walk.out, level, x, y, parent_range);
        return;
    }
    // Children nearest first, so hills in front hide what is behind them.
    const float mid_x = cell_lo.x + float(cells) * 0.5f, mid_y = cell_lo.y + float(cells) * 0.5f;
    const int32_t first_x = walk.camera_cells.x < mid_x ? 0 : 1, first_y = walk.camera_cells.y < mid_y ? 0 : 1;
    for (int32_t k = 0; k < 4; ++k) {
        far_node(walk, level - 1, x * 2 + (first_x ^ (k & 1)), y * 2 + (first_y ^ (k >> 1)), range);
    }
}

void Terrain::collect(const Frustum& frustum, const glm::vec3& camera, const TerrainSettings& settings,
                      TerrainDraws& out) {
    out.clear();
    m_drawn_near = m_drawn_far = 0;
    if (!m_heights_ready) {
        return;
    }
    const float ring = std::clamp(settings.full_res_radius, 0.0f, float(k_max_full_res_radius));
    const glm::vec2 cam(camera.x / m_cell_size, camera.y / m_cell_size);
    // Full resolution only inside the ring measured from the current camera: cells kept resident by the release
    // hysteresis must not render differently depending on where the camera has been.
    m_scratch.clear();
    for (uint32_t ci : m_near_cells) {
        const LandCell& c = m_cells[ci];
        const float d = glm::length(glm::vec2(c.x + 0.5f, c.y + 0.5f) - cam);
        if (c.near == NearState::ready && d <= ring + 0.75f && frustum.aabb(c.lo - camera, c.hi - camera) != Cull::outside) {
            m_scratch.emplace_back(d, ci);
        }
    }
    std::sort(m_scratch.begin(), m_scratch.end());
    for (const auto& [d, ci] : m_scratch) {
        out.near.push_back(m_cells[ci].near_slot * k_cell_vertices);
    }
    FarWalk walk{frustum, camera, cam, ring, settings.pixels_per_radian / std::max(settings.pixel_error, 0.01f), out};
    if (!m_levels.empty()) { // a space with no land: an offscreen view can ask in the frame a travel arrives
        far_node(walk, int(m_levels.size()) - 1, 0, 0, 0.0f);
    }
    m_drawn_near = static_cast<uint32_t>(out.near.size());
    m_drawn_far = static_cast<uint32_t>(out.far.size());
}

void Terrain::far_params(const TerrainSettings& settings, GpuFarLand& out) const {
    if (m_cells.empty()) {
        return;
    }
    const float x0 = float(m_grid_min[0]) * m_cell_size, y0 = float(m_grid_min[1]) * m_cell_size;
    const float sx = 1.0f / (float(m_grid_size[0]) * m_cell_size), sy = 1.0f / (float(m_grid_size[1]) * m_cell_size);
    out.maps = glm::uvec4(m_far_colour, m_far_normal, m_far_noise, uint32_t(m_height_width));
    out.heights = m_heights.address;
    out.map = glm::vec4(sx, sy, -x0 * sx, -y0 * sy);
    out.samples = glm::vec4(x0, y0, m_cell_size / 32.0f, m_height_bias);
    // The ring's outer cells turn into level 0 of the far land, in shape and in colour, before it ends.
    const float ring = std::clamp(settings.full_res_radius, 0.0f, float(k_max_full_res_radius)) * m_cell_size;
    const float length = 1.5f * m_cell_size;
    out.ring = glm::vec4(ring - length, 1.0f / length, ring - length, 1.0f / length);
}

bool Terrain::area_bounds(int32_t x0, int32_t y0, int32_t x1, int32_t y1, glm::vec3& lo, glm::vec3& hi) const {
    lo = glm::vec3(1e30f);
    hi = glm::vec3(-1e30f);
    bool any = false;
    for (const LandCell& c : m_cells) {
        if (c.x < x0 || c.x > x1 || c.y < y0 || c.y > y1) {
            continue;
        }
        lo = glm::min(lo, c.lo);
        hi = glm::max(hi, c.hi);
        any = true;
    }
    return any;
}

TerrainStats Terrain::stats() const {
    TerrainStats s;
    s.land_cells = static_cast<uint32_t>(m_cells.size());
    for (uint32_t ci : m_near_cells) {
        if (m_cells[ci].near == NearState::ready) {
            ++s.near_ready;
        } else {
            ++s.near_pending;
        }
    }
    s.near_capacity = m_near_capacity;
    s.drawn_near = m_drawn_near;
    s.drawn_far = m_drawn_far;
    return s;
}

} // namespace spud::render
