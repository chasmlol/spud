#include "audio/ambience.hpp"

#include "audio/conditions.hpp"
#include "audio/internal.hpp"
#include "audio/music.hpp"
#include "core/log.hpp"
#include "core/units.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <unordered_map>

// Implements, "Region sounds", "Weather sounds", "Water" and
// "Placed emitters", from the game / Sky / the game / the game code.

namespace spud::audio {

namespace {

constexpr float k_unit = 0.0142875f; // metres per Skyrim unit

ReverbI3DL2 to_i3dl2(const osd::Reverb& r) {
    // The game accessors.
    ReverbI3DL2 p;
    p.wet_dry_mix = 100.0f;
    p.room = 100 * r.room_filter;
    p.room_hf = 100 * r.room_hf_filter;
    p.room_rolloff = 0.0f;
    p.decay_time = r.decay_time_ms * 0.001f;
    p.decay_hf_ratio = r.decay_hf_ratio * 0.01f;
    p.reflections = 100 * r.reflections;
    p.reflections_delay = r.reflect_delay * 0.0012f;
    p.reverb = 100 * r.reverb_amp;
    p.reverb_delay = r.reverb_delay * 0.001f;
    p.diffusion = static_cast<float>(r.diffusion);
    p.density = static_cast<float>(r.density);
    p.hf_reference = static_cast<float>(r.hf_reference_hz);
    return p;
}

// Even-odd crossing test after a bounding-box reject.
bool polygon_contains(const float* pts, uint32_t n, float x, float y) {
    if (n < 3) {
        return false;
    }
    float minx = pts[0], maxx = pts[0], miny = pts[1], maxy = pts[1];
    for (uint32_t i = 1; i < n; ++i) {
        minx = std::min(minx, pts[2 * i]);
        maxx = std::max(maxx, pts[2 * i]);
        miny = std::min(miny, pts[2 * i + 1]);
        maxy = std::max(maxy, pts[2 * i + 1]);
    }
    if (x < minx || x > maxx || y < miny || y > maxy) {
        return false;
    }
    bool inside = false;
    for (uint32_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = pts[2 * i], yi = pts[2 * i + 1], xj = pts[2 * j], yj = pts[2 * j + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
            inside = !inside;
        }
    }
    return inside;
}

} // namespace

struct Ambience::State {
    // Lookups.
    std::map<std::tuple<uint32_t, int32_t, int32_t>, uint32_t> exterior_cells;
    std::unordered_map<uint32_t, uint32_t> interior_cells;
    std::unordered_map<uint32_t, uint32_t> world_index, location_index, weather_index, water_index,
        region_index;
    uint32_t default_reverb = osd::k_none;    // DOBJ RVBT
    uint32_t underwater_loop = osd::k_none;   // DOBJ UWLS
    uint32_t underwater_reverb = osd::k_none; // DOBJ URVT

    // The player's cell.
    uint32_t cell = osd::k_none;
    bool have_cell = false;
    bool in_interior = false;

    // Acoustic spaces.
    uint32_t active_space = osd::k_none;
    uint32_t cell_space = osd::k_none;
    uint32_t ambient_loop = 0;
    int last_leave_kind = 0;                           // 1 interior, 2 exterior
    std::unordered_map<uint32_t, bool> inside_volume;  // emitter index -> inside last tick
    std::unordered_map<uint32_t, float> pending_leave; // emitter index -> seconds left (5 frames at 60 Hz)
    bool reverb_from_space = false;

    // Region sounds (the game + the game region pass).
    std::vector<osd::RegionSound> region_sounds;
    uint32_t region_music = osd::k_none;
    std::unordered_map<uint32_t, uint32_t> region_loops; // descriptor -> sound id
    double last_region_pass = -1.0;

    // Weather sounds (Sky).
    struct WeatherEntry {
        uint32_t sound = 0;
        uint32_t weather = 0; // WTHR FormID
        osd::WeatherSoundType type{};
        uint32_t descriptor = osd::k_none;
        double restart_at = 0.0;
        double next_thunder_check = 0.0;
        bool remove = false;
    };
    std::vector<WeatherEntry> weather_entries;
    uint32_t weather_current = 0, weather_last = 0;
    bool sky_on = false;
    uint32_t thunder_count = 0;

    // Water.
    uint32_t water_descriptor = osd::k_none;
    uint32_t water_ambient = 0;
    uint32_t water_samples = 0; // the last environment's, for state_json
    glm::vec3 water_emitter{0};
    uint32_t underwater_loop_sound = 0;
    bool underwater = false;

    // Placed emitters of the loaded cells.
    std::unordered_map<uint32_t, uint32_t> emitter_sounds; // emitter index -> sound id

