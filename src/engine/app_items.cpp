// The player's items in the world: the game's item state (src/game), what the crosshair points at, the Activate key,
// and the menus' game side.

#include "core/file.hpp"
#include "core/path.hpp"
#include "engine/app_internal.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/units.hpp"
#include "engine/game_natives.hpp"
#include "engine/item_menus.hpp"
#include "formats/forms_typed.hpp"
#include "game/game.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>

namespace spud::engine {

namespace {

// fActivatePickLength:Interface (180 in the image): how far the player reaches to activate.
constexpr float k_activate_m = 180.0f * units::k_skyrim_unit_m;
constexpr float k_pick_margin_m = 0.1f; // a surface this far in front of a target's box doesn't hide it
constexpr float k_door_depth_m = 0.2f;  // a door's front stands up to this far before its doorway
constexpr uint32_t k_player_npc = 0x7;  // the Player NPC_: both of the player's bodies are actors
constexpr float k_placed_radius_m = 0.25f;

// Where a line passes an actor's upright axis, metres along the line, and the two points that close.
struct AxisPass {
    glm::vec3 on_line, on_axis;
    float along;
};

AxisPass pass_axis(const glm::vec3& from, const glm::vec3& dir, float length, const glm::vec3& foot, float low,
                   float high) {
    const glm::vec2 flat(dir), to_axis = glm::vec2(foot) - glm::vec2(from);
    const float flat2 = glm::dot(flat, flat);
    float along = flat2 > 1e-8f ? std::clamp(glm::dot(to_axis, flat) / flat2, 0.0f, length) : 0.0f;
    // Past either end the axis's end is what is nearest: closest on the axis, then back on the line.
    glm::vec3 on_axis(foot.x, foot.y, std::clamp(from.z + dir.z * along, foot.z + low, foot.z + high));
    along = std::clamp(glm::dot(on_axis - from, dir), 0.0f, length);
    on_axis.z = std::clamp(from.z + dir.z * along, foot.z + low, foot.z + high);
    return {from + dir * along, on_axis, along};
}

// The ray's entry into a reference's OBND, metres from `from`; nullopt when it misses.
std::optional<float> ray_box(const osw::Ref& r, const std::array<float, 6>& bounds, const glm::vec3& from,
                             const glm::vec3& dir) {
    const glm::quat q(r.rotation[3], r.rotation[0], r.rotation[1], r.rotation[2]);
    const glm::quat inv = glm::conjugate(q);
    const float scale = r.scale * units::k_skyrim_unit_m;
    const glm::vec3 o = inv * (from - glm::vec3(r.position[0], r.position[1], r.position[2])) / scale;
    const glm::vec3 d = inv * dir / scale;
    float t0 = 0.0f, t1 = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-8f) {
            if (o[i] < bounds[size_t(i)] || o[i] > bounds[size_t(i) + 3]) {
                return std::nullopt;
            }
            continue;
        }
        float a = (bounds[size_t(i)] - o[i]) / d[i], b = (bounds[size_t(i) + 3] - o[i]) / d[i];
        if (a > b) {
            std::swap(a, b);
        }
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
    }
    return t0 <= t1 ? std::optional<float>(t0) : std::nullopt;
}

uint32_t parse_form(const std::string& s) {
    uint32_t v = 0;
    const char* b = s.data() + (s.starts_with("0x") ? 2 : 0);
    if (std::from_chars(b, s.data() + s.size(), v, 16).ec != std::errc()) {
        throw Error("not a FormID: " + s);
    }
    return v;
}

} // namespace

void App::start_game() {
    if (m_game) {
        return;
    }
    try {
        const auto start = Clock::now();
        m_game = std::make_unique<game::Game>(m_opt.cache / "forms.osf");
        wire_world_fx();
        m_game->projectiles().before_player_shot = [this](game::LaunchData& shot) { try_ranged_kill_camera(shot); };
        m_game->projectiles().after_player_shot = [this](uint32_t id) {
            if (m_kill_camera.active && m_kill_camera.ranged && !m_kill_camera.projectile) {
                m_kill_camera.projectile = id;
                m_game->projectiles().tracked = id;
            }
        };
        m_game->vats().area_free = [this](uint32_t ref, int side) { return vats_area_free(ref, side); };
        m_game->vats().target_visible = [this](uint32_t ref, uint32_t target, int side) { return vats_target_visible(ref, target, side); };
        m_item_menus = std::make_unique<ItemMenus>(*m_game, *m_ui);
        m_item_menus->on_drop = [this](const game::Stack& s) { drop_item(s); };
        m_game->crime().witnesses = [this] { return crime_witnesses(); };
        m_game->crime().pass_hours = [this](double hours) { m_clock.pass_hours(float(hours)); };
        m_character_menus = std::make_unique<CharacterMenus>(*m_game, [this](const char* s) { ui_play_sound(s); });
        m_face_host = std::make_unique<PlayerFace>(*this);
        m_character_menus->set_face_host(m_face_host.get());
        m_game->character().start();
        connect_game();
        log::info("game: items ready in {:.0f} ms", ms_between(start, Clock::now()));
    } catch (const std::exception& e) {
        log::warn("game: no item system: {}", e.what()); // a cache without forms.osf (skytool import writes it)
    }
}

