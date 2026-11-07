#pragma once

// Water for one frame: which surfaces are drawn, how, and the game's material constants. The
// renderer's side is render/renderer_water.cpp.

#include "formats/osa.hpp"
#include "formats/osw.hpp"
#include "render/gpu_types.hpp"
#include "render/view.hpp"

#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud {

class WaterSystem {
public:
    using TextureResolver = std::function<uint32_t(std::string_view path)>;

    WaterSystem(const osa::Atmosphere& atmosphere, TextureResolver textures);

    // Once a frame, after the sky: `grids` is uGridsToLoad. `seconds` is the water's clock.
    void update(const osw::World& world, const render::View& view, const render::Environment& env,
                const glm::vec3& water_multiplier, float sun_value, int grids, float seconds, float game_seconds,
                float dt, const glm::vec3& player, bool wading);

    // a mark where something met the water, units; ignored past the wading window.
    void add_ripple(const glm::vec2& at);
    // A moving actor in the water leaves a segment; `id` keeps its last place.
    void wade(uint32_t id, const glm::vec3& feet);
    // The lights nearest the camera, for the surface's light pass.
    void set_lights(std::span<const render::PointLight> lights, const glm::vec3& camera);

    const render::WaterFrame* frame() const {
        return m_frame.surfaces.empty() && m_frame.placed.empty() ? nullptr : &m_frame;
    }
    uint32_t image_space() const { return m_image_space; } // the water's INAM while the camera is in it, else 0
    // The centres of the near cells' water, 5 units up: where rain's ripple mesh goes.
    std::span<const glm::vec3> ripple_places() const { return m_ripple_places; }

private:
    std::vector<glm::vec4> m_stamps; // queued segments, units
    std::unordered_map<uint32_t, glm::vec2> m_waders;
    struct Cell {
        int32_t x, y;
        float height; // metres
        uint32_t material;
        int32_t flow; // index into World::flow_cells, or -1
    };

    void set_world(const osw::World& world);
    uint32_t material_of(uint32_t form);

    const osa::Atmosphere* m_atm;
    TextureResolver m_textures;
    const osw::World* m_world = nullptr;
    std::string m_world_edid;
    std::vector<Cell> m_cells;
    static uint64_t cell_key(int32_t x, int32_t y) { return uint64_t(uint32_t(x)) << 32 | uint32_t(y); }
    std::unordered_map<uint64_t, float> m_cell_at; // wade's: the height of a cell's water
    std::vector<const osa::Water*> m_forms; // a material per form in use
    std::unordered_map<uint32_t, uint32_t> m_material_of;
    std::vector<glm::uvec4> m_noise;
    std::vector<render::GpuWaterMaterial> m_materials;
    std::vector<render::GpuWaterInstance> m_near, m_flow, m_far, m_surfaces;
    std::vector<uint32_t> m_surface_material; // parallel to World::water_surfaces
    std::vector<render::GpuWaterVertex> m_placed;
    std::vector<glm::vec3> m_ripple_places;
    uint32_t m_flow_normals = 0;
    int32_t m_flow_slots[121]; // the flow cell in each slot of the 11 x 11 atlas, -1 empty
    std::vector<render::WaterFlowUpload> m_flow_uploads;
    render::WaterFrame m_frame;
    uint32_t m_image_space = 0;
    // The ripples: the player's snapped position (4 units), the wader's last, the simulation's clock.
    glm::vec2 m_snapped{0.0f};
    glm::vec2 m_last_wader{0.0f};
    bool m_have_wader = false;
    bool m_ripples_running = false;
    float m_since_ripple = 1e9f;
    float m_step_time = 0.0f;
};

} // namespace spud
