// The world beyond the loaded cells: which squares of cells stand at full detail, region
// proxies where they don't, and tree impostors.

#include "engine/space.hpp"

#include "core/log.hpp"
#include "render/renderer.hpp"

#include <algorithm>
#include <cmath>

namespace spud::engine {

namespace {

constexpr int32_t k_square = 4;         // cells: a level-1 region's side
// Cells: a square is at full detail while its centre is this much nearer than the draw distance would have a cell.
constexpr float k_square_margin = 1.0f;
constexpr uint32_t k_tree_block = 8;    // tree cells culled together
constexpr float k_acquire_margin = 1.3f; // of the level-2 range: level 1 proxies stream in this far out
constexpr float k_release_margin = 1.6f;

float box_distance(const glm::vec3& p, const glm::vec3& lo, const glm::vec3& hi) {
    return glm::length(p - glm::clamp(p, lo, hi));
}

} // namespace

void Space::far_init() {
    const osh::FarView& far = *m_far;
    m_far_grid_min[0] = far.cell_min[0];
    m_far_grid_min[1] = far.cell_min[1];
    m_far_squares[0] = (far.cell_max[0] - far.cell_min[0]) / k_square + 1;
    m_far_squares[1] = (far.cell_max[1] - far.cell_min[1]) / k_square + 1;
    m_far_full.assign(size_t(m_far_squares[0]) * m_far_squares[1], 0);
    m_far_proxies.assign(far.proxies.size(), {});
    for (uint32_t i = 0; i < far.regions.size(); ++i) {
        const osh::Region& r = far.regions[i];
        if (r.level == 2) {
            m_far_top.push_back(i);
        } else if (r.parent == osh::k_none) {
            m_far_orphans.push_back(i);
        }
    }
    m_far_children.resize(far.regions.size());
    for (uint32_t i = 0; i < far.regions.size(); ++i) {
        if (far.regions[i].parent != osh::k_none) {
            m_far_children[far.regions[i].parent].push_back(i);
        }
    }
    // Trees in blocks of consecutive cells; a cell's bound holds its trees' crowns.
    m_far_tree_bounds.resize(far.tree_cells.size());
    for (uint32_t c = 0; c < far.tree_cells.size(); ++c) {
        const osh::TreeCell& tc = far.tree_cells[c];
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (uint32_t i = tc.first; i < tc.first + tc.count; ++i) {
            const osh::TreeInstance& t = far.trees[i];
            m_far_tree_forms.insert(t.form_id);
            const glm::vec3 p(t.position[0], t.position[1], t.position[2]);
            const float r = far.impostors[t.impostor].radius * t.scale * 2.0f;
            lo = glm::min(lo, p - r);
            hi = glm::max(hi, p + r);
        }
        m_far_tree_bounds[c] = {lo, hi};
    }
    for (uint32_t b = 0; b < far.tree_cells.size(); b += k_tree_block) {
        TreeBlock block{glm::vec3(1e30f), glm::vec3(-1e30f), b, std::min<uint32_t>(b + k_tree_block, uint32_t(far.tree_cells.size()))};
        for (uint32_t c = block.first; c < block.last; ++c) {
            block.lo = glm::min(block.lo, m_far_tree_bounds[c].first);
            block.hi = glm::max(block.hi, m_far_tree_bounds[c].second);
        }
        m_far_tree_blocks.push_back(block);
    }
    m_far_trees = std::make_unique<render::FarTrees>(m_renderer.device(), m_renderer.uploader(), m_renderer.textures(), m_far);
}

int32_t Space::far_square(int32_t x, int32_t y) const {
    const int32_t gx = x - m_far_grid_min[0], gy = y - m_far_grid_min[1];
    if (gx < 0 || gy < 0 || gx / k_square >= m_far_squares[0] || gy / k_square >= m_far_squares[1]) {
        return -1;
    }
    return (gy / k_square) * m_far_squares[0] + gx / k_square;
}

float Space::stream_distance(int32_t x, int32_t y, const glm::vec2& camera_cells) const {
    if (m_far) {
        const int32_t s = far_square(x, y);
        if (s >= 0) {
            const int32_t sx = s % m_far_squares[0], sy = s / m_far_squares[0];
            const glm::vec2 centre(float(m_far_grid_min[0] + sx * k_square) + k_square * 0.5f,
                                   float(m_far_grid_min[1] + sy * k_square) + k_square * 0.5f);
            return glm::length(centre - camera_cells) - k_square_margin;
        }
    }
    return glm::length(glm::vec2(float(x) + 0.5f, float(y) + 0.5f) - camera_cells);
}

void Space::far_mark_squares(const glm::vec2& camera_cells, float max_distance) {
    for (uint32_t s : m_far_marked) {
        m_far_full[s] = 0;
    }
    m_far_marked.clear();
    const int32_t reach = int32_t(std::ceil(max_distance + k_square_margin)) + k_square;
    const int32_t cx = int32_t(std::floor(camera_cells.x)), cy = int32_t(std::floor(camera_cells.y));
    const int32_t x0 = std::max(cx - reach, m_far_grid_min[0]), y0 = std::max(cy - reach, m_far_grid_min[1]);
    const int32_t x1 = cx + reach, y1 = cy + reach;
    for (int32_t y = y0 - (y0 - m_far_grid_min[1]) % k_square; y <= y1; y += k_square) {
        for (int32_t x = x0 - (x0 - m_far_grid_min[0]) % k_square; x <= x1; x += k_square) {
            const int32_t s = far_square(x, y);
            if (s < 0 || stream_distance(x, y, camera_cells) > max_distance) {
                continue;
            }
            bool settled = true;
            for (int32_t j = 0; j < k_square && settled; ++j) {
                for (int32_t i = 0; i < k_square && settled; ++i) {
                    const int32_t ci = cell_lookup(x + i, y + j);
                    if (ci < 0) {
                        continue;
                    }
                    const int32_t slot = m_slot_of_cell[ci];
                    settled = slot >= 0 && m_slots[slot].waiting == 0;
                }
            }
            if (settled) {
                m_far_full[s] = 1;
                m_far_marked.push_back(uint32_t(s));
            }
        }
    }
}

void Space::far_update(const glm::vec3& camera, const SpaceSettings& settings, const render::TerrainSettings& terrain) {
    const osh::FarView& far = *m_far;
    auto& meshes = m_renderer.meshes();
    m_far_range = 1.5f * settings.pixels_per_radian / std::max(terrain.pixel_error, 0.01f);
    for (const osh::Region& r : far.regions) {
        if (r.level == 2) {
            m_far_range = r.error * settings.pixels_per_radian / std::max(terrain.pixel_error, 0.01f);
            break;
        }
    }
    const bool plan = glm::length(camera - m_far_plan_pos) > 30.0f || m_far_range != m_far_planned_range;
    if (plan) {
        m_far_plan_pos = camera;
        m_far_planned_range = m_far_range;
        for (const osh::Region& r : far.regions) {
            // A level 1 region is drawn whenever its parent splits, however far it is itself; one without a parent
            // always is.
            const osh::Region& decides = r.parent != osh::k_none ? far.regions[r.parent] : r;
            const float d = box_distance(camera, glm::vec3(decides.lo[0], decides.lo[1], decides.lo[2]),
                                         glm::vec3(decides.hi[0], decides.hi[1], decides.hi[2]));
            const bool always = r.level == 2 || r.parent == osh::k_none;
            const bool want = always || d < m_far_range * k_acquire_margin;
            const bool drop = !always && d > m_far_range * k_release_margin;
            for (uint32_t p = r.first_proxy; p < r.first_proxy + r.proxy_count; ++p) {
                FarProxy& proxy = m_far_proxies[p];
                if (want && proxy.mesh == render::k_invalid_mesh) {
                    proxy.mesh = meshes.acquire(far.meshes[far.proxies[p].mesh], d);
                    m_far_pending.push_back(uint64_t(p) << 32 | uint32_t(&r - far.regions.data()));
                } else if (drop && proxy.mesh != render::k_invalid_mesh) {
                    if (proxy.instance != ~0u) {
                        // Frames in flight may still draw from the slot.
                        m_renderer.device().defer([this, slot = proxy.instance] { m_instances.free(slot); });
                    }
                    meshes.release(proxy.mesh);
                    proxy = {};
                }
            }
        }
    }
    size_t keep = 0;
    for (const uint64_t item : m_far_pending) {
        const uint32_t p = uint32_t(item >> 32);
        const osh::Region& r = far.regions[uint32_t(item)];
        FarProxy& proxy = m_far_proxies[p];
        if (proxy.mesh == render::k_invalid_mesh || proxy.instance != ~0u) {
            continue; // released meanwhile
        }
        if (meshes.failed(proxy.mesh)) {
            continue;
        }
        if (!meshes.ready(proxy.mesh)) {
            m_far_pending[keep++] = item;
            continue;
        }
        const uint32_t slot = m_instances.allocate();
        if (slot == ~0u) {
            continue;
        }
        render::GpuInstanceData d;
        for (int i = 0; i < 3; ++i) {
            d.rows[i] = glm::vec4(0.0f);
            d.rows[i][i] = 1.0f;
            d.rows[i].w = r.centre[i];
        }
        const glm::vec3 lo(r.lo[0], r.lo[1], r.lo[2]), hi(r.hi[0], r.hi[1], r.hi[2]);
        d.sphere = glm::vec4((lo + hi) * 0.5f, glm::length(hi - lo) * 0.5f);
        m_instances.write(slot, d);
        proxy.instance = slot;
        const uint32_t directional = far.proxies[p].directional;
        if (directional < m_world->directional_materials.size()) {
            proxy.variant = meshes.directional_variant(proxy.mesh, m_world->directional_materials[directional]);
        }
    }
    m_far_pending.resize(keep);
}

size_t Space::far_busy() const { return m_far_pending.size(); }

void Space::far_draw_region(const render::View& view, const osh::Region& r, render::DrawList& out, float pixels_per_radian) {
    const glm::vec3 lo(r.lo[0], r.lo[1], r.lo[2]), hi(r.hi[0], r.hi[1], r.hi[2]);
    if (view.frustum.aabb(lo - view.position, hi - view.position) == render::Cull::outside) {
        return;
    }
    const float d = box_distance(view.position, lo, hi);
    for (uint32_t p = r.first_proxy; p < r.first_proxy + r.proxy_count; ++p) {
        const FarProxy& proxy = m_far_proxies[p];
        if (proxy.instance != ~0u) {
            // The coarsest level within a pixel from the region's nearest point, as a reference's.
            const auto& errors = m_renderer.meshes().lod_error(proxy.mesh);
            uint8_t lod = 0;
            for (uint8_t l = osm::k_lod_levels; l > 0; --l) {
                if (errors[l - 1] >= 0.0f && errors[l - 1] * pixels_per_radian <= d) {
                    lod = l;
                    break;
                }
            }
            out.add(proxy.mesh, proxy.instance, d * d, proxy.variant, false, lod);
        }
    }
}

void Space::far_draws(const render::View& view, render::DrawList& out, float pixels_per_radian) {
    const osh::FarView& far = *m_far;
    auto full = [&](const osh::Region& r) {
        const int32_t s = far_square(r.x, r.y);
        return s >= 0 && m_far_full[s] != 0;
    };
    for (uint32_t i : m_far_orphans) {
        if (!full(far.regions[i])) {
            far_draw_region(view, far.regions[i], out, pixels_per_radian);
        }
    }
    for (uint32_t i : m_far_top) {
        const osh::Region& top = far.regions[i];
        const glm::vec3 lo(top.lo[0], top.lo[1], top.lo[2]), hi(top.hi[0], top.hi[1], top.hi[2]);
        bool split = box_distance(view.position, lo, hi) < m_far_range;
        for (uint32_t c : m_far_children[i]) {
            split = split || full(far.regions[c]);
        }
        if (!split) {
            far_draw_region(view, top, out, pixels_per_radian);
            continue;
        }
        for (uint32_t c : m_far_children[i]) {
            if (!full(far.regions[c])) {
                far_draw_region(view, far.regions[c], out, pixels_per_radian);
            }
        }
    }
}

void Space::far_tree_runs(const render::View& view) {
    m_far_tree_runs.clear();
    if (!m_far_trees || !m_far_trees->ready()) {
        return;
    }
    const osh::FarView& far = *m_far;
    uint32_t first = 0, count = 0;
    bool limited = false; // the run's cells are loaded: their near trees are real, and its impostors stop short of them
    auto flush = [&] {
        if (count > 0) {
            m_far_tree_runs.push_back({6, count, limited ? 6u : 0u, first});
            count = 0;
        }
    };
    const glm::vec3 cam = view.position;
    for (const TreeBlock& b : m_far_tree_blocks) {
        const render::Cull cull = view.frustum.aabb(b.lo - cam, b.hi - cam);
        if (cull == render::Cull::outside) {
            flush();
            continue;
        }
        for (uint32_t c = b.first; c < b.last; ++c) {
            const osh::TreeCell& tc = far.tree_cells[c];
            const int32_t s = far_square(tc.x, tc.y);
            const auto& [lo, hi] = m_far_tree_bounds[c];
            const bool loaded = s >= 0 && m_far_full[s];
            const glm::vec3 reach = glm::max(glm::abs(lo - cam), glm::abs(hi - cam));
            if ((loaded && (m_tree_near <= 0.0f || glm::dot(reach, reach) < m_tree_near * m_tree_near)) ||
                (cull == render::Cull::intersect && view.frustum.aabb(lo - cam, hi - cam) == render::Cull::outside)) {
                flush();
                continue;
            }
            if (count > 0 && (first + count != tc.first || limited != loaded)) {
                flush();
            }
            if (count == 0) {
                first = tc.first;
                limited = loaded;
            }
            count += tc.count;
        }
    }
    flush();
}

} // namespace spud::engine