void App::connect_game() {
    connect_combat();
    if (!m_game || !m_papyrus) {
        return;
    }
    bind_game_natives(m_papyrus->vm(), *m_game);
    bind_character_natives(m_papyrus->vm(), *m_game, *m_ui);
    ReferenceHost host;
    host.player_position = [this] {
        const glm::vec3 u = player_position_units();
        return std::array<float, 3>{u.x, u.y, u.z};
    };
    host.player_cell = [this] { return m_game->world().player_cell; };
    host.loaded = [this](uint32_t ref) { return ref == game::k_player_ref || m_game->placed_base(ref) != 0 || std::ranges::find(m_high_actors, ref) != m_high_actors.end() || (m_space && m_space->ref_loaded(ref)); };
    host.activate = [this](uint32_t ref, uint32_t activator, bool default_only) {
        const game::Target t = m_game->inspect(ref);
        if (default_only) { // the event has been: this is the script asking for what it blocked
            if (activator == game::k_player_ref && t.kind == game::TargetKind::furniture) {
                furniture_default(t);
            }
            return;
        }
        m_papyrus->activate(ref, t.base, activator);
        activate_children(ref);
    };
    host.play_form_sound = [this](uint32_t sound) -> uint32_t {
        return m_audio ? uint32_t(m_audio->play(sound)) : 0;
    };
    host.weather = [this](int what) {
        return !m_sky ? 0u : what == 0 ? m_sky->weather() : m_sky->outgoing_weather();
    };
    host.weather_transition = [this] { return m_sky ? m_sky->transition() : 1.0f; };
    host.current_location = [this] { return m_game->world().player_location; };
    host.say_favor = [this](uint32_t actor, uint16_t subtype) {
        game::Dialogue* dialogue = m_papyrus ? m_papyrus->dialogue() : nullptr;
        constexpr uint8_t k_category_favors = 4;
        if (game::Info* line = dialogue ? dialogue->pick_by_subtype(k_category_favors, subtype, actor, game::k_player_ref) : nullptr; line && line->topic) {
            dialogue->say(actor, line->topic->form, game::k_player_ref);
        }
    };
    host.move_player = [this](const std::array<float, 3>& p) {
        m_script_moves.push_back({0, p, false, 0, false});
    };
    host.move_player_to = [this](uint32_t target, const std::array<float, 3>& at, bool match, uint32_t stack) {
        m_script_moves.push_back({target, at, match, stack, true});
    };
    host.inventory_filter = [this](script::Handle handle, uint32_t form, int what) {
        if (what > 0) {
            m_papyrus->add_inventory_filter(handle, form);
        } else if (what == 0) {
            m_papyrus->remove_inventory_filter(handle, form);
        } else {
            m_papyrus->clear_inventory_filters(handle);
        }
    };
    host.place = [this](uint32_t base, const std::array<float, 3>& p) {
        const uint32_t id = m_game->place_form(base, p);
        const forms::FormEntry* e = m_game->db().find(base);
        if (e && (e->type == forms::sig("NPC_") || e->type == forms::sig("LVLN"))) {
            m_game->ai().add(id, base, player_space(), p, 0.0f, 1.0f); // the roster starts it
            m_game->script_event(id, "OnLoad"); // binds its base's scripts too: it has no record to find them by
            return id;
        }
        const game::Item* item = m_game->items().find(base);
        const glm::vec3 spot = glm::vec3(p[0], p[1], p[2]) * units::k_skyrim_unit_m;
        if (const std::string model = item ? item->model : base_model(base); !model.empty() && m_space) {
            if (ActorSet* actors = m_space->actors(); !actors || !actors->add_object(id, base, model, spot)) {
                m_space->spawn(model, spot, 0.0f, id);
            }
        }
        m_game->script_event(id, "OnLoad"); // and its base's scripts: a caller casts the result to them at once
        return id;
    };
    host.moved = m_ref_moved = [this](uint32_t ref) {
        trigger_moved(ref);
        const auto position = m_game->ref_position(ref);
        if (m_space && position) {
            const auto angle = m_game->ref_angle(ref);
            // As the importer turns a placement (world_import.cpp euler_to_quat).
            const glm::quat q = glm::angleAxis(-angle[0], glm::vec3(1, 0, 0)) * glm::angleAxis(-angle[1], glm::vec3(0, 1, 0)) *
                                glm::angleAxis(-angle[2], glm::vec3(0, 0, 1));
            m_space->move_ref(ref, glm::vec3((*position)[0], (*position)[1], (*position)[2]) * units::k_skyrim_unit_m, q);
        }
    };
    host.remove = [this](uint32_t ref) {
        if (m_space) {
            m_space->despawn(ref);
            if (ActorSet* actors = m_space->actors()) {
                actors->remove_object(ref);
            }
            if (m_physics) {
                m_physics->remove_object(ref);
            }
            m_space->set_ref_hidden(ref, true);
        }
        m_game->forget_ref(ref);
    };
    host.register_sleep = [this](script::Handle handle, bool on) {
        if (on) {
            m_sleep_listeners.insert(handle);
        } else {
            m_sleep_listeners.erase(handle);
        }
    };
    host.havok_impulse = [this](uint32_t ref, const std::array<float, 3>& direction, float magnitude) {
        havok_impulse(ref, direction, magnitude);
    };
    host.damage_object = [this](uint32_t ref, float amount) { damage_ref(ref, amount, false); };
    host.play_gamebryo = [this](uint32_t ref, std::string_view name, bool start_over, float ease_in) {
        return play_gamebryo(ref, name, start_over, ease_in);
    };
    host.trap_hit = [this](uint32_t target, uint32_t trap, float damage, float pushback, const glm::vec3& velocity, const glm::vec3& position) {
        trap_hit(target, trap, damage, pushback, velocity, position);
    };
    host.expression_override = [this](uint32_t ref, int index, float strength) {
        if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
            index < 0 ? actors->clear_expression_override(ref) : actors->set_expression_override(ref, index, strength);
        }
    };
    host.clear_destruction = [this](uint32_t ref) {
        clear_destruction(ref);
    };
    host.destruction_stage = [this](uint32_t ref) { return m_destruction ? m_destruction->stage(ref) : 0; };
    host.kill = [this](uint32_t ref) {
        ActorSet* actors = m_space ? m_space->actors() : nullptr;
        return actors && actors->kill(ref);
    };
    host.is_dead = [this](uint32_t ref) { // a corpse of the world's, loaded or not
        ActorSet* actors = m_space ? m_space->actors() : nullptr;
        return (actors && actors->dead(ref)) || (m_world_sim && m_world_sim->ref_dead(ref));
    };
    host.weapon_drawn = [this](uint32_t ref) {
        ActorSet* actors = m_space ? m_space->actors() : nullptr;
        return actors && actors->weapon_drawn(ref == game::k_player_ref ? ActorSet::k_player_body : ref);
    };
    host.resurrect = [this](uint32_t ref) {
        ActorSet* actors = m_space ? m_space->actors() : nullptr;
        if (actors && !actors->resurrect(ref)) {
            return false;
        }
        if (m_world_sim) {
            m_world_sim->set_ref_dead(ref, false);
        }
        return true;
    };
    host.redress = [this](uint32_t ref) {
        if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
            actors->restart(ref);
        }
    };
    host.npc_equip = [this, redress = host.redress](uint32_t ref, uint32_t form, bool on) {
        if (m_actor_data) {
            m_actor_data->npc_equip(ref, form, on);
        }
        redress(ref);
        if (const auto seat = m_seat_idles.find(ref); seat != m_seat_idles.end()) {
            m_vehicle_idles[ref] = seat->second; // a new body sits down again
        }
        const std::vector<game::Game::ScriptArg> args = {{form, "Form"}, {0, "ObjectReference"}};
        m_game->script_event(ref, on ? "OnObjectEquipped" : "OnObjectUnequipped", args);
    };
    host.npc_bare = [this, redress = host.redress](uint32_t ref) {
        if (m_actor_data) {
            m_actor_data->npc_bare(ref);
        }
        redress(ref);
    };
    const auto actors_of = [this] { return m_space ? m_space->actors() : nullptr; };
    host.animation_event = [actors_of](uint32_t ref, std::string_view event) {
        ActorSet* actors = actors_of();
        return actors && actors->send_event(ref == game::k_player_ref ? ActorSet::k_player_body : ref, event);
    };
    host.wait_animation = [this](uint32_t ref, std::string_view event, uint32_t stack) {
        m_animation_waits.push_back({ref, path::to_lower(event), stack});
    };
    host.look_at = [this](uint32_t actor, uint32_t target) {
        if (target) {
            m_look_at[actor] = target;
        } else if (m_look_at.erase(actor)) {
            if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
                actors->set_look_target(actor, nullptr);
            }
        }
    };
    host.play_idle = [this, actors_of](uint32_t ref, std::string_view idle) {
        ActorSet* actors = actors_of();
        if (m_vehicles.contains(ref)) {
            m_seat_idles[ref] = std::string(idle);
        }
        if (actors && actors->play_idle(ref == game::k_player_ref ? ActorSet::k_player_body : ref, idle)) {
            return true;
        }
        if (m_vehicles.contains(ref)) { // a body that isn't there yet takes its seat when it is
            m_vehicle_idles[ref] = std::string(idle);
            return true;
        }
        return false;
    };
    host.graph_variable = [actors_of](uint32_t ref, std::string_view name, float value) {
        ActorSet* actors = actors_of();
        return actors && actors->set_variable(ref == game::k_player_ref ? ActorSet::k_player_body : ref, name, value);
    };
    host.register_animation = [this](script::Handle handle, uint32_t sender, std::string_view event, bool on) {
        const std::string tag = path::to_lower(event);
        std::erase_if(m_animation_listeners, [&](const AnimationListener& l) {
            return l.handle == handle && l.sender == sender && l.event == tag;
        });
        if (on) {
            m_animation_listeners.push_back({handle, sender, tag});
        }
    };
    m_help.post = [this](const std::string& text) {
        if (m_ui) {
            m_ui->hud_tutorial_hint(text);
        }
    };
    host.help_message = [this](const std::string& event, const std::string& text, float duration, float interval, int times) {
        m_help.show(event, text, duration, interval, times);
    };
    host.help_reset = [this](const std::string& event) { m_help.reset(event); };
    host.open_training = [this](uint32_t trainer) {
        m_trainer = trainer;
        m_ui->open("Training Menu");
    };
    host.open_gift = [this](uint32_t ref, bool giving, uint32_t filter, bool stolen, std::function<void(int)> done) {
        m_item_menus->open_gift(m_game->base_of(ref), m_game->inspect(ref).name, giving, filter, stolen, std::move(done));
    };
    host.register_los = [this](uint64_t handle, uint32_t viewer, uint32_t target, int type) {
        const auto it = std::ranges::find_if(m_los, [&](const LosRecord& r) {
            return r.handle == handle && r.viewer == viewer && r.target == target;
        });
        if (it != m_los.end()) {
            it->type = type;
        } else {
            m_los.push_back({handle, viewer, target, type, 0});
        }
    };
    host.unregister_los = [this](uint64_t handle, uint32_t viewer, uint32_t target) {
        std::erase_if(m_los, [&](const LosRecord& r) { return r.handle == handle && r.viewer == viewer && r.target == target; });
    };
    host.critical_end = [this](uint32_t ref) {
        if (m_space) {
            m_space->set_ref_disabled(ref, true); // the 3D goes; the reference stays, as its ash pile leads to it
            if (ActorSet* actors = m_space->actors()) {
                actors->drop_body(ref);
            }
        }
    };
    host.shake_camera = [this](uint32_t source, float strength, float duration) {
        const auto at = m_game && source ? m_game->ref_position(source) : std::nullopt;
        shake_camera(at ? glm::vec3((*at)[0], (*at)[1], (*at)[2])
                        : (m_player ? m_player->feet() : m_camera.position) / units::k_skyrim_unit_m,
                     strength, duration);
    };
    host.apply_modifier = [this](const std::string& edid, float strength, float fade_in) {
        if (m_sky) {
            m_sky->apply_modifier(edid, strength, fade_in);
        }
    };
    host.remove_modifier = [this](const std::string& edid, float fade_out) {
        if (m_sky) {
            m_sky->remove_modifier(edid, fade_out);
        }
    };
    host.stop_sound = [this](uint32_t id) {
        if (m_audio) {
            m_audio->stop(audio::SoundId(id));
        }
    };
    // What the AI holds is where an actor is: a move it doesn't hear is undone on the next tick. Target 0: this space.
    host.move_agent = [this](uint32_t ref, uint32_t target, const std::array<float, 3>& at) {
        const game::Agent* a = m_game->ai().agent(ref);
        if (!a) {
            return;
        }
        uint32_t space = a->space;
        WorldSim::Place place;
        glm::vec3 where;
        if (target && ref_place(target, place, where)) {
            space = place.world ? place.world : place.cell;
        }
        m_game->ai().place_at(ref, space, game::NavPoint{at[0], at[1], at[2]}, a->heading);
        // What it was doing was for where it was: a low-process actor would be sent straight back.
        m_game->ai().evaluate(ref);
    };
    host.tether = [this](uint32_t cart, uint32_t horse) { tether_cart(cart, horse); };
    host.hold_cart = [this](uint32_t cart, bool held) { hold_cart(cart, held); };
    host.set_vehicle = [this](uint32_t rider, uint32_t vehicle) { set_vehicle(rider, vehicle); };
    host.hud_mode = [this](const char* mode, bool on) {
        if (m_ui) {
            m_ui->hud_mode(mode, on);
        }
    };
    host.sitting_rotation = [this](float degrees) { m_sitting_rotation = glm::radians(degrees); };
    host.first_person_geometry = [this](bool show) { m_first_person_geometry = show; };
    host.sound_volume = [this](uint32_t id, float volume) {
        if (m_audio) {
            m_audio->set_volume(audio::SoundId(id), volume);
        }
    };
    host.particles = [this](uint32_t spgd, float fade, bool on) {
        if (m_sky) {
            on ? m_sky->apply_particles(spgd, fade) : m_sky->remove_particles(spgd, fade);
        }
    };
    host.cell_attached = [this](uint32_t cell) { return m_world_sim && m_world_sim->cell_attached(cell); };
    host.mass = [this](uint32_t ref) { return m_physics ? m_physics->ref_mass(ref) : 0.0f; };
    host.find_weather = [this](int type) { return m_sky ? m_sky->find_weather(type) : 0u; };
    host.sky_mode = [this] { return m_space && m_space->interior() ? 1 : 3; }; // Sky: 3 is the whole sky
    host.quit_to_menu = [this] { return_to_main_menu(); };
    host.force_weather = [this](uint32_t weather) {
        if (m_sky) {
            m_sky->force_weather(weather);
        }
    };
    host.release_weather = [this] {
        if (m_sky) {
            m_sky->release_override();
        }
    };
    host.force_view = [this](bool first_person) {
        if (m_player) {
            m_player->set_view(first_person ? View::first_person : View::third_person);
        }
    };
    host.fade = [this](bool fade_out, bool black, float wait, float seconds) {
        m_ui->request_fade({fade_out, black, double(seconds), double(wait)});
    };
    host.open_barter = [this](uint32_t npc) { m_item_menus->open_vendor(npc); };
    host.open_inventory = [this](uint32_t ref) {
        m_item_menus->open_container(ref, m_game->inspect(ref).name, false);
        m_ui->open("ContainerMenu");
        container_open(ref, true);
    };
    host.trigger_count = [this](uint32_t ref) {
        return int32_t(std::ranges::count_if(m_trigger_refs, [ref](const TriggerRef& t) { return t.ref == ref && t.inside; }));
    };
    host.translate = [this](const Translation& t) {
        if (!m_space || !m_space->ref_loaded(t.ref)) {
            log::warn("papyrus: Failed to setup moving reference because it has no parent cell or no 3D");
            m_papyrus->vm().send_event(script::form_handle(t.ref), "OnTranslationFailed");
            return;
        }
        std::erase_if(m_translations, [&t](const Translation& other) { return other.ref == t.ref; });
        m_translations.push_back(t);
    };
    host.stop_translation = [this](uint32_t ref) {
        // Stopping counts as failing.
        if (std::erase_if(m_translations, [ref](const Translation& t) { return t.ref == ref; }) > 0) {
            m_papyrus->vm().send_event(script::form_handle(ref), "OnTranslationFailed");
        }
    };
    host.node_offset = [actors_of](uint32_t ref, std::string_view node) {
        ActorSet* actors = actors_of();
        const glm::vec3 offset = actors ? actors->node_offset(ref, path::to_lower(node)) : glm::vec3(0.0f);
        return std::array<float, 3>{offset.x / units::k_skyrim_unit_m, offset.y / units::k_skyrim_unit_m, offset.z / units::k_skyrim_unit_m};
    };
    m_game->on_outfit = [this](uint32_t npc, uint32_t outfit) {
        if (m_actor_data) {
            m_actor_data->set_outfit(npc, outfit);
        }
    };
    m_game->ref_dead = host.is_dead;
    m_game->ref_loaded = host.loaded;
    m_game->dialogue_speaker = [this] { return m_papyrus && m_papyrus->dialogue() ? m_papyrus->dialogue()->speaker_ref() : 0u; };
    m_papyrus->host().runtime_actor = [this](uint32_t form) {
        const forms::FormEntry* e = m_game->db().find(m_game->placed_base(form));
        return e && (e->type == forms::sig("NPC_") || e->type == forms::sig("LVLN"));
    };
    host.in_interior = [this] { return m_space && m_space->interior(); };
    host.bool_event = [this](uint32_t ref, const std::string& event, bool value) { m_papyrus->ref_event(ref, event, {script::Value::of(value)}); };
    host.graph_float = [this](uint32_t ref, const std::string& variable) {
        ActorSet* actors = m_space ? m_space->actors() : nullptr;
        int32_t out = 0;
        return actors && actors->get_int(ref == game::k_player_ref ? ActorSet::k_player_body : ref, variable, out) ? float(out) : 0.0f;
    };
    host.refs_near = [this](const std::array<float, 3>& at, float radius, std::vector<uint32_t> bases, bool actors_only) {
        std::vector<std::pair<float, uint32_t>> found;
        const glm::vec3 centre(at[0], at[1], at[2]);
        if (actors_only) {
            for (const ActorSet::Who& a : m_space && m_space->actors() ? m_space->actors()->who() : std::vector<ActorSet::Who>{}) {
                if (const auto p = m_game->ref_position(a.ref); p && a.ref != game::k_player_ref) {
                    const float d = glm::length(glm::vec3((*p)[0], (*p)[1], (*p)[2]) - centre);
                    if (d <= radius) {
                        found.emplace_back(d, a.ref);
                    }
                }
            }
        } else if (m_space) {
            for (const osw::Ref& r : m_space->world().refs) {
                if (std::ranges::find(bases, r.base_form_id) != bases.end() && !m_game->ref_disabled(r.form_id)) {
                    const float d = glm::length(glm::vec3(r.position[0], r.position[1], r.position[2]) / units::k_skyrim_unit_m - centre);
                    if (d <= radius) {
                        found.emplace_back(d, r.form_id);
                    }
                }
            }
        }
        std::ranges::sort(found);
        std::vector<uint32_t> out;
        for (const auto& [d, ref] : found) {
            out.push_back(ref);
        }
        return out;
    };
    host.using_gamepad = [this] { return m_user.number("bGamepadEnable:Controls") != 0.0f; };
    host.player_ai_driven = [this](bool driven) { m_player_ai_driven = driven; };
    host.set_weather = [this](uint32_t weather, bool accelerate) {
        if (m_sky) {
            m_sky->set_weather(weather, accelerate);
        }
    };
    host.music = [this](uint32_t music, bool add) {
        if (m_audio) {
            add ? m_audio->music_add(music) : m_audio->music_remove(music);
        }
    };
    host.location_loaded = [this](uint32_t location) { return m_game->world().player_location == location; };
    bind_actor_natives(m_papyrus->vm(), *m_game, host);
    bind_misc_natives(m_papyrus->vm(), *m_game, host);
    bind_presentation_natives(m_papyrus->vm(), *m_game, host);
    bind_reference_natives(m_papyrus->vm(), *m_game, std::move(host));
    ensure_world_sim();
    bind_world_natives();
    connect_quest_world();
    m_game->on_item_moved = [this](uint32_t form, int32_t count, uint32_t from, uint32_t to) {
        m_papyrus->item_moved(form, count, from, to);
        // What's taken off a corpse leaves the body.
        ActorSet* actors = m_space ? m_space->actors() : nullptr;
        const game::Item* item = m_game->items().find(form);
        const bool gear = item && (item->kind == game::ItemKind::armor || item->kind == game::ItemKind::weapon);
        if (gear && actors && from && actors->dead(from) && m_game->container(from).count(form) == 0 && m_actor_data) {
            m_actor_data->npc_equip(from, form, false);
            actors->restart(from);
        }
    };
    index_script_refs();
    m_game->on_read = [this](uint32_t book, uint32_t ref) {
        ref ? m_papyrus->ref_event(ref, "OnRead") : m_papyrus->read_event(book);
    };
    m_game->projectiles().on_bow_shot = [this](const game::Item* weapon, const game::Item* ammo, float power) {
        if (weapon->animation_type == 9) { // a crossbow's shot sends none
            return;
        }
        script::VirtualMachine& vm = m_papyrus->vm();
        m_papyrus->player_event("OnPlayerBowShot", {vm.form_value(weapon->form, "Weapon"), vm.form_value(ammo ? ammo->form : 0, "Ammo"),
                                                    script::Value::of(power), script::Value::of(false)});
    };
    m_game->on_equipped = [this](uint32_t form) {
        m_papyrus->player_event("OnObjectEquipped", {m_papyrus->vm().form_value(form, "Form"), m_papyrus->vm().form_value(0, "ObjectReference")});
        m_papyrus->item_event(game::k_player_ref, form, "OnEquipped", {m_papyrus->vm().form_value(game::k_player_ref, "Actor")});
    };
    m_game->on_unequipped = [this](uint32_t form) {
        m_papyrus->player_event("OnObjectUnequipped", {m_papyrus->vm().form_value(form, "Form"), m_papyrus->vm().form_value(0, "ObjectReference")});
        m_papyrus->item_event(game::k_player_ref, form, "OnUnequipped", {m_papyrus->vm().form_value(game::k_player_ref, "Actor")});
    };
    m_game->on_script_event = [this](uint32_t ref, std::string_view event, std::vector<game::Game::ScriptArg> forms, std::optional<int32_t> number) {
        std::vector<script::Value> args;
        for (const game::Game::ScriptArg& f : forms) {
            args.push_back(m_papyrus->vm().form_value(f.form, f.type));
        }
        if (number) {
            args.push_back(script::Value::of(*number));
        }
        m_papyrus->ref_event(ref, event, std::move(args));
    };
    m_papyrus->host().created_base = [this](uint32_t ref) { return m_game->base_of(ref); };
    m_papyrus->effects_on = [this](uint32_t ref) {
        std::vector<uint32_t> ids;
        for (const game::ActiveEffect& e : m_game->magic_target(ref).effects()) {
            ids.push_back(e.id);
        }
        return ids;
    };
    m_game->on_ref_state = [this](uint32_t ref, bool disabled) {
        if (m_world_sim) {
            m_world_sim->set_marker_disabled(ref, disabled);
        }
        m_game->ai().set_disabled(ref, disabled);
        if (m_space) {
            m_space->set_ref_disabled(ref, disabled);
        }
        if (m_physics) {
            m_physics->set_ref_disabled(ref, disabled);
        }
        m_game->ai().roster_changed();
    };
    m_papyrus->conditions().set_facts(m_game.get());
    m_game->script_facts = m_papyrus.get();
    game::CharacterEvents& events = m_game->character().events;
    events.start_quest = [this](uint32_t quest, uint16_t stage) {
        if (game::Quest* q = m_papyrus->quests()->quest(quest); q && m_papyrus->quests()->start(*q, true)) {
            m_papyrus->quests()->set_stage(*q, stage);
        }
    };
    events.perk_fragment = [this](uint32_t target, uint32_t perk, int fragment) {
        m_papyrus->run_perk_fragment(target, perk, fragment);
    };
    events.read_global = [this](uint32_t global) {
        float value = 0;
        m_papyrus->global_value(global, value);
        return value;
    };
    events.write_global = [this](uint32_t global, float value) { m_papyrus->set_global(global, value); };
    events.effect_started = [this](const game::ActiveEffect& e) { m_papyrus->effect_started(e.id, e.effect, e.caster, e.target); };
    events.effect_finished = [this](const game::ActiveEffect& e) { m_papyrus->effect_finished(e.id, e.caster, e.target); };
    // The new body comes with Actors; until it does the scripts go on at once.
    events.race_changed = [this](uint32_t) { m_papyrus->player_event("OnRaceSwitchComplete"); };
    for (const game::ActiveEffect& e : m_game->character().effects()) {
        if (e.started && e.setting->has_scripts && !m_loading_save) { // what took hold before scripts were up
            m_papyrus->effect_started(e.id, e.effect, e.caster);
        }
    }
}

