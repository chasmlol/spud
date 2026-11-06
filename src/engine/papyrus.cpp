#include "core/trace.hpp"
#include "engine/papyrus.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "formats/forms_typed.hpp"
#include "game/character.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace spud {

Papyrus::Papyrus(const std::filesystem::path& cache, GameClock& clock)
    : m_clock(clock), m_scripts(oss::read(read_file(cache / "scripts" / "scripts.oss"))),
      m_forms(std::make_unique<forms::Database>(cache / "forms.osf")), m_host(m_forms.get()) {
    m_host.on_log = [](int severity, std::string_view text) {
        if (severity >= 2) {
            log::warn("papyrus: {}", text);
        } else {
            log::info("papyrus: {}", text);
        }
    };
    m_host.real_time = [this] { return static_cast<double>(static_cast<uint64_t>(m_clock_ms)) * 0.001; };
    m_host.read_global = [this](uint32_t form, float& value) { return m_clock.read_global(form, value); };
    m_host.write_global = [this](uint32_t form, float value) { return m_clock.write_global(form, value); };
    m_vm = std::make_unique<script::VirtualMachine>(m_scripts, m_host);
    script::bind_natives(*m_vm, m_host);
    m_conditions = std::make_unique<game::Conditions>(*m_forms, m_world);
    m_quest_world = std::make_unique<game::QuestWorld>(*m_forms);
    m_quests = std::make_unique<game::QuestSystem>(*m_forms, m_scripts, *m_vm, *m_conditions, *m_quest_world);
}

