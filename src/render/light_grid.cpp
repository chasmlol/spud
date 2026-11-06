#include "render/light_grid.hpp"

#include "core/units.hpp"

#include <algorithm>
#include <cmath>

namespace spud::render {

namespace {

// a node whose world bound radius is below 150 units gets one list, sorted.
constexpr float k_small_node = units::to_metres(150.0f);
constexpr float k_attach_dimmer = 0.05f; // dimmer below this is never attached

} // namespace

LightGrid::LightGrid(const osw::World& world) : m_world(world) {
    m_cell_size = world.cell_size > 0.0f ? world.cell_size : units::k_cell_size_m;
    m_min[0] = world.cell_min[0];
    m_min[1] = world.cell_min[1];
    m_size[0] = world.cell_max[0] - world.cell_min[0] + 1;
    m_size[1] = world.cell_max[1] - world.cell_min[1] + 1;
    if (world.lights.empty() || m_size[0] <= 0 || m_size[1] <= 0) {
        m_size[0] = m_size[1] = 0;
        return;
    }
    m_cells.resize(size_t(m_size[0]) * m_size[1]);
    for (size_t i = 0; i < world.lights.size() && i < k_no_light; ++i) {
        const auto& l = world.lights[i];
        if ((l.flags & osw::light_initially_disabled) || l.dimmer < k_attach_dimmer) {
            continue;
        }
        const int32_t x0 = std::max(int32_t(std::floor((l.position[0] - l.radius) / m_cell_size)) - m_min[0], 0);
        const int32_t y0 = std::max(int32_t(std::floor((l.position[1] - l.radius) / m_cell_size)) - m_min[1], 0);
        const int32_t x1 =
            std::min(int32_t(std::floor((l.position[0] + l.radius) / m_cell_size)) - m_min[0], m_size[0] - 1);
        const int32_t y1 =
            std::min(int32_t(std::floor((l.position[1] + l.radius) / m_cell_size)) - m_min[1], m_size[1] - 1);
        for (int32_t y = y0; y <= y1; ++y) {
            for (int32_t x = x0; x <= x1; ++x) {
                m_cells[size_t(y) * m_size[0] + x].push_back(static_cast<uint16_t>(i));
            }
        }
    }
}

uint32_t LightGrid::list(const glm::vec3& centre, float radius, bool landscape, uint32_t out[4],
                         std::span<const uint16_t> unlit) const {
    for (uint32_t i = 0; i < 4; ++i) {
        out[i] = 0xFFFFFFFFu;
    }
    if (m_cells.empty()) {
        return 0;
    }
    // The cells the object's sphere touches hold every light that can reach it.
    const int32_t x0 = std::max(int32_t(std::floor((centre.x - radius) / m_cell_size)) - m_min[0], 0);
    const int32_t y0 = std::max(int32_t(std::floor((centre.y - radius) / m_cell_size)) - m_min[1], 0);
    const int32_t x1 = std::min(int32_t(std::floor((centre.x + radius) / m_cell_size)) - m_min[0], m_size[0] - 1);
    const int32_t y1 = std::min(int32_t(std::floor((centre.y + radius) / m_cell_size)) - m_min[1], m_size[1] - 1);
    thread_local std::vector<Candidate> found; // the streaming thread, and the frame for what lands in play
    found.clear();
    for (int32_t y = y0; y <= y1; ++y) {
        for (int32_t x = x0; x <= x1; ++x) {
            for (uint16_t i : m_cells[size_t(y) * m_size[0] + x]) {
                const auto& l = m_world.lights[i];
                if (landscape && (l.flags & osw::light_no_land)) {
                    continue; // landscape only takes lights that affect land
                }
                if (std::binary_search(unlit.begin(), unlit.end(), i)) {
                    continue;
                }
                const float d = glm::length(glm::vec3(l.position[0], l.position[1], l.position[2]) - centre);
                if (d - radius < l.radius) {
                    found.push_back({i, (d - radius) / l.radius});
                }
            }
        }
    }
    // Registration order (the world's light order), without the duplicates of lights spanning several cells.
    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) { return a.light < b.light; });
    found.erase(std::unique(found.begin(), found.end(),
                            [](const Candidate& a, const Candidate& b) { return a.light == b.light; }),
                found.end());
    // Shadow-casting lights come first in a pass; a small node's other
    // lights are sorted by k, largest first. Ties keep registration
    // order, where the game's qsort leaves it unspecified.
    auto shadow = [&](const Candidate& c) { return (m_world.lights[c.light].flags & osw::light_shadow) != 0; };
    const auto rest = std::stable_partition(found.begin(), found.end(), shadow);
    if (radius < k_small_node) {
        std::stable_sort(rest, found.end(), [](const Candidate& a, const Candidate& b) { return a.k > b.k; });
    }
    const size_t n = std::min<size_t>(found.size() + k_moving_lights, k_light_list_size);
    for (size_t i = 0; i < n; ++i) {
        uint32_t& word = out[i / 2];
        const uint32_t shift = (i & 1) * 16;
        const size_t light = i < found.size() ? found[i].light : m_world.lights.size() + (i - found.size());
        word = (word & ~(0xFFFFu << shift)) | (uint32_t(light) << shift);
    }
    return static_cast<uint32_t>(found.size());
}

} // namespace spud::render