void App::update_character(float dt) {
    if (!m_game) {
        return;
    }
    game::Character& c = m_game->character();
    game::WorldState& world = m_game->world();
    world.game_hour = m_clock.hour();
    if (m_sky) {
        world.weather = m_sky->weather();
        world.outgoing_weather = m_sky->outgoing_weather();
    }
    if (m_space) {
        m_space->set_ref_overrides(&m_game->ref_overrides());
        if (m_physics.get() != m_physics_seen) { // a new world's collision hears what changed so far
            m_physics_seen = m_physics.get();
            for (const auto& [ref, disabled] : m_game->ref_overrides()) {
                if (m_physics) {
                    m_physics->set_ref_disabled(ref, disabled);
                }
            }
        }
        int32_t cell_x = 0, cell_y = 0;
        const WorldSim::Place place = m_world_sim ? player_place(cell_x, cell_y) : WorldSim::Place{};
        world.player_cell = place.cell;
        world.player_worldspace = place.world;
        world.player_interior = m_space->interior();
        const glm::vec3 u = player_position_units();
        world.player_position = {u.x, u.y, u.z};
        world.player_heading = m_player ? m_player->yaw() : 0.0f;
        world.player_first_person = m_player && m_player->view() == View::first_person;
        world.player_race = m_game->character().race();
        world.player_female = m_game->character().female();
        world.player_swimming = m_player && m_player->status().swimming;
        world.player_sneaking = m_player && m_player->status().sneaking;
    }
    if (m_papyrus) {
        m_papyrus->world() = world;
    }
    // The movement simulation runs the body: its health and stamina are the character's, and its limits come from it.
    const bool body = m_walk && m_player;
    if (body) {
        const glm::vec2 change = m_player->take_body_change();
        if (m_body_health >= 0.0f) {
            // On a mount the stamina the simulation runs is the mount's.
            c.body_changed(change.x, m_player->riding() ? 0.0f : change.y);
            // Health gone by the body's own hurts, a fall or drowning, is death like any other (Actor::Kill).
            game::Combat& combat = m_game->combat();
            if (c.values().current(game::av_health) <= 0.0f && combat.fighter(game::k_player_ref).life == game::LifeState::alive) {
                combat.kill(game::k_player_ref, 0);
            }
            if (const uint32_t falls = m_player->status().falls_hurt; falls != m_falls_seen) {
                m_falls_seen = falls;
                player_pain(true, true);
            }
        }
    }
    m_game->update_worn_effects();
    m_game->flush_item_moves();
    update_triggers();
    update_translations(dt);
    if (m_papyrus && (m_alias_spell_timer -= dt) <= 0.0f) {
        // CastAliasSpellVisitor: an alias on the player holds its spells for as long as its quest runs.
        m_alias_spell_timer = 1.0f;
        std::vector<uint32_t> spells;
        for (const auto& q : m_papyrus->quests()->all_quests()) {
            if (q->running_bit()) {
                for (const game::Alias& a : q->aliases) {
                    const auto filled = q->filled_refs.find(a.id);
                    if (!a.spells.empty() && filled != q->filled_refs.end() && filled->second == game::k_player_ref) {
                        spells.insert(spells.end(), a.spells.begin(), a.spells.end());
                    }
                }
            }
        }
        for (const uint32_t s : m_alias_spells) {
            if (std::find(spells.begin(), spells.end(), s) == spells.end()) {
                c.dispel_spell(s);
            }
        }
        for (const uint32_t s : spells) {
            // A load brings them back with the save's effects.
            if (std::find(m_alias_spells.begin(), m_alias_spells.end(), s) == m_alias_spells.end() &&
                !c.magic_target().has_effect_from(s)) {
                c.apply(s);
            }
        }
        m_alias_spells = std::move(spells);
    }
    if (m_character_menus && m_character_menus->sleep_level() && !m_ui->is_open("StatsMenu")) {
        m_character_menus->set_sleep_level(false);
    }
    c.update(dt, false, body);
    if (body) {
        const game::ActorValues& v = c.values();
        MovementTuning& t = m_player->tuning();
        t.max_health = v.maximum(game::av_health);
        t.heal_rate = v.current(game::av_heal_rate);
        m_body_health = v.current(game::av_health);
        m_body_stamina = v.current(game::av_stamina);
        if (m_player->riding()) {
            m_player->set_body(m_body_health, m_player->status().stamina);
        } else {
            t.max_stamina = v.maximum(game::av_stamina);
            t.stamina_rate = v.current(game::av_stamina_rate);
            t.stamina_rate_mult = v.current(game::av_stamina_rate_mult);
            m_player->set_body(m_body_health, m_body_stamina);
        }
    } else {
        m_body_health = -1.0f;
    }
}

ui::ItemSource* App::ui_item_source() { return m_item_menus.get(); }
ui::CharacterSource* App::ui_character() { return m_character_menus.get(); }
ui::RaceSexSource* App::ui_race_sex() { return m_character_menus.get(); }

int App::ui_beast_form() {
    if (!m_game || !m_game->character().beast_form()) {
        return 0;
    }
    return m_character_menus->werewolf() ? 1 : 2;
}

void App::ui_rest_started(bool sleeping, double hours) {
    if (sleeping && m_papyrus) {
        const float now = float(m_clock.days_passed());
        m_papyrus->send_sleep(m_sleep_listeners, "OnSleepStart", {script::Value::of(now), script::Value::of(now + float(hours) / 24.0f)});
    }
}

void App::ui_rest_ended(bool sleeping, bool interrupted) {
    if (!sleeping) {
        return;
    }
    if (m_papyrus) {
        m_papyrus->send_sleep(m_sleep_listeners, "OnSleepStop", {script::Value::of(interrupted)});
    }
    // with Survival Mode on a level waits for the sleep.
    if (m_game && m_game->survival() && m_game->character().can_level_up()) {
        m_character_menus->set_sleep_level(true);
        m_ui->open("StatsMenu");
    }
}

bool App::ui_survival(double& temperature, double& cold, double& hunger, double& exhaustion) {
    if (!m_game || !m_game->survival() || !m_papyrus) {
        return false;
    }
    // Default objects 331 to 334 (Update.esm 01002EDD to 01002EE0).
    const auto read = [this](uint32_t id) {
        float v = 0;
        m_papyrus->global_value(id, v);
        return double(v);
    };
    temperature = read(0x01002EDD);
    cold = read(0x01002EDE);
    hunger = read(0x01002EDF);
    exhaustion = read(0x01002EE0);
    return true;
}