bool Papyrus::condition_value(uint16_t fn, uint32_t subject, uint32_t target, uint32_t p1, uint32_t p2, double& out) {
    // GetQuestRunning, GetStage, GetStageDone, GetQuestCompleted.
    constexpr uint16_t k_get_quest_running = 56, k_get_stage = 58, k_get_stage_done = 59, k_get_quest_completed = 543;
    // The evaluating quest's aliases and scenes.
    constexpr uint16_t k_get_in_current_loc = 359, k_get_in_current_loc_alias = 360, k_is_scene_playing = 248,
                       k_is_scene_action_complete = 550, k_get_is_editor_location = 565, k_get_is_alias_ref = 566,
                       k_get_is_editor_loc_alias = 567, k_loc_alias_is_location = 605, k_loc_alias_has_keyword = 610,
                       k_get_event_data = 576;
    game::Quest* run = m_quests->quest(m_conditions->run_quest());
    const auto location_alias = [run](uint32_t alias) -> uint32_t {
        if (!run) {
            return 0;
        }
        const auto it = run->filled_locations.find(alias);
        return it == run->filled_locations.end() ? 0 : it->second;
    };
    if (fn == k_get_event_data) {
        // Parameter 1 packs the function and the member's code; the result converts as unsigned.
        const game::StoryEvent* event = m_quests->story_event;
        const game::StoryEvent::Member* m = event ? event->member(uint16_t(p1 >> 16)) : nullptr;
        out = 0.0;
        if (m) {
            const forms::FormEntry* e = m_forms->find(m->value);
            switch (p1 & 0xFFFF) {
            case 0: out = m->value == p2 ? 1.0 : 0.0; break;
            case 1: {
                const forms::FormEntry* list = m_forms->find(p2);
                const auto items = list ? forms::Form(*m_forms, *list).all("LNAM") : std::vector<const forms::Value*>{};
                out = std::ranges::any_of(items, [m](const forms::Value* v) { return v->form == m->value; }) ? 1.0 : 0.0;
                break;
            }
            case 2: out = m->value; break;
            case 3: {
                const auto keywords = e ? forms::Form(*m_forms, *e).keywords() : std::vector<uint32_t>{};
                out = std::find(keywords.begin(), keywords.end(), p2) != keywords.end() ? 1.0 : 0.0;
                break;
            }
            default: break;
            }
        }
        return true;
    }
    switch (fn) {
    case k_is_scene_playing: out = m_quests->scenes().is_playing(p1) ? 1.0 : 0.0; return true;
    case k_is_scene_action_complete: out = m_quests->scenes().action_complete(p1, p2) ? 1.0 : 0.0; return true;
    case k_get_is_alias_ref: {
        out = 0.0;
        if (run) {
            const auto it = run->filled_refs.find(p1);
            out = it != run->filled_refs.end() && it->second == subject ? 1.0 : 0.0;
        }
        return true;
    }
    case k_get_in_current_loc_alias:
    case k_get_is_editor_loc_alias: {
        const uint32_t location = location_alias(p1);
        const uint16_t walk = fn == k_get_in_current_loc_alias ? k_get_in_current_loc : k_get_is_editor_location;
        out = location ? m_conditions->value(walk, subject, target, location, 0) : 0.0;
        return true;
    }
    case k_loc_alias_is_location: out = location_alias(p1) == p2 ? 1.0 : 0.0; return true;
    case 563: { // LocationHasRefType: one of the type that isn't disabled; a dead one past its respawn isn't read, so it counts
        out = 0.0;
        for (const uint32_t ref : m_quests->world().location_refs(p1, p2)) {
            out = !m_quests->world().ref_state(ref).disabled ? 1.0 : out;
        }
        return true;
    }
    case 601: { // GetLocAliasRefTypeAliveCount, as 592 on the alias's location
        out = 0.0;
        for (const uint32_t ref : m_quests->world().location_refs(location_alias(p1), p2)) {
            const game::RefState state = m_quests->world().ref_state(ref);
            out += !state.disabled && !state.dead ? 1.0 : 0.0;
        }
        return true;
    }
    case k_loc_alias_has_keyword: {
        const forms::FormEntry* e = m_forms->find(location_alias(p1));
        const auto keywords = e && p2 ? forms::Form(*m_forms, *e).keywords() : std::vector<uint32_t>{};
        out = std::find(keywords.begin(), keywords.end(), p2) != keywords.end() ? 1.0 : 0.0;
        return true;
    }
    default: break;
    }
    if (fn != k_get_quest_running && fn != k_get_stage && fn != k_get_stage_done && fn != k_get_quest_completed) {
        return false;
    }
    game::Quest* q = m_quests->quest(p1);
    if (!q) {
        out = 0.0;
        return true;
    }
    switch (fn) {
    case k_get_quest_running: out = q->is_running() ? 1.0 : 0.0; break;
    case k_get_stage: out = q->current_stage; break;
    case k_get_stage_done: {
        const game::Stage* s = q->stage(uint16_t(p2));
        out = s && (s->flags & game::stage_done) ? 1.0 : 0.0;
        break;
    }
    default: out = q->is_stopped() && q->current_stage > 0 ? 1.0 : 0.0; break;
    }
    return true;
}

bool Papyrus::global_value(uint32_t global, float& out) {
    if (m_clock.read_global(global, out)) {
        return true;
    }
    const auto it = m_host.global_values.find(global);
    if (it == m_host.global_values.end()) {
        return false;
    }
    out = it->second;
    return true;
}

void Papyrus::set_global(uint32_t global, float value) {
    if (!m_clock.write_global(global, value)) {
        m_host.global_values[global] = value;
    }
}

void Papyrus::activate(uint32_t ref, uint32_t base, uint32_t activator) {
    (void)base;
    ref_event(ref, "OnActivate", {m_vm->form_value(activator, "ObjectReference")});
}

void Papyrus::hit_event(uint32_t victim, uint32_t attacker, uint32_t source, uint32_t projectile, uint32_t flags) {
    ref_event(victim, "OnHit",
              {m_vm->form_value(attacker, "ObjectReference"), m_vm->form_value(source, "Form"),
               m_vm->form_value(projectile, "Projectile"), script::Value::of((flags & 0x10000) != 0),
               script::Value::of((flags & 0x800) != 0), script::Value::of((flags & 0xC000) != 0),
               script::Value::of((flags & 0x3) != 0)});
}

