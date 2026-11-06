#pragma once

// Terrain and water for one space (worldspace), always for the WHOLE world:
//  - a full-resolution (33x33) ring around the camera in a fixed slot pool, built on demand on workers, drawn with
//    the game's landscape shader. Quadrants are separate vertex grids (their layer sets differ) with skirts on the
//    cell's outer edges;
// - everything else as continuous LOD: a quadtree over the resident heightfield, one
//    shared 17 x 17 grid per node, chosen by screen-space error and morphing into its parent by camera distance,
//    drawn with the macro colour and normal maps the importer bakes.

#include "render/device.hpp"
#include "render/frustum.hpp"
#include "render/gpu_types.hpp"
#include "render/jobs.hpp"
#include "render/textures.hpp"
#include "render/uploader.hpp"

#include "formats/osh.hpp"
#include "formats/osw.hpp"

#include <memory>
#include <vector>

namespace spud::render {

struct TerrainSettings {
    float full_res_radius = 12.0f; // cells around the camera at 33x33
    float pixel_error = 1.0f;      // the coarsest LOD whose error stays under this many pixels (ARCHITECTURE.md, LOD)
    float pixels_per_radian = 1131.0f; // set per frame from the view
};

struct TerrainStats {
    uint32_t land_cells = 0;
    uint32_t near_ready = 0;
    uint32_t near_pending = 0;
    uint32_t near_capacity = 0;
    uint32_t drawn_near = 0; // in the last collect
    uint32_t drawn_far = 0;
};

// Visible terrain, roughly front to back: full-resolution cells (first vertex of each) and far nodes, each drawn as
// one instanced draw with vertex pulling, instead of one indirect command per cell (which made the GPU front end the
// bottleneck with ~11k tiny draws).
struct ShadowCascade;

struct TerrainDraws {
    std::vector<uint32_t> near;
    std::vector<GpuFarNode> far;
    void clear() {
        near.clear();
        far.clear();
    }
    size_t total() const { return near.size() + far.size(); }
    size_t bytes() const { return near.size() * sizeof(uint32_t) + far.size() * sizeof(GpuFarNode) + 32; }
};

// A view's land in a frame's arena.
struct TerrainFrame {
    VkDeviceAddress near = 0;
    VkDeviceAddress far = 0;
    uint32_t near_count = 0;
    uint32_t far_count = 0;
};

class Terrain {
public:
    static constexpr int k_max_full_res_radius = 24;

    // Any thread: the tables, buffers and the heightfield. finish_textures() completes it. `far` is the importer's
    // far view of the world, null without one: the land then draws without its macro maps.
    Terrain(Device& device, Uploader& uploader, JobSystem& jobs, TextureManager& textures,
            std::shared_ptr<const osw::World> world, std::shared_ptr<const osh::FarView> far);
    ~Terrain();
    // Render thread, before the terrain is drawn: acquires the land textures.
    void finish_textures();
    Terrain(const Terrain&) = delete;
    Terrain& operator=(const Terrain&) = delete;

    // Render thread, once per frame: maintains the full-resolution ring.
    void update(const glm::vec3& camera, const TerrainSettings& settings);
    // Visible land. `camera` is in this space's coordinates.
    void collect(const Frustum& frustum, const glm::vec3& camera, const TerrainSettings& settings,
                 TerrainDraws& out);
    // The land a sun cascade reaches, for the god-ray map.
    void collect_cascade(const ShadowCascade& cascade, const glm::vec3& camera, const TerrainSettings& settings,
                         TerrainDraws& out) const;
    // The bounds (absolute) of the land cells in the cell rectangle [x0, x1] x [y0, y1]; false if it has none.
    bool area_bounds(int32_t x0, int32_t y0, int32_t x1, int32_t y1, glm::vec3& lo, glm::vec3& hi) const;
    uint32_t near_index_count() const { return m_near_index_count; }
    uint32_t far_first_index() const { return m_near_index_count; }
    uint32_t far_index_count() const { return m_far_index_count; }
    // The macro maps and where they lie, for GpuFrame.
    void far_params(const TerrainSettings& settings, GpuFarLand& out) const;

