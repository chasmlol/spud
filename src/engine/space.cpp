#include "engine/space.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/trace.hpp"
#include "core/units.hpp"

#include <algorithm>
#include <utility>
#include <cstring>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>

namespace spud::engine {

namespace {

using Clock = std::chrono::steady_clock;

// How far (in cells) the camera may move before the streaming plan is rebuilt. The rebuild scans
// ~700 cells, so doing it often is cheap.
constexpr float k_replan_cells = 0.125f;

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// fFlickeringLightDistance:General (exe default): only lights this close to the player animate.
constexpr float k_flickering_light_distance = units::to_metres(1024.0f);
constexpr float k_two_pi = 6.2831855f;
constexpr float k_room_lighting_transition = 3.0f; // fRoomLightingTransitionDuration
// Close to the camera a bush can show round a plane no wall quite backs in the merged world (metres, squared).
constexpr float k_occlude_far_sq = 30.0f * 30.0f;
// How far over a cell's top a sight line is taken, for the terrain's coarser far levels (metres).
constexpr float k_land_margin = 3.0f;

// The game's 512-entry sine and cosine tables: entries at float steps of 2 pi / 512
// from 0; a lookup of angle a reads entry trunc(a x 512 / 2 pi) & 511.
struct SinCos {
    float sin[512];
    float cos[512];
    SinCos() {
        const float step = k_two_pi / 512.0f;
        float x = 0.0f;
        for (int i = 0; i < 512; ++i) {
            sin[i] = std::sin(x);
            cos[i] = std::cos(x);
            x += step;
        }
    }
};
const SinCos& sin_cos() {
    static const SinCos table;
    return table;
}
constexpr float k_table_scale = 81.487328f; // 512 / 2 pi
// Table entry for a pre-scaled index (the game folds the angle multipliers into the index).
float sin_at(float index) { return sin_cos().sin[static_cast<int32_t>(index) & 511]; }
float cos_at(float index) { return sin_cos().cos[static_cast<int32_t>(index) & 511]; }

// uniform in [lo, hi).
float random_range(std::mt19937& g, float lo, float hi) {
    return lo + (hi - lo) * (float(g() >> 8) * (1.0f / 16777216.0f));
}

} // namespace

std::filesystem::path Space::find_file(const std::filesystem::path& cache_root, const std::string& name) {
    const std::string file = path::to_lower(name) + ".osw";
    std::error_code ec;
    for (const char* dir : {"worlds", "interiors"}) {
        const auto p = cache_root / dir / file;
        if (std::filesystem::exists(p, ec)) {
            return p;
        }
    }
    throw Error("world file not found: " + (cache_root / "worlds" / file).string());
}

std::shared_ptr<osw::World> Space::read_world(const std::filesystem::path& file) {
    auto w = std::make_shared<osw::World>(osw::read(read_file(file)));
    if (w->cell_size <= 0) {
        throw Error("world " + w->edid + " has no cell size");
    }
    return w;
}

namespace {

// The importer's far view of a world; null for worlds without land.
std::shared_ptr<const osh::FarView> read_far_view(const std::filesystem::path& cache_root, const std::string& edid) {
    const auto file = cache_root / "far" / (path::to_lower(edid) + ".osh");
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return nullptr;
    }
    return std::make_shared<osh::FarView>(osh::read(read_file(file)));
}

} // namespace

Space::Space(render::Renderer& renderer, const std::filesystem::path& cache_root, const std::string& edid)
    : m_renderer(renderer), m_instances(renderer.device(), 1u << 20) {
    const auto start = Clock::now();
    m_world = read_world(find_file(cache_root, edid));
    log::info("space {}: read in {:.0f} ms", m_world->edid, ms_since(start));
    init(true);
}

Space::Space(render::Renderer& renderer, std::shared_ptr<osw::World> world, uint32_t spare)
    : m_renderer(renderer), m_world(std::move(world)),
      m_instances(renderer.device(), uint32_t(m_world->refs.size()) + spare) {
    init(false);
}

void Space::finish() {
    m_terrain->finish_textures();
    if (m_far_trees) {
        m_far_trees->finish_textures();
    }
}

void Space::init(bool render_thread) {
    const auto& w = *m_world;
    log::info("space {}: \"{}\", {} cells, {} lands, {} refs, {} meshes, {} land textures, {} load doors{}", w.edid,
              w.name, w.cells.size(), w.lands.size(), w.refs.size(), w.meshes.size(), w.land_textures.size(),
              w.doors.size(),
              w.rooms.empty() ? std::string() : std::format(", {} rooms, {} portals", w.rooms.size(), w.portals.size()));
    if (!w.occluders.empty()) {
        m_occluders = std::make_unique<Occluders>(w);
    }
    if (!w.rooms.empty() && interior()) { // exteriors have no portal graph
        m_rooms = std::make_unique<Rooms>(w);
    }

    int32_t lo[2] = {INT32_MAX, INT32_MAX}, hi[2] = {INT32_MIN, INT32_MIN};
    for (const auto& c : w.cells) {
        lo[0] = std::min(lo[0], c.x);
        lo[1] = std::min(lo[1], c.y);
        hi[0] = std::max(hi[0], c.x);
        hi[1] = std::max(hi[1], c.y);
    }
    if (!w.cells.empty()) {
        m_grid_min[0] = lo[0];
        m_grid_min[1] = lo[1];
        m_grid_size[0] = hi[0] - lo[0] + 1;
        m_grid_size[1] = hi[1] - lo[1] + 1;
        m_grid.assign(size_t(m_grid_size[0]) * m_grid_size[1], -1);
        for (uint32_t i = 0; i < w.cells.size(); ++i) {
            const auto& c = w.cells[i];
            m_grid[size_t(c.y - lo[1]) * m_grid_size[0] + (c.x - lo[0])] = static_cast<int32_t>(i);
        }
    }
    m_land_floor.assign(m_grid.size(), -std::numeric_limits<float>::infinity());
    for (const auto& c : w.cells) {
        if (c.land < w.lands.size()) {
            const auto& heights = w.lands[c.land].heights;
            m_land_floor[size_t(c.y - lo[1]) * m_grid_size[0] + (c.x - lo[0])] =
                *std::min_element(std::begin(heights), std::end(heights));
        }
    }
    m_slot_of_cell.assign(w.cells.size(), -1);
    m_models.resize(w.meshes.size());
    m_light_grid = std::make_unique<render::LightGrid>(w);
    m_light_state.resize(w.lights.size());
    m_point_lights.resize(w.lights.size() + render::k_moving_lights);
    for (uint32_t i = 0; i < w.lights.size(); ++i) {
        if (w.lights[i].flags & osw::light_shadow) {
            m_shadow_candidates.push_back(i);
        }
    }
    for (const auto& l : w.lights) {
        if ((l.flags & osw::light_region_emittance) &&
            std::find(m_emittance_weathers.begin(), m_emittance_weathers.end(), l.emittance_weather) ==
                m_emittance_weathers.end()) {
            m_emittance_weathers.push_back(l.emittance_weather);
        }
    }
    for (const auto& e : w.emittances) {
        if (e.weather && std::find(m_emittance_weathers.begin(), m_emittance_weathers.end(), e.weather) ==
                             m_emittance_weathers.end()) {
            m_emittance_weathers.push_back(e.weather);
        }
    }
    if (!w.lights.empty()) {
        log::info("space {}: {} point lights", w.edid, w.lights.size());
    }
    m_far = read_far_view(m_renderer.cache_root(), w.edid);
    m_terrain = std::make_unique<render::Terrain>(m_renderer.device(), m_renderer.uploader(), m_renderer.jobs(),
                                                  m_renderer.textures(), m_world, m_far);
    if (m_far) {
        far_init();
    }
    if (!w.grasses.empty()) {
        m_grass = std::make_unique<GrassField>(m_renderer, m_world);
    }
    if (render_thread) {
        finish();
    }
}

void Space::retire() {
    m_actors.reset();
    for (uint32_t s = 0; s < m_slots.size(); ++s) {
        if (m_slots[s].cell != ~0u) {
            unload_slot(s);
        }
    }
    m_retired = true;
}

Space::~Space() {
    if (m_retired) {
        return;
    }
    for (uint32_t i : m_fx_instances) {
        if (i != ~0u) {
            m_renderer.device().defer([this, i] { m_instances.free(i); });
        }
    }
    for (uint32_t m : m_fx_materials) {
        if (m != ~0u) {
            m_renderer.device().defer([this, m] { m_renderer.meshes().free_material(m); });
        }
    }
    for (auto& [path, id] : m_fx_meshes) {
        m_renderer.meshes().release(id);
    }
    if (m_ripple_mesh != render::k_invalid_mesh) {
        for (uint32_t i : m_ripple_instances) {
            if (i != ~0u) {
                m_renderer.device().defer([this, i] { m_instances.free(i); });
            }
        }
        m_renderer.meshes().release(m_ripple_mesh);
    }
    if (m_sky_model.mesh != render::k_invalid_mesh) {
        if (m_sky_model.instance != ~0u) {
            m_renderer.device().defer([this, i = m_sky_model.instance] { m_instances.free(i); });
        }
        m_renderer.meshes().release(m_sky_model.mesh);
    }
    for (const auto& s : m_spawned) {
        if (s.instance != ~0u) {
            m_renderer.device().defer([this, i = s.instance] { m_instances.free(i); });
        }
        m_renderer.meshes().release(s.mesh);
    }
    for (uint32_t s = 0; s < m_slots.size(); ++s) {
        if (m_slots[s].cell != ~0u) {
            unload_slot(s);
        }
    }
    // Jobs, uploads and deferred frees may still refer to the terrain and instance table.
    m_renderer.drain();
}

int32_t Space::cell_lookup(int32_t x, int32_t y) const {
    const int32_t gx = x - m_grid_min[0], gy = y - m_grid_min[1];
    if (gx < 0 || gy < 0 || gx >= m_grid_size[0] || gy >= m_grid_size[1]) {
        return -1;
    }
    return m_grid[size_t(gy) * m_grid_size[0] + gx];
}

bool Space::has_cell(int32_t x, int32_t y) const { return cell_lookup(x, y) >= 0; }