void Papyrus::effect_started(uint32_t id, uint32_t effect, uint32_t caster, uint32_t target) {
    const script::Handle handle = script::active_effect_handle(id);
    m_vm->bind_scripts(effect, -1, handle);
    m_vm->send_event(handle, "OnEffectStart", {m_vm->form_value(target, "Actor"), m_vm->form_value(caster, "Actor")});
}

void Papyrus::effect_finished(uint32_t id, uint32_t caster, uint32_t target) {
    const script::Handle handle = script::active_effect_handle(id);
    m_vm->send_event(handle, "OnEffectFinish", {m_vm->form_value(target, "Actor"), m_vm->form_value(caster, "Actor")});
    m_finished_effects.push_back(handle);
}

void Papyrus::send_sleep(const std::unordered_set<uint64_t>& listeners, std::string_view name, std::vector<script::Value> args) {
    for (const uint64_t handle : listeners) {
        m_vm->send_event(handle, name, args);
    }
}

void Papyrus::player_event(std::string_view name, std::vector<script::Value> args) {
    ref_event(game::k_player_ref, name, std::move(args));
}

void Papyrus::ref_event(uint32_t ref, std::string_view name, std::vector<script::Value> args) {
    // Binds what's on the reference and its base, if nothing has yet.
    m_vm->form_value(ref, "Form");
    const script::Handle handle = script::form_handle(ref);
    {
        m_vm->bind_scripts(ref);
        const uint32_t base = m_host.reference_base(ref);
        if (base && m_bound_bases.insert(ref).second) { // a created reference has no record to bind by, so it's done once here
            m_vm->bind_scripts(base, -1, handle);
        }
    }
    m_vm->send_event(handle, name, args);
    for (const auto& q : m_quests->all_quests()) {
        for (const auto& [alias, filled] : q->filled_refs) {
            if (filled == ref) {
                m_vm->send_event(script::alias_handle(q->form, alias, false), name, args);
            }
        }
    }
    if (effects_on) {
        for (const uint32_t id : effects_on(ref)) {
            m_vm->send_event(script::active_effect_handle(id), name, args);
        }
    }
}

void Papyrus::item_event(uint32_t holder, uint32_t form, std::string_view name, std::vector<script::Value> args) {
    if (const auto it = m_item_handles.find({holder, form}); it != m_item_handles.end()) {
        for (const script::Handle handle : it->second) {
            m_vm->send_event(handle, name, args);
        }
    }
}

void Papyrus::read_event(uint32_t book) {
    if (m_vm->has_scripts(book)) {
        m_vm->bind_scripts(book);
        m_vm->send_event(script::form_handle(book), "OnRead");
    }
}

void Papyrus::trigger_event(uint32_t ref, bool entered, uint32_t who) {
    ref_event(ref, entered ? "OnTriggerEnter" : "OnTriggerLeave", {m_vm->form_value(who, "ObjectReference")});
}

void Papyrus::remove_inventory_filter(script::Handle handle, uint32_t form) {
    if (const auto it = m_inventory_filters.find(handle); it != m_inventory_filters.end()) {
        std::erase(it->second, form);
    }
}