    // The game's sound pools: NPC footsteps (8) and collisions (6).
    struct Pool {
        std::vector<uint32_t> sound;
        std::vector<float> normalized;
        std::vector<double> started;
        explicit Pool(size_t n) : sound(n, 0), normalized(n, 0.0f), started(n, 0.0) {}
    };
    Pool footstep_pool{8};
    Pool collision_pool{6};
    std::vector<std::tuple<uint32_t, uint32_t, double>> recent_collisions; // material pair, time
};

Ambience::Ambience(AudioSystem::Impl& audio) : m_audio(audio), m_state(std::make_unique<State>()) {
    auto& st = *m_state;
    const auto& db = audio.db;
    for (uint32_t i = 0; i < db.cells.size(); ++i) {
        const auto& c = db.cells[i];
        if (c.flags & osd::cell_interior) {
            st.interior_cells[c.form_id] = i;
        } else {
            st.exterior_cells[{c.world, c.x, c.y}] = i;
        }
    }
    for (uint32_t i = 0; i < db.worlds.size(); ++i) {
        st.world_index[db.worlds[i].form_id] = i;
    }
    for (uint32_t i = 0; i < db.locations.size(); ++i) {
        st.location_index[db.locations[i].form_id] = i;
    }
    for (uint32_t i = 0; i < db.weathers.size(); ++i) {
        st.weather_index[db.weathers[i].form_id] = i;
    }
    for (uint32_t i = 0; i < db.waters.size(); ++i) {
        st.water_index[db.waters[i].form_id] = i;
    }
    auto dobj = [&](const char* sig) -> uint32_t {
        for (const auto& d : db.default_objects) {
            if (std::memcmp(d.signature, sig, 4) == 0) {
                return d.form_id;
            }
        }
        return 0;
    };
    const uint32_t rvbt = dobj("RVBT"), uwls = dobj("UWLS"), urvt = dobj("URVT");
    for (uint32_t i = 0; i < db.reverbs.size(); ++i) {
        if (db.reverbs[i].form_id == rvbt) {
            st.default_reverb = i;
        }
        if (urvt && db.reverbs[i].form_id == urvt) {
            st.underwater_reverb = i;
        }
    }
    for (uint32_t i = 0; i < db.descriptors.size(); ++i) {
        if (uwls && db.descriptors[i].form_id == uwls) {
            st.underwater_loop = i;
        }
    }
}

Ambience::~Ambience() = default;

// --- helpers on the audio thread ---------------------------------------------------------------------------

struct AmbienceOps {
    Ambience::State& st;
    AudioSystem::Impl& audio;
    const osd::SoundDb& db = audio.db;

    float setting(const char* name, float fallback) const { return audio.setting(name, fallback); }

    const osd::Weather* weather(uint32_t form) const {
        auto it = st.weather_index.find(form);
        return it == st.weather_index.end() ? nullptr : &db.weathers[it->second];
    }

    ConditionContext context() const { return audio.condition_context(); }

    bool descriptor_conditions(uint32_t d) const {
        const auto& desc = db.descriptors[d];
        return evaluate(std::span(db.conditions).subspan(desc.first_condition, desc.condition_count),
                        context());
    }

    // --- acoustic spaces ---

    void set_reverb(uint32_t reverb) {
        if (reverb == osd::k_none || !audio.device) {
            return;
        }
        const auto ms = static_cast<uint32_t>(setting("fReverbTransitionTime:Audio", 0.5f) * 1000.0f);
        audio.device->transition_reverb(to_i3dl2(db.reverbs[reverb]), ms);
    }

    // The game.
    void activate(uint32_t space) {
        if (space == st.active_space) {
            return;
        }
        if (st.ambient_loop) {
            audio.fade_out_and_release(st.ambient_loop, setting("fASFadeOutTime:Audio", 8.0f));
            st.ambient_loop = 0;
        }
        const auto& a = db.acoustic_spaces[space];
        if (a.reverb != osd::k_none) {
            set_reverb(a.reverb); // a space without BNAM keeps the current reverb
        }
        if (a.ambient != osd::k_none) {
            PlayRequest r;
            r.descriptor = a.ambient;
            r.flags = 0x80;
            r.fade_in = setting("fASFadeInTime:Audio", 3.0f);
            st.ambient_loop = audio.start_sound(r);
        }
        st.active_space = space;
    }

    // The game: `space` = k_none clears whatever is active.
    void clear(uint32_t space) {
        if (st.active_space == osd::k_none || (space != osd::k_none && space != st.active_space)) {
            return;
        }
        if (st.ambient_loop) {
            audio.fade_out_and_release(st.ambient_loop, setting("fASFadeOutTime:Audio", 8.0f));
            st.ambient_loop = 0;
        }
        set_reverb(st.default_reverb);
        st.active_space = osd::k_none;
    }

    // --- cells ---

    uint32_t current_cell(glm::ivec2& grid) const {
        const auto& env = audio.env;
        if (env.interior) {
            auto it = st.interior_cells.find(env.interior);
            return it == st.interior_cells.end() ? osd::k_none : it->second;
        }
        const glm::vec3 p = audio.listener.position;
        grid = {static_cast<int>(std::floor(p.x / (4096.0f * k_unit))),
                static_cast<int>(std::floor(p.y / (4096.0f * k_unit)))};
        auto it = st.exterior_cells.find({env.world, grid.x, grid.y});
        return it == st.exterior_cells.end() ? osd::k_none : it->second;
    }

