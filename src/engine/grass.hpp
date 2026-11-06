#pragma once

// Grass, placed as the game places it: every cell near the camera generates its blades on a
// worker from the land, with the game's generator seeded by the cell, into a slot of one instance buffer.

#include "formats/osw.hpp"
#include "render/renderer.hpp"
#include "render/view.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace spud::engine {

// The game's instance record: 16 half floats (6), the last two holding the form and the 12-cell block.
struct GrassInstance {
    uint16_t half[14];
    uint32_t packed;
};
static_assert(sizeof(GrassInstance) == 32);

// One cell's blades, in the game's generation order within each form. Bit-exact: the regression target is the capture
std::vector<GrassInstance> generate_grass(const osw::World& world, const osw::Cell& cell);

class GrassField {
public:
    GrassField(render::Renderer& renderer, std::shared_ptr<const osw::World> world);
    ~GrassField();
    GrassField(const GrassField&) = delete;
    GrassField& operator=(const GrassField&) = delete;

    // `fade_seconds`: elapsed play for the fade-in of new cells; below 0, a scripted run's: they stand at once.
    void update(const glm::vec3& camera, float fade_seconds, int cell_radius);
    void collect(const render::View& view);
    void fill(render::SpaceView& out) const;
    size_t busy() const { return m_in_flight; }

private:
    static constexpr uint32_t k_slots = 64; // a radius of 3 is 49 cells
    static constexpr uint32_t k_slot_instances = 49152;
    struct Group {
        uint32_t form;
        uint32_t first; // in the slot
        uint32_t count;
    };
    struct Slot {
        int32_t cell = -1; // index into World::cells
        uint32_t generation = 0;
        bool ready = false;
        float alpha = 0.0f; // the fade-in
        std::vector<Group> groups;
        glm::vec3 lo{0.0f}, hi{0.0f};
    };
    void request(int32_t cell);
    int32_t lookup(int32_t x, int32_t y) const;
    void build_forms();

    render::Renderer& m_renderer;
    std::shared_ptr<const osw::World> m_world;
    render::Buffer m_instances;
    render::Buffer m_forms;
    std::vector<render::MeshId> m_meshes; // per form
    bool m_forms_ready = false;
    std::vector<Slot> m_slots;
    std::vector<int32_t> m_cell_lookup;
    int32_t m_lookup_min[2] = {0, 0};
    int32_t m_lookup_size[2] = {0, 0};
    int32_t m_centre[2] = {INT32_MIN, INT32_MIN};
    int m_cell_radius = 2;
    size_t m_in_flight = 0;
    std::vector<render::GrassRun> m_runs;
};

} // namespace spud::engine