glm::vec2 Space::centre() const {
    const float cs = m_world->cell_size;
    return {(m_grid_min[0] + m_grid_size[0] * 0.5f) * cs, (m_grid_min[1] + m_grid_size[1] * 0.5f) * cs};
}

std::optional<float> Space::terrain_height(float x, float y) const {
    const float cs = m_world->cell_size;
    const int32_t cx = static_cast<int32_t>(std::floor(x / cs));
    const int32_t cy = static_cast<int32_t>(std::floor(y / cs));
    const int32_t ci = cell_lookup(cx, cy);
    if (ci < 0) {
        return std::nullopt;
    }
    const auto& cell = m_world->cells[ci];
    if (cell.land == osw::k_none || cell.land >= m_world->lands.size()) {
        return std::nullopt;
    }
    const auto& land = m_world->lands[cell.land];
    const float fx = std::clamp((x / cs - cx) * 32.0f, 0.0f, 32.0f);
    const float fy = std::clamp((y / cs - cy) * 32.0f, 0.0f, 32.0f);
    const int ix = std::min(static_cast<int>(fx), 31), iy = std::min(static_cast<int>(fy), 31);
    const float tx = fx - ix, ty = fy - iy;
    auto h = [&](int r, int c) { return land.heights[r * osw::k_land_verts + c]; };
    const float a = h(iy, ix) + (h(iy, ix + 1) - h(iy, ix)) * tx;
    const float b = h(iy + 1, ix) + (h(iy + 1, ix + 1) - h(iy + 1, ix)) * tx;
    return a + (b - a) * ty;
}

// in game units: the 2048-unit quadrant, then the 128-unit square in it (a point on a boundary
// belongs to the lower one, except at 0), then the triangle. Squares whose indices in the quadrant add up odd
// are split along (1,0)-(0,1) and give (0,0) or (1,1); even ones along (0,0)-(1,1) and give (0,1) or (1,0).
uint32_t Space::dominant_land_texture(float x, float y) const {
    const float cs = m_world->cell_size;
    const int32_t cx = static_cast<int32_t>(std::floor(x / cs));
    const int32_t cy = static_cast<int32_t>(std::floor(y / cs));
    const int32_t ci = cell_lookup(cx, cy);
    if (ci < 0 || m_world->cells[ci].land == osw::k_none || m_world->cells[ci].land >= m_world->lands.size()) {
        return 0;
    }
    const auto& land = m_world->lands[m_world->cells[ci].land];
    const float lx = (x - cx * cs) / units::k_skyrim_unit_m, ly = (y - cy * cs) / units::k_skyrim_unit_m;
    const int qx = std::clamp(int(lx / 2048.0f), 0, 1), qy = std::clamp(int(ly / 2048.0f), 0, 1);
    const auto& quadrant = land.quadrants[qy * 2 + qx];
    const int vx = std::clamp(int(std::lround((lx - qx * 2048.0f) / 128.0f)), 0, osw::k_quad_verts - 1);
    const int vy = std::clamp(int(std::lround((ly - qy * 2048.0f) / 128.0f)), 0, osw::k_quad_verts - 1);
    float largest = -3.4028235e38f, sum = 0.0f;
    uint16_t texture = quadrant.base_texture;
    for (uint32_t i = 0; i < quadrant.layer_count; ++i) {
        const auto& layer = m_world->land_layers[quadrant.first_layer + i];
        const float w = float(layer.opacity[vy * osw::k_quad_verts + vx]) / 255.0f;
        sum += w;
        if (w > largest) {
            largest = w;
            texture = layer.texture;
        }
    }
    if (largest <= 1.0f - sum) {
        texture = quadrant.base_texture;
    }
    return texture < m_world->land_textures.size() ? m_world->land_textures[texture].form_id : 0;
}

std::optional<float> Space::land_vertex_height(float x, float y) const {
    const float cs = m_world->cell_size;
    const int32_t cx = static_cast<int32_t>(std::floor(x / cs));
    const int32_t cy = static_cast<int32_t>(std::floor(y / cs));
    const int32_t ci = cell_lookup(cx, cy);
    if (ci < 0) {
        return std::nullopt;
    }
    const auto& cell = m_world->cells[ci];
    if (cell.land == osw::k_none || cell.land >= m_world->lands.size()) {
        return std::nullopt;
    }
    const auto& land = m_world->lands[cell.land];
    auto index = [](float local, float size) {
        int i = static_cast<int>(local / size);
        if (static_cast<int>(local) % static_cast<int>(size) == 0 && static_cast<int>(local) != 0) {
            --i;
        }
        return i;
    };
    const float lx = (x - cx * cs) / units::k_skyrim_unit_m, ly = (y - cy * cs) / units::k_skyrim_unit_m;
    const int qx = index(lx, 2048.0f), qy = index(ly, 2048.0f);
    const float ux = lx - qx * 2048.0f, uy = ly - qy * 2048.0f;
    const int sx = index(ux, 128.0f), sy = index(uy, 128.0f);
    const float fx = ux - sx * 128.0f, fy = uy - sy * 128.0f;
    int ox = 0, oy = 0;
    if ((sx + sy) & 1) {
        ox = oy = fx + fy <= 128.0f ? 0 : 1;
    } else if (fx <= fy) {
        oy = 1;
    } else {
        ox = 1;
    }
    const int col = std::clamp(qx * 16 + sx + ox, 0, 32), row = std::clamp(qy * 16 + sy + oy, 0, 32);
    return land.heights[row * osw::k_land_verts + col];
}

void Space::load_cell(uint32_t cell_index, float priority) {
    if (on_cell) {
        on_cell(m_world->cells[cell_index].form_id, true);
    }
    uint32_t slot;
    if (!m_free_slots.empty()) {
        slot = m_free_slots.back();
        m_free_slots.pop_back();
    } else {
        slot = static_cast<uint32_t>(m_slots.size());
        m_slots.emplace_back();
    }
    LoadedCell& lc = m_slots[slot];
    m_draw_order_dirty = true;
    lc.cell = cell_index;
    ++lc.generation;
    lc.refs.clear();
    lc.lo = glm::vec3(1e30f);
    lc.hi = glm::vec3(-1e30f);
    lc.ready = 0;
    lc.waiting = 0;
    m_slot_of_cell[cell_index] = static_cast<int32_t>(slot);
    ++m_loaded_count;

    const auto& w = *m_world;
    const auto& c = w.cells[cell_index];
    const uint64_t end = std::min<uint64_t>(uint64_t(c.first_ref) + c.ref_count, w.refs.size());
    auto& meshes = m_renderer.meshes();
    lc.refs.reserve(end > c.first_ref ? end - c.first_ref : 0);
    for (uint64_t i = c.first_ref; i < end; ++i) {
        const auto& ref = w.refs[i];
        if (ref.mesh >= m_models.size()) {
            continue;
        }
        LoadedRef lr;
        lr.ref = static_cast<uint32_t>(i);
        lr.model = ref.mesh;
        lr.disabled = (ref.flags & osw::ref_initially_disabled) != 0;
        if (m_ref_overrides && !m_ref_overrides->empty()) {
            if (const auto it = m_ref_overrides->find(ref.form_id); it != m_ref_overrides->end()) {
                lr.disabled = it->second;
            }
        }
        lr.tree = ref.tree != osw::k_none;
        lr.impostor = lr.tree && m_far_tree_forms.contains(ref.form_id);
        lr.hidden = !m_hidden.empty() && m_hidden.contains(ref.form_id);
        lr.occludable = (ref.flags & osw::ref_occludable) != 0;
        lr.occludable_far = (ref.flags & osw::ref_occludable_far) != 0;
        Model& m = m_models[ref.mesh];
        if (m.refs++ == 0) {
            m.lod0 = meshes.acquire(w.meshes[ref.mesh], priority);
            m.pending = true;
            m_pending_models.push_back(ref.mesh);
        }
        lc.refs.push_back(lr);
        if (m.pending) {
            m.waiting.push_back({slot, lc.generation, static_cast<uint32_t>(lc.refs.size() - 1)});
            ++lc.waiting;
        } else {
            finalize_ref(lc, lc.refs.back());
        }
    }
    m_refs_loaded += static_cast<uint32_t>(lc.refs.size());
}

void Space::unload_slot(uint32_t slot) {
    LoadedCell& lc = m_slots[slot];
    if (on_cell && lc.cell != ~0u) {
        on_cell(m_world->cells[lc.cell].form_id, false);
    }
    m_draw_order_dirty = true;
    auto& meshes = m_renderer.meshes();
    for (auto& r : lc.refs) {
        if (r.instance != ~0u) {
            m_renderer.device().defer([this, s = r.instance] { m_instances.free(s); });
            --m_refs_ready;
        }
        Model& m = m_models[r.model];
        if (--m.refs == 0) {
            meshes.release(m.lod0);
            m.lod0 = render::k_invalid_mesh;
            m.pending = false;
            m.waiting.clear();
        }
    }
    m_refs_loaded -= static_cast<uint32_t>(lc.refs.size());
    m_slot_of_cell[lc.cell] = -1;
    lc.cell = ~0u;
    ++lc.generation;
    lc.refs.clear();
    lc.ready = 0;
    m_free_slots.push_back(slot);
    --m_loaded_count;
}

namespace {
// The pseudo-weather whose emittance colour the sky statics take: the game points them at
// the sky's colour 13.
constexpr uint32_t k_sky_static_emittance = 0xFFFFFFFFu;
} // namespace

void Space::attach_actors(ActorData& data) {
    if (!m_actors) {
        m_actors = std::make_unique<ActorSet>(m_renderer, data, m_world);
    }
}