bool App::ui_meter_percent(int actor_value, double& percent) {
    if (actor_value == game::av_stamina && m_mount.ref && m_player) { // the bar is the mount's in the saddle
        percent = 100.0 * m_player->status().stamina / std::max(m_player->tuning().max_stamina, 1.0f);
        return true;
    }
    if (!m_game) {
        return false;
    }
    const game::ActorValues& v = m_game->character().values();
    const float current = v.current(actor_value), maximum = current - v.modifier(actor_value, game::Modifier::damage);
    percent = maximum == 0.0f ? 0.0 : std::max(current, 0.0f) / maximum * 100.0;
    return true;
}
ui::CraftingSource* App::ui_crafting_source() { return m_item_menus.get(); }

bool App::ui_book(ui::BookInfo& book) {
    if (!m_item_menus) {
        return false;
    }
    book = m_item_menus->book();
    return book.present;
}

uint32_t App::pick_loose(float range_m) {
    if (!m_game || !m_space || !m_physics) {
        return 0;
    }
    const glm::vec3 eye = m_camera.position, dir = m_camera.forward();
    m_pick_refs.clear();
    m_space->refs_on_ray(eye, dir, range_m, m_pick_refs);
    uint32_t best = 0;
    float best_t = range_m;
    for (const osw::Ref* r : m_pick_refs) {
        const std::array<float, 6>* bounds = m_game->activatable(r->base_form_id);
        const auto t = bounds ? ray_box(*r, *bounds, eye, dir) : std::nullopt;
        if (t && *t < best_t && m_physics->ref_dynamic(r->form_id)) {
            best = r->form_id;
            best_t = *t;
        }
    }
    return best;
}

uint32_t App::pick_ref() {
    if (!m_game || !m_space) {
        return 0;
    }
    const glm::vec3 eye = m_camera.position, dir = m_camera.forward();
    const auto solid_before = [&](float t) {
        const auto hit = m_physics->raycast(eye, dir, t);
        return hit && hit->distance + k_pick_margin_m < t;
    };
    m_pick_refs.clear();
    m_space->refs_on_ray(eye, dir, k_activate_m, m_pick_refs);
    uint32_t best = 0;
    float best_t = k_activate_m;
    for (const osw::Ref* r : m_pick_refs) {
        const std::array<float, 6>* bounds = m_game->activatable(r->base_form_id);
        if (!bounds) {
            continue;
        }
        // An activator with no name offers nothing, and its bounds can span a street (WRMainRoadPlainsRiver01ACT).
        const auto t = ray_box(*r, *bounds, eye, dir);
        if (t && *t < best_t && m_game->inspect(r->form_id).kind != game::TargetKind::none) {
            best = r->form_id;
            best_t = *t;
        }
    }
    // A door that is open, or hung from its other side, has no reference drawn: its doorway answers for it.
    float door_t = 0.0f;
    if (const uint32_t door = aimed_hidden_door(door_t)) {
        door_t = std::max(door_t - k_door_depth_m, 0.0f);
        if (door_t < best_t) {
            best = door;
            best_t = door_t;
        }
    }
    // Actors, as the pick has them: a line through the controller's hull is a hit. A reference's box reaches
    // further than its shape, so the body comes first unless something solid is in front of it (the rack an NPC stands
    // at, the chair behind one). A line that misses by less than fActivatePickRadius, the sphere swept down it, is a
    // near miss for later.
    m_pick_actor = {};
    ActorSet::Entry near_actor;
    glm::vec3 near_point(0.0f);
    if (ActorSet* actors = m_space->actors()) {
        const float sweep = m_game->settings().number("fActivatePickRadius", 16.0f) * units::k_skyrim_unit_m;
        float nearest = std::numeric_limits<float>::max(), actor_t = k_activate_m;
        for (const ActorSet::Who& a : actors->who()) {
            // No NPC: an animated object, picked as the reference it is. A corpse's body is its ragdoll's.
            if (a.npc == k_player_npc || !a.npc || actors->dead(a.ref)) {
                continue;
            }
            const std::array<float, 3> half = m_game->body_half(a.npc);
            const float radius = 0.5f * (half[0] + half[1]) * units::k_skyrim_unit_m +
                                 physics::CharacterSettings{}.convex_radius;
            const float height = 2.0f * half[2] * units::k_skyrim_unit_m;
            const AxisPass pass = pass_axis(eye, dir, k_activate_m, a.position, radius,
                                            std::max(height - radius, radius));
            const glm::vec3 off = pass.on_line - pass.on_axis;
            const float miss = glm::length(off);
            const ActorSet::Entry who{a.ref, a.npc, a.position, {}, {}};
            if (miss <= radius) {
                const float t = std::max(pass.along - std::sqrt(radius * radius - miss * miss), 0.0f);
                if (t < actor_t && (!m_physics || !solid_before(t))) {
                    best = a.ref;
                    best_t = actor_t = t;
                    m_pick_actor = who;
                }
            } else if (miss <= radius + sweep && miss < nearest) {
                nearest = miss;
                near_actor = who;
                near_point = pass.on_axis + off * (radius / miss);
            }
        }
        // A corpse is its ragdoll: the first thing on the ray, as the game's pick finds it on the biped layers.
        if (const auto hit = m_physics ? m_physics->ragdoll_on_ray(eye, dir, k_activate_m) : std::nullopt) {
            for (const ActorSet::Who& a : actors->who()) {
                if (a.npc && a.npc != k_player_npc && actors->ragdoll_of(a.ref) == hit->first) {
                    best = a.ref;
                    best_t = hit->second;
                    m_pick_actor = ActorSet::Entry{a.ref, a.npc, a.position, {}, {}};
                }
            }
        }
    }
    // What the player dropped lies in no cell's references: a bound the size of a hand.
    for (const auto& [id, placed] : m_game->placed()) {
        const glm::vec3 centre(placed.position[0], placed.position[1], placed.position[2]);
        const float t = glm::dot(centre - eye, dir);
        const float miss = glm::length(eye + dir * t - centre);
        if (t > 0.0f && t < best_t && miss < k_placed_radius_m) {
            best = id;
            best_t = t;
            m_pick_actor = {};
        }
    }
    if (best && m_physics) {
        if (const auto hit = m_physics->raycast(eye, dir, best_t); hit && hit->distance + k_pick_margin_m < best_t) {
            best = 0; // behind something solid
        }
    }
    // A near miss counts when nothing was hit and the line to the point it touched is clear.
    if (!best && near_actor.ref) {
        const glm::vec3 to = near_point - eye;
        const float length = glm::length(to);
        const auto hit = m_physics ? m_physics->raycast(eye, to / length, length) : std::nullopt;
        if (!hit || hit->distance + k_pick_margin_m >= length) {
            best = near_actor.ref;
            m_pick_actor = near_actor;
        }
    }
    return best;
}

void App::update_crosshair() {
    if (!m_game || !m_ui || !m_walk) {
        return;
    }
    const uint32_t ref = m_mount.ref ? 0 : pick_ref(); // the crosshair text is blank in the saddle
    const bool open = door_open(ref);
    if (ref == m_crosshair_ref && open == m_crosshair_open) {
        return;
    }
    m_crosshair_ref = ref;
    m_crosshair_open = open;
    m_ui->set_crosshair(m_item_menus->crosshair(target_of(ref)));
}

game::Target App::target_of(uint32_t ref) {
    if (ref && m_pick_actor.ref == ref) {
        return target_of_actor(ref, m_pick_actor.npc);
    }
    game::Target t = m_game->inspect(ref);
    if (t.kind == game::TargetKind::door && door_open(ref)) {
        t.verb = m_game->settings().text("sClose");
    }
    return t;
}

game::Target App::target_of_actor(uint32_t ref, uint32_t npc) {
    if (const ActorSet* actors = m_space ? m_space->actors() : nullptr; actors && actors->dead(ref)) {
        uint32_t inventory = 0;
        std::vector<uint32_t> worn;
        if (actors->worn_items(ref, inventory, worn)) {
            m_game->stock_corpse(ref, inventory, worn);
        }
    }
    game::Target t = m_game->inspect_actor(ref, npc, m_player && m_player->status().sneaking);
    if (!t.mount && tamed_dragon(ref) && m_game->flight_radius(npc)) {
        // Perk entry 91 on the alias's keyword: the prompt is Ride, and a tamed dragon is no theft.
        t.mount = true;
        t.steal = false;
        t.verb = m_game->settings().text("sRide");
    }
    if (t.verb.empty()) {
        t.kind = game::TargetKind::none;
    }
    return t;
}

void App::activate() {
    if (m_mount.ref) {
        dismount(); // with nothing to pick, the mount
        return;
    }
    activate_target(m_game ? target_of(pick_ref()) : game::Target{});
}

// what's left once scripts have had the event.
void App::furniture_default(const game::Target& target) {
    if (m_seat.ref) { // sitting: stand up. It's how a mining script lets the player go when the vein is spent.
        stand_player();
        return;
    }
    if (target.bed) {
        // Any bed offers a jailed player their time.
        game::Crime& crime = m_game->crime();
        if (crime.jail_days() > 0 && !crime.escaping()) {
            const game::Settings& s = m_game->settings();
            m_ui->message(s.text("sServeSentenceQuestion"), {s.text("sYes"), s.text("sNo")}, [this](int button) {
                if (button == 0) {
                    m_game->crime().serve_sentence();
                }
            });
            return;
        }
        if (!ui_can_rest(true)) {
            return;
        }
        if (target.steal) {
            m_ui->notification(m_game->settings().text("sNoSleepInOwnedBed"));
            return;
        }
        m_ui->open_sleep();
        return;
    }
    if (m_game->furniture_bench(target.base)) {
        m_item_menus->open_station(target.base);
        m_ui->open("Crafting Menu");
        return;
    }
    sit_player(target.ref);
}

void App::activate_target(const game::Target& target) {
    // a pile passes the activation to the body it was left by.
    if (const uint32_t body = m_game->ash_pile_actor(target.ref)) {
        activate_target(m_game->inspect(body));
        return;
    }
    // The game tests the lock first, open or shut.
    if (target.kind == game::TargetKind::door && target.locked()) {
        if (!m_item_menus->open_lock(target) && m_game->inspect(target.ref).lock_level == 0 && !travel_door(target.ref)) {
            activate_door();
        }
        return;
    }
    if (target.kind == game::TargetKind::none || target.kind == game::TargetKind::door) {
        if (!travel_door(target.ref)) {
            activate_door();
        }
        return;
    }
    if (target.locked() && target.kind == game::TargetKind::container) {
        if (!m_item_menus->open_lock(target) && m_game->inspect(target.ref).lock_level == 0) {
            activate_target(m_game->inspect(target.ref)); // opened with its key
        }
        return;
    }
    switch (target.kind) {
    case game::TargetKind::actor:
        if (target.corpse) { // mode 0, and a dead actor's things aren't theft
            m_item_menus->open_container(target.ref, target.name, false);
            m_ui->open("ContainerMenu");
            break;
        }
        if (target.mount) {
            mount(target);
            break;
        }
        if (!target.steal) {
            start_quests();
            m_papyrus->dialogue()->start_conversation(target.ref);
        }
        // Talking is Dialogue's; a sneaking player's hand goes to their pockets (ContainerMenu mode 2).
        if (target.steal) {
            m_item_menus->open_pickpocket(target.ref, target.base, target.name);
        }
        break;
    case game::TargetKind::item:
        if (target.item->kind == game::ItemKind::book && !(target.item->book_flags & game::book_teaches_spell)) {
            m_item_menus->open_book(target.base, target.ref, target.steal);
        } else {
            take_ref(target);
        }
        break;
    case game::TargetKind::container:
        m_item_menus->open_container(target.ref, target.name, target.steal);
        m_ui->open("ContainerMenu");
        container_open(target.ref, true);
        break;
    case game::TargetKind::activator:
        papyrus_activate(target.ref, target.base); // scripts hear it even when blocked: the default action is what's blocked
        break;
    case game::TargetKind::furniture:
        papyrus_activate(target.ref, target.base);
        if (!m_game->activation_blocked(target.ref)) { // a script that blocks it calls Activate itself, default processing only
            furniture_default(target);
        }
        break;
    case game::TargetKind::flora:
        if (m_game->harvest(target) && m_space) {
            for (const osw::Ref& r : m_space->world().refs) {
                if (r.form_id == target.ref) {
                    show_harvested(r);
                    break;
                }
            }
        }
        m_item_menus->flush_notices();
        m_crosshair_ref = ~0u;
        break;
    default:
        break;
    }
}

