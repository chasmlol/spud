#include "engine/world_map_scene.hpp"

#include "core/units.hpp"

#include <charconv>
#include <filesystem>

namespace spud::engine {

namespace {
constexpr uint32_t k_instances = 512;
constexpr const char* k_cloud_bank = "meshes/sky/skyrimworldmapcloudbank.osm"; // WRLD MODL's default

// <world>.<level>.<x>.<y>.osm: the block's cell.
bool block_cell(const std::string& name, const std::string& world, std::string_view level, int& x, int& y) {
    const std::string head = world + "." + std::string(level) + ".";
    if (!name.starts_with(head)) {
        return false;
    }
    const char* p = name.data() + head.size();
    const char* end = name.data() + name.size();
    const auto a = std::from_chars(p, end, x);
    if (a.ec != std::errc{} || a.ptr == end || *a.ptr != '.') {
        return false;
    }
    return std::from_chars(a.ptr + 1, end, y).ec == std::errc{};
}
} // namespace

WorldMapScene::WorldMapScene(render::Renderer& renderer, std::string cache_root)
    : m_renderer(renderer), m_cache_root(std::move(cache_root)), m_instances(renderer.device(), k_instances) {}

WorldMapScene::~WorldMapScene() {
    close();
    m_renderer.drain(); // deferred frees refer to the instance table
}

void WorldMapScene::open(const std::string& world) {
    close();
    namespace fs = std::filesystem;
    const float cell = units::to_metres(4096.0f);
    const auto add = [&](const std::string& path, const glm::vec3& origin) {
        m_models.push_back({m_renderer.meshes().acquire(path, 1.0f), origin});
    };
    std::error_code ec;
    const std::string land = "meshes/terrain/" + world;
    for (const auto& e : fs::directory_iterator(fs::path(m_cache_root) / land, ec)) {
        const std::string name = e.path().filename().string();
        int x = 0, y = 0;
        if (e.path().extension() == ".osm" && block_cell(name, world, "32", x, y)) {
            add(land + "/" + name, glm::vec3(float(x) * cell, float(y) * cell, 0.0f)); // the land is block relative
        }
    }
    for (const auto& e : fs::directory_iterator(fs::path(m_cache_root) / land / "objects", ec)) {
        int x = 0, y = 0;
        if (e.path().extension() == ".osm" && block_cell(e.path().filename().string(), world, "16", x, y)) {
            add(land + "/objects/" + e.path().filename().string(), glm::vec3(0.0f));
        }
    }
    if (!m_models.empty()) {
        add(k_cloud_bank, glm::vec3(0.0f));
    }
}

void WorldMapScene::close() {
    if (m_models.empty()) {
        return;
    }
    std::vector<uint32_t> slots;
    for (const Model& m : m_models) {
        m_renderer.meshes().release(m.mesh);
        if (m.slot != ~0u) {
            slots.push_back(m.slot);
        }
    }
    m_models.clear();
    m_draws.clear();
    m_renderer.device().defer([this, slots = std::move(slots)] {
        for (uint32_t s : slots) {
            m_instances.free(s);
        }
    });
}

void WorldMapScene::apply(render::SpaceView& space, const render::View& view) {
    m_draws.clear();
    for (Model& m : m_models) {
        if (m.mesh == render::k_invalid_mesh || !m_renderer.meshes().ready(m.mesh)) {
            continue;
        }
        const glm::vec4 local = m_renderer.meshes().sphere(m.mesh);
        const glm::vec3 centre = glm::vec3(local) + m.origin;
        if (m.slot == ~0u) {
            m.slot = m_instances.allocate();
            if (m.slot == ~0u) {
                continue;
            }
            render::GpuInstanceData g{};
            g.rows[0] = glm::vec4(1, 0, 0, m.origin.x);
            g.rows[1] = glm::vec4(0, 1, 0, m.origin.y);
            g.rows[2] = glm::vec4(0, 0, 1, m.origin.z);
            g.sphere = glm::vec4(centre, local.w);
            m_instances.write(m.slot, g);
        }
        const glm::vec3 rel = centre - view.position;
        m_draws.add(m.mesh, m.slot, glm::dot(rel, rel));
    }
    space.draws = &m_draws;
    space.instance_table = m_instances.address();
    space.terrain = nullptr;
    space.far_trees = 0;
    space.far_tree_table = 0;
    space.far_tree_runs = {};
    space.grass_runs = {};
    space.particles = nullptr;
    space.lights = {};
    space.shadow_lights = {};
    space.sun_shadows = nullptr;
    for (auto& d : space.shadow_draws) {
        d = nullptr;
    }
    for (auto& d : space.shadow_light_draws) {
        d = nullptr;
    }
}

} // namespace spud::engine
