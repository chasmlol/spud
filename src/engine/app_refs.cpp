// Scripted references in the frame: which cells hold them, what their scripts hear as cells come and go, and trigger
// volumes around the player.

#include "core/log.hpp"
#include "engine/app_internal.hpp"

#include "core/units.hpp"
#include "formats/forms_typed.hpp"
#include "game/game.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>

namespace spud::engine {

namespace {

// A script's heat triggers keep their own size in collision spud doesn't read for a moved reference: what the
// scripts themselves count as near.
constexpr float k_moved_trigger_units = 580.0f;

} // namespace

void App::index_script_refs() {
    // Every reference with a script of its own or on its base, by the cell it's in. Once, away from the frame.
    game::Game* g = m_game.get();
    script::VirtualMachine* vm = &m_papyrus->vm();
    m_script_refs_ready = std::async(std::launch::async, [g, vm] {
        std::unordered_map<uint32_t, std::vector<uint32_t>> cells;
        const forms::Database& db = g->db();
        // A persistent reference outside belongs to the world's persistent cell: it hears the cell it stands in.
        std::unordered_map<uint64_t, uint32_t> grid;
        auto grid_key = [](uint32_t world, int32_t x, int32_t y) {
            return uint64_t(world) << 32 | uint64_t(uint16_t(x)) << 16 | uint16_t(y);
        };
        for (const forms::FormEntry* e : db.of_type(forms::sig("CELL"))) {
            if (const auto xclc = db.field(*e, forms::sig("XCLC")); xclc && xclc->data.size() >= 8 && e->parent) {
                int32_t at[2];
                std::memcpy(at, xclc->data.data(), 8);
                grid[grid_key(e->parent, at[0], at[1])] = e->form_id;
            }
        }
        constexpr uint32_t k_persistent = 0x400;
        for (const forms::Sig type : {forms::sig("REFR"), forms::sig("ACHR")}) {
            for (const forms::FormEntry* e : db.of_type(type)) {
                const auto name = db.field(*e, forms::sig("NAME"));
                if (!vm->has_scripts(e->form_id) && !(name && vm->has_scripts(name->as<uint32_t>()))) {
                    continue;
                }
                uint32_t cell = e->parent;
                const forms::FormEntry* parent = db.find(cell);
                if (parent && parent->parent && (e->flags & k_persistent)) {
                    if (const auto placed = forms::placement(forms::Form(db, *e))) {
                        const auto x = int32_t(std::floor(placed->position[0] / 4096.0f));
                        const auto y = int32_t(std::floor(placed->position[1] / 4096.0f));
                        if (const auto it = grid.find(grid_key(parent->parent, x, y)); it != grid.end()) {
                            cell = it->second;
                        }
                    }
                }
                cells[cell].push_back(e->form_id);
            }
        }
        return cells;
    });
}

void App::reset_papyrus() {
    if (m_script_refs_ready.valid()) {
        m_script_refs = m_script_refs_ready.get(); // it reads the VM going away
    }
    m_trigger_refs.clear();
    m_animation_listeners.clear();
    m_animation_waits.clear();
    m_translations.clear();
    m_sleep_listeners.clear();
    m_alias_spells.clear();
    if (m_game) {
        m_game->script_facts = nullptr; // what scripts know goes with them
    }
    m_papyrus.reset();
}

void App::update_attached_cells(const WorldSim::Place& place, int32_t cell_x, int32_t cell_y) {
    // What scripts hear comes with the game's attached cells, the uGridsToLoad square around the player outside, not
    // with the wider streaming here.
    if (!m_papyrus || !m_game || (m_attached_space == m_space.get() && m_attached_at[0] == cell_x && m_attached_at[1] == cell_y)) {
        return;
    }
    m_attached_space = m_space.get();
    m_attached_at[0] = cell_x;
    m_attached_at[1] = cell_y;
    std::vector<uint32_t> now;
    if (place.world) {
        const int32_t half = m_grids_to_load / 2;
        for (int32_t y = cell_y - half; y <= cell_y + half; ++y) {
            for (int32_t x = cell_x - half; x <= cell_x + half; ++x) {
                if (const uint32_t cell = m_world_sim->exterior_cell(place.world, x, y)) {
                    now.push_back(cell);
                }
            }
        }
    } else if (place.cell) {
        now.push_back(place.cell);
    }
    std::ranges::sort(now);
    for (const uint32_t cell : m_attached_cells) {
        if (!std::ranges::binary_search(now, cell)) {
            cell_scripts(cell, false);
        }
    }
    for (const uint32_t cell : now) {
        if (!std::ranges::binary_search(m_attached_cells, cell)) {
            cell_scripts(cell, true);
        }
    }
    m_attached_cells = std::move(now);
}

void App::cell_scripts(uint32_t cell, bool loaded) {
    if (!m_papyrus || !m_game || m_loading_save) {
        return;
    }
    if (m_script_refs_ready.valid()) {
        m_script_refs = m_script_refs_ready.get();
    }
    const auto it = m_script_refs.find(cell);
    if (it == m_script_refs.end()) {
        return;
    }
    for (const uint32_t ref : it->second) {
        m_papyrus->ref_event(ref, loaded ? "OnCellAttach" : "OnCellDetach");
        m_papyrus->ref_event(ref, loaded ? "OnAttachedToCell" : "OnDetachedFromCell");
        if (loaded) {
            m_papyrus->ref_event(ref, "OnCellLoad");
        }
        if (!m_game->ref_disabled(ref)) { // a disabled reference has no 3D to load
            m_papyrus->ref_event(ref, loaded ? "OnLoad" : "OnUnload");
        }
    }
    if (loaded) {
        const auto& primitives = m_space->world().primitives;
        if (m_primitive_space != m_space.get()) {
            m_primitive_space = m_space.get();
            m_primitive_index.clear();
            for (uint32_t i = 0; i < primitives.size(); ++i) {
                m_primitive_index.emplace(primitives[i].form_id, i);
            }
        }
        for (const uint32_t ref : it->second) {
            if (const auto p = m_primitive_index.find(ref); p != m_primitive_index.end()) {
                // A copy, not an index: after a door the old space's triggers stay listed for a few frames.
                const osw::Primitive& q = primitives[p->second];
                TriggerRef t;
                t.ref = ref;
                t.has_box = true;
                t.sphere = q.type == osw::PrimitiveType::sphere;
                t.half = glm::vec3(q.half_extents[0], q.half_extents[1], q.half_extents[2]);
                t.position = glm::vec3(q.position[0], q.position[1], q.position[2]);
                t.rotation = glm::quat(q.rotation[3], q.rotation[0], q.rotation[1], q.rotation[2]);
                m_trigger_refs.push_back(std::move(t));
                continue;
            }
            const forms::FormEntry* e = m_game->db().find(ref);
            if (!e) {
                continue;
            }
            const forms::Form form(m_game->db(), *e);
            const forms::Value* box = form.get("XPRM");
            const auto placed = box ? forms::placement(form) : std::nullopt;
            const forms::Value* kind = box ? box->member("type") : nullptr;
            if (!placed || !kind || (kind->integer != 1 && kind->integer != 2)) {
                continue;
            }
            TriggerRef t;
            t.ref = ref;
            t.has_box = true;
            t.sphere = kind->integer == 2;
            const auto axis = [&](const char* name) { return float(box->path(std::string("bounds.") + name)->real); };
            t.half = glm::vec3(axis("x"), axis("y"), axis("z")) * units::k_skyrim_unit_m;
            t.position = glm::vec3(placed->position[0], placed->position[1], placed->position[2]) * units::k_skyrim_unit_m;
            t.rotation = glm::angleAxis(-placed->rotation[0], glm::vec3(1, 0, 0)) *
                         glm::angleAxis(-placed->rotation[1], glm::vec3(0, 1, 0)) *
                         glm::angleAxis(-placed->rotation[2], glm::vec3(0, 0, 1));
            m_trigger_refs.push_back(t);
        }
    } else {
        std::erase_if(m_trigger_refs, [&](const TriggerRef& t) {
            return std::find(it->second.begin(), it->second.end(), t.ref) != it->second.end();
        });
    }
}

void App::trigger_moved(uint32_t ref) {
    const auto it = std::find_if(m_trigger_refs.begin(), m_trigger_refs.end(), [ref](const TriggerRef& t) { return t.ref == ref; });
    if (it == m_trigger_refs.end() && m_papyrus) {
        TriggerRef t;
        t.ref = ref;
        m_trigger_refs.push_back(std::move(t));
    }
}

// The moving list of attached cells (): at least 0.05 units a step, the almost-there event once
// at 0.9 of the way, and at the end the exact target.
void App::update_translations(float dt) {
    if (!m_papyrus || !m_game || m_translations.empty()) {
        return;
    }
    script::VirtualMachine& vm = m_papyrus->vm();
    const float k_radians = 3.14159265f / 180.0f;
    std::erase_if(m_translations, [&](Translation& t) {
        const auto from = m_game->ref_position(t.ref);
        if (!from || !m_space || !m_space->ref_loaded(t.ref)) {
            return false; // waits for its cell
        }
        const std::array<float, 3> d{t.position[0] - (*from)[0], t.position[1] - (*from)[1], t.position[2] - (*from)[2]};
        const float left = std::hypot(d[0], d[1], d[2]);
        const float step = std::max(dt * t.speed, 0.05f);
        const float travel = std::min(step, left);
        const float ratio = left > 0.0f ? travel / left : 1.0f;
        std::array<float, 3> angle = m_game->ref_angle(t.ref);
        for (size_t i = 0; i < 3; ++i) {
            // The shortest way round; a limit of 0 is none.
            const float target = t.angle[i] * k_radians;
            float turn = std::remainder(target - angle[i], 2.0f * 3.14159265f) * ratio;
            if (t.max_rotation > 0.0f) {
                const float most = t.max_rotation * k_radians * dt;
                turn = std::clamp(turn, -most, most);
            }
            angle[i] += turn;
        }
        const bool done = step >= left;
        const auto position = done ? t.position : std::array<float, 3>{(*from)[0] + d[0] * ratio, (*from)[1] + d[1] * ratio, (*from)[2] + d[2] * ratio};
        if (done) {
            for (size_t i = 0; i < 3; ++i) {
                angle[i] = t.angle[i] * k_radians;
            }
        }
        m_game->changes(t.ref).angle = angle;
        m_game->move_ref(t.ref, position);
        m_ref_moved(t.ref);
        if (t.start_distance == 0.0f) {
            t.start_distance = left;
        }
        if (!t.almost_sent && !done && left - travel <= t.start_distance * 0.1f) {
            t.almost_sent = true;
            vm.send_event(script::form_handle(t.ref), "OnTranslationAlmostComplete");
        }
        if (done) {
            vm.send_event(script::form_handle(t.ref), "OnTranslationComplete");
        }
        return done;
    });
}

void App::update_triggers() {
    if (!m_papyrus || !m_space || !m_game || m_trigger_refs.empty()) {
        return;
    }
    const glm::vec3 feet = player_position_units() * units::k_skyrim_unit_m;
    const auto holds = [&](const TriggerRef& t, const glm::vec3& at) {
        if (const auto moved = m_game->moved_position(t.ref)) {
            const glm::vec3 p = glm::vec3((*moved)[0], (*moved)[1], (*moved)[2]) * units::k_skyrim_unit_m;
            return glm::distance(at, p) <= k_moved_trigger_units * units::k_skyrim_unit_m;
        }
        if (!t.has_box) {
            return false;
        }
        const glm::vec3 d = glm::conjugate(t.rotation) * (at - t.position);
        return t.sphere ? glm::length(d) <= t.half.x : glm::all(glm::lessThanEqual(glm::abs(d), t.half));
    };
    for (TriggerRef& t : m_trigger_refs) {
        const bool on = !m_game->ref_disabled(t.ref);
        const bool inside = on && holds(t, feet);
        if (inside != t.inside) {
            t.inside = inside;
            m_papyrus->trigger_event(t.ref, inside, game::k_player_ref);
        }
        // Every actor around goes through them as the player does: a stage waits for a horse.
        for (const uint32_t actor : m_high_actors) {
            const game::Agent* a = actor == game::k_player_ref ? nullptr : m_game->ai().agent(actor);
            if (!a) {
                continue;
            }
            const glm::vec3 at = glm::vec3(a->position[0], a->position[1], a->position[2]) * units::k_skyrim_unit_m;
            const bool in = on && holds(t, at);
            const auto was = std::ranges::find(t.actors, actor);
            if (in != (was != t.actors.end())) {
                in ? t.actors.push_back(actor) : (void)t.actors.erase(was);
                m_papyrus->trigger_event(t.ref, in, actor);
            }
        }
    }
}

} // namespace spud::engine