void App::take_ref(const game::Target& target) {
    if (m_game->take(target) && m_space) {
        m_space->set_ref_hidden(target.ref, true);
        m_space->despawn(target.ref);
        if (m_physics) {
            m_physics->remove_object(target.ref);
        }
        m_crosshair_ref = ~0u;
    }
    m_item_menus->flush_notices();
}

// Damage to a destructible reference: it crosses stages, swaps its model, and tells its scripts.
void App::damage_ref(uint32_t form_id, float amount, bool self_damage) {
    if (!m_space || !m_game) {
        return;
    }
    if (!m_destruction) {
        m_destruction = std::make_unique<Destruction>(m_game->db());
    }
    const osw::World& world = m_space->world();
    const auto ref = std::ranges::find(world.refs, form_id, &osw::Ref::form_id);
    if (ref == world.refs.end()) {
        return;
    }
    const auto change = m_destruction->damage(form_id, ref->base_form_id, world.meshes[ref->mesh], amount, self_damage);
    if (!change) {
        return;
    }
    if (!change->mesh.empty()) {
        swap_ref_model(*ref, change->mesh);
    }
    // Each crossed stage's explosion, from the reference (its blast nodes when it
    // has any, which `.osm` doesn't keep, else a point of its bound).
    for (const uint32_t explosion : change->explosions) {
        game::ExplosionSpawn s;
        s.form = explosion;
        s.source = form_id;
        s.position = {ref->position[0] / units::k_skyrim_unit_m, ref->position[1] / units::k_skyrim_unit_m,
                      ref->position[2] / units::k_skyrim_unit_m};
        m_game->projectiles().explode(s);
    }
    for (const Destruction::Change::Debris& d : change->debris) {
        spawn_debris(d.form, d.count, glm::vec3(ref->position[0], ref->position[1], ref->position[2]), ref->scale);
    }
    if (change->disable) {
        m_game->set_ref_disabled(form_id, true);
        m_space->despawn(form_id);
    }
    ensure_papyrus();
    m_papyrus->ref_event(form_id, "OnDestructionStageChanged",
                         {script::Value::of(change->old_index), script::Value::of(change->new_index)});
}

// an entry gets count x percentage / 100 pieces, or one while fewer than `count` have been made,
// each at the same spot on a random heading with no velocity. With collision it's a body for 90 s; without, a temp particle.
void App::wire_world_fx() {
    const auto disabled = [this](uint32_t form) { return m_game->ref_disabled(form); };
    for (WorldFx* fx : {m_world_fx.get(), m_portal_fx.get()}) {
        if (fx) {
            fx->ref_disabled = disabled;
        }
    }
}

void App::spawn_debris(uint32_t debr, uint32_t count, const glm::vec3& at, float scale) {
    constexpr float k_debris_seconds = 90.0f;
    constexpr float k_particle_scale = 10.0f;
    const forms::Database& db = m_game->db();
    const forms::FormEntry* entry = db.find(debr);
    if (!entry || entry->type != forms::sig("DEBR") || !m_space) {
        return;
    }
    const forms::Form form(db, *entry);
    uint32_t made = 0;
    uint32_t state = debr ^ static_cast<uint32_t>(at.x * 1000.0f);
    const auto unit = [&state] {
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) / float(1u << 24) * 2.0f - 1.0f;
    };
    for (const forms::Value* v : form.all("DATA")) {
        const forms::Value* percentage = v->member("percentage");
        const forms::Value* model = v->member("model");
        const forms::Value* collision = v->member("has_collision");
        if (!percentage || !model || model->text.empty() || percentage->integer <= 0) {
            continue;
        }
        uint32_t pieces = count * uint32_t(percentage->integer) / 100u;
        if (pieces == 0) {
            if (made >= count) {
                continue;
            }
            pieces = 1;
        }
        made += pieces;
        const std::string mesh = game::model_path(model->text);
        for (uint32_t i = 0; i < pieces; ++i) {
            glm::vec3 direction(unit(), unit(), unit());
            const float length = glm::length(direction);
            direction = length > 1e-6f ? direction / length : glm::vec3(0.0f);
            const glm::quat turn = glm::angleAxis(std::atan2(direction.x, direction.y), glm::vec3(0, 0, -1)) *
                                   glm::angleAxis(std::asin(direction.z), glm::vec3(-1, 0, 0));
            if (collision && (collision->integer & 1)) {
                const uint32_t id = m_next_debris++;
                m_space->spawn(mesh, at, turn, scale, id);
                if (m_physics) {
                    m_physics->add_object(id, mesh, at, turn);
                }
                m_debris.push_back({id, k_debris_seconds});
            } else if (m_vfx) {
                const glm::mat3 axes = glm::mat3_cast(turn);
                const auto units_of = [](const glm::vec3& v) { return game::Units3{v.x, v.y, v.z}; };
                m_vfx->temp_particle(mesh, units_of(at / units::k_skyrim_unit_m),
                                     {units_of(axes[0]), units_of(axes[1]), units_of(axes[2])}, scale * k_particle_scale,
                                     k_debris_seconds);
            }
        }
    }
}

// The old model and its collision go with the old 3D; the new model brings its own.
void App::swap_ref_model(const osw::Ref& ref, const std::string& mesh) {
    const uint32_t form_id = ref.form_id;
    const glm::vec3 position(ref.position[0], ref.position[1], ref.position[2]);
    const glm::quat rotation(ref.rotation[3], ref.rotation[0], ref.rotation[1], ref.rotation[2]);
    m_space->set_ref_hidden(form_id, true);
    m_space->despawn(form_id);
    m_space->spawn(mesh, position, rotation, ref.scale, form_id);
    if (m_physics) {
        m_physics->set_ref_solid(form_id, false);
        m_physics->remove_object(form_id);
        m_physics->add_object(form_id, mesh, position, rotation, true);
    }
}

// A space that comes up after a load, or a door away, shows what was broken in it.
void App::apply_destruction() {
    if (!m_destruction || !m_space) {
        return;
    }
    const auto& refs = m_space->world().refs;
    for (const auto& [form, mesh] : m_destruction->swapped()) {
        if (const auto ref = std::ranges::find(refs, form, &osw::Ref::form_id); ref != refs.end()) {
            swap_ref_model(*ref, mesh);
        }
    }
}

const std::unordered_map<uint32_t, ots::Swap>* App::texture_swaps() {
    if (!m_texture_swaps) {
        m_texture_swaps.emplace();
        const auto file = m_opt.cache / "rules" / "texture_swaps.ots";
        std::error_code missing;
        if (std::filesystem::exists(file, missing)) {
            for (ots::Swap& s : ots::read(read_file(file)).swaps) {
                if (s.variant == ots::variant_model) {
                    m_texture_swaps->emplace(s.base_form_id, std::move(s));
                } else {
                    m_addon_swaps.emplace(uint64_t(s.base_form_id) << 3 | s.variant, std::move(s));
                }
            }
        }
    }
    return &*m_texture_swaps;
}

// What a base form's record draws, for what isn't an item.
std::string App::base_model(uint32_t base) const {
    const forms::FormEntry* e = base ? m_game->db().find(base) : nullptr;
    const forms::Value* model = e ? forms::Form(m_game->db(), *e).get("MODL") : nullptr;
    return model && !model->text.empty() ? game::model_path(model->text) : std::string();
}

// PlayGamebryoAnimation.
int App::play_gamebryo(uint32_t form, std::string_view name, bool start_over, float ease_in) {
    if (!m_space || !m_world_fx || !m_game) {
        return 1;
    }
    // A placed reference, or one a script made.
    const osw::World& world = m_space->world();
    const auto ref = std::ranges::find(world.refs, form, &osw::Ref::form_id);
    const std::string model = ref != world.refs.end() && ref->mesh < world.meshes.size() ? world.meshes[ref->mesh]
                                                                                        : base_model(m_game->placed_base(form));
    const auto position = m_game->ref_position(form);
    if (model.empty() || (ref == world.refs.end() && !position)) {
        return 1;
    }
    if (!m_world_fx->has_effect(model)) {
        return 2;
    }
    const std::string sequence = m_world_fx->sequence_named(model, name);
    if (sequence.empty()) {
        return 3;
    }
    if (ActorSet* actors = m_space->actors(); actors && actors->play_sequence(form, sequence, start_over, ease_in)) {
        return 0; // its collision moves with it
    }
    ObjectSequence& o = m_object_sequences[form];
    if (!o.handle && ref != world.refs.end()) {
        const glm::quat rotation(ref->rotation[3], ref->rotation[0], ref->rotation[1], ref->rotation[2]);
        o.handle = m_world_fx->track(model, glm::mat3_cast(rotation), glm::vec3(ref->position[0], ref->position[1], ref->position[2]), ref->scale);
    } else if (!o.handle) {
        const float heading = m_game->ref_angle(form)[2];
        const glm::mat3 rotation = glm::mat3_cast(glm::angleAxis(-heading, glm::vec3(0.0f, 0.0f, 1.0f)));
        o.handle = m_world_fx->track(model, rotation, glm::vec3((*position)[0], (*position)[1], (*position)[2]) * units::k_skyrim_unit_m, 1.0f);
    }
    o.sequence = sequence;
    o.start_over = start_over;
    o.ease_in = ease_in;
    return 0;
}

void App::container_open(uint32_t ref, bool open) {
    if (play_gamebryo(ref, open ? "Open" : "Close", true, 0.0f) == 0) {
        m_object_sequences[ref].back = !open;
    }
    m_open_container = open ? ref : 0;
}

void App::update_object_sequences() {
    if (m_open_container && m_ui && !m_ui->is_open("ContainerMenu")) {
        container_open(m_open_container, false);
    }
    std::erase_if(m_object_sequences, [this](const auto& entry) {
        const auto& [form, o] = entry;
        fx::EffectInstance* instance = o.back && o.sequence.empty() ? m_world_fx->tracked(o.handle) : nullptr;
        if (!instance || instance->sequence_time(o.running) < instance->sequence_duration(o.running)) {
            return false;
        }
        m_world_fx->stop(o.handle);
        m_space->set_ref_hidden(form, false);
        return true;
    });
    for (auto& [form, o] : m_object_sequences) {
        fx::EffectInstance* instance = o.sequence.empty() ? nullptr : m_world_fx->tracked(o.handle);
        if (!instance) {
            continue;
        }
        const bool again = instance->sequence_active(o.sequence);
        if (!again || o.start_over) {
            for (const std::string& s : instance->sequence_names()) {
                instance->deactivate_sequence(s, o.ease_in);
            }
            instance->activate_sequence(o.sequence, 1.0f, o.ease_in);
        }
        m_space->set_ref_hidden(form, true);
        o.running = std::exchange(o.sequence, {});
    }
}

void App::clear_destruction(uint32_t form_id) {
    if (!m_destruction || !m_space) {
        return;
    }
    m_destruction->clear(form_id);
    m_space->despawn(form_id);
    m_space->set_ref_hidden(form_id, false);
    if (m_physics) {
        m_physics->remove_object(form_id);
        m_physics->set_ref_solid(form_id, true);
    }
}

namespace {
constexpr float k_havok_units = 69.991249f; // game units to the Havok metre,
} // namespace

void App::trap_hit(uint32_t target, uint32_t trap, float damage, float pushback, const glm::vec3& velocity, const glm::vec3& position) {
    const forms::FormEntry* entry = m_game ? m_game->db().find(target) : nullptr;
    if (target != game::k_player_ref && (!entry || entry->type != forms::sig("ACHR"))) {
        return;
    }
    // The event's Havok units become the hit's game units.
    const auto units = [](const glm::vec3& v) {
        return game::Units3{v.x * k_havok_units, v.y * k_havok_units, v.z * k_havok_units};
    };
    m_game->combat().trap_hit(target, trap, std::abs(damage), pushback, units(position), units(velocity));
}

