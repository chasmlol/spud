#include "engine/local_map.hpp"

#include "core/units.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace spud::engine {

namespace {

constexpr float k_cell = 4096.0f;
constexpr float k_grid = 256.0f;            // 4096 x 0.0625
constexpr float k_seen_radius = 1024.0f;    // fSeenDataUpdateRadius
constexpr int k_cell_full = 248;            // of the 256 points: an exterior cell counts as seen whole
constexpr int k_exterior_vertices = 16;
constexpr int k_interior_vertices = 32;
constexpr float k_interior_offset = 2048.0f; // an interior's grid cells stand half a cell off

} // namespace

void SeenData::see(uint32_t place, bool interior, float x, float y) {
    // the position falls to the grid; nothing more happens until that changes.
    x = std::floor(x / k_grid) * k_grid;
    y = std::floor(y / k_grid) * k_grid;
    if (place == m_last_place && x == m_last[0] && y == m_last[1]) {
        return;
    }
    m_last_place = place;
    m_last[0] = x;
    m_last[1] = y;
    const float offset = interior ? k_interior_offset : 0.0f;
    const int32_t cx = int32_t(std::floor((x - offset) / k_cell)), cy = int32_t(std::floor((y - offset) / k_cell));
    const float ox = float(cx) * k_cell + offset, oy = float(cy) * k_cell + offset;
    // The neighbours in reach (). An interior's test for the upper side compares with the cell's
    // corner plus 0x1000800, so that side is never taken.
    const float reach_up = interior ? 16779264.0f - offset : k_cell;
    const int32_t dx = x - k_seen_radius > ox ? (x + k_seen_radius >= ox + reach_up ? 1 : 0) : -1;
    const int32_t dy = y - k_seen_radius > oy ? (y + k_seen_radius >= oy + reach_up ? 1 : 0) : -1;
    see_cell(place, cx, cy, ox, oy, x, y, interior);
    if (dx) {
        see_cell(place, cx + dx, cy, ox + float(dx) * k_cell, oy, x, y, interior);
    }
    if (dy) {
        see_cell(place, cx, cy + dy, ox, oy + float(dy) * k_cell, x, y, interior);
    }
    if (dx && dy) {
        see_cell(place, cx + dx, cy + dy, ox + float(dx) * k_cell, oy + float(dy) * k_cell, x, y, interior);
    }
}

void SeenData::see_cell(uint32_t place, int32_t cell_x, int32_t cell_y, float origin_x, float origin_y, float x,
                        float y, bool interior) {
    Bits& bits = m_cells[{place, cell_x, cell_y}];
    int count = 0;
    bool changed = false;
    for (int i = 0; i < 256; ++i) {
        uint32_t& word = bits[size_t(i >> 5)];
        const uint32_t bit = 1u << (i & 31);
        if (!(word & bit)) {
            const float px = origin_x + float(i & 15) * k_grid - x, py = origin_y + float(i >> 4) * k_grid - y;
            if (std::sqrt(px * px + py * py) >= k_seen_radius) {
                continue;
            }
            word |= bit;
            changed = true;
        }
        ++count;
    }
    if (!interior && count >= k_cell_full) {
        bits.fill(~0u);
    }
    m_changes += changed ? 1 : 0;
}

bool SeenData::seen(uint32_t place, int32_t x, int32_t y) const {
    const auto it = m_cells.find({place, x >> 4, y >> 4});
    if (it == m_cells.end()) {
        return false;
    }
    const int i = (x & 15) + (y & 15) * 16;
    return (it->second[size_t(i >> 5)] >> (i & 31)) & 1u;
}

int SeenData::level(uint32_t place, bool interior, float x, float y) const {
    const float offset = interior ? k_interior_offset : 0.0f;
    const int32_t gx = int32_t(std::floor((x - offset) / k_grid)), gy = int32_t(std::floor((y - offset) / k_grid));
    return int(seen(place, gx, gy)) + int(seen(place, gx + 1, gy)) + int(seen(place, gx, gy + 1)) +
           int(seen(place, gx + 1, gy + 1));
}

void SeenData::clear() {
    m_cells.clear();
    m_last_place = 0;
    ++m_changes;
}

void SeenData::save(BinaryWriter& out) const {
    out.write<uint32_t>(uint32_t(m_cells.size()));
    for (const auto& [key, bits] : m_cells) {
        out.write<uint32_t>(std::get<0>(key));
        out.write<int32_t>(std::get<1>(key));
        out.write<int32_t>(std::get<2>(key));
        out.write_span(std::span<const uint32_t>(bits));
    }
}

void SeenData::load(BinaryReader& in) {
    clear();
    const auto n = in.read<uint32_t>();
    for (uint32_t i = 0; i < n; ++i) {
        const auto place = in.read<uint32_t>();
        const auto x = in.read<int32_t>();
        const auto y = in.read<int32_t>();
        Bits& bits = m_cells[{place, x, y}];
        for (uint32_t& word : bits) {
            word = in.read<uint32_t>();
        }
    }
}

LocalMap::LocalMap(render::Renderer& renderer, ui::Ui& ui) : m_renderer(renderer) {
    // Render targets 16 and 17 (the local map and its scene) are half the screen: 960 x 540 at 1920 x 1080
    // (measured, the game in run images1, DXGI_FORMAT_R8G8B8A8_UNORM).
    const VkExtent2D e = renderer.render_extent();
    const VkExtent2D size{std::max(1u, e.width / 2), std::max(1u, e.height / 2)};
    m_scene = renderer.create_offscreen_target(size, "local map");
    m_target = renderer.create_offscreen_target(size, "local map");
    renderer.expose_offscreen_color(m_scene, render::Bindless::k_local_map_slot);
    ui.set_render_target_image(ui::k_local_map_image, renderer.offscreen_target_view(m_target));
}

