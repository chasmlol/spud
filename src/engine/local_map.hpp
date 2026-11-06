#pragma once

// The Map Menu's local map: while the UI shows it, the loaded cells
// drawn from the game's orthographic camera into an offscreen target the Map movie shows as
// img://Local_Map.
//
// As the game (LocalMapMenu slot 7 ->): only the attached cells' land and objects, no
// sky, no LOD, on black, into a target half the screen's size (render target 16/17, 960 x 540 at 1920 x 1080,
// measured), then the fog of war and ISLocalMap (shaders/local_map.frag) into the image the movie shows.
// The objects are unlit, their diffuse alone; the land keeps its shading and the water is the LOD technique.

#include "engine/space.hpp"
#include "render/renderer.hpp"
#include "render/view.hpp"
#include "ui/ui.hpp"

#include "core/binary.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace spud::engine {

// What the player has seen of each place (the cells' SeenData, CHANGE_CELL_SEENDATA in the game's saves): a 16 x 16
// grid of points 256 units apart in every cell. `place` is the worldspace's FormID, or an interior cell's.
class SeenData {
public:
    // As the player moves ( from the game). Skyrim units.
    void see(uint32_t place, bool interior, float x, float y);
    // How many of the four grid points around a position are seen, 0 to 4.
    int level(uint32_t place, bool interior, float x, float y) const;
    uint32_t changes() const { return m_changes; }
    void clear();
    void save(BinaryWriter& out) const;
    void load(BinaryReader& in);

private:
    using Key = std::tuple<uint32_t, int32_t, int32_t>; // place, the grid's cell
    using Bits = std::array<uint32_t, 8>;
    bool seen(uint32_t place, int32_t x, int32_t y) const;
    void see_cell(uint32_t place, int32_t cell_x, int32_t cell_y, float origin_x, float origin_y, float x, float y,
                  bool interior);

    std::map<Key, Bits> m_cells;
    uint32_t m_changes = 0;
    uint32_t m_last_place = 0;
    float m_last[2] = {0, 0};
};

class LocalMap {
public:
    // Makes the target (at start-up: no device memory is allocated later) and registers it with the UI.
    LocalMap(render::Renderer& renderer, ui::Ui& ui);

    // Each frame: what the player sees now, then the local map's view when the UI shows it, else nothing. `player` in
    // metres, `feet` the player's in units, `place` as SeenData's.
    std::span<const render::OffscreenView> frame(const ui::Ui& ui, Space* space,
                                                 const render::RenderSettings& rs, const glm::vec3& player,
                                                 const glm::vec3& feet, uint32_t place, int grids_to_load,
                                                 const render::WaterFrame* water);

    SeenData& seen() { return m_seen; }

    // The attached cells around a position (Skyrim units): the uGridsToLoad square around its cell.
    static void attached_cells(const glm::vec3& units, float cell_units, int grids_to_load, int32_t& x0,
                               int32_t& y0, int32_t& x1, int32_t& y1);

private:
    // ISLocalMap's constants: the camera and the fog of war's vertices over what is attached.
    void build_fog(const ui::LocalMapView& view, uint32_t place, bool interior, int32_t x0, int32_t y0, int32_t x1,
                   int32_t y1);

    render::Renderer& m_renderer;
    SeenData m_seen;
    uint32_t m_scene = 0, m_target = 0; // render targets 17 and 16
    std::vector<std::byte> m_fog;
    std::tuple<uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t> m_fog_key{};
    render::View m_view;
    render::DrawList m_draws;
    render::OffscreenView m_offscreen;
};

} // namespace spud::engine