// The game: what the player's controller overlaps, stepped with the world. An overlap hits when the trap
// hasn't for fTrapHitEventDelayMS: the clock is the trap's, so leaving and coming back doesn't reset it. Its
// scripts do the rest. A live NPC counts by its controller, and those aren't in the physics world yet.
void App::update_traps(float dt) {
    if (!m_physics || !m_player) {
        return;
    }
    constexpr float k_hit_delay = 0.5f; // fTrapHitEventDelayMS
    const glm::vec3 half = physics::CharacterSettings{}.half_extents;
    const glm::vec3 feet = m_player->feet();
    std::vector<physics::World::TrapTouch> touching;
    // The controller stays keepDistance off what it meets, so a shove alone never overlaps: the listener's overlap reaches
    // the collision tolerance further.
    constexpr float k_collision_tolerance = 0.15f; // startPointTolerance: keepDistance 0.05 + keepContactTolerance 0.1
    m_physics->traps_touching(feet + glm::vec3(0, 0, half.x), feet + glm::vec3(0, 0, half.z * 2.0f - half.x),
                              half.x + k_collision_tolerance, touching);
    if (touching.empty() && m_traps.empty()) {
        return;
    }
    ensure_papyrus();
    auto& vm = m_papyrus->vm();
    // The trap's body at the contact, in Havok units: its position gives a shove its direction.
    const auto hit_args = [&](const physics::World::TrapTouch& touch) {
        const glm::vec3 velocity = touch.velocity / units::k_skyrim_unit_m / k_havok_units;
        const glm::vec3 position = touch.position / units::k_skyrim_unit_m / k_havok_units;
        constexpr bool initial = false; // passes 0
        return std::vector<script::Value>{vm.form_value(game::k_player_ref, "ObjectReference"),
                                          script::Value::of(velocity.x), script::Value::of(velocity.y),
                                          script::Value::of(velocity.z), script::Value::of(position.x),
                                          script::Value::of(position.y), script::Value::of(position.z),
                                          script::Value::of(int32_t(0)), script::Value::of(initial),
                                          script::Value::of(int32_t(0))};
    };
    for (auto& [trap, t] : m_traps) {
        t.wait -= dt;
    }
    for (const physics::World::TrapTouch& touch : touching) {
        const uint32_t trap = touch.form;
        TrapContact& t = m_traps[trap];
        if (!t.touching) { // a new touch, and the clock starts
            t.touching = true;
            t.wait = k_hit_delay;
            m_papyrus->ref_event(trap, "OnTrapHitStart", hit_args(touch));
        } else if (t.wait <= 0.0f) {
            t.wait = k_hit_delay;
            m_papyrus->ref_event(trap, "OnTrapHit", hit_args(touch));
        }
    }
    std::erase_if(m_traps, [&](auto& entry) {
        auto& [trap, t] = entry;
        if (std::ranges::find(touching, trap, &physics::World::TrapTouch::form) != touching.end()) {
            return false;
        }
        if (t.touching) {
            t.touching = false;
            m_papyrus->ref_event(trap, "OnTrapHitStop", {vm.form_value(game::k_player_ref, "ObjectReference")});
        }
        return t.wait <= 0.0f;
    });
}

void App::update_destruction(float dt) {
    std::erase_if(m_debris, [&](DebrisPiece& p) {
        if ((p.left -= dt) > 0.0f) {
            return false;
        }
        if (m_physics) {
            m_physics->remove_object(p.id);
        }
        if (m_space) {
            m_space->despawn(p.id);
        }
        return true;
    });
    if (!m_destruction || m_destruction->self_damaging().empty()) {
        return;
    }
    const auto burning = m_destruction->self_damaging();
    for (const auto& [ref, rate] : burning) {
        damage_ref(ref, rate * dt, true);
    }
}

// The player's drop spot is a ring around them; the item then falls. One with no collision rests on what is
// under that spot.
void App::drop_item(const game::Stack& stack) {
    const game::Item* item = m_game->items().find(stack.form);
    if (!item || !m_space) {
        return;
    }
    const glm::vec3 forward = glm::normalize(glm::vec3(m_camera.forward().x, m_camera.forward().y, 0.0f));
    glm::vec3 spot = m_camera.position + forward * (50.0f * units::k_skyrim_unit_m);
    const uint32_t id = m_game->place(stack, {spot.x, spot.y, spot.z});
    const glm::quat turn = glm::angleAxis(-m_camera.yaw, glm::vec3(0, 0, 1));
    if (m_physics && !m_physics->add_object(id, item->model, spot, turn)) {
        if (const auto hit = m_physics->raycast(spot, glm::vec3(0, 0, -1), 5.0f)) {
            spot.z -= hit->distance;
        }
    }
    m_space->spawn(item->model, spot, m_camera.yaw, id);
}

bool App::ui_lock(ui::LockInfo& lock) {
    if (!m_item_menus) {
        return false;
    }
    lock = m_item_menus->lock();
    return lock.present;
}

void App::ui_lock_event(std::string_view what, double pick_health) {
    if (!m_item_menus) {
        return;
    }
    m_item_menus->lock_event(what, pick_health);
    if (what == "unlocked") {
        activate_target(m_game->inspect(m_item_menus->unlocked_ref())); // FinishUnlock activates the target
    }
}

// The player's weight into movement (no running over the limit), and the reminder every fEncumberedReminderTimer
// seconds (30, the game's) while it lasts and no menu pauses.
void App::update_encumbrance(float dt) {
    if (!m_game) {
        return;
    }
    m_game->days_passed = m_clock.days_passed();
    update_character(dt);
    m_game->crime().update(dt, m_clock.days_passed() * 24.0);
    if (m_space) {
        const uint32_t cell = m_space->interior() && !m_space->world().cells.empty() ? m_space->world().cells[0].form_id : 0;
        if (cell != m_player_cell) {
            m_player_cell = cell;
            m_game->crime().enter_cell(cell);
        }
    }
    apply_jail_move();
    apply_script_moves();
    m_item_menus->flush_notices(); // what scripts and crimes did
    if (!m_walk) {
        return;
    }
    m_tuning.body.inventory_weight = m_game->encumbrance();
    m_tuning.body.carry_weight = m_game->carry_weight();
    if (!m_game->over_encumbered()) {
        m_encumbered_reminder = 0;
        return;
    }
    if (!m_ui || m_ui->pausing_menus() > 0) {
        return;
    }
    m_encumbered_reminder -= dt;
    if (m_encumbered_reminder <= 0) {
        m_encumbered_reminder = m_game->settings().number("fEncumberedReminderTimer", 30.0f);
        m_ui->notification(m_game->settings().text("sOverEncumbered"));
    }
}

// Who is around the player: the loaded actors, and whether detection has them seeing the player. `game seen on` makes
// everyone see, for automation.
std::vector<game::Witness> App::crime_witnesses() {
    std::vector<game::Witness> out;
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        for (const ActorSet::Who& a : actors->who()) {
            const game::Agent* agent = m_ai_ready ? m_game->ai().agent(a.ref) : nullptr;
            out.push_back({a.ref, a.npc, m_seen || (agent && agent->detection > 0), true});
        }
    }
    return out;
}

// A move the crime system asked for: the arriving door's space, at its teleport position.
void App::apply_jail_move() {
    const std::optional<game::JailMove> move = m_game->crime().take_move();
    if (!move) {
        return;
    }
    // Whoever brought the player here doesn't come along.
    if (m_ui) {
        m_ui->close("Dialogue Menu");
    }
    const forms::Database& db = m_game->db();
    const forms::FormEntry* door = db.find(move->door);
    const forms::FormEntry* cell = door ? db.find(door->parent) : nullptr;
    if (!cell) {
        return;
    }
    // An exterior cell's space is its worldspace, an interior's its own.
    const forms::FormEntry* world = cell->parent ? db.find(cell->parent) : nullptr;
    load_world(std::string(db.editor_id(world ? *world : *cell)));
    const glm::vec3 feet = glm::vec3(move->position[0], move->position[1], move->position[2]) * units::k_skyrim_unit_m;
    place_player(feet, glm::degrees(move->heading), 0.0f);
    m_item_menus->flush_notices();
}

// Script moves of the player: to where a reference is, across spaces, or (no target) within this one.
void App::apply_script_moves() {
    std::vector<ScriptMove> moves;
    moves.swap(m_script_moves);
    for (const ScriptMove& m : moves) {
        Pose pose;
        pose.position_units = glm::vec3(m.at[0], m.at[1], m.at[2]);
        pose.z_above_terrain = false;
        pose.heading_deg = m_camera.skyrim_heading_deg();
        std::string space = "-";
        WorldSim::Place place;
        glm::vec3 where;
        if (m.target && ref_place(m.target, place, where)) {
            const forms::Database& db = m_game->db();
            if (const forms::FormEntry* e = db.find(place.world ? place.world : place.cell)) {
                space = std::string(db.editor_id(*e));
            }
            if (const forms::FormEntry* r = m.match_rotation ? db.find(m.target) : nullptr) {
                if (const auto placed = forms::placement(db, *r)) {
                    pose.heading_deg = glm::degrees(placed->rotation[2]);
                }
            }
        }
        travel(space, pose);
        if (m.wakes && m_papyrus) {
            m_papyrus->vm().return_from_latent(m.stack, {});
        }
    }
}