// Each actor's instance: new when it has moved, so a slot a frame in flight reads is never rewritten.
void Space::write_actor_instances() {
    for (ActorSet::Visual* v : m_actors->visuals()) {
        // Between ticks what the last tick moved shows on its way: the last tick's step is `m_actor_blend` done.
        const bool on_its_way = m_actor_blend < 1.0f && v->seen && v->moved_tick + 1 == m_actors->tick();
        glm::vec3 position;
        glm::mat3 rotation;
        const bool held = m_actors->held_player(*v, position, rotation);
        if (!v->moved && !on_its_way && !held) {
            continue;
        }
        const uint32_t slot = m_instances.allocate();
        if (slot == ~0u) {
            continue;
        }
        if (!held) {
            const float t = on_its_way ? m_actor_blend : 1.0f;
            position = glm::mix(v->before_position, v->position, t);
            rotation = v->rotation;
            if (t < 1.0f) {
                for (int c = 0; c < 2; ++c) {
                    rotation[c] = glm::normalize(glm::mix(v->before_rotation[c], v->rotation[c], t));
                }
            }
        }
        render::GpuInstanceData d;
        for (int i = 0; i < 3; ++i) {
            d.rows[i] = glm::vec4(rotation[0][i] * v->scale, rotation[1][i] * v->scale, rotation[2][i] * v->scale, position[i]);
        }
        d.sphere = v->sphere;
        d.form_id = v->ref;
        d.flags = uint32_t(render::InstanceFade::never) << render::k_instance_fade_shift;
        const uint32_t group = m_rooms ? m_rooms->place(v->sphere, osw::k_none) : Rooms::k_cell;
        m_light_grid->list(glm::vec3(v->sphere), v->sphere.w, false, d.lights,
                           m_rooms ? m_rooms->unlit(group) : std::span<const uint16_t>{});
        m_instances.write(slot, d);
        if (v->instance != ~0u) {
            m_renderer.device().defer([this, old = v->instance] { m_instances.free(old); });
        }
        v->instance = slot;
        v->moved = false;
    }
}

void Space::actor_draws(const render::View& view, render::DrawList& out, bool main) {
    for (const ActorSet::Visual* v : m_actors->visuals()) {
        if (v->instance == ~0u || v->hidden) {
            continue;
        }
        const glm::vec3 rel = glm::vec3(v->sphere) - view.position;
        if (!view.frustum.sphere(rel, v->sphere.w)) {
            continue;
        }
        if (main && m_rooms && !m_rooms->outside_visible() &&
            !m_rooms->visible(m_rooms->place(v->sphere, osw::k_none), rel, v->sphere.w)) {
            continue;
        }
        const float d2 = glm::dot(rel, rel);
        v->seen = true;
        for (const ActorDraw& part : v->draws) {
            out.add(part.mesh, v->instance, d2, part.variant);
        }
        for (const ActorMembrane& m : v->membranes) {
            out.add_over(m.mesh, m.variant, m.group, v->instance, d2, m.material, m.state);
        }
        for (const ActorSkinDecal& d : v->skin_decals) {
            out.add_skin_decal({d.slot, v->instance, d.texture, d.fade});
        }
    }
}

void Space::set_emittance(uint32_t weather, const glm::vec3& colour) {
    auto [it, added] = m_emittance.try_emplace(weather, colour);
    if (added || it->second != colour) {
        it->second = colour;
        m_emittance_changed = true;
    }
}

void Space::finalize_ref(LoadedCell& cell, LoadedRef& r) {
    const auto& meshes = m_renderer.meshes();
    const Model& m = m_models[r.model];
    if (m.lod0 == render::k_invalid_mesh || !meshes.ready(m.lod0)) {
        return; // failed meshes are simply not drawn
    }
    const auto& ref = m_world->refs[r.ref];
    glm::mat3 rot = ref_rotation(m_rotation_mode, ref.rotation, ref.source_euler);
    const float s = ref.scale;
    glm::vec3 t(ref.position[0], ref.position[1], ref.position[2]);
    if (!m_moved.empty()) {
        if (const auto moved = m_moved.find(ref.form_id); moved != m_moved.end()) {
            rot = glm::mat3_cast(moved->second.rotation);
            t = moved->second.position;
        }
    }
    const glm::vec4 local = meshes.sphere(m.lod0);
    const glm::vec3 centre = rot * (s * glm::vec3(local)) + t;
    const float radius = std::abs(s) * local.w;
    // The game's own bound of the model's root: LOD fades and light reach measure from it (2.2.10, 9.4); culling
    // keeps the box's sphere, which is sure to hold every vertex.
    const glm::vec4 fade_local = meshes.fade_sphere(m.lod0);
    const glm::vec4 fade_sphere(rot * (s * glm::vec3(fade_local)) + t, std::abs(s) * fade_local.w);

    const uint32_t slot = m_instances.allocate();
    if (slot == ~0u) {
        if (!m_logged_full) {
            log::warn("space {}: instance table full ({} slots)", edid(), m_instances.capacity());
            m_logged_full = true;
        }
        return;
    }
    render::GpuInstanceData d;
    for (int i = 0; i < 3; ++i) {
        d.rows[i] = glm::vec4(rot[0][i] * s, rot[1][i] * s, rot[2][i] * s, t[i]);
    }
    if (m_rooms) {
        r.group = m_rooms->place(fade_sphere, ref.room_hint);
    }
    m_light_grid->list(glm::vec3(fade_sphere), fade_sphere.w, false, d.lights,
                       m_rooms ? m_rooms->unlit(r.group) : std::span<const uint16_t>{});
    d.sphere = fade_sphere;
    d.form_id = ref.form_id;
    d.leaf_amplitude = 0.0f;
    d.leaf_frequency = 0.0f;
    d.flags = 0;
    using render::InstanceFade;
    InstanceFade fade = InstanceFade::objects;
    if (ref.flags & osw::ref_fade_never) {
        fade = InstanceFade::never;
    } else if (ref.flags & osw::ref_fade_visible_when_distant) {
        fade = InstanceFade::visible_when_distant;
    } else if (ref.flags & osw::ref_fade_sky_cell) {
        fade = InstanceFade::sky_cell;
    } else if (ref.flags & osw::ref_fade_items) {
        fade = InstanceFade::items;
    }
    d.flags |= uint32_t(fade) << render::k_instance_fade_shift;
    if (ref.tree < m_world->trees.size()) {
        const auto& tree = m_world->trees[ref.tree];
        d.leaf_amplitude = tree.leaf_amplitude;
        d.leaf_frequency = tree.leaf_frequency;
        d.flags |= render::k_instance_leaf_anim;
    }
    if (ref.emittance < m_world->emittances.size()) {
        const auto& e = m_world->emittances[ref.emittance];
        glm::vec3 c(e.color[0], e.color[1], e.color[2]);
        if (e.weather) {
            r.emittance_weather = e.weather;
            auto it = m_emittance.find(e.weather);
            c = it != m_emittance.end() ? it->second : glm::vec3(1.0f);
        }
        d.flags |= render::pack_emittance(&c.x);
    }
    if (m_sky_static_bases.contains(ref.base_form_id)) {
        r.sky_static = true;
        r.emittance_weather = k_sky_static_emittance;
        auto it = m_emittance.find(k_sky_static_emittance);
        const glm::vec3 c = it != m_emittance.end() ? it->second : glm::vec3(1.0f);
        d.flags = sky_static_flags(d.flags | render::pack_emittance(&c.x), ref.base_form_id);
    }
    m_instances.write(slot, d);
    const ots::Swap* swap = nullptr;
    if (m_texture_swaps) {
        if (const auto it = m_texture_swaps->find(ref.base_form_id); it != m_texture_swaps->end()) {
            swap = &it->second;
        }
    }
    if (swap && ref.directional >= m_world->directional_materials.size()) {
        osw::DirectionalMaterial plain{};
        plain.base_form_id = ref.base_form_id;
        r.variant = m_renderer.meshes().directional_variant(m.lod0, plain, swap);
    }
    if (ref.directional < m_world->directional_materials.size()) {
        const auto& dm = m_world->directional_materials[ref.directional];
        r.variant = m_renderer.meshes().directional_variant(m.lod0, dm, swap);
        if ((dm.flags & osw::directional_multi_pass) && dm.pass < m_world->directional_passes.size()) {
            r.second = m_renderer.meshes().multipass_variant(m.lod0, dm, m_world->directional_passes[dm.pass]);
        }
    }
    r.instance = slot;
    r.sphere = glm::vec4(centre, radius);
    r.pixel_radius = std::min(radius, fade_sphere.w);
    for (uint32_t l = 0; l < osm::k_lod_levels; ++l) {
        const float e = meshes.lod_error(m.lod0)[l];
        r.lod_error[l] = e < 0.0f ? -1.0f : e * std::abs(s);
    }
    cell.lo = glm::min(cell.lo, centre - glm::vec3(radius));
    cell.hi = glm::max(cell.hi, centre + glm::vec3(radius));
    ++cell.ready;
    ++m_refs_ready;
}

void Space::process_ready_models(double budget_ms) {
    const auto start = Clock::now();
    const auto& meshes = m_renderer.meshes();
    size_t keep = 0;
    size_t i = 0;
    for (; i < m_pending_models.size(); ++i) {
        const uint32_t mi = m_pending_models[i];
        Model& m = m_models[mi];
        if (!m.pending) {
            continue; // released meanwhile
        }
        const bool done = meshes.ready(m.lod0) || meshes.failed(m.lod0);
        if (!done || (i % 16 == 0 && ms_since(start) > budget_ms)) {
            m_pending_models[keep++] = mi;
            continue;
        }
        m.pending = false;
        for (const auto& h : m.waiting) {
            LoadedCell& lc = m_slots[h.slot];
            if (lc.generation == h.generation && lc.cell != ~0u) {
                finalize_ref(lc, lc.refs[h.index]);
                --lc.waiting;
            }
        }
        m.waiting.clear();
        m.waiting.shrink_to_fit();
    }
    m_pending_models.resize(keep);
}

void Space::rebuild_instances() {
    for (auto& lc : m_slots) {
        if (lc.cell == ~0u) {
            continue;
        }
        lc.lo = glm::vec3(1e30f);
        lc.hi = glm::vec3(-1e30f);
        lc.ready = 0;
        for (auto& r : lc.refs) {
            if (r.instance != ~0u) {
                m_renderer.device().defer([this, s = r.instance] { m_instances.free(s); });
                r.instance = ~0u;
                --m_refs_ready;
            }
            if (!m_models[r.model].pending) {
                finalize_ref(lc, r);
            }
        }
    }
}

