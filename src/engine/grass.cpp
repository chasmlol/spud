#include "engine/grass.hpp"

#include "core/game_random.hpp"
#include "core/log.hpp"
#include "core/units.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>

namespace spud::engine {

namespace {

constexpr int k_quad = osw::k_quad_verts;
constexpr float k_min_grass_size = 20.0f; // iMinGrassSize:Grass
constexpr uint32_t k_max_entries = 16;
constexpr uint32_t k_max_forms_per_texture = 3; // iMaxGrassTypesPerTexure:Grass, tested after adding

// float32 to half and back, round to nearest even (the game's).
uint16_t to_half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exponent = int32_t((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = x & 0x7FFFFFu;
    if (exponent >= 31) {
        return uint16_t(sign | 0x7C00u);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return uint16_t(sign);
        }
        mantissa |= 0x800000u;
        const uint32_t shift = uint32_t(14 - exponent);
        const uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1u), middle = 1u << (shift - 1);
        return uint16_t(sign | (half + (rest > middle || (rest == middle && (half & 1u)) ? 1u : 0u)));
    }
    const uint32_t half = (uint32_t(exponent) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1FFFu;
    return uint16_t(sign | (half + (rest > 0x1000u || (rest == 0x1000u && (half & 1u)) ? 1u : 0u)));
}

float from_half(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 31u, mantissa = h & 1023u;
    uint32_t x;
    if (exponent == 0) {
        if (mantissa == 0) {
            x = sign;
        } else {
            exponent = 127 - 14;
            while (!(mantissa & 1024u)) {
                mantissa <<= 1;
                --exponent;
            }
            x = sign | (exponent << 23) | ((mantissa & 1023u) << 13);
        }
    } else {
        x = sign | ((exponent == 31 ? 255u : exponent + 112u) << 23) | (mantissa << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

// S(u) = u x 2^-31 - 1, u converted as unsigned.
float signed_unit(uint32_t u) { return float(double(u)) * 4.6566129e-10f - 1.0f; }

const std::array<float, 512>& cos_table() {
    static const std::array<float, 512> t = [] {
        std::array<float, 512> c{};
        for (int i = 0; i < 512; ++i) {
            c[size_t(i)] = float(std::cos(2.0 * 3.14159265358979323846 * i / 512.0));
        }
        return c;
    }();
    return t;
}

struct Sample {
    float height;
    glm::vec3 normal;
    glm::vec3 colour;
};

// LAND_LocateTriangle and LAND_SampleAt (6.2): the height on the land's triangle, normal and colour of its nearest
// vertex. lx, ly relative to the cell.
bool sample_land(const osw::Land& land, const float* z, float lx, float ly, Sample& out) {
    if (lx < 0.0f || ly < 0.0f || lx > 4096.0f || ly > 4096.0f) {
        return false;
    }
    auto split = [](float v, int size, int& index, float& rest) {
        int i = int(v) / size;
        if (int(v) != 0 && int(v) % size == 0) {
            --i;
        }
        index = i;
        rest = v - float(i * size);
    };
    int qx, qy;
    float rx, ry;
    split(lx, 2048, qx, rx);
    split(ly, 2048, qy, ry);
    int vx, vy;
    float fx, fy;
    split(rx, 128, vx, fx);
    split(ry, 128, vy, fy);
    const int col = qx * 16 + vx, row = qy * 16 + vy;
    auto at = [&](int c, int r) { return z[(row + r) * osw::k_land_verts + col + c]; };
    struct Corner {
        int c, r;
    };
    Corner a, b, c;
    bool upper;
    const bool odd = ((vx + vy) & 1) != 0;
    if (odd) {
        b = {1, 0};
        c = {0, 1};
        upper = fx + fy > 128.0f;
        a = upper ? Corner{1, 1} : Corner{0, 0};
    } else {
        b = {0, 0};
        c = {1, 1};
        upper = fx <= fy;
        a = upper ? Corner{0, 1} : Corner{1, 0};
    }
    const float za = at(a.c, a.r), zb = at(b.c, b.r), zc = at(c.c, c.r);
    constexpr float I = 0.0078125f;
    if (odd) {
        out.height = upper ? za - ((za - zb) * (128.0f - fy) + (za - zc) * (128.0f - fx)) * I
                           : (((zb - za) * I) * fx + za) + ((zc - za) * I) * fy;
    } else {
        out.height = upper ? (((za - zb) * I) * fy + zb) + ((zc - za) * I) * fx
                           : (((za - zb) * I) * fx + zb) + ((zc - za) * I) * fy;
    }
    // Nearest of A, B, C; earlier wins ties. On a vertex exactly, that vertex.
    int nc, nr;
    if (int(lx) % 128 != 0 || int(ly) % 128 != 0) {
        const Corner corners[3] = {a, b, c};
        float best = FLT_MAX;
        nc = nr = 0;
        for (const Corner& k : corners) {
            const float dx = fx - float(k.c) * 128.0f, dy = fy - float(k.r) * 128.0f;
            const float d = dx * dx + dy * dy;
            if (d < best) {
                best = d;
                nc = k.c;
                nr = k.r;
            }
        }
        nc += col;
        nr += row;
    } else {
        nc = qx * 16 + int(rx) / 128;
        nr = qy * 16 + int(ry) / 128;
    }
    const int vi = nr * osw::k_land_verts + nc;
    glm::vec3 n(float(land.normals[vi * 3]) * 0.0078740157f, float(land.normals[vi * 3 + 1]) * 0.0078740157f,
                float(land.normals[vi * 3 + 2]) * 0.0078740157f);
    const float length = glm::length(n);
    out.normal = length > 0 ? n / length : n;
    out.colour = glm::vec3(land.colors[vi * 3], land.colors[vi * 3 + 1], land.colors[vi * 3 + 2]) * 0.0039215689f;
    return true;
}

bool water_allows(uint32_t type, float h, float w, float u) {
    switch (type) {
    case 0: return h >= w - u;
    case 1: return w <= h && h <= w + u;
    case 2: return h <= w - u;
    case 3: return w - u <= h && h <= w;
    case 4: return h >= w + u || h <= w - u;
    case 5: return w - u <= h && h <= w + u;
    default: return true;
    }
}

struct Entry {
    uint32_t form;
    float w[9];
};

} // namespace

std::vector<GrassInstance> generate_grass(const osw::World& world, const osw::Cell& cell) {
    std::vector<GrassInstance> out;
    if (cell.land >= world.lands.size() || world.grasses.empty()) {
        return out;
    }
    const osw::Land& land = world.lands[cell.land];
    float z[osw::k_land_verts * osw::k_land_verts];
    for (int i = 0; i < osw::k_land_verts * osw::k_land_verts; ++i) {
        z[i] = std::round(land.heights[i] / units::k_skyrim_unit_m / 8.0f) * 8.0f;
    }
    const float water = (cell.flags & osw::cell_has_water) ? cell.water_height / units::k_skyrim_unit_m : -FLT_MAX;
    const int32_t cx = cell.x, cy = cell.y;
    const int32_t block_x = 12 * (cx / 12), block_y = 12 * (cy / 12);
    const float origin_x = float(block_x) * 4096.0f, origin_y = float(block_y) * 4096.0f;
    GameRandom random(uint32_t(cx * cy));
    const auto& cosines = cos_table();
    std::vector<Entry> entries;
    const uint32_t packed_block = uint32_t(cx / 12 + 128) << 8 | uint32_t(cy / 12 + 128) << 16;

    for (int q = 0; q < 4; ++q) {
        const osw::LandQuadrant& quad = land.quadrants[q];
        const uint32_t layers = std::min<uint32_t>(quad.layer_count, 5);
        auto weight = [&](uint32_t slot, int v) -> uint32_t {
            const uint32_t l = quad.first_layer + slot;
            return l < world.land_layers.size() ? world.land_layers[l].opacity[v] : 0u;
        };
        for (int x = 1; x <= 15; x += 2) {
            for (int y = 1; y <= 15; y += 2) {
                const int v = x + k_quad * y;
                const int nine[9] = {v - 18, v - 17, v - 16, v - 1, v, v + 1, v + 16, v + 17, v + 18};
                entries.clear();
                auto add_forms = [&](uint32_t texture, auto&& present) {
                    if (texture >= world.land_textures.size()) {
                        return;
                    }
                    const osw::LandTexture& t = world.land_textures[texture];
                    uint32_t added = 0;
                    for (uint32_t i = 0; i < t.grass_count; ++i) {
                        if (entries.size() >= k_max_entries || added >= k_max_forms_per_texture) {
                            break;
                        }
                        const uint32_t form = world.land_grass[t.first_grass + i];
                        const float density = float(world.grasses[form].density) * 0.0099999998f;
                        Entry e{form, {}};
                        for (int k = 0; k < 9; ++k) {
                            e.w[k] = present(nine[k]) ? density : 0.0f;
                        }
                        entries.push_back(e);
                        ++added;
                    }
                };
                add_forms(quad.base_texture, [&](int n) {
                    uint32_t sum = 0;
                    for (uint32_t s = 0; s < layers; ++s) {
                        sum += weight(s, n);
                    }
                    return 1.0f - float(sum) * 0.0039215689f > 0.0f;
                });
                for (uint32_t s = 0; s < layers; ++s) {
                    bool strong = false;
                    for (int n : nine) {
                        strong = strong || weight(s, n) > 25u;
                    }
                    if (strong && quad.first_layer + s < world.land_layers.size()) {
                        add_forms(world.land_layers[quad.first_layer + s].texture, [&](int n) { return weight(s, n) > 0u; });
                    }
                }
                const float vertex_x = float(x * 128 + (q & 1) * 2048 + cx * 4096);
                const float vertex_y = float(y * 128 + ((q & 2) >> 1) * 2048 + cy * 4096);
                const float ox = vertex_x - 128.0f, oy = vertex_y - 128.0f;
                for (const Entry& e : entries) {
                    const osw::Grass& g = world.grasses[e.form];
                    const float side = 256.0f;
                    const int n = std::min(int(side / g.position_range), int(side / k_min_grass_size));
                    if (n <= 0) {
                        continue;
                    }
                    const float inv = 1.0f / float(n);
                    const float hs = (side * inv) * 0.5f;
                    const float two = inv + inv;
                    for (int j = 0; j < n; ++j) {
                        const float J = float(j) + 0.5f;
                        const float xoff = (hs + hs) * J;
                        const float wx = two * J;
                        const int cxi = int(wx);
                        const float fx = wx - float(cxi);
                        for (int i = 0; i < n; ++i) {
                            const float I = float(i) + 0.5f;
                            const float wy = two * I;
                            const int cyi = int(wy);
                            const float fy = wy - float(cyi);
                            const int idx = cxi + 3 * cyi;
                            const float a = ((1.0f - fx) * e.w[idx] + e.w[idx + 1] * fx) * (1.0f - fy);
                            const float b = ((1.0f - fx) * e.w[idx + 3] + e.w[idx + 4] * fx) * fy;
                            const int chance = int((a + b) * 32768.0f * 1.0f);
                            if (chance == 0) {
                                continue;
                            }
                            if (int(random.next_u32() & 0x7FFFu) >= chance) {
                                continue;
                            }
                            const uint32_t u1 = random.next_u32(), u2 = random.next_u32();
                            const float px = (signed_unit(u1) * hs + (xoff + ox)) - origin_x;
                            const float py = (signed_unit(u2) * hs + ((I * hs) + (I * hs) + oy)) - origin_y;
                            const uint16_t hx = to_half(px), hy = to_half(py);
                            const float wxq = origin_x + from_half(hx), wyq = origin_y + from_half(hy);
                            Sample s;
                            if (!sample_land(land, z, wxq - float(cx) * 4096.0f, wyq - float(cy) * 4096.0f, s)) {
                                continue;
                            }
                            if (!water_allows(g.water_type, s.height, water, float(g.units_from_water))) {
                                continue;
                            }
                            const float slope_max = cosines[size_t(int(float(g.max_slope) * 1.4222221f) & 511)];
                            const float slope_min = cosines[size_t(int(float(g.min_slope) * 1.4222221f) & 511)];
                            if (!(slope_max <= s.normal.z && s.normal.z <= slope_min)) {
                                continue;
                            }
                            const uint32_t u3 = random.next_u32(), u4 = random.next_u32(), u5 = random.next_u32();
                            const float luma = (s.colour.r * 0.31f + s.colour.g * 0.37f) + s.colour.b * 0.32f;
                            const float m = (float(double(u3)) * 1.1641532e-10f - 0.25f) * g.color_range + (1.0f - g.color_range);
                            float brightness = luma * m;
                            brightness = brightness >= 1.0f ? 0.99f : std::max(brightness, 0.0f);
                            const float sn = signed_unit(u4);
                            const float cs = std::sqrt(std::max(1.0f - sn * sn, 0.0f));
                            glm::vec3 P, Q, N;
                            if (g.flags & osw::grass_fit_to_slope) {
                                const glm::vec3 nn = s.normal;
                                glm::vec3 T;
                                if (!(std::abs(nn.x) > std::abs(nn.y) || std::abs(nn.x) > std::abs(nn.z))) {
                                    T = glm::vec3(0.0f, -nn.z, nn.y);
                                } else if (!(std::abs(nn.y) > std::abs(nn.x) || std::abs(nn.y) > std::abs(nn.z))) {
                                    T = glm::vec3(-nn.z, 0.0f, nn.x);
                                } else {
                                    T = glm::vec3(-nn.y, nn.x, 0.0f);
                                }
                                const float tl = glm::length(T);
                                T = tl > 1e-6f ? T / tl : glm::vec3(0.0f);
                                const glm::vec3 B(nn.z * T.y - nn.y * T.z, nn.x * T.z - nn.z * T.x, nn.y * T.x - nn.x * T.y);
                                P = cs * T + sn * B;
                                Q = sn * T - cs * B;
                                N = nn;
                            } else {
                                P = glm::vec3(sn, cs, 0.0f);
                                Q = glm::vec3(-cs, sn, 0.0f);
                                N = glm::vec3(0.0f, 0.0f, 1.0f);
                            }
                            const float d = signed_unit(u5) * g.height_range;
                            GrassInstance r{};
                            r.half[0] = hx;
                            r.half[1] = hy;
                            r.half[2] = to_half(s.height);
                            r.half[3] = to_half(brightness);
                            r.half[4] = to_half(P.x);
                            r.half[5] = to_half(Q.x);
                            r.half[6] = to_half(N.x);
                            r.half[7] = to_half(Q.z);
                            r.half[8] = to_half(P.y);
                            r.half[9] = to_half(Q.y);
                            r.half[10] = to_half(N.y);
                            r.half[11] = to_half(N.z);
                            r.half[12] = to_half(P.z);
                            r.half[13] = to_half(d);
                            r.packed = packed_block | e.form;
                            out.push_back(r);
                        }
                    }
                }
            }
        }
    }
    return out;
}

GrassField::GrassField(render::Renderer& renderer, std::shared_ptr<const osw::World> world)
    : m_renderer(renderer), m_world(std::move(world)), m_slots(k_slots) {
    const auto& w = *m_world;
    auto& device = m_renderer.device();
    m_instances = device.create_buffer(VkDeviceSize(k_slots) * k_slot_instances * sizeof(GrassInstance),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       render::MemoryUsage::gpu_only, render::Lifetime::asset, "grass instances", true);
    // The forms, then each slot's fade-in in the `pad` of an entry past the 256 a blade can name.
    m_forms = device.create_buffer((256 + k_slots) * sizeof(render::GpuGrassForm),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, render::MemoryUsage::dynamic,
                                   render::Lifetime::asset, "grass forms");
    m_meshes.assign(w.grasses.size(), render::k_invalid_mesh);
    for (size_t i = 0; i < w.grasses.size(); ++i) {
        if (w.grasses[i].mesh < w.meshes.size()) {
            m_meshes[i] = m_renderer.meshes().acquire(w.meshes[w.grasses[i].mesh], 0.0f);
        }
    }
    int32_t lo[2] = {INT32_MAX, INT32_MAX}, hi[2] = {INT32_MIN, INT32_MIN};
    for (const auto& c : w.cells) {
        lo[0] = std::min(lo[0], c.x);
        lo[1] = std::min(lo[1], c.y);
        hi[0] = std::max(hi[0], c.x);
        hi[1] = std::max(hi[1], c.y);
    }
    m_lookup_min[0] = lo[0];
    m_lookup_min[1] = lo[1];
    m_lookup_size[0] = hi[0] - lo[0] + 1;
    m_lookup_size[1] = hi[1] - lo[1] + 1;
    m_cell_lookup.assign(size_t(m_lookup_size[0]) * m_lookup_size[1], -1);
    for (size_t i = 0; i < w.cells.size(); ++i) {
        m_cell_lookup[size_t(w.cells[i].y - lo[1]) * m_lookup_size[0] + (w.cells[i].x - lo[0])] = int32_t(i);
    }
}

GrassField::~GrassField() {
    for (render::MeshId m : m_meshes) {
        if (m != render::k_invalid_mesh) {
            m_renderer.meshes().release(m);
        }
    }
    m_renderer.device().destroy_buffer(m_instances);
    m_renderer.device().destroy_buffer(m_forms);
}

int32_t GrassField::lookup(int32_t x, int32_t y) const {
    const int32_t gx = x - m_lookup_min[0], gy = y - m_lookup_min[1];
    if (gx < 0 || gy < 0 || gx >= m_lookup_size[0] || gy >= m_lookup_size[1]) {
        return -1;
    }
    return m_cell_lookup[size_t(gy) * m_lookup_size[0] + gx];
}

constexpr float k_fade_in_seconds = 1.8f; // fGrassFadeInTime:Grass

void GrassField::build_forms() {
    const auto& meshes = m_renderer.meshes();
    for (render::MeshId m : m_meshes) {
        if (m != render::k_invalid_mesh && !meshes.ready(m) && !meshes.failed(m)) {
            return;
        }
    }
    auto* forms = m_forms.as<render::GpuGrassForm>();
    for (size_t i = 0; i < m_meshes.size(); ++i) {
        render::GpuGrassForm f{};
        const osw::Grass& g = m_world->grasses[i];
        f.wave_period = g.wave_period;
        f.uniform = (g.flags & osw::grass_uniform_scale) ? 1.0f : 0.0f;
        if (m_meshes[i] != render::k_invalid_mesh && meshes.ready(m_meshes[i]) && !meshes.groups(m_meshes[i]).empty()) {
            const render::DrawGroup& dg = meshes.group(meshes.groups(m_meshes[i])[0]);
            f.material = dg.material;
        }
        forms[i] = f;
    }
    m_forms_ready = true;
}

void GrassField::request(int32_t cell) {
    uint32_t free_slot = ~0u;
    int best = -1;
    const auto& c = m_world->cells[size_t(cell)];
    for (uint32_t s = 0; s < k_slots; ++s) {
        Slot& slot = m_slots[s];
        if (slot.cell == cell) {
            return;
        }
        if (slot.cell < 0) {
            free_slot = s;
            best = INT32_MAX;
        } else if (slot.ready) {
            const auto& o = m_world->cells[size_t(slot.cell)];
            const int d = std::max(std::abs(o.x - m_centre[0]), std::abs(o.y - m_centre[1]));
            if (d > m_cell_radius && d > best) {
                best = d;
                free_slot = s;
            }
        }
    }
    if (free_slot == ~0u) {
        return;
    }
    Slot& slot = m_slots[free_slot];
    slot.cell = cell;
    slot.ready = false;
    slot.groups.clear();
    const uint32_t generation = ++slot.generation;
    ++m_in_flight;
    const float cs = m_world->cell_size;
    m_renderer.jobs().submit(1.0f, [this, free_slot, generation, cell, world = m_world, cs] {
        const osw::Cell& c = world->cells[size_t(cell)];
        std::vector<GrassInstance> blades = generate_grass(*world, c);
        if (blades.size() > k_slot_instances) {
            log::warn("grass: cell {} {}: {} blades, the slot holds {}", c.x, c.y, blades.size(), k_slot_instances);
            blades.resize(k_slot_instances);
        }
        std::stable_sort(blades.begin(), blades.end(), [](const GrassInstance& a, const GrassInstance& b) {
            return (a.packed & 0xFFu) < (b.packed & 0xFFu);
        });
        for (GrassInstance& blade : blades) {
            blade.packed = (blade.packed & 0xFFFFFFu) | (free_slot << 24);
        }
        auto groups = std::make_shared<std::vector<Group>>();
        for (uint32_t i = 0; i < blades.size(); ++i) {
            const uint32_t form = blades[i].packed & 0xFFu;
            if (groups->empty() || groups->back().form != form) {
                groups->push_back({form, i, 0});
            }
            ++groups->back().count;
        }
        const auto& land = world->lands[c.land < world->lands.size() ? c.land : 0];
        float zmin = 1e30f, zmax = -1e30f;
        if (c.land < world->lands.size()) {
            for (float h : land.heights) {
                zmin = std::min(zmin, h);
                zmax = std::max(zmax, h);
            }
        }
        const glm::vec3 lo(float(c.x) * cs, float(c.y) * cs, zmin - 1.0f);
        const glm::vec3 hi(float(c.x + 1) * cs, float(c.y + 1) * cs, zmax + 3.0f);
        render::UploadRequest req;
        const size_t bytes = blades.size() * sizeof(GrassInstance);
        req.data.resize(std::max<size_t>(bytes, 4));
        std::memcpy(req.data.data(), blades.data(), bytes);
        if (bytes > 0) {
            req.buffer_regions.push_back(
                {m_instances.buffer, VkDeviceSize(free_slot) * k_slot_instances * sizeof(GrassInstance), 0, bytes});
        }
        req.on_complete = [this, free_slot, generation, groups, lo, hi] {
            --m_in_flight;
            Slot& s = m_slots[free_slot];
            if (s.generation == generation) {
                s.groups = std::move(*groups);
                s.lo = lo;
                s.hi = hi;
                s.ready = true;
                s.alpha = 0.0f;
                m_forms.as<render::GpuGrassForm>()[256 + free_slot].pad = 0.0f;
            }
        };
        m_renderer.uploader().submit(std::move(req));
    });
}

void GrassField::update(const glm::vec3& camera, float fade_seconds, int cell_radius) {
    m_cell_radius = cell_radius;
    if (!m_forms_ready) {
        build_forms();
    }
    // Each frame a new cell gains min(dt / fGrassFadeInTime, 0.1), up to 1.
    const float gain = fade_seconds < 0.0f ? 1.0f : std::min(fade_seconds / k_fade_in_seconds, 0.1f);
    auto* forms = m_forms.as<render::GpuGrassForm>();
    for (uint32_t s = 0; s < k_slots; ++s) {
        Slot& slot = m_slots[s];
        if (slot.ready && slot.alpha < 1.0f) {
            slot.alpha = std::min(slot.alpha + gain, 1.0f);
            forms[256 + s].pad = slot.alpha;
        }
    }
    const float cs = m_world->cell_size;
    const int32_t cx = int32_t(std::floor(camera.x / cs)), cy = int32_t(std::floor(camera.y / cs));
    m_centre[0] = cx;
    m_centre[1] = cy;
    for (int32_t r = 0; r <= m_cell_radius; ++r) {
        for (int32_t y = cy - r; y <= cy + r; ++y) {
            for (int32_t x = cx - r; x <= cx + r; ++x) {
                if (std::max(std::abs(x - cx), std::abs(y - cy)) == r) {
                    if (const int32_t c = lookup(x, y); c >= 0) {
                        request(c);
                    }
                }
            }
        }
    }
}

void GrassField::collect(const render::View& view) {
    m_runs.clear();
    if (!m_forms_ready) {
        return;
    }
    const auto& w = *m_world;
    const auto& meshes = m_renderer.meshes();
    for (uint32_t s = 0; s < k_slots; ++s) {
        const Slot& slot = m_slots[s];
        if (!slot.ready || slot.groups.empty()) {
            continue;
        }
        const auto& c = w.cells[size_t(slot.cell)];
        if (std::max(std::abs(c.x - m_centre[0]), std::abs(c.y - m_centre[1])) > m_cell_radius ||
            view.frustum.aabb(slot.lo - view.position, slot.hi - view.position) == render::Cull::outside) {
            continue;
        }
        for (const Group& g : slot.groups) {
            const render::MeshId mesh = m_meshes[g.form];
            if (mesh != render::k_invalid_mesh && meshes.ready(mesh) && !meshes.groups(mesh).empty()) {
                m_runs.push_back({meshes.groups(mesh)[0], s * k_slot_instances + g.first, g.count});
            }
        }
    }
}

void GrassField::fill(render::SpaceView& out) const {
    out.grass_instances = m_instances.address;
    out.grass_forms = m_forms.address;
    out.grass_runs = m_runs;
}

} // namespace spud::engine