std::string App::game_command(const std::vector<std::string>& args) {
    const std::string& sub = args.at(0);
    if (sub == "start") {
        start_game();
        return m_game ? "started" : "no forms database";
    }
    if (!m_game) {
        throw Error("game: not started (game start)");
    }
    if (sub == "additem" && args.size() >= 2) {
        const int count = args.size() > 2 ? std::stoi(args[2]) : 1;
        m_game->player().inventory.add(parse_form(args[1]), count);
        return std::format("{}", m_game->player().inventory.count(parse_form(args[1])));
    }
    if (sub == "equip" && args.size() >= 2) {
        const uint32_t form = parse_form(args[1]);
        auto& stacks = m_game->player().inventory.stacks();
        for (size_t i = 0; i < stacks.size(); ++i) {
            if (stacks[i].form == form) {
                m_game->select(i, args.size() > 2 ? std::stoi(args[2]) : -1);
                m_item_menus->flush_notices();
                return "equipped";
            }
        }
        return "not carried";
    }
    if (sub == "var" && args.size() >= 3) { // game var <form> <name> [value]: a script variable, for checks of a state machine
        const script::Value* v = m_papyrus->vm().find_variable(script::form_handle(parse_form(args[1])), args[2]);
        if (v && args.size() >= 4) { // a test's shortcut past what would have set it
            auto* w = const_cast<script::Value*>(v);
            if (w->tag == script::Tag::boolean) {
                w->boolean = args[3] != "0";
            } else if (w->tag == script::Tag::integer) {
                w->integer = std::stoi(args[3]);
            } else if (w->tag == script::Tag::real) {
                w->real = std::stof(args[3]);
            }
        }
        return !v ? "none" : v->tag == script::Tag::integer ? std::to_string(v->integer) : v->tag == script::Tag::real ? std::to_string(v->real)
               : v->tag == script::Tag::boolean ? (v->boolean ? "true" : "false") : "object";
    }
    if (sub == "stubs") { // natives the game declares that nothing here answers
        std::string out;
        for (const std::string& name : m_papyrus->vm().stub_natives()) {
            out += name + '\n';
        }
        return out;
    }
    if (sub == "refs") {
        // What the player could act on in this space, for scripts to name; with a base, where that base stands.
        std::string out;
        int n = 0;
        const uint32_t only = args.size() >= 2 ? parse_form(args[1]) : 0;
        for (const osw::Ref& r : m_space->world().refs) {
            if (only) {
                if (r.base_form_id == only) {
                    const glm::vec3 p = glm::vec3(r.position[0], r.position[1], r.position[2]) / units::k_skyrim_unit_m;
                    out += std::format("{:08X} at {:.0f} {:.0f} {:.0f}\n", r.form_id, p.x, p.y, p.z);
                }
                continue;
            }
            if (m_game->activatable(r.base_form_id) && n++ < 60) {
                const game::Target t = m_game->inspect(r.form_id);
                out += std::format("{:08X} {} \"{}\" \"{}\" x{} lock {} steal {}\n", t.ref, int(t.kind), t.verb, t.name,
                                   t.count, t.lock_level, t.steal);
            }
        }
        return out;
    }
    if (sub == "respawning") {
        // Containers here that refill with their cell's reset, for checks of it: reference, its cell.
        std::string out;
        const forms::Database& db = m_game->db();
        int n = 0;
        for (const osw::Ref& r : m_space->world().refs) {
            const forms::FormEntry* base = db.find(r.base_form_id);
            const forms::FormEntry* ref = db.find(r.form_id);
            if (base && ref && base->type == forms::sig("CONT") && n < 8) {
                const forms::Value* flags = forms::Form(db, *base).get("DATA.flags");
                if (flags && (flags->integer & 2)) {
                    out += std::format("{:08X} cell {:08X}\n", r.form_id, ref->parent);
                    ++n;
                }
            }
        }
        return out;
    }
    if (sub == "touch" && args.size() >= 2) {
        // A container the player has changed, or (with `count`) how many items it holds.
        game::Inventory& inv = m_game->container(parse_form(args[1]));
        if (args.size() < 3) {
            inv.add(0xF, 1000);
        }
        return std::format("{}", inv.count(0xF));
    }
    if (sub == "target") {
        const game::Target t = args.size() > 1 ? m_game->inspect(parse_form(args[1])) : target_of(pick_ref());
        return std::format("{:08X} {} \"{}\" \"{}\" count {} lock {}", t.ref, int(t.kind), t.verb, t.name, t.count,
                           t.lock_level);
    }
    if (sub == "open" && args.size() >= 2) {
        // What Activate does on that reference, wherever the camera points.
        activate_target(m_game->inspect(parse_form(args[1])));
        return "activated";
    }
    if (sub == "talk" && args.size() >= 2) {
        // A conversation with that reference: the greeting is the game's random pick, and a goodbye-only greeting opens no
        // menu, so a script that goes on to topics asks again as a player would.
        const game::Target t = m_game->inspect(parse_form(args[1]));
        start_quests();
        for (int tries = 0; tries < 32; ++tries) {
            if (m_papyrus->dialogue()->start_conversation(t.ref)) {
                return "talking";
            }
        }
        throw Error("game talk: no conversation");
    }
    if (sub == "quests") {
        start_quests(); // what a New Game does behind its loading screen
        return "started";
    }
    if (sub == "topic" && args.size() >= 2) {
        // The topic at that place in the open conversation's list, as the Dialogue Menu's click.
        m_papyrus->dialogue()->choose_topic(std::stoi(args[1]));
        return "chosen";
    }
    if (sub == "pickpocket" && args.size() >= 2) {
        m_item_menus->open_pickpocket(parse_form(args[1]), m_game->base_of(parse_form(args[1])), "");
        return "pickpocket";
    }
    if (sub == "path" && args.size() >= 8) {
        // game path <space> x y z x y z: a path over the navmeshes, Skyrim units.
        const auto start = Clock::now();
        game::NavMeshes& nav = m_game->navmeshes();
        const double index_ms = ms_between(start, Clock::now());
        const uint32_t space = parse_form(args[1]);
        const auto point = [&](size_t i) {
            return game::NavPoint{std::stof(args[i]), std::stof(args[i + 1]), std::stof(args[i + 2])};
        };
        const auto a = nav.locate(space, point(2), 20000.0f), b = nav.locate(space, point(5), 20000.0f);
        if (!a || !b) {
            return a ? "no navmesh at the end" : "no navmesh at the start";
        }
        const std::vector<game::NavPoint> path = nav.path(*a, *b);
        std::string out = std::format("index {:.0f} ms, search {:.1f} ms, {} corners:", index_ms,
                                      ms_between(start, Clock::now()) - index_ms, path.size());
        for (const game::NavPoint& p : path) {
            out += std::format(" ({:.0f} {:.0f} {:.0f})", p[0], p[1], p[2]);
        }
        return out;
    }
    if (sub == "fly" && args.size() >= 3) {
        // game fly <ref> orbit|hover|land: enables a flier and orders it, around where the player is.
        const glm::vec3 me = (m_player ? m_player->feet() : m_camera.position) / units::k_skyrim_unit_m;
        const game::AiMode mode = args[2] == "orbit" ? game::AiMode::orbit : args[2] == "hover" ? game::AiMode::hover
                                                                                               : game::AiMode::travel;
        m_game->set_ref_disabled(parse_form(args[1]), false);
        m_game->ai().order_flight(parse_form(args[1]), mode, {me.x, me.y, me.z});
        m_game->ai().roster_changed();
        const float g = m_game->ai().ground_height ? m_game->ai().ground_height(me.x, me.y) : -1.0f;
        return std::format("{} over ground {:.0f} at z {:.0f}", args[2], g, me.z);
    }
    if (sub == "ride" && args.size() >= 2) {
        // game ride <ref> | off: as the Activate key on that horse, or in the saddle.
        if (args[1] == "off") {
            dismount();
            return "off";
        }
        const uint32_t ref = parse_form(args[1]);
        m_ride_any_dragon = args.size() > 2 && args[2] == "tamed";
        if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
            for (const ActorSet::Entry& e : actors->list()) {
                if (e.ref == ref) {
                    game::Target t = target_of_actor(ref, e.npc);
                    activate_target(t);
                }
            }
        }
        return m_mount.ref ? "riding" : "refused";
    }
    if (sub == "watch") {
        // game watch [ref|walking|using] [units]: the camera that far behind an actor (250; negative is in front of them), the first
        // near the player who is walking, or on some furniture, without a ref.
        game::Ai& ai = m_game->ai();
        const bool using_it = args.size() > 1 && args[1] == "using";
        const bool any = using_it || (args.size() > 1 && args[1] == "walking");
        const game::Agent* a = args.size() > 1 && !any ? ai.agent(parse_form(args[1])) : nullptr;
        const float back = args.size() > 2 ? std::stof(args[2]) : 250.0f;
        if (ActorSet* actors = m_space ? m_space->actors() : nullptr; !a && actors) {
            for (const ActorSet::Entry& e : actors->list()) {
                const game::Agent* c = ai.agent(m_watching ? m_watching : e.ref);
                if (c && (using_it ? c->use == game::UseState::using_it : c->next < c->path.size())) {
                    a = c;
                    break;
                }
            }
        }
        if (!a) {
            return "nobody is walking";
        }
        m_watching = a->ref;
        const glm::vec3 feet = glm::vec3(a->position[0], a->position[1], a->position[2]) * units::k_skyrim_unit_m;
        const glm::vec3 ahead(std::sin(a->heading), std::cos(a->heading), 0.0f);
        if (m_player) {
            m_player->teleport(feet - ahead * (back * units::k_skyrim_unit_m));
        }
        m_camera.position = feet - ahead * (back * units::k_skyrim_unit_m) + glm::vec3(0, 0, 1.6f);
        m_camera.yaw = back < 0 ? a->heading + glm::pi<float>() : a->heading;
        m_camera.pitch = glm::radians(-5.0f);
        std::string clips;
        if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
            for (const ActorSet::Entry& e : actors->list()) {
                if (e.ref == a->ref) {
                    for (const std::string& c : e.clips) {
                        clips += " " + c;
                    }
                }
            }
        }
        return std::format("{:08X} at ({:.0f} {:.0f} {:.0f}) speed {:.0f} flies {} clips{}", a->ref, a->position[0], a->position[1],
                           a->position[2], a->speed, a->flier ? int(a->fly) : -1, clips);
    }
    if (sub == "ai") {
        // game ai [space] [high]: the agents in that space, or the current one's first few; near the player only with high.
        game::Ai& ai = m_game->ai();
        const glm::vec3 me = (m_player ? m_player->feet() : m_camera.position) / units::k_skyrim_unit_m;
        std::string out = std::format("{} actors, {} searches, player at ({:.0f} {:.0f} {:.0f})", ai.agents(), ai.searches(), me.x,
                                      me.y, me.z);
        const uint32_t space = args.size() > 1 ? parse_form(args[1]) : 0;
        if (const game::Agent* a = ai.agent(space)) { // game ai <actor>: where that one is
            return std::format("{:08X} in {:08X} at ({:.0f} {:.0f} {:.0f}) pack {:08X} high {} disabled {} {}", a->ref, a->space,
                               a->position[0], a->position[1], a->position[2], a->package ? a->package->form : 0, a->high,
                               a->disabled, m_game->ref_disabled(a->ref));
        }
        int n = 0;
        const bool high = args.size() > 2 && args[2] == "high";
        for (const game::Agent* a : ai.in_space(space)) {
            if (high && !a->high) {
                continue;
            }
            if (n++ < 300) {
                out += std::format("; {:08X} pack {:08X} mode {} at ({:.0f} {:.0f} {:.0f}) goal {:08X} ({:.0f} {:.0f} r{:.0f}) "
                                   "high {} path {}/{} search {} door {:08X} detect {} los {} act {} use {} {:08X} heading {:.2f}",
                                   a->ref, a->package ? a->package->form : 0, a->package ? int(a->package->mode) : -1,
                                   a->position[0], a->position[1], a->position[2], a->location.space,
                                   a->location.point[0], a->location.point[1], a->location.radius, a->high, a->next,
                                   a->path.size(), a->searching, a->leg_door, a->detection, a->los, int(a->activity),
                                   int(a->use), a->use_ref, a->heading);
                if (a->flier) {
                    out += std::format(" flies {} speed {:.0f} pitch {:.2f}", int(a->fly), a->speed, a->pitch);
                }
            }
        }
        return out + std::format(" ({} here)", n);
    }
    if (sub == "seen" && args.size() >= 2) {
        m_seen = args[1] == "on";
        return args[1];
    }
    if (sub == "bounty") {
        game::Crime& crime = m_game->crime();
        if (args.size() >= 3) {
            crime.set_crime_gold(parse_form(args[1]), false, std::stoi(args[2]));
        }
        std::string out = "{";
        for (const auto& [faction, b] : crime.bounties()) {
            out += std::format("{}\"{:08X}\": [{}, {}]", out.size() > 1 ? ", " : "", faction, b.violent, b.non_violent);
        }
        return out + std::format("}} jail {} days {}", crime.jail_faction() ? "yes" : "no", crime.jail_days());
    }
    if (sub == "fine" && args.size() >= 2) {
        m_game->crime().pay_fine(parse_form(args[1]), args.size() > 2 && args[2] == "out", true);
        return "paid";
    }
    if (sub == "jail" && args.size() >= 2) {
        m_game->crime().go_to_jail(parse_form(args[1]), true, true);
        return "jailed";
    }
    if (sub == "serve") {
        m_game->crime().serve_sentence();
        return "served";
    }
    if (sub == "steal" && args.size() >= 3) {
        m_game->crime().report_theft(parse_form(args[1]), parse_form(args[2]), 1, 0, m_game->owner_of(parse_form(args[1])), false);
        return "reported";
    }
    if (sub == "learn" && args.size() >= 2) {
        m_game->player().known_enchantments.insert(parse_form(args[1]));
        return "learned";
    }
    if (sub == "cast" && args.size() >= 3) {
        // <spell> <target>: its effects land on the target from the player, for what they show there.
        return m_game->magic_target(parse_form(args[2])).apply(parse_form(args[1]), game::k_player_ref) ? "cast" : "not cast";
    }
    if (sub == "ready" && args.size() >= 2) {
        // <spell> [right|left]: in both hands or that one, weapon out, for casting art.
        if (args.size() < 3 || args[2] == "left") {
            m_game->equip_spell(parse_form(args[1]), 1);
        }
        if (args.size() < 3 || args[2] == "right") {
            m_game->equip_spell(parse_form(args[1]), 0);
        }
        m_game->combat().fighter(game::k_player_ref).weapon_drawn = true;
        return "ready";
    }
    if (sub == "inventory") {
        std::string out;
        for (const game::Stack& s : m_game->player().inventory.stacks()) {
            out += std::format("{:08X} {} x{} worn {} value {}", s.form, m_game->display_name(s), s.count,
                               int(s.worn), m_game->value(s));
            out += '\n';
        }
        return out;
    }
    if (sub == "skill" && args.size() >= 3) {
        m_game->character().values().set_base(std::stoi(args[1]), std::stof(args[2]));
        return args[2];
    }
    if (sub == "character") {
        // game character [use <av> <amount> | increment <av> <n> | set <av> <value> | level <av> | perk <form> | spell <form>
        //                 | remove <form> | cast <form> | race <form> | condition <function> <param1>]: what follows is the state.
        game::Character& c = m_game->character();
        const std::string what = args.size() >= 2 ? args[1] : "";
        if (what == "use" && args.size() >= 4) {
            c.use_skill(std::stoi(args[2]), std::stof(args[3]));
        } else if (what == "increment" && args.size() >= 4) {
            c.increment_skill(std::stoi(args[2]), std::stoi(args[3]));
        } else if (what == "set" && args.size() >= 4) {
            c.values().set_base(std::stoi(args[2]), std::stof(args[3]));
        } else if (what == "level" && args.size() >= 3) {
            c.count_level();
            c.add_perk_points(1);
            c.choose_attribute(std::stoi(args[2]));
        } else if (what == "perk" && args.size() >= 3) {
            return std::format("take {}", c.take_perk(parse_form(args[2])));
        } else if (what == "spell" && args.size() >= 3) {
            return std::format("add {}", c.add_spell(parse_form(args[2])));
        } else if (what == "remove" && args.size() >= 3) {
            return std::format("remove {}", c.remove_spell(parse_form(args[2])));
        } else if (what == "cast" && args.size() >= 3) {
            return std::format("apply {}", c.apply(parse_form(args[2])));
        } else if ((what == "race" || what == "racekeep") && args.size() >= 3) {
            c.switch_race(parse_form(args[2]), what == "racekeep");
        } else if (what == "skill" && args.size() >= 3) {
            const int av = std::stoi(args[2]);
            const ui::StatsPlayer p = m_character_menus->player();
            return std::format("{} base {} percent {} player {} / magicka {} {} {}", c.perks().skill(av).name, c.values().base(av),
                               c.skill_percent(av), p.percent, p.magicka.current, p.magicka.maximum, p.magicka.color);
        } else if (what == "entry" && args.size() >= 4) {
            return std::format("{}", c.entry_point(std::stoi(args[2]), std::stof(args[3])));
        } else if (what == "give" && args.size() >= 3) {
            m_game->player().inventory.add(parse_form(args[2]), 1);
            return "given";
        } else if (what == "stage" && args.size() >= 4) {
            game::Quest* q = m_papyrus->quests()->quest(parse_form(args[2]));
            return std::format("stage {}", q && m_papyrus->quests()->set_stage(*q, uint16_t(std::stoi(args[3]))));
        } else if (what == "native" && args.size() >= 5) {
            // native <Class> <form> <Name> [i:int r:real b:bool Class:form ...]: the result comes through the log.
            if (!m_papyrus) {
                return "scripts aren't running (papyrus new_game)";
            }
            script::VirtualMachine& vm = m_papyrus->vm();
            const uint32_t form = parse_form(args[3]); // 0: a global function of the class
            const script::Value self = form ? vm.form_value(form, args[2]) : script::Value{};
            if (form && !self.object) {
                return "no such object";
            }
            std::vector<script::Value> values;
            for (size_t i = 5; i < args.size(); ++i) {
                const size_t colon = args[i].find(':');
                const std::string kind = args[i].substr(0, colon), text = args[i].substr(colon + 1);
                values.push_back(kind == "i"   ? script::Value::of(int32_t(std::stoi(text)))
                                 : kind == "r" ? script::Value::of(std::stof(text))
                                 : kind == "b" ? script::Value::of(text != "0")
                                 : kind == "s" ? script::Value::of_string(vm.intern(text))
                                               : vm.form_value(parse_form(text), kind));
            }
            const std::string label = args[4];
            const auto report = [&vm, label](const script::Value& v) { log::info("native {} = {}", label, vm.to_string(v)); };
            if (form) {
                vm.call_method(self.object, label, std::move(values), report);
            } else {
                vm.call_static(args[2], label, std::move(values), report);
            }
            return "called";
        } else if (what == "disabled" && args.size() >= 3) {
            return std::format("{}", m_game->ref_disabled(parse_form(args[2])));
        } else if (what == "sliders") {
            std::string out;
            for (const ui::RaceSexSlider& l : m_character_menus->sliders(false)) {
                out += std::format("{}|{}|{}|{}|{}|{}|{:.2f}|{}\n", l.label, l.filter, l.callback, l.min, l.max, l.position, l.step, l.id);
            }
            return out;
        } else if (what == "raceindex" && args.size() >= 3) {
            m_character_menus->change_race(std::stoi(args[2]));
            return m_character_menus->races().at(size_t(std::stoi(args[2]))).name;
        } else if (what == "refs" && args.size() >= 3) {
            const uint32_t base = parse_form(args[2]);
            std::string list;
            for (const forms::FormEntry* e : m_game->db().of_type(forms::sig("REFR"))) {
                if (const auto name = m_game->db().field(*e, forms::sig("NAME")); name && name->as<uint32_t>() == base) {
                    list += std::format("{:08X} ", e->form_id);
                }
            }
            return list;
        } else if (what == "activate" && args.size() >= 4) {
            papyrus_activate(parse_form(args[2]), parse_form(args[3]));
            return "activated";
        } else if (what == "condition" && args.size() >= 4) {
            return std::format("{}", m_game->conditions().value(uint16_t(std::stoi(args[2])), game::k_player_ref, 0, parse_form(args[3]), 0));
        }
        std::string j = std::format("{{\"level\": {}, \"xp\": {:.2f}, \"threshold\": {:.0f}, \"points\": {}, \"race\": \"{:08X}\", \"values\": {{",
                                    c.level(), c.experience(), c.experience_threshold(), c.perk_points(), c.race());
        for (int av : {6, 11, 13, 14, 15, 17, 20, 24, 25, 26, 27, 28, 29, 32, 39, 44, 45, 155, 156, 157}) {
            const game::ActorValues& v = c.values();
            j += std::format("{}\"{}\": [{:.2f}, {:.2f}, {:.2f}]", av == 6 ? "" : ", ", game::actor_value_info(av).name, v.base(av),
                             v.permanent(av), v.current(av));
        }
        j += "}, \"effects\": [";
        for (const game::ActiveEffect& e : c.effects()) {
            j += std::format("\"{:08X}/{:08X} {:.1f}{}\", ", e.spell, e.effect, e.magnitude, e.active ? "" : " off");
        }
        return j + "]}";
    }
    if (sub == "player") {
        const game::Player& p = m_game->player();
        size_t created = 0;
        m_game->ai().for_each([&](const game::Agent& a) { created += a.ref >= game::k_created_refs; });
        std::string j = std::format("{{\"effects\": {}, \"created_actors\": {}, \"crimes\": {}, ",
                                    m_game->character().effects().size(), created, m_game->crime().crimes_under_way());
        j += std::format("\"gold\": {}, \"weight\": {:.2f}, \"carry\": {:.0f}, \"armor\": {:.0f}, \"damage\": {}, "
                                    "\"skills\": [",
                                    p.inventory.gold(), m_game->encumbrance(), m_game->carry_weight(),
                                    m_game->total_armor(), m_game->total_damage());
        const game::Character& c = m_game->character();
        for (int i = 0; i < game::k_skill_count; ++i) {
            const int av = game::k_first_skill + i;
            j += std::format("{}[{:.0f}, {:.2f}]", i ? ", " : "", c.values().base(av), c.progress(av).xp);
        }
        return j + "]}";
    }
    if (sub == "barter" && args.size() >= 2) {
        return m_item_menus->open_vendor(parse_form(args[1])) ? "barter" : "not a vendor";
    }
    if (sub == "card" && args.size() >= 2) {
        return m_item_menus->card_text(parse_form(args[1]));
    }
    if (sub == "activate") {
        if (args.size() >= 2) {
            activate_target(target_of(parse_form(args[1])));
        } else {
            activate();
        }
        return "activated";
    }
    throw Error("game: start | additem <form> [count] | target [ref] | activate");
}