void Papyrus::item_moved(uint32_t form, int32_t count, uint32_t from, uint32_t to) {
    const script::Value base = m_vm->form_value(form, "Form");
    const script::Value none = m_vm->form_value(0, "ObjectReference");
    const auto ref = [this](uint32_t r) { return m_vm->form_value(r, "ObjectReference"); };
    // a scripted item is an object of its own while it's in an inventory.
    if (m_vm->has_scripts(form)) {
        auto& held = m_item_handles[{from, form}];
        script::Handle handle = script::k_no_handle;
        if (from && !held.empty()) {
            handle = held.back();
            held.pop_back();
        } else {
            handle = script::inventory_handle(form, m_next_item++);
            m_vm->bind_scripts(form, -1, handle);
        }
        if (to) {
            m_item_handles[{to, form}].push_back(handle);
        } else {
            m_finished_effects.push_back(handle);
        }
        m_vm->send_event(handle, "OnContainerChanged", {ref(to), ref(from)});
    }
    const auto listening = [this, form](script::Handle handle) {
        // no filter passes everything.
        const auto it = m_inventory_filters.find(handle);
        if (it == m_inventory_filters.end() || it->second.empty()) {
            return true;
        }
        for (const uint32_t filter : it->second) {
            if (filter == form) {
                return true;
            }
            if (const forms::FormEntry* e = m_forms->find(filter); e && e->type == forms::sig("FLST")) {
                const auto list = forms::form_list(forms::Form(*m_forms, *e));
                if (std::find(list.begin(), list.end(), form) != list.end()) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto tell = [&](uint32_t container, std::string_view event, uint32_t other) {
        if (!container) {
            return;
        }
        const std::vector<script::Value> args = {base, script::Value::of(count), none, ref(other)};
        const script::Handle own = script::form_handle(container);
        if (listening(own)) {
            m_vm->send_event(own, event, args);
        }
        for (const auto& q : m_quests->all_quests()) {
            for (const auto& [alias, filled] : q->filled_refs) {
                const script::Handle handle = script::alias_handle(q->form, alias, false);
                if (filled == container && listening(handle)) {
                    m_vm->send_event(handle, event, args);
                }
            }
        }
    };
    tell(from, "OnItemRemoved", to);
    tell(to, "OnItemAdded", from);
}

bool Papyrus::quest_variable(uint32_t quest, const std::string& name, double& out) {
    const script::Value* v = m_vm->find_variable(script::form_handle(quest), name);
    if (!v) {
        return false;
    }
    switch (v->tag) {
    case script::Tag::integer: out = v->integer; break;
    case script::Tag::real: out = v->real; break;
    case script::Tag::boolean: out = v->boolean ? 1.0 : 0.0; break;
    default: return false;
    }
    return true;
}

void Papyrus::run_perk_fragment(uint32_t target, uint32_t perk, int fragment) {
    // The perk's script object gets Fragment_<n>(target).
    m_vm->bind_scripts(perk);
    for (script::Object* o : m_vm->objects(script::form_handle(perk))) {
        m_vm->call_method(o, std::format("Fragment_{}", fragment), {m_vm->form_value(target, "ObjectReference")});
    }
}

void Papyrus::attach_dialogue(game::DialogueOutput& output) {
    m_dialogue = std::make_unique<game::Dialogue>(*m_quests, output);
}

void Papyrus::new_game() {
    m_quests->new_game();
}

Papyrus::~Papyrus() = default;

void Papyrus::update(float dt_seconds, bool menu_mode) {
    m_clock_ms += static_cast<double>(dt_seconds) * 1000.0;
    const auto now = static_cast<uint64_t>(m_clock_ms);
    m_host.days_passed = m_clock.days_passed();
    m_host.menu_mode = menu_mode;
    {
        SPUD_TRACE("quests");
        m_quests->update(dt_seconds, m_clock.days_passed(), menu_mode);
    }
    for (const script::Handle h : std::exchange(m_finished_effects, {})) {
        m_vm->unbind(h);
    }
    if (m_dialogue) {
        SPUD_TRACE("dialogue");
        m_dialogue->update(dt_seconds, m_clock.days_passed());
    }
    {
        SPUD_TRACE("vm update");
        m_vm->update(static_cast<uint32_t>(now - m_last_ms));
    }
    m_last_ms = now;
    SPUD_TRACE("vm dispatch");
    m_vm->dispatch();
}

void Papyrus::fast_travel_ended(float hours) {
    m_vm->send_event(script::form_handle(script::k_player_ref), "OnPlayerFastTravelEnd", {script::Value::of(hours)});
}

void Papyrus::after_render() {
    m_vm->dispatch();
}

bool Papyrus::call(std::string_view target) {
    const size_t dot = target.find('.');
    if (dot == target.npos || !m_vm->find_class(target.substr(0, dot))) {
        return false;
    }
    m_vm->call_static(target.substr(0, dot), target.substr(dot + 1));
    return true;
}

} // namespace spud