void Space::update(const glm::vec3& camera, const glm::vec3& forward, const SpaceSettings& settings,
                   const render::TerrainSettings& terrain) {
    if (m_actors) {
        m_actors->update(camera);
        write_actor_instances();
    }
    const auto start = Clock::now();
    if (settings.rotation_mode != m_rotation_mode) {
        m_rotation_mode = settings.rotation_mode;
        rebuild_instances(); // debug toggle: allowed to cost a frame
    }

    const float cs = m_world->cell_size;
    const int32_t cx = static_cast<int32_t>(std::floor(camera.x / cs));
    const int32_t cy = static_cast<int32_t>(std::floor(camera.y / cs));
    const float radius = std::max(settings.draw_distance, 0.0f);
    const glm::vec2 cam(camera.x / cs, camera.y / cs);
    auto cell_dist = [&](const osw::Cell& c) { return stream_distance(c.x, c.y, cam); };

    // Re-plan whenever the camera has moved a fraction of a cell, not only on cell changes: the plan is
    // measured from the camera, so a stale one would load a different set depending on where the camera
    // entered the cell — what is drawn must depend only on where the camera is now.
    if (cx != m_cam_cell[0] || cy != m_cam_cell[1] || radius != m_last_distance ||
        glm::length(cam - m_plan_pos) > k_replan_cells) {
        SPUD_TRACE("replan");
        m_cam_cell[0] = cx;
        m_cam_cell[1] = cy;
        m_last_distance = radius;
        m_plan_pos = cam;
        // Wanted: within the radius, not loaded; nearest and in-view first.
        m_wanted.clear();
        m_wanted_next = 0;
        glm::vec2 fwd(forward.x, forward.y);
        const float fl = glm::length(fwd);
        fwd = fl > 1e-4f ? fwd / fl : glm::vec2(0);
        const int r = static_cast<int>(std::ceil(radius)) + 1;
        for (int y = cy - r; y <= cy + r; ++y) {
            for (int x = cx - r; x <= cx + r; ++x) {
                const int32_t ci = cell_lookup(x, y);
                if (ci < 0 || m_slot_of_cell[ci] >= 0) {
                    continue;
                }
                const auto& c = m_world->cells[ci];
                const float d = cell_dist(c);
                // Plan a superset of what build_draws keeps (radius + 0.5 from the *current* camera): the
                // camera may be up to k_replan_cells from where this plan was made.
                if (d > radius + 0.5f + k_replan_cells) {
                    continue;
                }
                const glm::vec2 dir = glm::vec2(c.x + 0.5f, c.y + 0.5f) - cam;
                const float dl = glm::length(dir);
                const float facing = dl > 1e-4f ? glm::dot(dir / dl, fwd) : 1.0f;
                const float priority = d < 1.5f ? d : d * (1.75f - 0.75f * facing);
                m_wanted.emplace_back(priority, static_cast<uint32_t>(ci));
            }
        }
        std::sort(m_wanted.begin(), m_wanted.end());
        m_to_unload.clear();
        for (uint32_t s = 0; s < m_slots.size(); ++s) {
            if (m_slots[s].cell != ~0u && cell_dist(m_world->cells[m_slots[s].cell]) > radius + settings.unload_margin) {
                m_to_unload.push_back(s);
            }
        }
    }

    // Time-budgeted: unloads first (they free memory), then loads.
    SPUD_TRACE("load/unload");
    while (!m_to_unload.empty() && ms_since(start) < settings.load_budget_ms) {
        const uint32_t s = m_to_unload.back();
        m_to_unload.pop_back();
        if (m_slots[s].cell != ~0u) {
            unload_slot(s);
        }
    }
    while (m_wanted_next < m_wanted.size() && ms_since(start) < settings.load_budget_ms) {
        const auto [priority, ci] = m_wanted[m_wanted_next++];
        if (m_slot_of_cell[ci] < 0) {
            load_cell(ci, priority);
        }
    }
    {
        SPUD_TRACE("ready models");
        process_ready_models(std::max(0.05, settings.load_budget_ms - ms_since(start)));
    }
    SPUD_TRACE("terrain update");
    m_terrain->update(camera, terrain);
    if (m_far) {
        far_update(camera, settings, terrain);
    }
    if (m_grass) {
        m_grass->update(camera, settings.fade_seconds, settings.grass_cell_radius);
    }
}