    VkDeviceAddress vertex_address() const { return m_vertices.address; }
    VkDeviceAddress heights_address() const { return m_heights.address; }
    VkBuffer index_buffer() const { return m_indices.buffer; }
    VkDeviceAddress quads_address() const { return m_quads.address; }
    VkDeviceAddress land_textures_address() const { return m_land_textures.address; }

    size_t busy() const { return m_pending + m_near_in_flight + (m_near_wanted_unscheduled ? 1 : 0); }
    TerrainStats stats() const;

private:
    enum class NearState : uint8_t { none, pending, ready, abandoned };

    struct LandCell {
        int32_t x, y;
        uint32_t land;
        glm::vec3 lo{0}, hi{0}; // AABB, absolute
        NearState near = NearState::none;
        uint32_t near_slot = ~0u;
    };
    // A quadtree node: 2^level cells square. Bounds and error cover the land under it.
    struct Node {
        float zmin = 1e30f;
        float zmax = -1e30f;
        float error = 0.0f; // metres: the most the node's grid strays from the full heightfield
        bool land = false;  // some cell under it has land
        bool full = false;  // all of them have: it can be drawn as one piece
    };
    struct Level {
        int32_t width = 0, height = 0; // nodes
        std::vector<Node> nodes;
        Node& at(int32_t x, int32_t y) { return nodes[size_t(y) * width + x]; }
        const Node& at(int32_t x, int32_t y) const { return nodes[size_t(y) * width + x]; }
    };
    struct FarWalk {
        const Frustum& frustum;
        glm::vec3 camera;
        glm::vec2 camera_cells;
        float ring;        // cells: what the full-resolution ring may reach
        float range_scale; // metres of LOD range per metre of error
        TerrainDraws& out;
    };

    void build_index_templates();
    void build_tables();
    void build_heightfield();
    void request_near(uint32_t cell, float priority);
    void release_near(uint32_t cell);
    float node_range(int level, const Node& node, float range_scale) const;
    void far_node(FarWalk& walk, int level, int32_t x, int32_t y, float parent_range);
    void add_far_node(TerrainDraws& out, int level, int32_t x, int32_t y, float parent_range) const;

    Device& m_device;
    Uploader& m_uploader;
    JobSystem& m_jobs;
    TextureManager& m_textures;
    std::shared_ptr<const osw::World> m_world;
    float m_cell_size = 0;

    std::vector<LandCell> m_cells;
    std::vector<int32_t> m_grid; // land-cell index by (x - min, y - min), -1 = none
    int32_t m_grid_min[2] = {0, 0};
    int32_t m_grid_size[2] = {0, 0};

    std::shared_ptr<const osh::FarView> m_far;
    TextureId m_far_colour = k_invalid_slot;
    TextureId m_far_normal = k_invalid_slot;
    TextureId m_far_noise = k_invalid_slot;

    // The heightfield: every land vertex of the world, 33 per cell sharing their edges, in the game's 8-unit steps.
    int32_t m_height_width = 0;
    int32_t m_height_rows = 0;
    float m_height_bias = 0.0f; // metres of sample value 0
    std::vector<Level> m_levels;

    Buffer m_vertices;
    Buffer m_heights;
    Buffer m_indices;
    Buffer m_quads;
    Buffer m_land_textures;
    std::vector<TextureId> m_acquired;

    uint32_t m_near_index_count = 0;
    uint32_t m_far_index_count = 0;
    uint32_t m_near_capacity = 0;
    std::vector<uint32_t> m_free_slots;

    size_t m_pending = 0;
    bool m_heights_ready = false;
    size_t m_near_in_flight = 0;
    bool m_near_wanted_unscheduled = false;
    int32_t m_last_cam_cell[2] = {INT32_MIN, INT32_MIN};
    float m_last_radius = -1;
    glm::vec2 m_plan_pos{1e30f}; // camera (in cells) the full-res ring was planned from
    std::vector<uint32_t> m_near_cells; // cells with near != none
    std::vector<std::pair<float, uint32_t>> m_scratch;
    uint32_t m_drawn_near = 0;
    uint32_t m_drawn_far = 0;
};

} // namespace spud::render