    // The game on cell enter: regions of the cell's XCLR whose area contains the
    // listener's XY, override and priority rules, the region music.
    void rebuild_regions(uint32_t cell) {
        st.region_sounds.clear();
        st.region_music = osd::k_none;
        if (cell == osd::k_none) {
            return;
        }
        const auto& c = db.cells[cell];
        if (c.flags & osd::cell_interior) {
            return; // interiors have no region list
        }
        const glm::vec3 p = audio.listener.position;
        bool have_override = false;
        uint8_t override_priority = 0;
        for (uint32_t k = 0; k < c.region_count; ++k) {
            const auto& g = db.regions[db.cell_regions[c.first_region + k]];
            if (g.world && g.world != c.world) {
                continue;
            }
            bool contains = false;
            for (uint32_t a = 0; a < g.area_count && !contains; ++a) {
                const auto& area = db.region_areas[g.first_area + a];
                contains =
                    polygon_contains(&db.region_points[2 * area.first_point], area.point_count, p.x, p.y);
            }
            if (!contains || !g.has_sounds) {
                continue;
            }
            if (g.sound_override) {
                if (!have_override || g.sound_priority > override_priority) {
                    have_override = true;
                    override_priority = g.sound_priority;
                    st.region_sounds.assign(db.region_sounds.begin() + g.first_sound,
                                            db.region_sounds.begin() + g.first_sound + g.sound_count);
                    if (g.music != osd::k_none) {
                        st.region_music = g.music; // without RDMO the earlier region music stays
                    }
                }
                continue;
            }
            if (have_override) {
                continue;
            }
            st.region_sounds.insert(st.region_sounds.end(), db.region_sounds.begin() + g.first_sound,
                                    db.region_sounds.begin() + g.first_sound + g.sound_count);
            if (g.music != osd::k_none &&
                (st.region_music == osd::k_none ||
                 db.music_types[g.music].priority < db.music_types[st.region_music].priority)) {
                st.region_music = g.music;
            }
        }
    }

    uint32_t location_music(uint32_t location) const {
        for (int depth = 0; location && depth < 32; ++depth) {
            auto it = st.location_index.find(location);
            if (it == st.location_index.end()) {
                return osd::k_none;
            }
            const auto& l = db.locations[it->second];
            if (l.music != osd::k_none) {
                return l.music;
            }
            location = l.parent;
        }
        return osd::k_none;
    }

    // The game: XCMO, location music (with parents), then for exteriors the
    // region music, then the worldspace's ZNAM.
    uint32_t cell_music(uint32_t cell) const {
        if (cell == osd::k_none) {
            return osd::k_none;
        }
        const auto& c = db.cells[cell];
        if (c.music != osd::k_none) {
            return c.music;
        }
        const osd::World* world = nullptr;
        if (auto it = st.world_index.find(c.world); it != st.world_index.end()) {
            world = &db.worlds[it->second];
        }
        uint32_t location = c.location;
        if (!location && world && !(c.flags & osd::cell_interior)) {
            location = world->location;
        }
        if (auto m = location_music(location); m != osd::k_none) {
            return m;
        }
        if (c.flags & osd::cell_interior) {
            return osd::k_none;
        }
        if (st.region_music != osd::k_none) {
            return st.region_music;
        }
        return world ? world->music : osd::k_none;
    }

    void on_cell_change(uint32_t from, uint32_t to) {
        // Leave.
        if (from != osd::k_none) {
            const auto& c = db.cells[from];
            if (c.acoustic_space != osd::k_none) {
                st.cell_space = osd::k_none;
                clear(c.acoustic_space);
            }
            st.last_leave_kind = (c.flags & osd::cell_interior) ? 1 : 2;
            st.region_sounds.clear();
        }
        // Enter.
        const bool interior = to != osd::k_none && (db.cells[to].flags & osd::cell_interior);
        if ((st.last_leave_kind == 1 && !interior) || (st.last_leave_kind == 2 && interior)) {
            clear(osd::k_none);
        }
        st.cell_space = to != osd::k_none ? db.cells[to].acoustic_space : osd::k_none;
        st.last_leave_kind = 0;
        st.in_interior = interior;
        rebuild_regions(to);
        audio.music->set_cell_music(cell_music(to));
    }

    // --- placed emitters: the loaded cells (uGridsToLoad around the player outside, the interior inside) ---

