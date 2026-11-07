// Papyrus natives on where references are and whether they live.

#include "engine/app_internal.hpp"

#include "core/binary.hpp"
#include "core/units.hpp"
#include "formats/ost.hpp"
#include "game/game.hpp"
#include "game/quests.hpp"

#include <algorithm>
#include <map>
#include <glm/glm.hpp>

#include <cmath>

namespace spud::engine {

namespace {

using script::NativeCall;
using script::Value;

uint32_t form_of(const Value& v) {
    return v.tag == script::Tag::object && v.object ? script::handle_form(v.object->handle) : 0;
}

uint32_t self_form(const NativeCall& c) { return c.self ? script::handle_form(c.self->handle) : 0; }

} // namespace

void App::bind_world_natives() {
    script::VirtualMachine& vm = m_papyrus->vm();
    auto bind = [&vm](const char* script_name, const char* name, script::NativeFunction fn) {
        vm.bind_native(script_name, name, std::move(fn), script::native_flags(script_name, name));
    };
    // Scripts' effects on a reference: an EFSH, or an RFCT's art and shader. Stop plays out the end.
    const auto scripted_effects = std::make_shared<std::map<std::pair<uint32_t, uint32_t>, uint32_t>>();
    const auto start_effect = [this, scripted_effects](NativeCall& c, uint32_t art, uint32_t shader) {
        const uint32_t target = form_of(c.args[0]);
        if (!target) {
            c.vm.log_error(c.stack, "Cannot play an effect shader on a None object", 2);
            return;
        }
        RefEffects::Hit hit;
        hit.id = m_game->next_effect_id();
        hit.target = target;
        hit.art = art;
        hit.shader = shader;
        hit.persist = c.args[1].real < 0.0;
        hit.lifetime = float(c.args[1].real);
        ref_effects().start(hit);
        (*scripted_effects)[{self_form(c), target}] = hit.id;
    };
    const auto stop_effect = [this, scripted_effects](NativeCall& c) {
        const auto it = scripted_effects->find({self_form(c), form_of(c.args[0])});
        if (it != scripted_effects->end()) {
            ref_effects().finish(it->second);
            scripted_effects->erase(it);
        }
    };
    bind("EffectShader", "Play", [start_effect](NativeCall& c) { start_effect(c, 0, self_form(c)); });
    bind("EffectShader", "Stop", stop_effect);
    bind("VisualEffect", "Play", [this, start_effect](NativeCall& c) {
        // RFCT DATA: art, shader, flags.
        const forms::FormEntry* e = m_game->db().find(self_form(c));
        const forms::Value* data = e ? forms::Form(m_game->db(), *e).get("DATA") : nullptr;
        const forms::Value* art = data ? data->member("effect_art") : nullptr;
        const forms::Value* shader = data ? data->member("shader") : nullptr;
        start_effect(c, art ? art->form : 0, shader ? shader->form : 0);
    });
    bind("VisualEffect", "Stop", stop_effect);
    bind("Game", "SetPlayerReportCrime", [this](NativeCall& c) { m_game->crime().reporting = c.args[0].boolean; });
    bind("ObjectReference", "GetCurrentLocation", [this](NativeCall& c) {
        if (const uint32_t location = ref_location(self_form(c))) {
            c.result = c.vm.form_value(location, "Location");
        }
    });
    bind("ObjectReference", "IsInLocation", [this](NativeCall& c) {
        uint32_t location = ref_location(self_form(c));
        const uint32_t wanted = form_of(c.args[0]);
        while (location && location != wanted) {
            location = m_world_sim->location_parent(location);
        }
        c.result = Value::of(location != 0);
    });
}

void App::connect_quest_world() {
    game::QuestWorld::Live& live = m_papyrus->quest_world().live;
    live.loaded_refs = [this] {
        std::vector<uint32_t> refs = m_space ? m_space->loaded_refs() : std::vector<uint32_t>{};
        refs.insert(refs.end(), m_high_actors.begin(), m_high_actors.end());
        return refs;
    };
    live.loaded_actors = [this] { return m_high_actors; };
    live.distance_to_player = [this](uint32_t ref) {
        WorldSim::Place here, place;
        glm::vec3 from, to;
        const bool found = ref_place(script::k_player_ref, here, from) && ref_place(ref, place, to) &&
                           m_world_sim->same_space(here, place);
        return found ? glm::length(from - to) : 1e30f;
    };
    live.ref_location = [this](uint32_t ref) { return ref_location(ref); };
    live.text_setting = [this](std::string_view name) { return m_game->settings().text(name); };
    live.number_setting = [this](std::string_view name) { return m_game->settings().number(name, 0.0f); };
    live.global_value = [this](uint32_t global) {
        float value = 0;
        m_game->global_value(global, value);
        return value;
    };
    live.calendar = [this] {
        return std::array<float, 4>{m_clock.year(), m_clock.month(), m_clock.day(), m_clock.days_passed()};
    };
    live.alias_effects = [this](const game::Alias& a, uint32_t ref, bool on) {
        // The alias's inventory goes to whatever fills it, a levelled list rolled at the player's level. It stays when the
        // alias lets go.
        if (on) {
            game::Inventory& inventory = ref == game::k_player_ref ? m_game->player().inventory : m_game->container(ref);
            for (const auto& [item, count] : a.items) {
                std::vector<game::Rolled> rolled;
                m_game->leveled().roll(item, count, uint16_t(std::max(m_game->character().level(), 1)), rolled);
                for (const game::Rolled& r : rolled) {
                    inventory.add(r.form, r.count, r.extra);
                }
            }
        }
        if (!m_game->is_actor(ref)) {
            return;
        }
        for (const uint32_t faction : a.factions) {
            m_game->set_faction_rank(ref, faction, on ? 0 : game::k_left_faction);
        }
        for (const uint32_t spell : a.spells) {
            if (on) {
                m_game->magic_target(ref).add_spell(spell);
            } else {
                m_game->magic_target(ref).remove_spell(spell);
            }
        }
    };
    live.notice = [this](int type, uint32_t quest, const std::string& text) {
        if (m_ui) {
            m_ui->hud_quest_notice(type, quest, text);
        }
    };
    m_game->set_quests(m_papyrus->quests());
    m_journal = std::make_unique<GameJournal>(*m_game, *m_papyrus->quests());
    m_papyrus->conditions().ref_location = [this](uint32_t ref) { return ref_location(ref); };
    m_papyrus->conditions().created_base = [this](uint32_t ref) { return m_game->placed_base(ref); };
    live.location_cleared = [this](uint32_t location) { return m_world_sim->location_cleared(location); };
    live.create_ref = [this](uint32_t base, uint32_t at, bool inside, uint32_t level) {
        return create_ref(base, at, inside, level);
    };
    live.faction_changes = [this](uint32_t faction) { return m_game->refs_changing_faction(faction); };
    live.ref_state = [this](uint32_t ref, game::RefState& state) {
        state.exists = state.exists || m_game->placed_base(ref) != 0;
        state.disabled = m_game->ref_disabled(ref);
        state.dead = m_world_sim->ref_dead(ref);
    };
}

// at the anchor's place, or into its inventory.
uint32_t App::create_ref(uint32_t base, uint32_t at, bool inside, uint32_t level) {
    WorldSim::Place place;
    glm::vec3 position;
    const forms::FormEntry* e = m_game->db().find(base);
    if (!e || !ref_place(at, place, position)) {
        return 0;
    }
    const uint32_t id = m_game->place_form(base, {position.x, position.y, position.z});
    m_place_of_created[id] = place;
    if (e->type == forms::sig("NPC_") || e->type == forms::sig("LVLN")) {
        uint32_t npc = base;
        if (m_actor_data) {
            const uint32_t zone = m_world_sim->cell_zone(place.cell, place.world);
            m_actor_data->set_created(id, zone, level < 4 ? int32_t(level) : -1);
            npc = m_actor_data->created_npc(base, id);
        }
        if (!npc) {
            m_game->forget_ref(id); // the list gave nobody
            m_place_of_created.erase(id);
            return 0;
        }
        const game::Agent* anchor = m_game->ai().agent(at);
        m_game->ai().add(id, npc, place.world ? place.world : place.cell, {position.x, position.y, position.z},
                         anchor ? anchor->heading : 0.0f, 1.0f);
        return id;
    }
    if (const game::Item* item = m_game->items().find(base)) {
        if (inside) {
            m_game->inventory_of(at).add(base, 1);
        } else if (m_space) {
            m_space->spawn(item->model, position * units::k_skyrim_unit_m, 0.0f, id);
        }
    }
    return id;
}

std::vector<std::byte> App::save_actors() const {
    BinaryWriter w;
    const auto rerolls = m_actor_data ? ost::sorted(m_actor_data->rerolls()) : decltype(ost::sorted(m_actor_data->rerolls())){};
    w.write<uint32_t>(uint32_t(rerolls.size()));
    for (const auto& [ref, count] : rerolls) {
        w.write<uint32_t>(ref);
        w.write<uint32_t>(count);
    }
    const auto created = m_actor_data ? m_actor_data->created() : std::vector<std::array<uint32_t, 3>>{};
    w.write<uint32_t>(uint32_t(created.size()));
    for (const auto& c : created) {
        w.write_span(std::span<const uint32_t>(c));
    }
    const auto places = ost::sorted(m_place_of_created);
    w.write<uint32_t>(uint32_t(places.size()));
    for (const auto& [ref, place] : places) {
        w.write<uint32_t>(ref);
        w.write<uint32_t>(place.world);
        w.write<uint32_t>(place.cell);
    }
    const std::vector<std::byte> game = m_game->save_actors();
    w.write_span(std::span<const std::byte>(game));
    return w.take();
}

void App::load_actors(std::span<const std::byte> bytes) {
    BinaryReader r(bytes);
    std::unordered_map<uint32_t, uint32_t> rerolls;
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ref = r.read<uint32_t>();
        rerolls[ref] = r.read<uint32_t>();
    }
    if (m_actor_data) {
        m_actor_data->set_rerolls(std::move(rerolls));
    }
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ref = r.read<uint32_t>();
        const auto zone = r.read<uint32_t>();
        const auto modifier = int32_t(r.read<uint32_t>());
        if (m_actor_data) {
            m_actor_data->set_created(ref, zone, modifier);
        }
    }
    m_place_of_created.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ref = r.read<uint32_t>();
        WorldSim::Place& place = m_place_of_created[ref];
        place.world = r.read<uint32_t>();
        place.cell = r.read<uint32_t>();
    }
    m_game->load_actors(bytes.subspan(bytes.size() - r.remaining()));
    if (m_ai_ready) {
        m_game->ai().apply_saved();
    }
}

uint32_t App::ref_location(uint32_t ref) {
    if (ref == script::k_player_ref) {
        return m_world_sim->player_location();
    }
    WorldSim::Place place;
    glm::vec3 at;
    if (!ref_place(ref, place, at)) {
        return 0;
    }
    return m_world_sim->cell_location(place.cell, place.world, int32_t(std::floor(at.x / 4096.0f)),
                                      int32_t(std::floor(at.y / 4096.0f)));
}

} // namespace spud::engine