void Space::build_draws(const render::View& view, const SpaceSettings& settings, render::DrawList& out, bool main) {
    out.clear();
    if (main && m_sky_model.mesh != render::k_invalid_mesh && m_renderer.meshes().ready(m_sky_model.mesh) &&
        m_sky_model_fade > 0.0f) {
        if (m_sky_model.instance == ~0u) {
            m_sky_model.instance = m_instances.allocate();
        }
        if (m_sky_model.instance != ~0u) {
            render::GpuInstanceData d{};
            for (int i = 0; i < 3; ++i) {
                d.rows[i] = glm::vec4(0.0f);
                d.rows[i][i] = 1.0f;
                d.rows[i].w = m_sky_model.position[i];
            }
            const glm::vec4 local = m_renderer.meshes().sphere(m_sky_model.mesh);
            d.sphere = glm::vec4(glm::vec3(local) + m_sky_model.position, local.w);
            d.leaf_amplitude = m_sky_model_fade;
            d.flags = render::k_instance_sky_fade;
            m_instances.write(m_sky_model.instance, d);
            out.add_sky(m_sky_model.mesh, m_sky_model.instance);
        }
    }
    if (main && m_ripple_strength > 0.0f && m_ripple_mesh != render::k_invalid_mesh && m_renderer.meshes().ready(m_ripple_mesh)) {
        const glm::vec4 local = m_renderer.meshes().sphere(m_ripple_mesh);
        for (size_t i = 0; i < m_ripple_positions.size(); ++i) {
            if (i == m_ripple_instances.size()) {
                m_ripple_instances.push_back(m_instances.allocate());
            }
            if (m_ripple_instances[i] == ~0u) {
                continue;
            }
            render::GpuInstanceData d{};
            for (int r = 0; r < 3; ++r) {
                d.rows[r] = glm::vec4(0.0f);
                d.rows[r][r] = 1.0f;
                d.rows[r].w = m_ripple_positions[i][r];
            }
            d.sphere = glm::vec4(glm::vec3(local) + m_ripple_positions[i], local.w);
            d.leaf_amplitude = m_ripple_strength;
            d.flags = render::k_instance_sky_fade;
            m_instances.write(m_ripple_instances[i], d);
            const glm::vec3 rel = glm::vec3(d.sphere) - view.position;
            out.add(m_ripple_mesh, m_ripple_instances[i], glm::dot(rel, rel));
        }
    }
    for (const DrawnDoorShape& s : m_door_shapes) {
        const glm::vec3 rel = glm::vec3(s.sphere) - view.position;
        out.add_group(s.mesh, s.group, s.instance, glm::dot(rel, rel));
    }
    for (size_t i = 0; main && i < m_fx_shapes.size(); ++i) {
        const FxShape& s = m_fx_shapes[i];
        auto [it, fresh] = m_fx_meshes.try_emplace(s.model, render::k_invalid_mesh);
        if (fresh) {
            it->second = m_renderer.meshes().acquire(s.model, 1.0f);
        }
        auto& meshes = m_renderer.meshes();
        if (!meshes.ready(it->second)) {
            continue;
        }
        const auto groups = meshes.groups(it->second);
        for (uint32_t k = 0; k < groups.size(); ++k) {
            if (meshes.group(groups[k]).submesh != s.submesh) {
                continue;
            }
            if (i >= m_fx_instances.size()) {
                m_fx_instances.resize(i + 1, ~0u);
                m_fx_materials.resize(i + 1, ~0u);
            }
            // A fresh slot and material a frame: one a frame in flight may read is never rewritten, or that frame draws
            // something else at this shape's pose. The shape's own controllers stop: the effect has run them on its own
            // clock.
            const uint32_t instance = m_instances.allocate();
            if (instance == ~0u) {
                break;
            }
            if (m_fx_instances[i] != ~0u) {
                m_renderer.device().defer([this, old = m_fx_instances[i]] { m_instances.free(old); });
            }
            m_fx_instances[i] = instance;
            render::GpuInstanceData d{};
            for (int r = 0; r < 3; ++r) {
                d.rows[r] = glm::vec4(s.rotation[0][r], s.rotation[1][r], s.rotation[2][r], s.position[r]);
            }
            const glm::vec4 local = meshes.sphere(it->second);
            d.sphere = glm::vec4(s.rotation * glm::vec3(local) + s.position, local.w);
            m_light_grid->list(glm::vec3(d.sphere), d.sphere.w, false, d.lights, {});
            m_instances.write(instance, d);
            const uint32_t material = meshes.copy_material(groups[k], [&](render::GpuMaterial& m) {
                for (int c = 0; c < 4 && s.animated; ++c) {
                    m.base_color[c] = s.base_color[c];
                    m.uv[c] = s.uv[c];
                }
                m.uv_anim = ~0u;
            });
            if (m_fx_materials[i] != ~0u) {
                m_renderer.device().defer([this, old = m_fx_materials[i]] { m_renderer.meshes().free_material(old); });
            }
            m_fx_materials[i] = material;
            const glm::vec3 rel = glm::vec3(d.sphere) - view.position;
            out.add_group(it->second, k, m_fx_instances[i], glm::dot(rel, rel),
                          material == ~0u ? render::k_group_material : material);
            break;
        }
    }
    for (auto& s : m_spawned) {
        const auto& meshes = m_renderer.meshes();
        if ((s.instance == ~0u || s.moved) && meshes.ready(s.mesh)) {
            const glm::mat3& rot = s.rotation;
            const glm::vec4 local = meshes.sphere(s.mesh);
            render::GpuInstanceData d{};
            for (int i = 0; i < 3; ++i) {
                d.rows[i] = glm::vec4(rot[0][i], rot[1][i], rot[2][i], s.position[i]);
            }
            const float size = glm::length(rot[0]);
            s.sphere = glm::vec4(rot * glm::vec3(local) + s.position, local.w * size);
            const glm::vec4 fade = meshes.fade_sphere(s.mesh);
            d.sphere = glm::vec4(rot * glm::vec3(fade) + s.position, fade.w * size);
            m_light_grid->list(glm::vec3(d.sphere), d.sphere.w, false, d.lights);
            if (s.instance == ~0u) {
                s.instance = m_instances.allocate();
            }
            if (s.instance != ~0u) {
                m_instances.write(s.instance, d);
            }
            s.moved = false;
        }
        if (s.instance != ~0u && !s.hidden) {
            const glm::vec3 rel = glm::vec3(s.sphere) - view.position;
            out.add(s.mesh, s.instance, glm::dot(rel, rel));
        }
    }
    const glm::vec3 cam = view.position;
    const float cs = m_world->cell_size;
    const glm::vec2 camc(cam.x / cs, cam.y / cs);
    const float max_d = settings.draw_distance + 0.5f;
    const bool show_disabled = settings.show_disabled;
    // r * k / d < min  <=>  r^2 * (k / min)^2 < d^2
    const float k = settings.min_pixel_radius > 0 ? settings.pixels_per_radian / settings.min_pixel_radius : 0.0f;
    const float k2 = k * k;
    // A view through a door stands somewhere it can't see from: behind a wall, in the next room, inside a hill. Which room
    // it is in, and what the land hides from there, say nothing of what shows through the doorway.
    const bool rooms = m_rooms != nullptr && settings.room_culling && main && !view.clipped();
    if (rooms) {
        m_rooms->update(view);
    }
    const bool room_cull = rooms && !m_rooms->outside_visible();
    const bool occlude = m_occluders && settings.occlusion && main;
    if (occlude) {
        m_occluders->update(view);
    }
    const float far = m_view_distance;
    if (m_draw_order_dirty) {
        m_draw_order.clear();
        for (uint32_t s = 0; s < m_slots.size(); ++s) {
            if (m_slots[s].cell != ~0u) {
                m_draw_order.push_back(s);
            }
        }
        std::sort(m_draw_order.begin(), m_draw_order.end(),
                  [&](uint32_t a, uint32_t b) { return m_slots[a].cell < m_slots[b].cell; });
        m_draw_order_dirty = false;
    }
    if (m_far) {
        far_mark_squares(camc, max_d);
    }
    m_near_order.clear();
    for (uint32_t slot : m_draw_order) {
        const auto& lc = m_slots[slot];
        if (lc.cell == ~0u || lc.ready == 0) {
            continue;
        }
        const auto& c = m_world->cells[lc.cell];
        const glm::vec2 to_cell = glm::vec2(c.x + 0.5f, c.y + 0.5f) - camc;
        const float d2 = glm::dot(to_cell, to_cell);
        const int32_t square = m_far ? far_square(c.x, c.y) : -1;
        // Else in the unload hysteresis band, or standing behind its square's proxy until all of the square is here.
        if (square >= 0 ? m_far_full[square] != 0 : d2 <= max_d * max_d) {
            m_near_order.emplace_back(d2, slot);
        }
    }
    // Ties go by the cell order m_draw_order holds: a stable sort keeps it.
    std::stable_sort(m_near_order.begin(), m_near_order.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    const bool horizon = settings.occlusion && main && !interior() && !view.clipped();
    if (main) {
        m_tree_near = m_far_trees && m_far_trees->ready() ? settings.tree_near : 0.0f;
    }
    const float tree_near2 = main ? m_tree_near * m_tree_near : 0.0f;
    const float lod_pixels = settings.lod_pixels;
    const float lod_scale2 = settings.pixels_per_radian * settings.pixels_per_radian / (lod_pixels * lod_pixels);
    for (const auto& [cell_distance, slot] : m_near_order) {
        const auto& lc = m_slots[slot];
        const render::Cull cull = view.frustum.aabb(lc.lo - cam, lc.hi - cam);
        if (cull == render::Cull::outside) {
            continue;
        }
        if (horizon && cell_distance > 4.0f && land_hides(cam, lc.lo, lc.hi)) {
            continue;
        }
        const bool test = cull == render::Cull::intersect;
        const uint32_t occluders = occlude ? m_occluders->reaching(lc.lo - cam, lc.hi - cam) : 0;
        for (const auto& r : lc.refs) {
            if (r.instance == ~0u || (r.disabled && !show_disabled) || r.hidden) {
                continue;
            }
            if (r.sky_static && (m_instances.flags(r.instance) &
                                 (render::k_instance_static_current | render::k_instance_static_outgoing)) == 0) {
                continue;
            }
            const glm::vec3 rel = glm::vec3(r.sphere) - cam;
            if (test && !view.frustum.sphere(rel, r.sphere.w)) {
                continue;
            }
            const float d2 = glm::dot(rel, rel);
            if (tree_near2 > 0.0f && r.impostor && d2 > tree_near2) {
                continue; // its impostor stands here: far_tree_runs
            }
            const float pixel_radius = settings.tight_pixel_radius ? r.pixel_radius : r.sphere.w;
            if (k2 > 0 && pixel_radius * pixel_radius * k2 < d2) {
                continue; // sub-pixel
            }
            if (far > 0.0f && d2 > (far + r.sphere.w) * (far + r.sphere.w)) {
                continue; // beyond the camera's far plane
            }
            if (room_cull && !m_rooms->visible(r.group, rel, r.sphere.w)) {
                continue;
            }
            if (occluders && (r.occludable || (r.occludable_far && d2 > k_occlude_far_sq)) &&
                m_occluders->hides(rel, r.sphere.w, occluders)) {
                continue;
            }
            // The coarsest level whose error stays under the pixel threshold (ARCHITECTURE.md, LOD).
            uint8_t lod = 0;
            for (uint8_t l = osm::k_lod_levels; l > 0; --l) {
                const float e = r.lod_error[l - 1];
                if (e >= 0.0f && e * e * lod_scale2 <= d2) {
                    lod = l;
                    break;
                }
            }
            out.add(m_models[r.model].select(d2), r.instance, d2, r.variant, false, lod);
            if (r.second != render::k_no_variant) {
                out.add(m_models[r.model].select(d2), r.instance, d2, r.second, true);
            }
        }
    }
    if (m_actors) {
        actor_draws(view, out, main);
    }
    if (m_far) {
        far_draws(view, out, settings.pixels_per_radian / settings.lod_pixels);
        if (main) {
            far_tree_runs(view);
        }
    }
    if (m_grass && main) {
        m_grass->collect(view);
    }
}

void Space::build_shadow_draws(const render::View& view, const render::SunShadows& sun,
                               const render::ShadowSettings& shadows, const SpaceSettings& settings) {
    for (auto& list : m_shadow_draws) {
        list.clear();
    }
    if (sun.count == 0) {
        return;
    }
    const glm::vec3 cam = view.position;
    for (const DrawnDoorShape& s : m_door_shapes) {
        const glm::vec3 rel = glm::vec3(s.sphere) - cam;
        for (uint32_t c = 0; c < sun.drawn(); ++c) {
            if (sun.cascades[c].sphere(rel, s.sphere.w)) {
                m_shadow_draws[c].add_group(s.mesh, s.group, s.instance, glm::dot(rel, rel));
            }
        }
    }
    const float max_tree_height = units::to_metres(shadows.max_tree_height);
    for (const auto& s : m_spawned) {
        if (s.instance == ~0u || s.hidden) {
            continue;
        }
        const glm::vec3 rel = glm::vec3(s.sphere) - cam;
        for (uint32_t c = 0; c < sun.drawn(); ++c) {
            if (sun.cascades[c].sphere(rel, s.sphere.w)) {
                m_shadow_draws[c].add(s.mesh, s.instance, glm::dot(rel, rel));
            }
        }
    }
    const float cs = m_world->cell_size;
    const glm::vec2 camc(cam.x / cs, cam.y / cs);
    const float max_d = settings.draw_distance + 0.5f;
    for (uint32_t slot : m_draw_order) {
        const auto& lc = m_slots[slot];
        if (lc.cell == ~0u || lc.ready == 0) {
            continue;
        }
        const auto& c = m_world->cells[lc.cell];
        if (glm::length(glm::vec2(c.x + 0.5f, c.y + 0.5f) - camc) > max_d) {
            continue;
        }
        const glm::vec3 centre = (lc.lo + lc.hi) * 0.5f - cam;
        const float radius = glm::length(lc.hi - lc.lo) * 0.5f;
        uint32_t cascades = 0;
        for (uint32_t k = 0; k < sun.drawn(); ++k) {
            cascades |= sun.cascades[k].sphere(centre, radius) ? 1u << k : 0u;
        }
        if (cascades == 0) {
            continue;
        }
        for (const auto& r : lc.refs) {
            if (r.instance == ~0u || (r.disabled && !settings.show_disabled)) {
                continue;
            }
            const glm::vec3 rel = glm::vec3(r.sphere) - cam;
            // a shadow cull leaves out trees high above the camera.
            if (r.tree && shadows.disable_high_tree_shadow && rel.z > max_tree_height) {
                continue;
            }
            const float d2 = glm::dot(rel, rel);
            for (uint32_t k = 0; k < sun.drawn(); ++k) {
                if ((cascades >> k) & 1u && sun.cascades[k].sphere(rel, r.sphere.w)) {
                    m_shadow_draws[k].add(m_models[r.model].select(d2), r.instance, d2);
                }
            }
        }
    }
    if (m_actors) {
        for (const ActorSet::Visual* v : m_actors->visuals()) {
            if (v->instance == ~0u || !v->casts_shadow) {
                continue;
            }
            const glm::vec3 rel = glm::vec3(v->sphere) - cam;
            const float d2 = glm::dot(rel, rel);
            for (uint32_t k = 0; k < sun.drawn(); ++k) {
                if (sun.cascades[k].sphere(rel, v->sphere.w)) {
                    v->seen = true; // its shadow shows it, even out of the view
                    for (const ActorDraw& part : v->draws) {
                        m_shadow_draws[k].add(part.mesh, v->instance, d2, part.variant);
                    }
                }
            }
        }
    }
}

void Space::build_shadow_lights(const render::View& view, const render::SunShadows& sun,
                                const render::ShadowSettings& shadows, float lod_adjust, bool interior,
                                const SpaceSettings& settings) {
    m_shadow_lights.clear();
    for (auto& list : m_shadow_light_draws) {
        list.clear();
    }
    if (!shadows.enabled || m_shadow_candidates.empty()) {
        return;
    }
    const glm::vec3 cam = view.position;
    const float distance = interior ? shadows.interior_distance : shadows.distance;
    const float metres = units::to_metres(distance);
    // The mask's channels: the sun takes one.
    const size_t limit = render::k_max_shadow_lights - (sun.count > 0 ? 1 : 0);
    for (uint32_t index : m_shadow_candidates) {
        render::PointLight& p = m_point_lights[index];
        p.shadow = ~0u;
        if (m_shadow_lights.size() >= limit || !(p.flags & render::point_light_on)) {
            continue;
        }
        // The light's update (): its sphere in the frustum and, if it fades, nearer than the
        // shadow distance, the distance taken with the sphere's radius off and the camera's LOD adjust.
        const glm::vec3 rel = p.position - cam;
        if (!view.frustum.sphere(rel, p.radius)) {
            continue;
        }
        const bool fades = !(p.flags & render::point_light_never_fades);
        if (fades && (glm::dot(rel, rel) - p.radius * p.radius) * lod_adjust >= metres * metres) {
            continue;
        }
        const osw::Light& l = m_world->lights[index];
        const glm::vec4 rotation(l.rotation[0], l.rotation[1], l.rotation[2], l.rotation[3]);
        if (l.shadow_fov < 3.1415927f && !render::spot_light_visible(view, rel, rotation, p.radius, l.shadow_fov)) {
            continue;
        }
        p.shadow = static_cast<uint32_t>(m_shadow_lights.size());
        render::ShadowLight s = render::compute_shadow_light(
            rel, rotation, p.radius, l.shadow_fov, l.shadow_falloff, l.shadow_near, l.shadow_bias, fades, distance);
        s.light = index;
        m_shadow_lights.push_back(s);
    }
    for (size_t i = 0; i < m_shadow_lights.size(); ++i) {
        const render::PointLight& p = m_point_lights[m_shadow_lights[i].light];
        for (const DrawnDoorShape& s : m_door_shapes) {
            const glm::vec3 d = glm::vec3(s.sphere) - p.position;
            if (glm::dot(d, d) < (p.radius + s.sphere.w) * (p.radius + s.sphere.w)) {
                m_shadow_light_draws[i].add_group(s.mesh, s.group, s.instance, glm::dot(d, d));
            }
        }
    }
    // Casters: what the light's sphere reaches.
    for (uint32_t slot : m_draw_order) {
        const auto& lc = m_slots[slot];
        if (lc.cell == ~0u || lc.ready == 0) {
            continue;
        }
        for (size_t i = 0; i < m_shadow_lights.size(); ++i) {
            const render::PointLight& p = m_point_lights[m_shadow_lights[i].light];
            const glm::vec3 nearest = glm::clamp(p.position, lc.lo, lc.hi);
            if (glm::dot(nearest - p.position, nearest - p.position) > p.radius * p.radius) {
                continue;
            }
            for (const auto& r : lc.refs) {
                if (r.instance == ~0u || (r.disabled && !settings.show_disabled)) {
                    continue;
                }
                const glm::vec3 d = glm::vec3(r.sphere) - p.position;
                const float reach = p.radius + r.sphere.w;
                const glm::vec3 rel = glm::vec3(r.sphere) - cam;
                if (glm::dot(d, d) <= reach * reach &&
                    !(r.tree && shadows.disable_high_tree_shadow && rel.z > units::to_metres(shadows.max_tree_height))) {
                    const float d2 = glm::dot(rel, rel);
                    m_shadow_light_draws[i].add(m_models[r.model].select(d2), r.instance, d2);
                }
            }
        }
    }
    if (m_actors) {
        for (size_t i = 0; i < m_shadow_lights.size(); ++i) {
            const render::PointLight& p = m_point_lights[m_shadow_lights[i].light];
            for (const ActorSet::Visual* v : m_actors->visuals()) {
                    const glm::vec3 d = glm::vec3(v->sphere) - p.position;
                const float reach = p.radius + v->sphere.w;
                if (v->instance != ~0u && v->casts_shadow && glm::dot(d, d) <= reach * reach) {
                    const glm::vec3 rel = glm::vec3(v->sphere) - cam;
                    v->seen = true;
                    for (const ActorDraw& part : v->draws) {
                        m_shadow_light_draws[i].add(part.mesh, v->instance, glm::dot(rel, rel), part.variant);
                    }
                }
            }
        }
    }
}

void Space::begin_portal_shadows() {
    m_main_light_shadows.clear();
    for (const render::PointLight& p : m_point_lights) {
        m_main_light_shadows.push_back(p.shadow);
    }
    std::swap(m_shadow_draws, m_portal_shadow_draws);
    std::swap(m_shadow_lights, m_portal_shadow_lights);
    std::swap(m_shadow_light_draws, m_portal_shadow_light_draws);
}

void Space::end_portal_shadows() {
    m_portal_point_lights = m_point_lights;
    for (size_t i = 0; i < m_point_lights.size(); ++i) {
        m_point_lights[i].shadow = m_main_light_shadows[i];
    }
    std::swap(m_shadow_draws, m_portal_shadow_draws);
    std::swap(m_shadow_lights, m_portal_shadow_lights);
    std::swap(m_shadow_light_draws, m_portal_shadow_light_draws);
}

render::SpaceView Space::portal_space_view(const render::View& view, const SpaceSettings& settings) {
    build_draws(view, settings, m_portal_draws, false);
    render::SpaceView sv = space_view();
    sv.draws = &m_portal_draws;
    return sv;
}

bool Space::land_hides(const glm::vec3& eye, const glm::vec3& lo, const glm::vec3& hi) const {
    const float cs = m_world->cell_size;
    const float top = hi.z + k_land_margin;
    const glm::vec2 mid = (glm::vec2(lo) + glm::vec2(hi)) * 0.5f;
    const float xs[3] = {lo.x, mid.x, hi.x}, ys[3] = {lo.y, mid.y, hi.y};
    for (float px : xs) {
        for (float py : ys) {
            const glm::vec2 d(px - eye.x, py - eye.y);
            const float length = glm::length(d);
            const int steps = static_cast<int>(length / cs);
            bool blocked = false;
            for (int i = 1; i < steps && !blocked; ++i) {
                const float t = float(i) / float(steps);
                const int32_t gx = static_cast<int32_t>(std::floor((eye.x + d.x * t) / cs)) - m_grid_min[0];
                const int32_t gy = static_cast<int32_t>(std::floor((eye.y + d.y * t) / cs)) - m_grid_min[1];
                if (gx >= 0 && gy >= 0 && gx < m_grid_size[0] && gy < m_grid_size[1]) {
                    blocked = eye.z + (top - eye.z) * t < m_land_floor[size_t(gy) * m_grid_size[0] + gx];
                }
            }
            if (!blocked) {
                return false;
            }
        }
    }
    return true;
}

void Space::animate_light(const osw::Light& l, LightState& s, float dt) {
    // h = half the intensity amplitude.
    const float h = l.intensity_amplitude * 0.5f;
    const float am = l.movement_amplitude;
    if (l.flags & osw::light_flicker) {
        // A random walk of three phases, then sums of table sines.
        s.phase[0] += dt * l.frequency * random_range(s.random, 1.1f, 13.1f);
        s.phase[1] += dt * l.frequency * random_range(s.random, 1.2f, 13.2f);
        s.phase[2] += dt * l.frequency * random_range(s.random, 1.3f, 19.3f);
        for (float& p : s.phase) {
            if (p > k_two_pi) {
                p -= k_two_pi;
            }
        }
        const float s1 = sin_at((s.phase[0] + 1.7f) * k_table_scale);
        const float s2 = sin_at((s.phase[1] + 0.5f) * k_table_scale);
        const float s3 = sin_at((s.phase[2] + 0.3f) * k_table_scale);
        float m = am * s1 * s2 * 0.5f;
        if (m + am <= 0.0f) {
            m = 0.0f;
        }
        s.offset = glm::vec3(m * s1, m * s2, m * s3);
        float v = (sin_at(s.phase[1] * 105.93353f + 52.966763f) + 1.0f) *
                      (sin_at(s.phase[0] * 89.636063f + 152.38132f) + 1.0f) / 12.0f +
                  0.2f * sin_at(s.phase[2] * 244.46198f + 73.3386f);
        v = std::clamp(v, -1.0f, 1.0f);
        s.factor = h * v + 1.0f - h;
    } else if (l.flags & osw::light_pulse) {
        s.phase[0] += dt * l.frequency;
        if (s.phase[0] > k_two_pi) {
            s.phase[0] -= k_two_pi;
        }
        const float c = cos_at(s.phase[0] * k_table_scale);
        const float sn = sin_at(s.phase[0] * k_table_scale);
        s.factor = h * c + 1.0f - h;
        s.offset = am * glm::vec3(c, sn, sn * c);
    }
}

void Space::update_lights(const glm::vec3& camera, float time, const SpaceSettings& settings) {
    // Region emittance follows the hour (9.2): drawn references with a region source take the current colour.
    if (m_emittance_changed) {
        m_emittance_changed = false;
        for (auto& lc : m_slots) {
            if (lc.cell == ~0u) {
                continue;
            }
            for (auto& r : lc.refs) {
                if (r.instance == ~0u || !r.emittance_weather) {
                    continue;
                }
                if (auto it = m_emittance.find(r.emittance_weather); it != m_emittance.end()) {
                    const uint32_t keep = m_instances.flags(r.instance) & 0xFFu & ~render::k_instance_emittance;
                    m_instances.set_flags(r.instance, keep | render::pack_emittance(&it->second.x));
                }
            }
        }
    }
    const auto& lights = m_world->lights;
    const float dt = m_light_time >= 0.0f ? std::max(time - m_light_time, 0.0f) : 0.0f;
    m_light_time = time;
    const float near_sq = k_flickering_light_distance * k_flickering_light_distance;
    for (size_t i = 0; i < lights.size(); ++i) {
        const osw::Light& l = lights[i];
        LightState& s = m_light_state[i];
        const bool loaded = l.cell < m_slot_of_cell.size() && m_slot_of_cell[l.cell] >= 0;
        if (loaded && !s.loaded) {
            // GenDynamic creates the light afresh: phases from its attenuation values, the created dimmer.
            std::copy_n(l.phase, 3, s.phase);
            s.factor = 1.0f;
            s.offset = glm::vec3(0.0f);
            s.random.seed(l.form_id);
        }
        s.loaded = loaded;
        const glm::vec3 p(l.position[0], l.position[1], l.position[2]);
        if (loaded && settings.animate_lights && (l.flags & (osw::light_flicker | osw::light_pulse)) &&
            glm::dot(p - camera, p - camera) < near_sq) {
            animate_light(l, s, dt);
        }
        render::PointLight& out = m_point_lights[i];
        // The movement is the light's local translation: the reference's rotation and scale apply to it.
        const glm::vec3 u(l.rotation[0], l.rotation[1], l.rotation[2]);
        const glm::vec3 o = s.offset * l.scale;
        const glm::vec3 t = 2.0f * glm::cross(u, o);
        out.position = p + o + l.rotation[3] * t + glm::cross(u, t);
        out.radius = l.radius;
        glm::vec3 colour(l.color[0], l.color[1], l.color[2]);
        if (l.flags & osw::light_region_emittance) {
            if (auto it = m_emittance.find(l.emittance_weather); it != m_emittance.end()) {
                colour *= it->second;
            }
        }
        out.color = colour * (l.dimmer * s.factor);
        bool disabled = (l.flags & osw::light_initially_disabled) != 0;
        if (m_ref_overrides && !m_ref_overrides->empty()) {
            if (const auto it = m_ref_overrides->find(l.form_id); it != m_ref_overrides->end()) {
                disabled = it->second;
            }
        }
        const bool on = loaded && l.dimmer >= 0.05f && (!disabled || settings.show_disabled);
        out.flags = (on ? render::point_light_on : 0u) |
                    ((l.flags & osw::light_never_fades) ? render::point_light_never_fades : 0u) |
                    ((l.flags & osw::light_shadow) ? render::point_light_shadow : 0u);
    }
}

void Space::set_sky_static_bases(std::span<const uint32_t> bases) {
    m_sky_static_bases.insert(bases.begin(), bases.end());
}

void Space::show_sky_statics(std::span<const uint32_t> current, std::span<const uint32_t> outgoing,
                             const glm::vec3& colour) {
    set_emittance(k_sky_static_emittance, colour);
    std::unordered_set<uint32_t> cur(current.begin(), current.end()), out(outgoing.begin(), outgoing.end());
    if (cur == m_sky_static_current && out == m_sky_static_outgoing) {
        return;
    }
    m_sky_static_current = std::move(cur);
    m_sky_static_outgoing = std::move(out);
    for (auto& slot : m_slots) {
        for (auto& r : slot.refs) {
            if (r.sky_static && r.instance != ~0u) {
                m_instances.set_flags(r.instance, sky_static_flags(m_instances.flags(r.instance),
                                                                   m_world->refs[r.ref].base_form_id));
            }
        }
    }
}

Space::RoomBlend Space::room_blend(float dt) {
    // Sky::Update: a room change starts only when no transition runs; s over fRoomLightingTransitionDuration
    // (3 s), weight s^2 (3 - 2 s). Scripted runs hold the clock still and get the room's lighting at once.
    const auto lighting_of = [&](int32_t room) -> const osw::CellLighting* {
        if (room >= 0 && size_t(room) < m_world->rooms.size()) {
            const uint32_t i = m_world->rooms[size_t(room)].lighting;
            if (i < m_world->room_lighting.size()) {
                return &m_world->room_lighting[i];
            }
        }
        return &m_world->lighting;
    };
    const int32_t room = camera_room();
    if (m_room_timer < 0.0f && room != m_room_to) {
        m_room_from = m_room_to;
        m_room_to = room;
        m_room_timer = 0.0f;
    }
    const osw::CellLighting* from = lighting_of(m_room_from);
    const osw::CellLighting* to = lighting_of(m_room_to);
    float w = 1.0f;
    if (m_room_timer >= 0.0f) {
        m_room_timer += dt;
        const float s = dt > 0.0f ? std::clamp(m_room_timer / k_room_lighting_transition, 0.0f, 1.0f) : 1.0f;
        w = s * s * (3.0f - 2.0f * s);
        if (s >= 1.0f) {
            m_room_timer = -1.0f;
        } else {
            m_room_sunlight = from->sunlight_scale + (to->sunlight_scale - from->sunlight_scale) * w;
        }
    }
    const auto image_space_of = [&](int32_t r) -> uint32_t {
        return r >= 0 && size_t(r) < m_world->rooms.size() ? m_world->rooms[size_t(r)].image_space : 0;
    };
    // Sky::Update installs the image space while it blends and in a room that has one. A transition's end in a room
    // without one installs nothing: the last blend stays.
    const bool own = m_room_to >= 0 && size_t(m_room_to) < m_world->rooms.size() &&
                     m_world->rooms[size_t(m_room_to)].image_space != 0;
    if (m_room_timer < 0.0f && (own || m_room_sunlight < 0.0f)) {
        m_room_sunlight = to->sunlight_scale;
    }
    return {from, to, w, m_room_sunlight, image_space_of(m_room_from), image_space_of(m_room_to)};
}

void Space::set_refs_hidden(const std::unordered_map<uint32_t, bool>& changes) {
    for (const auto& [form_id, hidden] : changes) {
        if (hidden) {
            m_hidden.insert(form_id);
        } else {
            m_hidden.erase(form_id);
        }
    }
    for (Spawned& s : m_spawned) {
        if (const auto it = changes.find(s.id); it != changes.end()) {
            s.hidden = it->second;
        }
    }
    for (auto& lc : m_slots) {
        if (lc.cell == ~0u) {
            continue;
        }
        for (auto& r : lc.refs) {
            if (const auto it = changes.find(m_world->refs[r.ref].form_id); it != changes.end()) {
                r.hidden = it->second;
            }
        }
    }
}

void Space::set_ref_hidden(uint32_t form_id, bool hidden) {
    if (hidden) {
        m_hidden.insert(form_id);
    } else {
        m_hidden.erase(form_id);
    }
    for (Spawned& s : m_spawned) {
        s.hidden = s.id == form_id ? hidden : s.hidden;
    }
    for (auto& lc : m_slots) {
        if (lc.cell == ~0u) {
            continue;
        }
        for (auto& r : lc.refs) {
            if (m_world->refs[r.ref].form_id == form_id) {
                r.hidden = hidden;
            }
        }
    }
}

void Space::move_ref(uint32_t form_id, const glm::vec3& position, const glm::quat& rotation) {
    for (Spawned& s : m_spawned) {
        if (s.id == form_id) {
            s.position = position;
            s.rotation = glm::mat3_cast(rotation) * glm::length(s.rotation[0]);
            s.moved = true;
            if (s.instance != ~0u) { // a new slot next frame
                m_renderer.device().defer([this, old = s.instance] { m_instances.free(old); });
                s.instance = ~0u;
            }
            return;
        }
    }
    MovedRef& moved = m_moved[form_id];
    moved.position = position;
    moved.rotation = rotation;
    if (moved.slot >= m_slots.size() || m_slots[moved.slot].generation != moved.generation) {
        moved.slot = ~0u;
        for (uint32_t slot = 0; slot < m_slots.size() && moved.slot == ~0u; ++slot) {
            const LoadedCell& lc = m_slots[slot];
            for (uint32_t i = 0; lc.cell != ~0u && i < lc.refs.size(); ++i) {
                if (m_world->refs[lc.refs[i].ref].form_id == form_id) {
                    moved.slot = slot;
                    moved.generation = lc.generation;
                    moved.index = i;
                    break;
                }
            }
        }
    }
    if (moved.slot == ~0u) {
        return;
    }
    LoadedCell& lc = m_slots[moved.slot];
    LoadedRef& r = lc.refs[moved.index];
    if (r.instance == ~0u) {
        return; // finalize_ref places it when its model comes
    }
    // A new slot, so one a frame in flight reads is never rewritten.
    m_renderer.device().defer([this, old = r.instance] { m_instances.free(old); });
    r.instance = ~0u;
    --lc.ready;
    --m_refs_ready;
    finalize_ref(lc, r);
}

void Space::set_ref_disabled(uint32_t form_id, bool disabled) {
    for (auto& lc : m_slots) {
        if (lc.cell == ~0u) {
            continue;
        }
        for (auto& r : lc.refs) {
            if (m_world->refs[r.ref].form_id == form_id) {
                r.disabled = disabled;
            }
        }
    }
}

std::vector<uint32_t> Space::loaded_refs() const {
    std::vector<uint32_t> out;
    for (const auto& lc : m_slots) {
        if (lc.cell != ~0u) {
            for (const auto& r : lc.refs) {
                out.push_back(m_world->refs[r.ref].form_id);
            }
        }
    }
    return out;
}

bool Space::ref_loaded(uint32_t form_id) const {
    for (const auto& lc : m_slots) {
        if (lc.cell == ~0u) {
            continue;
        }
        for (const auto& r : lc.refs) {
            if (m_world->refs[r.ref].form_id == form_id) {
                return true;
            }
        }
    }
    return false;
}

void Space::set_ref_overrides(const std::unordered_map<uint32_t, bool>* overrides) {
    if (overrides == m_ref_overrides) {
        return;
    }
    m_ref_overrides = overrides;
    if (overrides) {
        for (const auto& [form, disabled] : *overrides) {
            set_ref_disabled(form, disabled);
        }
    }
}

void Space::refs_on_ray(const glm::vec3& from, const glm::vec3& direction, float distance,
                        std::vector<const osw::Ref*>& out) const {
    const glm::vec3 to = from + direction * distance;
    const glm::vec3 lo = glm::min(from, to), hi = glm::max(from, to);
    for (const auto& lc : m_slots) {
        if (lc.cell == ~0u || lc.ready == 0 || glm::any(glm::lessThan(lc.hi, lo)) ||
            glm::any(glm::greaterThan(lc.lo, hi))) {
            continue;
        }
        for (const auto& r : lc.refs) {
            if (r.disabled || r.hidden) {
                continue;
            }
            const glm::vec3 centre(r.sphere);
            const float t = std::clamp(glm::dot(centre - from, direction), 0.0f, distance);
            const glm::vec3 d = from + direction * t - centre;
            if (glm::dot(d, d) <= r.sphere.w * r.sphere.w) {
                out.push_back(&m_world->refs[r.ref]);
            }
        }
    }
}

uint32_t Space::sky_static_flags(uint32_t flags, uint32_t base) const {
    flags &= ~(render::k_instance_static_current | render::k_instance_static_outgoing);
    if (m_sky_static_current.contains(base)) {
        flags |= render::k_instance_static_current;
    }
    if (m_sky_static_outgoing.contains(base)) {
        flags |= render::k_instance_static_outgoing;
    }
    return flags;
}

void Space::set_sky_model(const std::string& mesh, const glm::vec3& camera, float fade) {
    if (mesh != m_sky_model_path) {
        if (m_sky_model.instance != ~0u) {
            m_renderer.device().defer([this, i = m_sky_model.instance] { m_instances.free(i); });
        }
        if (m_sky_model.mesh != render::k_invalid_mesh) {
            m_renderer.meshes().release(m_sky_model.mesh);
        }
        m_sky_model = Spawned{};
        m_sky_model_path = mesh;
        if (!mesh.empty()) {
            m_sky_model.mesh = m_renderer.meshes().acquire(mesh, 1.0f);
        }
    }
    m_sky_model.position = camera;
    m_sky_model_fade = fade;
}

void Space::set_door_shapes(std::span<const DoorShape> shapes) {
    m_door_shapes.clear();
    // A frame in flight still reads the last frame's slots, so a slot is never rewritten: a shape that hasn't changed keeps
    // its own, one that has takes a fresh one. Reused by place in the list, a door shutting would shift the rest and that
    // frame would draw it at another door's pose.
    std::vector<DoorSlot> last = std::move(m_door_slots);
    m_door_slots.clear();
    auto& meshes = m_renderer.meshes();
    for (const DoorShape& s : shapes) {
        auto [it, fresh] = m_fx_meshes.try_emplace(s.model, render::k_invalid_mesh);
        if (fresh) {
            it->second = meshes.acquire(s.model, 1.0f);
        }
        if (!meshes.ready(it->second)) {
            continue;
        }
        const auto groups = meshes.groups(it->second);
        for (uint32_t k = 0; k < groups.size(); ++k) {
            if (meshes.group(groups[k]).submesh != s.submesh) {
                continue;
            }
            render::GpuInstanceData d{};
            for (int r = 0; r < 3; ++r) {
                d.rows[r] = glm::vec4(s.rotation[0][r], s.rotation[1][r], s.rotation[2][r], s.position[r]);
            }
            const glm::vec4 local = meshes.sphere(it->second);
            d.sphere = glm::vec4(s.rotation * glm::vec3(local) + s.position, local.w);
            m_light_grid->list(glm::vec3(d.sphere), d.sphere.w, false, d.lights);
            d.leaf_frequency = -float(s.leaf);
            uint32_t instance = ~0u;
            for (DoorSlot& o : last) {
                if (o.slot != ~0u && std::memcmp(&o.data, &d, sizeof(d)) == 0) {
                    instance = std::exchange(o.slot, ~0u);
                    break;
                }
            }
            if (instance == ~0u) {
                instance = m_instances.allocate();
                if (instance == ~0u) {
                    continue;
                }
                m_instances.write(instance, d);
            }
            m_door_slots.push_back({d, instance});
            m_door_shapes.push_back({it->second, k, instance, d.sphere});
        }
    }
    std::erase_if(last, [](const DoorSlot& o) { return o.slot == ~0u; });
    if (!last.empty()) {
        m_renderer.device().defer([this, old = std::move(last)] {
            for (const DoorSlot& o : old) {
                m_instances.free(o.slot);
            }
        });
    }
}

void Space::set_moving_lights(std::span<const render::PointLight> lights) {
    const size_t first = m_world->lights.size();
    for (size_t i = 0; i < render::k_moving_lights; ++i) {
        m_point_lights[first + i] = i < lights.size() ? lights[i] : render::PointLight{};
    }
}

void Space::set_fx_shapes(std::span<const FxShape> shapes) {
    m_fx_shapes.assign(shapes.begin(), shapes.end());
}

void Space::set_rain_ripples(std::span<const glm::vec3> positions, float strength) {
    if (strength > 0.0f && m_ripple_mesh == render::k_invalid_mesh) {
        m_ripple_mesh = m_renderer.meshes().acquire("meshes/water/defaultwaterripples.osm", 1.0f);
    }
    m_ripple_positions.assign(positions.begin(), positions.end());
    m_ripple_strength = strength;
}

void Space::despawn(uint32_t id) {
    std::erase_if(m_spawned, [this, id](const Spawned& s) {
        if (s.id != id) {
            return false;
        }
        if (s.instance != ~0u) {
            m_renderer.device().defer([this, i = s.instance] { m_instances.free(i); });
        }
        m_renderer.meshes().release(s.mesh);
        return true;
    });
}

void Space::spawn(const std::string& mesh, const glm::vec3& position, const glm::quat& rotation, float scale, uint32_t id) {
    spawn(mesh, position, 0.0f, id);
    m_spawned.back().rotation = glm::mat3_cast(rotation) * scale;
}

void Space::spawn(const std::string& mesh, const glm::vec3& position, float heading, uint32_t id) {
    Spawned s;
    s.id = id;
    s.mesh = m_renderer.meshes().acquire(mesh, 0.0f);
    s.position = position;
    s.heading = heading;
    // Rotated about z by the heading (clockwise from north, as the game's z angle).
    s.rotation = glm::mat3(glm::vec3(std::cos(heading), -std::sin(heading), 0.0f),
                           glm::vec3(std::sin(heading), std::cos(heading), 0.0f), glm::vec3(0, 0, 1));
    m_spawned.push_back(s);
}

void Space::move_spawned(uint32_t id, const glm::vec3& position, const glm::mat3& rotation) {
    for (Spawned& s : m_spawned) {
        if (s.id == id) {
            s.position = position;
            s.rotation = rotation;
            s.moved = true;
        }
    }
}

void Space::set_spawned_hidden(uint32_t id, bool hidden) {
    for (Spawned& s : m_spawned) {
        if (s.id == id) {
            s.hidden = hidden;
        }
    }
}

bool Space::spawned_ready(uint32_t id) const {
    const auto& meshes = m_renderer.meshes();
    return std::ranges::all_of(m_spawned, [&](const Spawned& s) {
        return s.id != id || meshes.ready(s.mesh) || meshes.failed(s.mesh);
    });
}

void Space::build_area_draws(const render::View& view, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                             render::DrawList& out) const {
    out.clear();
    const glm::vec3 cam = view.position;
    for (uint32_t slot : m_draw_order) {
        const auto& lc = m_slots[slot];
        if (lc.cell == ~0u || lc.ready == 0) {
            continue;
        }
        const auto& c = m_world->cells[lc.cell];
        if (c.x < x0 || c.x > x1 || c.y < y0 || c.y > y1) {
            continue;
        }
        const render::Cull cull = view.frustum.aabb(lc.lo - cam, lc.hi - cam);
        if (cull == render::Cull::outside) {
            continue;
        }
        for (const auto& r : lc.refs) {
            if (r.instance == ~0u || r.disabled) {
                continue;
            }
            const glm::vec3 rel = glm::vec3(r.sphere) - cam;
            if (cull == render::Cull::intersect && !view.frustum.sphere(rel, r.sphere.w)) {
                continue;
            }
            // No directional material: the local map's objects are the base texture alone (mesh.frag).
            out.add(m_models[r.model].select(0.0f), r.instance, glm::dot(rel, rel));
        }
    }
}

bool Space::area_bounds(int32_t x0, int32_t y0, int32_t x1, int32_t y1, glm::vec3& lo, glm::vec3& hi) const {
    lo = glm::vec3(1e30f);
    hi = glm::vec3(-1e30f);
    bool any = false;
    for (const auto& lc : m_slots) {
        if (lc.cell == ~0u || lc.lo.x > lc.hi.x) {
            continue;
        }
        const auto& c = m_world->cells[lc.cell];
        if (c.x < x0 || c.x > x1 || c.y < y0 || c.y > y1) {
            continue;
        }
        lo = glm::min(lo, lc.lo);
        hi = glm::max(hi, lc.hi);
        any = true;
    }
    glm::vec3 tlo, thi;
    if (m_terrain->area_bounds(x0, y0, x1, y1, tlo, thi)) {
        lo = glm::min(lo, tlo);
        hi = glm::max(hi, thi);
        any = true;
    }
    return any;
}

render::SpaceView Space::space_view() const {
    render::SpaceView sv;
    sv.lights = m_point_lights;
    sv.door_lights = m_door_lights;
    sv.terrain = m_terrain.get();
    sv.draws = &m_draws;
    sv.instance_table = m_instances.address();
    sv.cell_size = m_world->cell_size;
    sv.environment = m_environment;
    if (m_grass) {
        m_grass->fill(sv);
    }
    if (m_far_trees) {
        sv.far_trees = m_far_trees->instances_address();
        sv.far_tree_table = m_far_trees->table_address();
        sv.far_tree_runs = m_far_tree_runs;
        sv.tree_near = m_tree_near;
    }
    if (interior()) {
        // Sky mode 1 (no Show Sky) leaves the cleared black; the light fade is the cell's or its room's, unless
        // it uses the sky's lighting (mode 3). A fade of 0 means fLightLODStartFade, an end of 0 that plus
        // fLightLODRange.
        sv.sky = (m_world->flags & osw::world_show_sky) != 0;
        if (!(m_world->flags & osw::world_use_sky_lighting) || !sv.sky) {
            sv.light_fade_begin = m_environment.light_fade_begin;
            sv.light_fade_end = m_environment.light_fade_end;
        }
    }
    return sv;
}

bool Space::busy() const {
    if (m_actors && m_actors->busy()) {
        return true;
    }
    return m_wanted_next < m_wanted.size() || !m_to_unload.empty() || !m_pending_models.empty() ||
           m_terrain->busy() > 0 || (m_far && far_busy() > 0) || (m_grass && m_grass->busy() > 0);
}

SpaceStats Space::stats() const {
    SpaceStats s;
    s.cells_total = static_cast<uint32_t>(m_world->cells.size());
    s.cells_loaded = m_loaded_count;
    s.cells_wanted = static_cast<uint32_t>(m_wanted.size() - m_wanted_next);
    s.refs_loaded = m_refs_loaded;
    s.refs_ready = m_refs_ready;
    for (const auto& m : m_models) {
        if (m.refs > 0) {
            ++s.models_loaded;
        }
    }
    s.models_pending = static_cast<uint32_t>(m_pending_models.size());
    s.drawn = static_cast<uint32_t>(m_draws.size());
    s.instance_slots = m_instances.used();
    return s;
}

} // namespace spud::engine