    void loaded_cells(std::vector<uint32_t>& out) const {
        out.clear();
        if (st.cell == osd::k_none) {
            return;
        }
        const auto& c = db.cells[st.cell];
        if (c.flags & osd::cell_interior) {
            out.push_back(st.cell);
            return;
        }
        const int half = static_cast<int>(setting("uGridsToLoad:General", 5.0f)) / 2;
        for (int dy = -half; dy <= half; ++dy) {
            for (int dx = -half; dx <= half; ++dx) {
                auto it = st.exterior_cells.find({c.world, c.x + dx, c.y + dy});
                if (it != st.exterior_cells.end()) {
                    out.push_back(it->second);
                }
            }
        }
    }
};

void Ambience::update(float dt) {
    auto& st = *m_state;
    AmbienceOps ops{st, m_audio};
    const auto& db = m_audio.db;
    const auto& env = m_audio.env;

    // 1. The player's cell.
    glm::ivec2 grid{};
    const uint32_t cell = ops.current_cell(grid);
    if (!st.have_cell || cell != st.cell) {
        ops.on_cell_change(st.have_cell ? st.cell : osd::k_none, cell);
        st.cell = cell;
        st.have_cell = true;
    }

    std::vector<uint32_t> loaded;
    ops.loaded_cells(loaded);

    // 2. Acoustic space trigger volumes: XPRM boxes, half extents = the bounds, rotated with the reference.
    std::unordered_map<uint32_t, bool> inside_now;
    for (uint32_t ci : loaded) {
        const auto& c = db.cells[ci];
        for (uint32_t k = 0; k < c.emitter_count; ++k) {
            const uint32_t ei = c.first_emitter + k;
            const auto& e = db.emitters[ei];
            if (e.kind != osd::EmitterKind::acoustic_space || e.acoustic_space == osd::k_none ||
                (e.flags & osd::emitter_initially_disabled) || e.primitive_type == osd::primitive_none) {
                continue;
            }
            const glm::quat q(e.rotation[3], e.rotation[0], e.rotation[1], e.rotation[2]);
            const glm::vec3 local =
                glm::inverse(q) *
                (m_audio.listener.position - glm::vec3(e.position[0], e.position[1], e.position[2]));
            // A sphere stores (r, r, r): it is a cube too.
            const bool inside = std::fabs(local.x) <= e.primitive_bounds[0] &&
                                std::fabs(local.y) <= e.primitive_bounds[1] &&
                                std::fabs(local.z) <= e.primitive_bounds[2];
            inside_now[ei] = inside;
            const bool was = st.inside_volume[ei];
            if (inside && !was) {
                if (st.pending_leave.erase(ei) == 0) {
                    ops.activate(e.acoustic_space); // enter: the latest event wins
                }
            } else if (!inside && was) {
                st.pending_leave[ei] = 5.0f / 60.0f; // leave after 5 frames
            }
        }
    }
    st.inside_volume = std::move(inside_now);
    for (auto it = st.pending_leave.begin(); it != st.pending_leave.end();) {
        it->second -= dt;
        if (it->second <= 0.0f) {
            ops.clear(db.emitters[it->first].acoustic_space);
            it = st.pending_leave.erase(it);
        } else {
            ++it;
        }
    }
    // The cell default comes back whenever nothing is active.
    if (st.active_space == osd::k_none && st.cell_space != osd::k_none) {
        ops.activate(st.cell_space);
    }

    // 3. Region sounds: one pass about every 500 ms.
    std::vector<osd::RegionSound> list = st.region_sounds;
    if (st.active_space != osd::k_none) {
        // An active space's RDAT region replaces the list (any cell kind).
        const uint32_t region_form = db.acoustic_spaces[st.active_space].region;
        for (const auto& g : db.regions) {
            if (region_form && g.form_id == region_form && g.has_sounds) {
                list.assign(db.region_sounds.begin() + g.first_sound,
                            db.region_sounds.begin() + g.first_sound + g.sound_count);
            }
        }
    }
    if (list.empty()) {
        for (auto& [d, id] : st.region_loops) {
            m_audio.fade_out_and_release(id, 0.3f);
        }
        st.region_loops.clear();
    } else if (m_audio.time - st.last_region_pass > 0.5) {
        st.last_region_pass = m_audio.time;
        int budget = static_cast<int>(ops.setting("uRegionSoundsAllowedPerUpdate:Audio", 1.0f));
        const float base = ops.setting("fRegionRandomSoundPlacementBase:Audio", 100.0f);
        const float offset = ops.setting("fRegionSoundPlacementRandomOffset:Audio", 650.0f);
        const float zoffset = ops.setting("fRegionSoundPlacementZOffset:Audio", 256.0f);
        uint8_t classification = 0;
        if (const auto* w = ops.weather(env.weather)) {
            classification = w->data[11] & 0x0F;
        }
        std::vector<uint32_t> in_list;
        for (const auto& rs : list) {
            if (rs.descriptor == osd::k_none) {
                continue;
            }
            in_list.push_back(rs.descriptor);
            const auto& d = db.descriptors[rs.descriptor];
            // Playable: an output model, a max distance over fRegionRandomSoundPlacementBase when it
            // attenuates, the SNDR's conditions, and the weather filter.
            bool playable = d.output_model != osd::k_none;
            if (playable) {
                const auto& om = db.output_models[d.output_model];
                if ((om.flags & osd::output_attenuates_with_distance) && !(om.max_distance > base * k_unit)) {
                    playable = false;
                }
            }
            playable = playable && ops.descriptor_conditions(rs.descriptor);
            if (playable && (rs.flags & 0xFFFF) && classification && !(rs.flags & classification)) {
                playable = false;
            }
            if (rs.chance >= 1.0f) {
                auto it = st.region_loops.find(rs.descriptor);
                const bool running = it != st.region_loops.end();
                if (!running && playable && budget > 0) {
                    PlayRequest r;
                    r.descriptor = rs.descriptor;
                    r.flags = 0x80;
                    r.priority = 192;
                    r.fade_in = ops.setting("fRegionLoopFadeInTime:Audio", 6.0f);
                    if (auto id = m_audio.start_sound(r)) {
                        st.region_loops[rs.descriptor] = id;
                    }
                    --budget;
                } else if (running && !playable && m_audio.sound_valid(it->second)) {
                    m_audio.fade_out_and_release(it->second,
                                                 ops.setting("fRegionLoopFadeOutTime:Audio", 8.0f));
                    st.region_loops.erase(it);
                }
                continue;
            }
            if (!playable || budget <= 0) {
                continue;
            }
            if (rs.chance * 0.5f >= m_audio.random01()) {
                // Placement relative to the listener: x +/- U[base, offset), y +/- base exactly
                // (a third random draw is discarded), z + zoffset.
                const float u1 = m_audio.random01(), u2 = m_audio.random01();
                (void)m_audio.random01();
                const float u4 = m_audio.random01();
                const glm::vec3 l = m_audio.listener.position;
                const float dx = (base + u1 * (offset - base)) * (u2 <= 0.5f ? -1.0f : 1.0f);
                const float dy = base * (u4 <= 0.5f ? -1.0f : 1.0f);
                PlayRequest r;
                r.descriptor = rs.descriptor;
                r.flags = 0x90;
                r.priority = 192;
                r.position = l + glm::vec3(dx, dy, zoffset) * k_unit;
                m_audio.start_sound(r);
                --budget;
            }
        }
        // Loops whose sound left the list fade out.
        for (auto it = st.region_loops.begin(); it != st.region_loops.end();) {
            if (std::find(in_list.begin(), in_list.end(), it->first) == in_list.end()) {
                m_audio.fade_out_and_release(it->second, ops.setting("fRegionLoopFadeOutTime:Audio", 8.0f));
                it = st.region_loops.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 4. Weather sounds: exteriors and "Show Sky" interiors only.
    const bool sky_on = st.cell == osd::k_none || !(db.cells[st.cell].flags & osd::cell_interior) ||
                        (db.cells[st.cell].flags & osd::cell_show_sky);
    if (!sky_on && st.sky_on) {
        for (auto& e : st.weather_entries) {
            m_audio.stop_sound(e.sound); // released at once, no fade
        }
        st.weather_entries.clear();
        st.thunder_count = 0;
    }
    st.sky_on = sky_on;
    if (sky_on) {
        auto start_weather = [&](uint32_t form) {
            const auto* w = ops.weather(form);
            if (!w) {
                return;
            }
            for (uint32_t k = 0; k < w->sound_count; ++k) {
                const auto& ws = db.weather_sounds[w->first_sound + k];
                if (ws.descriptor == osd::k_none || (ws.type == osd::WeatherSoundType::precipitation &&
                                                     ops.setting("bPrecipitation:Weather", 1.0f) == 0.0f)) {
                    continue;
                }
                const bool exists =
                    std::any_of(st.weather_entries.begin(), st.weather_entries.end(), [&](const auto& e) {
                        return e.descriptor == ws.descriptor && e.weather == form && e.type == ws.type;
                    });
                if (!exists) {
                    st.weather_entries.push_back({0, form, ws.type, ws.descriptor, 0.0, 0.0, false});
                    st.thunder_count += ws.type == osd::WeatherSoundType::thunder;
                }
            }
        };
        // Entries marked last update go now.
        for (auto it = st.weather_entries.begin(); it != st.weather_entries.end();) {
            if (it->remove) {
                m_audio.stop_sound(it->sound);
                st.thunder_count -= it->type == osd::WeatherSoundType::thunder;
                it = st.weather_entries.erase(it);
            } else {
                ++it;
            }
        }
        if (env.weather != st.weather_current) {
            st.weather_current = env.weather;
            start_weather(env.weather);
        }
        if (env.last_weather != st.weather_last) {
            st.weather_last = env.last_weather;
            start_weather(env.last_weather);
        }
        const float pct = std::clamp(env.weather_pct, 0.0f, 1.0f);
        const auto* cur = ops.weather(env.weather);
        const auto* last = ops.weather(env.last_weather);
        const float k = 0.999f / 255.0f;
        const float a = cur ? cur->data[6] * k : 0.0f;
        const float b = last ? last->data[7] * k + 0.001f : 0.0f;
        const float ta = cur ? cur->data[8] * k : 0.0f;
        const float tb = last ? last->data[9] * k + 0.001f : 0.0f;
        for (auto& e : st.weather_entries) {
            const bool is_current = e.weather == env.weather && env.weather != 0;
            const bool is_last = e.weather == env.last_weather && env.last_weather != 0;
            if (!is_current && !is_last) {
                m_audio.stop_sound(e.sound); // cut when the transition completes, no fade
                e.remove = true;
                continue;
            }
            float volume = 0.0f;
            switch (e.type) {
            case osd::WeatherSoundType::normal:
            case osd::WeatherSoundType::wind:
                volume = is_current ? pct : 1.0f - pct;
                break;
            case osd::WeatherSoundType::precipitation:
                volume = is_current ? (pct >= a && a < 1.0f ? (pct - a) / (1.0f - a) : 0.0f)
                                    : (pct < b ? 1.0f - pct / b : 0.0f);
                break;
            case osd::WeatherSoundType::thunder:
                volume = is_current ? (pct >= ta && ta < 1.0f ? (pct - ta) / (1.0f - ta) : 0.0f)
                                    : (pct < tb ? 1.0f - pct / tb : 0.0f);
                break;
            }
            if (e.type == osd::WeatherSoundType::thunder) {
                if (m_audio.sound_valid(e.sound) || m_audio.time < e.next_thunder_check) {
                    continue;
                }
                e.next_thunder_check = m_audio.time + 0.030;
                const auto* w = ops.weather(e.weather);
                const uint32_t n = w ? w->data[10] * st.thunder_count : 0;
                if (n == 0 || m_audio.random_below(n) != 0 || volume <= 0.0001f) {
                    continue; // a frequency of 0 divides by zero in the game; here it never thunders
                }
                PlayRequest r;
                r.descriptor = e.descriptor;
                r.flags = 0x0A;
                r.volume = volume;
                e.sound = m_audio.start_sound(r);
                continue;
            }
            if (!m_audio.sound_valid(e.sound) && m_audio.time >= e.restart_at) {
                PlayRequest r;
                r.descriptor = e.descriptor;
                r.flags = 0x80A; // forced loop, 2D fallback
                r.volume = std::max(volume, 1e-5f);
                e.sound = m_audio.start_sound(r);
                e.restart_at = m_audio.time + 1.0;
            }
            m_audio.sound_set_volume(e.sound, volume, 0.01f);
        }
    }

    // 5. Water ambience: the engine samples the wet points (like WaterAudio::Update); one ambient instance.
    uint32_t water_desc = osd::k_none;
    if (!st.in_interior && env.water) {
        if (auto it = st.water_index.find(env.water); it != st.water_index.end()) {
            water_desc = db.waters[it->second].sound;
        }
    }
    if (water_desc != st.water_descriptor && st.water_ambient) {
        m_audio.fade_out_and_release(st.water_ambient, ops.setting("fWaterAudioFadeOutSeconds:Audio", 5.0f));
        st.water_ambient = 0;
    }
    st.water_descriptor = water_desc;
    st.water_samples = env.water_samples;
    st.water_emitter = env.water_emitter;
    if (water_desc != osd::k_none && env.water_samples > 0) {
        const uint32_t n = static_cast<uint32_t>(ops.setting("uWaterAudioAxisSamplePoints:Audio", 8.0f));
        const float volume = static_cast<float>(env.water_samples) / static_cast<float>(2 * n);
        if (!m_audio.sound_valid(st.water_ambient)) {
            PlayRequest r;
            r.descriptor = water_desc;
            r.position = env.water_emitter;
            r.volume = volume;
            r.fade_in = ops.setting("fWaterAudioFadeInSeconds:Audio", 3.0f);
            st.water_ambient = m_audio.start_sound(r);
        } else {
            m_audio.sound_set_position(st.water_ambient, env.water_emitter);
            m_audio.sound_set_volume(st.water_ambient, volume, 0.0f);
        }
    } else if (st.water_ambient && (st.in_interior || env.water_samples == 0)) {
        m_audio.fade_out_and_release(st.water_ambient, ops.setting("fWaterAudioFadeOutSeconds:Audio", 5.0f));
        st.water_ambient = 0;
    }

    // Underwater: the loop, categories that mute when submerged, the underwater reverb (none in vanilla).
    if (env.underwater != st.underwater) {
        st.underwater = env.underwater;
        // The game: categories flagged "Mute When Submerged" mute over 100 ms.
        for (uint32_t i = 0; i < db.categories.size(); ++i) {
            if (db.categories[i].flags & osd::category_mute_when_submerged) {
                m_audio.category_mute(i, env.underwater, 0.1f);
            }
        }
        if (env.underwater) {
            if (st.underwater_loop != osd::k_none) {
                PlayRequest r;
                r.descriptor = st.underwater_loop;
                r.flags = 0x10;
                st.underwater_loop_sound = m_audio.start_sound(r);
            }
            ops.set_reverb(st.underwater_reverb);
        } else {
            m_audio.stop_sound(st.underwater_loop_sound);
            st.underwater_loop_sound = 0;
            // the active space's, else the default.
            const uint32_t r =
                st.active_space != osd::k_none ? db.acoustic_spaces[st.active_space].reverb : osd::k_none;
            ops.set_reverb(r != osd::k_none ? r : st.default_reverb);
        }
    }

    // 6. Placed looping emitters: start when their cell is loaded, fade out over 1 s when it unloads.
    std::unordered_map<uint32_t, bool> wanted;
    for (uint32_t ci : loaded) {
        const auto& c = db.cells[ci];
        for (uint32_t k = 0; k < c.emitter_count; ++k) {
            const uint32_t ei = c.first_emitter + k;
            const auto& e = db.emitters[ei];
            if (e.kind == osd::EmitterKind::acoustic_space || e.descriptor == osd::k_none ||
                (e.flags & osd::emitter_initially_disabled)) {
                continue;
            }
            wanted[ei] = true;
            if (st.emitter_sounds.contains(ei)) {
                continue;
            }
            PlayRequest r;
            r.descriptor = e.descriptor;
            if (e.primitive_type == osd::primitive_line && e.kind == osd::EmitterKind::sound_marker) {
                r.position = glm::vec3(e.line_start[0], e.line_start[1], e.line_start[2]);
                r.segment_end = glm::vec3(e.line_end[0], e.line_end[1], e.line_end[2]);
                r.flags = 0x4000;
            } else {
                r.position = glm::vec3(e.position[0], e.position[1], e.position[2]);
            }
            // The game builds the sound once, when the reference's 3D
            // loads: a sound whose conditions fail then stays silent until the reference loads again (id 0
            // kept).
            st.emitter_sounds[ei] = m_audio.start_sound(r);
        }
    }
    for (auto it = st.emitter_sounds.begin(); it != st.emitter_sounds.end();) {
        if (!wanted.contains(it->first)) {
            m_audio.fade_out_and_release(it->second, 1.0f);
            it = st.emitter_sounds.erase(it);
        } else {
            ++it;
        }
    }
}

namespace {

const osd::Impact* find_impact(const osd::SoundDb& db, uint32_t impact_set, uint32_t material) {
    if (impact_set >= db.impact_sets.size()) {
        return nullptr;
    }
    const auto& set = db.impact_sets[impact_set];
    for (uint32_t k = 0; k < set.count; ++k) {
        const auto& pair = db.impact_pairs[set.first + k];
        if (pair.material == material) {
            return pair.impact != osd::k_none ? &db.impacts[pair.impact] : nullptr;
        }
    }
    return nullptr;
}

} // namespace

// The game: an axis-aligned range test around the listener, then the
// first free slot, else the oldest slot whose sound is relatively farther (d^2 / max^2), else nothing.
static void play_pooled(AudioSystem::Impl& audio, Ambience::State::Pool& pool, uint32_t descriptor,
                        const glm::vec3& position) {
    const auto& d = audio.db.descriptors[descriptor];
    float max_distance = std::numeric_limits<float>::max();
    if (d.output_model != osd::k_none) {
        const auto& om = audio.db.output_models[d.output_model];
        if (om.flags & osd::output_attenuates_with_distance) {
            max_distance = om.max_distance;
        }
    }
    const glm::vec3 delta = position - audio.listener.position;
    if (!(std::fabs(delta.x) < max_distance && std::fabs(delta.y) < max_distance &&
          std::fabs(delta.z) < max_distance)) {
        return;
    }
    const float max_units = std::max(max_distance / k_unit, 1.0f);
    const float n = glm::dot(delta, delta) / (k_unit * k_unit) / (max_units * max_units);
    int slot = -1;
    for (size_t i = 0; i < pool.sound.size() && slot < 0; ++i) {
        if (!audio.sound_valid(pool.sound[i])) {
            slot = static_cast<int>(i);
        }
    }
    if (slot < 0) {
        for (size_t i = 0; i < pool.sound.size(); ++i) {
            if (pool.normalized[i] > n && (slot < 0 || pool.started[i] < pool.started[slot])) {
                slot = static_cast<int>(i);
            }
        }
        if (slot < 0) {
            return;
        }
        audio.stop_sound(pool.sound[slot]);
    }
    PlayRequest r;
    r.descriptor = descriptor;
    r.position = position;
    r.flags = 0x10;
    pool.sound[slot] = audio.start_sound(r);
    pool.normalized[slot] = n;
    pool.started[slot] = audio.time;
}

void Ambience::footstep(uint32_t footstep_set, osd::FootstepGait gait, std::string_view tag,
                        uint32_t material, const glm::vec3& position, bool player, bool first_person) {
    const auto& db = m_audio.db;
    if (footstep_set == osd::k_none) {
        // DOBJ DFTS: DefaultFootstepSet.
        const uint32_t form = m_audio.default_object("DFTS");
        for (uint32_t i = 0; i < db.footstep_sets.size(); ++i) {
            if (db.footstep_sets[i].form_id == form) {
                footstep_set = i;
            }
        }
    }
    if (footstep_set >= db.footstep_sets.size()) {
        return;
    }
    const auto& set = db.footstep_sets[footstep_set];
    for (uint32_t k = 0; k < set.count[gait]; ++k) {
        const uint32_t fi = db.footstep_lists[set.first[gait] + k];
        if (fi == osd::k_none || !path_equal(db.str(db.footsteps[fi].tag), tag)) {
            continue;
        }
        const auto* imp = find_impact(db, db.footsteps[fi].impact_set, material);
        if (!imp) {
            return;
        }
        if (player) {
            // The game: the player plays Sound 2 when there is one,
            // attached to the player's 3D, whose node output model replaces the SNDR's (1st / 3rd person,
            // DOBJ POPM / P3OM).
            PlayRequest r;
            r.position = position;
            r.flags = 0x10;
            if (imp->sound2 != osd::k_none) {
                r.descriptor = imp->sound2;
                const uint32_t form = m_audio.default_object(first_person ? "POPM" : "P3OM");
                for (uint32_t i = 0; i < db.output_models.size(); ++i) {
                    if (db.output_models[i].form_id == form) {
                        r.output_override = i;
                    }
                }
            } else {
                r.descriptor = imp->sound1;
            }
            if (r.descriptor != osd::k_none) {
                m_audio.start_sound(r);
            }
        } else if (imp->sound1 != osd::k_none) {
            play_pooled(m_audio, m_state->footstep_pool, imp->sound1, position);
        }
        return; // the first footstep with the tag decides
    }
}

void Ambience::impact(uint32_t impact_set, uint32_t material, const glm::vec3& position) {
    // PlayImpactEffect: both sounds at the impact point.
    const auto* imp = find_impact(m_audio.db, impact_set, material);
    if (!imp) {
        return;
    }
    for (uint32_t s : {imp->sound1, imp->sound2}) {
        if (s != osd::k_none) {
            PlayRequest r;
            r.descriptor = s;
            r.position = position;
            r.flags = 0x10;
            m_audio.start_sound(r);
        }
    }
}

void Ambience::combat_impact(uint32_t impact_set, uint32_t material, const glm::vec3& position,
                             bool player_attacker, bool normal_hit) {
    // The game: Sound 1, or Sound 2 for the player's normal hits; unpooled.
    const auto* imp = find_impact(m_audio.db, impact_set, material);
    if (!imp) {
        return;
    }
    PlayRequest r;
    r.descriptor = player_attacker && normal_hit && imp->sound2 != osd::k_none ? imp->sound2 : imp->sound1;
    r.position = position;
    r.flags = 0x10;
    if (r.descriptor != osd::k_none) {
        m_audio.start_sound(r);
    }
}

void Ambience::collision(uint32_t material_a, uint32_t material_b, const glm::vec3& position, float speed) {
    // The game and the game: at least fMinSoundVel
    // units/s, the same pair (either order) at most every iCollisionSoundTimeDelta ms, then (A's IPDS, B's
    // material) and (B's IPDS, A's material), each with Sound 2 at fCollisionSoundHeavyThreshold and above,
    // through the 6-slot pool.
    const auto& db = m_audio.db;
    auto& st = *m_state;
    const float units = speed / k_unit;
    if (units < m_audio.setting("fMinSoundVel:Audio", 60.0f)) {
        return;
    }
    const double now = m_audio.time;
    const double window = m_audio.setting("iCollisionSoundTimeDelta:Audio", 150.0f) / 1000.0;
    std::erase_if(st.recent_collisions, [&](const auto& c) { return now - std::get<2>(c) > window; });
    for (const auto& [a, b, t] : st.recent_collisions) {
        if ((a == material_a && b == material_b) || (a == material_b && b == material_a)) {
            return;
        }
    }
    st.recent_collisions.emplace_back(material_a, material_b, now);
    auto material = [&](uint32_t form) -> const osd::Material* {
        for (const auto& m : db.materials) {
            if (m.form_id == form) {
                return &m;
            }
        }
        return nullptr;
    };
    const auto* ma = material(material_a);
    const auto* mb = material(material_b);
    if (!ma || !mb) {
        return;
    }
    const bool heavy = units >= m_audio.setting("fCollisionSoundHeavyThreshold:Audio", 160.0f);
    for (auto [set_of, other] : {std::pair{ma, mb}, std::pair{mb, ma}}) {
        const auto* imp = find_impact(db, set_of->havok_impact_set, other->form_id);
        if (!imp) {
            continue;
        }
        const uint32_t sound = heavy && imp->sound2 != osd::k_none ? imp->sound2 : imp->sound1;
        if (sound != osd::k_none) {
            play_pooled(m_audio, st.collision_pool, sound, position);
        }
    }
}

bool Ambience::path_equal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

std::string Ambience::state_json() const {
    const auto& st = *m_state;
    const auto& db = m_audio.db;
    auto edid = [&](const auto& items, uint32_t i) {
        return i == osd::k_none ? std::string() : std::string(db.str(items[i].edid));
    };
    return std::format(
        "{{\"cell\": \"{}\", \"interior\": {}, \"acoustic_space\": \"{}\", \"cell_space\": \"{}\", "
        "\"region_sounds\": {}, \"region_loops\": {}, \"region_music\": \"{}\", \"weather_sounds\": {}, "
        "\"emitters\": {}, \"water\": {}, \"water_samples\": {}, "
        "\"water_emitter\": [{:.1f}, {:.1f}, {:.1f}]}}",
        st.cell == osd::k_none ? std::string() : std::string(db.str(db.cells[st.cell].edid)),
        st.in_interior ? "true" : "false", edid(db.acoustic_spaces, st.active_space),
        edid(db.acoustic_spaces, st.cell_space), st.region_sounds.size(), st.region_loops.size(),
        edid(db.music_types, st.region_music), st.weather_entries.size(), st.emitter_sounds.size(),
        st.water_ambient != 0 ? "true" : "false", st.water_samples,
        st.water_emitter.x / units::k_skyrim_unit_m, st.water_emitter.y / units::k_skyrim_unit_m,
        st.water_emitter.z / units::k_skyrim_unit_m);
}

} // namespace spud::audio