void LocalMap::build_fog(const ui::LocalMapView& view, uint32_t place, bool interior, int32_t x0, int32_t y0,
                         int32_t x1, int32_t y1) {
    struct Header {
        float centre_right[4], up_half[4], grid[4];
        uint32_t size[4];
    };
    const float s = std::sin(view.north_rotation), c = std::cos(view.north_rotation);
    Header h{{view.position[0], view.position[1], c, -s},
             {s, c, view.half_width, view.half_height},
             {float(x0) * k_cell, float(y0) * k_cell, k_cell, k_cell},
             {uint32_t(x1 - x0 + 1), uint32_t(y1 - y0 + 1), k_exterior_vertices, 0}};
    if (interior) {
        // One mesh over the loaded area.
        h.grid[0] = view.area_min[0];
        h.grid[1] = view.area_min[1];
        h.grid[2] = view.area_max[0] - view.area_min[0];
        h.grid[3] = view.area_max[1] - view.area_min[1];
        h.size[0] = h.size[1] = 1;
        h.size[2] = k_interior_vertices;
    }
    const auto key = std::make_tuple(m_seen.changes(), place, x0, y0, x1, y1);
    const uint32_t n = h.size[2];
    if (key != m_fog_key || m_fog.empty()) {
        m_fog_key = key;
        m_fog.assign(sizeof(Header) + (size_t(h.size[0]) * h.size[1] * n * n + 3) / 4 * 4, std::byte{0});
        std::byte* levels = m_fog.data() + sizeof(Header);
        for (uint32_t cy = 0; cy < h.size[1]; ++cy) {
            for (uint32_t cx = 0; cx < h.size[0]; ++cx) {
                for (uint32_t j = 0; j < n; ++j) {
                    for (uint32_t i = 0; i < n; ++i) {
                        const float x = h.grid[0] + (float(cx) + float(i) / float(n - 1)) * h.grid[2];
                        const float y = h.grid[1] + (float(cy) + float(j) / float(n - 1)) * h.grid[3];
                        *levels++ = std::byte(m_seen.level(place, interior, x, y));
                    }
                }
            }
        }
    }
    std::memcpy(m_fog.data(), &h, sizeof(Header)); // the camera moves with no change to the fog
}

void LocalMap::attached_cells(const glm::vec3& units, float cell_units, int grids_to_load, int32_t& x0,
                              int32_t& y0, int32_t& x1, int32_t& y1) {
    const int32_t cx = int32_t(std::floor(units.x / cell_units)),
                  cy = int32_t(std::floor(units.y / cell_units));
    const int32_t half = std::max(grids_to_load, 1) / 2;
    x0 = cx - half;
    x1 = cx + half;
    y0 = cy - half;
    y1 = cy + half;
}

std::span<const render::OffscreenView> LocalMap::frame(const ui::Ui& ui, Space* space,
                                                       const render::RenderSettings& rs,
                                                       const glm::vec3& player, const glm::vec3& feet,
                                                       uint32_t place, int grids_to_load,
                                                       const render::WaterFrame* water) {
    if (space) {
        m_seen.see(place, space->interior(), feet.x, feet.y);
    }
    const auto lm = ui.local_map_view();
    if (!lm || !space) {
        return {};
    }
    const float k = units::k_skyrim_unit_m;
    const glm::vec3 position(lm->position[0] * k, lm->position[1] * k, lm->position[2] * k);
    // The game's NiCamera (): looking straight down, the screen's up (sin r, cos r,
    // 0) and its right (cos r, -sin r, 0) for the north rotation r (measured at r = 0: world rotation rows
    // (0,0,1), (0,1,0), (-1,0,0), run images1).
    const float s = std::sin(lm->north_rotation), c = std::cos(lm->north_rotation);
    m_view = render::View::make_ortho(position, glm::vec3(c, -s, 0), glm::vec3(s, c, 0), lm->half_width * k,
                                      lm->half_height * k, lm->near_units * k, lm->far_units * k);
    // What the game draws: the attached cells, the uGridsToLoad square around the player's cell.
    int32_t x0, y0, x1, y1;
    attached_cells(player / k, space->cell_size() / k, grids_to_load, x0, y0, x1, y1);
    space->build_area_draws(m_view, x0, y0, x1, y1, m_draws);
    m_offscreen.view = &m_view;
    m_offscreen.space = space->space_view();
    m_offscreen.space.draws = &m_draws;
    // In an interior the game turns the water system off for this pass.
    m_offscreen.space.water = space->interior() ? nullptr : water;
    // No fog in the local map: the ground shows unfogged from 40000 units up in the game's picture.
    m_offscreen.space.environment.fog_max = 0.0f;
    m_offscreen.settings = rs;
    // A worldspace with flag 0x40 has its land's cell nodes left out.
    m_offscreen.settings.draw_terrain = rs.draw_terrain && lm->draw_land;
    m_offscreen.clear = glm::vec4(0, 0, 0, 1); // The game(0, 0, 0, 1) in
    m_offscreen.target = m_scene;
    build_fog(*lm, place, space->interior(), x0, y0, x1, y1);
    m_offscreen.local_map = m_fog;
    m_offscreen.local_map_target = m_target;
    return {&m_offscreen, 1};
}

} // namespace spud::engine