// The trainer's class says what they teach and how far (teaches, maximum level).
std::optional<std::pair<int, int>> App::trainer_skill() const {
    const forms::Database& db = m_game->db();
    const forms::FormEntry* npc = m_trainer ? db.find(m_game->base_of(m_trainer)) : nullptr;
    if (!npc) {
        return std::nullopt;
    }
    const forms::Form n(db, *npc);
    const forms::Value* klass = n.get("CNAM");
    const forms::FormEntry* e = klass ? db.find(klass->form) : nullptr;
    if (!e) {
        return std::nullopt;
    }
    const forms::Form c(db, *e);
    const forms::Value* data = c.get("DATA");
    const forms::Value* teaches = data ? data->path("teaches") : nullptr;
    const forms::Value* level = data ? data->path("maximum_training_level") : nullptr;
    if (!teaches || !level) {
        return std::nullopt;
    }
    return std::pair{game::k_first_skill_value + int(teaches->integer), int(level->integer)};
}

bool App::ui_training_card(ui::TrainingCard& card) {
    const auto taught = m_game ? trainer_skill() : std::nullopt;
    if (!taught) {
        return false;
    }
    const int av = taught->first;
    const game::Character& c = m_game->character();
    const forms::FormEntry* gold = m_game->db().find(game::k_gold);
    card.skill_name = m_game->skill_name(av);
    card.gold_name = gold ? std::string(forms::Form(m_game->db(), *gold).name()) : std::string();
    card.max_level = taught->second;
    card.times_trained = m_game->stat(std::format("Trained At Level {}", c.level()));
    card.player_level = c.level();
    card.base = c.values().base(av);
    card.permanent = c.skill(av);
    card.progress = c.skill_percent(av);
    card.gold = m_game->player().inventory.gold();
    card.xp = c.experience();
    return true;
}

// TrainingMenu::Train: the gold goes to the trainer, the skill up a level, the session counted against the
// player's level (player; a level-up clears it).
void App::ui_train(int cost) {
    const auto taught = trainer_skill();
    if (!taught) {
        return;
    }
    m_game->player().inventory.remove(game::k_gold, cost);
    m_game->container(m_game->base_of(m_trainer)).add(game::k_gold, cost);
    m_game->increment_skill(taught->first, 1);
    m_game->increment_stat(std::format("Trained At Level {}", m_game->character().level()));
    m_game->increment_stat("Training Sessions");
}

void App::update_los_events() {
    if (!m_game || !m_papyrus || m_los.empty()) {
        return;
    }
    game::CombatWorld& world = m_game->combat().world();
    for (size_t tried = 0; tried < m_los.size(); ++tried) {
        m_los_cursor = (m_los_cursor + 1) % m_los.size();
        LosRecord& r = m_los[m_los_cursor];
        game::CombatBody viewer, target;
        if (!world.body(r.viewer, viewer) || !world.body(r.target, target)) {
            continue;
        }
        // The player sees what's in the view; an NPC's is the in-cone flag of what it knows, its detection here.
        bool real = true;
        bool sees = false;
        if (r.viewer == game::k_player_ref) {
            const glm::vec3 to = glm::vec3(target.position[0], target.position[1], target.position[2] + target.height * 0.5f) * units::k_skyrim_unit_m - m_camera.position;
            real = glm::dot(glm::normalize(to), m_camera.forward()) > std::cos(m_camera.fov_y);
            sees = real && world.line_of_sight(r.viewer, r.target);
        } else {
            sees = world.detection(r.viewer, r.target) > 0;
        }
        const bool eligible = r.type == 2 || (r.type == 0 && sees) || (r.type == 1 && !sees);
        const int state = sees ? 1 : 2;
        if (eligible && state != r.last) {
            script::VirtualMachine& vm = m_papyrus->vm();
            const uint64_t handle = r.handle;
            const std::vector<script::Value> args = {vm.form_value(r.viewer, "Actor"), vm.form_value(r.target, "ObjectReference")};
            const bool single = r.type != 2;
            vm.send_event(handle, sees ? "OnGainLOS" : "OnLostLOS", args);
            if (single) {
                m_los.erase(m_los.begin() + std::ptrdiff_t(m_los_cursor));
                return;
            }
        }
        r.last = state;
        if (real) {
            return;
        }
    }
}

// A picked plant shows its model's other switch child: a second mesh, where the model has one.
void App::show_harvested(const osw::Ref& r) {
    const std::string mesh = path::replace_extension(m_space->world().meshes[r.mesh], ".harvested.osm");
    std::error_code ec;
    if (!std::filesystem::exists(m_opt.cache / mesh, ec)) {
        return;
    }
    m_space->set_ref_disabled(r.form_id, true);
    m_space->spawn(mesh, glm::vec3(r.position[0], r.position[1], r.position[2]),
                   glm::quat(r.rotation[3], r.rotation[0], r.rotation[1], r.rotation[2]), r.scale, r.form_id);
}

} // namespace spud::engine
