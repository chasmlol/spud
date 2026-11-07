#include "game/scenes.hpp"

#include "core/log.hpp"

#include "script/natives.hpp"

#include <algorithm>
#include <format>

namespace spud::game {

namespace {

constexpr uint32_t k_actor_optional = 0x2;
constexpr uint32_t k_player = 0x14;

} // namespace

Scenes::Scenes(QuestSystem& quests) : m_quests(quests) {
    load_records();
}

void Scenes::load_records() {
    const forms::Database& db = m_quests.db();
    for (const forms::FormEntry* e : db.of_type(forms::sig("SCEN"))) {
        Scene s;
        s.form = e->form_id;
        Phase* phase = nullptr;
        Action* action = nullptr;
        Actor* actor = nullptr;
        const forms::Form form(db, *e);
        for (const forms::DecodedField& f : form.fields()) {
            const std::string_view name = f.def ? std::string_view(f.def->name) : std::string_view{};
            const forms::Value& v = f.value;
            const auto u32 = static_cast<uint32_t>(v.integer);
            if (name == "flags") {
                s.flags = u32;
            } else if (name == "phase_start") {
                s.phases.emplace_back();
                phase = &s.phases.back();
            } else if (name == "phase_start_condition") {
                phase->start_conditions.push_back(condition_item(v));
            } else if (name == "phase_completion_condition") {
                phase->completion_conditions.push_back(condition_item(v));
            } else if (name == "actor_id") {
                s.actors.push_back({static_cast<int32_t>(v.integer), 0, 0x1A});
                actor = &s.actors.back();
            } else if (name == "actor_flags" && actor) {
                actor->flags = u32;
            } else if (name == "actor_behaviour_flags" && actor) {
                actor->behaviours = u32;
            } else if (name == "action_type") {
                s.actions.emplace_back();
                action = &s.actions.back();
                action->type = static_cast<uint16_t>(v.integer);
            } else if (name == "action_actor_id") {
                action->alias = static_cast<int32_t>(v.integer);
            } else if (name == "action_index") {
                action->index = u32 & 0x3FFFFFFF;
            } else if (name == "action_flags") {
                action->flags = u32;
            } else if (name == "action_start_phase") {
                action->start_phase = u32;
            } else if (name == "action_end_phase") {
                action->end_phase = u32;
            } else if (name == "timer_duration") {
                action->duration = static_cast<float>(v.real);
            } else if (name == "dialogue_topic") {
                action->topic = v.form;
            } else if (name == "dialogue_headtrack_actor_id") {
                action->head_track = static_cast<int32_t>(v.integer);
            } else if (name == "dialogue_looping_max") {
                action->loop_max = static_cast<float>(v.real);
            } else if (name == "dialogue_looping_min") {
                action->loop_min = static_cast<float>(v.real);
            } else if (name == "package") {
                action->packages.push_back(v.form);
            } else if (name == "quest") {
                s.quest = v.form;
            } else if (name == "condition") {
                s.conditions.push_back(condition_item(v));
            }
        }
        // Init: a scene without a quest does nothing, and actors and actions need an alias of it.
        Quest* q = m_quests.quest(s.quest);
        if (!q) {
            continue;
        }
        std::erase_if(s.actors, [q](const Actor& a) { return a.alias != -1 && !q->alias(static_cast<uint32_t>(a.alias)); });
        std::erase_if(s.actions, [q](const Action& a) { return a.alias != -1 && !q->alias(static_cast<uint32_t>(a.alias)); });
        q->scenes.push_back(s.form);
        if (q->story_event == event_code("ADIA")) {
            s.random = true;
            s.flags |= scene_interruptible;
        }
        m_index.emplace(s.form, m_scenes.size());
        m_scenes.push_back(std::move(s));
    }
}

Scenes::Scene* Scenes::scene(uint32_t form) {
    auto it = m_index.find(form);
    return it == m_index.end() ? nullptr : &m_scenes[it->second];
}

const Scenes::Scene* Scenes::scene(uint32_t form) const {
    auto it = m_index.find(form);
    return it == m_index.end() ? nullptr : &m_scenes[it->second];
}

uint32_t Scenes::alias_ref(const Scene& s, int32_t alias) {
    const Quest* q = m_quests.quest(s.quest);
    if (!q || alias < 0) {
        return 0;
    }
    auto it = q->filled_refs.find(static_cast<uint32_t>(alias));
    return it == q->filled_refs.end() ? 0 : it->second;
}

bool Scenes::quest_running(const Scene& s) {
    const Quest* q = m_quests.quest(s.quest);
    return q && q->is_running();
}

std::string Scenes::describe(uint32_t form) const {
    const Scene* s = scene(form);
    if (!s || !s->playing) {
        return {};
    }
    std::string out = std::format("scene {:08X} phase {}/{}", s->form, s->phase, s->phases.size());
    for (const Action& a : s->actions) {
        if (s->phase >= 0 && uint32_t(s->phase) >= a.start_phase && uint32_t(s->phase) <= a.end_phase) {
            out += std::format(" [action {} type {} alias {}{}{}{}]", a.index, a.type, a.alias, a.running ? " running" : "",
                               a.complete ? " done" : "", a.saying ? " saying" : "");
        }
    }
    return out;
}

bool Scenes::is_playing(uint32_t form) const {
    const Scene* s = scene(form);
    return s && s->playing;
}

int Scenes::current_phase(uint32_t form) const {
    const Scene* s = scene(form);
    return s ? s->phase : -1;
}

bool Scenes::action_complete(uint32_t form, uint32_t index) const {
    if (const Scene* s = scene(form)) {
        for (const Action& a : s->actions) {
            if (a.index == index) {
                return a.complete;
            }
        }
    }
    return false;
}

// 10: the fragment's call, or the callback at once.
void Scenes::fragment(Scene& s, oss::FragmentKind kind, uint32_t index, int32_t entry, std::function<void()> done) {
    const auto& list = m_quests.scripts().fragments;
    auto it = std::ranges::lower_bound(list, s.form, {}, &oss::Fragment::form_id);
    for (; it != list.end() && it->form_id == s.form; ++it) {
        if (it->kind != kind || it->index != index || it->entry != entry) {
            continue;
        }
        script::VirtualMachine& vm = m_quests.vm();
        vm.bind_scripts(s.form);
        script::Class* type = vm.find_class(m_quests.scripts().string(it->script));
        script::Object* object = type ? vm.object_for(script::form_handle(s.form), type) : nullptr;
        if (object) {
            vm.call_method(object, m_quests.scripts().string(it->function), {}, [done](const script::Value&) { done(); });
            return;
        }
        break;
    }
    done();
}

bool Scenes::start(uint32_t form) {
    Scene* s = scene(form);
    if (!s || !quest_running(*s)) {
        return false;
    }
    s->playing = true;
    return true;
}

bool Scenes::force_start(uint32_t form) {
    Scene* s = scene(form);
    if (!s || !quest_running(*s)) {
        return false;
    }
    s->playing = true;
    begin(*s);
    return true;
}

void Scenes::stop(uint32_t form) {
    if (Scene* s = scene(form)) {
        set_playing(*s, false);
    }
}

void Scenes::quest_started(Quest& q) {
    for (uint32_t form : q.scenes) {
        if (Scene* s = scene(form); s && (s->flags & scene_begin_on_quest_start)) {
            s->playing = true;
        }
    }
}

void Scenes::quest_stopped(Quest& q) {
    for (uint32_t form : q.scenes) {
        if (Scene* s = scene(form)) {
            set_playing(*s, false);
        }
    }
}

void Scenes::ref_left(Quest&, uint32_t) {}

void Scenes::alias_changed(Quest& q, uint32_t alias) {
    for (uint32_t form : q.scenes) {
        Scene* s = scene(form);
        if (!s || !s->playing) {
            continue;
        }
        for (Action& a : s->actions) {
            const bool in_range = s->phase >= 0 && static_cast<uint32_t>(s->phase) >= a.start_phase && static_cast<uint32_t>(s->phase) <= a.end_phase;
            if (a.alias == static_cast<int32_t>(alias) && (a.running || in_range)) {
                a.complete = false;
                action_start(*s, a, 0);
                s->all_done = false;
            }
        }
    }
}

// 3.1 step 5.
void Scenes::begin(Scene& s) {
    if (m_quests.traced() == s.quest) {
        log::info("scene {:08X}: begins", s.form);
    }
    Scene* p = &s;
    p->fragment_done = false;
    p->phase = 0;
    p->hold = true;
    fragment(s, oss::FragmentKind::scene_begin, 0, 0, [p] { p->fragment_done = true; });
}

// then the begin step's test. True when the scene may begin.
bool Scenes::update_random(Scene& s, float dt) {
    const Quest* q = m_quests.quest(s.quest);
    Conversations* talk = m_quests.conversations;
    if (!q || !talk) {
        return true;
    }
    std::array<uint32_t, 2> pair{};
    size_t filled = 0;
    bool actors = true;
    for (const auto& alias : q->aliases) {
        const auto it = q->filled_refs.find(alias.id);
        if (it == q->filled_refs.end() || !it->second) {
            continue;
        }
        actors = actors && m_actors->is_actor(it->second);
        if (filled < 2) {
            pair[filled] = it->second;
        }
        if (++filled == 2) {
            break;
        }
    }
    if (filled == 2 && actors) {
        s.pair = pair;
        if (!m_random_busy) {
            m_random_countdown -= dt;
        }
        if (m_random_countdown <= 0.0f && !s.speaker) {
            if (has_actor(pair[0]) || has_actor(pair[1])) {
                m_random_countdown = 1.0f;
            } else if (!talk->can_talk(pair[0], pair[1])) {
                set_playing(s, false);
                return false;
            } else {
                s.speaker = talk->initiator(pair[0], pair[1]);
                s.spoken_to = s.speaker == pair[0] ? pair[1] : pair[0];
                talk->start_talk(s.speaker, s.spoken_to);
                s.end_delay = talk->setting("fAIWalkAwayTimerForConversation", 2.0f);
            }
        }
    }
    if (m_random_busy || m_random_countdown > 0.0f) {
        return false;
    }
    m_random_busy = true;
    const float low = talk->setting("fRandomSceneAgainMinTime", 5.0f), high = talk->setting("fRandomSceneAgainMaxTime", 10.0f);
    m_random_countdown = low + (high - low) * float(GameRandom::shared().next_unit());
    return true;
}

uint32_t Scenes::scene_of(uint32_t ref) {
    for (Scene& s : m_scenes) {
        if (s.playing && std::ranges::any_of(s.actors, [&](const Actor& a) { return alias_ref(s, a.alias) == ref; })) {
            return s.form;
        }
    }
    return 0;
}

bool Scenes::has_actor(uint32_t ref) const {
    return std::ranges::find(m_in_scene, ref) != m_in_scene.end();
}

void Scenes::update_start(Scene& s, float dt) {
    if (s.end_sent && s.fragment_done) {
        set_playing(s, false, "its end ran");
        return;
    }
    if (!quest_running(s)) {
        set_playing(s, false, "quest not running");
        return;
    }
    if (s.phase == -1) {
        for (const Actor& a : s.actors) {
            if (const uint32_t ref = alias_ref(s, a.alias); ref && m_actors->is_actor(ref) && m_actors->is_dead(ref) && (a.behaviours & 0x3)) {
                if (!(a.flags & k_actor_optional)) {
                    set_playing(s, false, "an actor is dead");
                    return;
                }
            }
        }
        if (s.random && !update_random(s, dt)) {
            return;
        }
        begin(s);
    } else if (s.hold && s.fragment_done && (!s.speaker || !m_quests.conversations || m_quests.conversations->talk_ready(s.speaker))) {
        s.fragment_done = false;
        s.hold = false;
    }
    if (s.speaker && m_quests.conversations && m_quests.conversations->talk_broken(s.speaker, s.spoken_to)) {
        set_playing(s, false);
    }
}

void Scenes::action_start(Scene& s, Action& a, float dt) {
    switch (a.type) {
    case 0: {
        if ((a.flags & action_looping) && a.delay > 0) {
            a.delay -= dt;
            return;
        }
        a.running = true;
        a.saying = false;
        const uint32_t ref = alias_ref(s, a.alias);
        if (!ref) {
            action_end(s, a);
            return;
        }
        if (a.flags & action_looping) {
            a.delay = a.loop_min + (a.loop_max - a.loop_min) * 0.5f;
        }
        break;
    }
    case 1: {
        a.running = true;
        const uint32_t ref = alias_ref(s, a.alias);
        if (!ref || !m_actors->is_actor(ref) || a.packages.empty() || !m_actors->start_package(ref, a.packages[a.package])) {
            action_end(s, a);
        }
        break;
    }
    default: a.running = true; break;
    }
}

void Scenes::action_update(Scene& s, Action& a, float dt) {
    const uint32_t ref = alias_ref(s, a.alias);
    switch (a.type) {
    case 0:
        if (a.saying) {
            if (!m_actors->is_saying(ref)) {
                action_end(s, a);
            }
        } else if (!a.topic || !m_actors->start_saying(ref, a.topic, alias_ref(s, a.head_track))) {
            action_end(s, a);
        } else {
            a.saying = true;
        }
        if ((a.flags & action_looping) && !m_actors->is_saying(ref)) {
            action_start(s, a, dt);
        }
        break;
    case 1:
        if (!a.packages.empty() && m_actors->package_done(ref, a.packages[a.package])) {
            action_end(s, a);
        }
        break;
    default:
        a.elapsed += dt;
        if (a.elapsed >= a.duration || a.event_sent) {
            action_end(s, a);
        }
        break;
    }
}

void Scenes::action_end(Scene& s, Action& a) {
    if (a.type == 2) {
        // Two-step (5.3). No VMAD holds action fragments, so the callback reports at once.
        if (!a.event_sent) {
            a.event_sent = true;
            a.fragment_done = true;
            return;
        }
        if (a.fragment_done) {
            a.running = false;
            a.complete = true;
            a.event_sent = a.fragment_done = false;
        }
        return;
    }
    if (a.type == 0) {
        if (!(a.flags & action_looping)) {
            a.complete = true;
        }
        a.running = false;
        a.saying = false;
        return;
    }
    a.complete = true; // a package action stays running until its range ends (5.2)
    (void)s;
}

void Scenes::action_cleanup(Scene& s, Action& a) {
    if (a.type == 1) {
        a.running = false;
        if (const uint32_t ref = alias_ref(s, a.alias)) {
            m_actors->clear_package(ref);
        }
    }
}

// true when done.
bool Scenes::update_action(Scene& s, Action& a, float dt) {
    const auto phase = static_cast<uint32_t>(s.phase);
    const bool in_range = phase >= a.start_phase && phase <= a.end_phase;
    const uint32_t ref = alias_ref(s, a.alias);
    if (ref == m_actors->speaker()) {
        return true;
    }
    if (a.running && !in_range) {
        action_end(s, a);
        action_cleanup(s, a);
    } else if (in_range && !a.complete && !a.running) {
        action_start(s, a, dt);
    }
    if (a.running && !a.complete) {
        const bool in_next = phase + 1 >= a.start_phase && phase + 1 <= a.end_phase;
        action_update(s, a, dt);
        return in_next || (a.flags & action_looping) ? true : !a.running || a.complete;
    }
    if (a.running && a.complete && a.type == 1) {
        action_update(s, a, dt);
    }
    return true;
}

void Scenes::end_all_actions(Scene& s) {
    for (Action& a : s.actions) {
        if (a.running) {
            if (!(a.flags & action_looping)) {
                a.complete = true;
            }
            action_cleanup(s, a);
        }
        a.running = a.complete = a.saying = a.event_sent = a.fragment_done = false;
        a.delay = a.elapsed = 0;
        a.package = 0;
    }
}

void Scenes::update_scene(Scene& s, float dt) {
    if (s.phase == -1 || !s.playing || s.hold) {
        return;
    }
    if (static_cast<size_t>(s.phase) >= s.phases.size()) {
        if (s.end_delay > 0 && s.end_delay != std::numeric_limits<float>::max()) {
            s.end_delay -= dt;
            return;
        }
        s.end_delay = std::numeric_limits<float>::max();
        const Conditions::QuestScope scope(m_quests.conditions(), s.quest);
        if ((s.flags & scene_repeat_while_true) && m_quests.conditions().evaluate(s.conditions, 0, 0)) {
            s.phase = 0;
            s.all_done = false;
            end_all_actions(s);
            return;
        }
        set_playing(s, false);
        return;
    }
    Phase& p = s.phases[static_cast<size_t>(s.phase)];
    const auto index = static_cast<uint32_t>(s.phase);
    if (!p.started) {
        if (p.fragment_pending) {
            if (p.fragment_done) {
                p.fragment_pending = p.fragment_done = false;
                p.started = true;
            }
            return;
        }
        const Conditions::QuestScope scope(m_quests.conditions(), s.quest);
        if (m_quests.conditions().evaluate(p.start_conditions, k_player, 0)) {
            p.fragment_pending = true;
            Phase* phase = &p;
            fragment(s, oss::FragmentKind::scene_phase, index, 1, [phase] { phase->fragment_done = true; });
        } else {
            ++s.phase;
        }
        return;
    }
    if (p.ending) {
        if (p.fragment_done) {
            p = Phase{std::move(p.start_conditions), std::move(p.completion_conditions)};
            s.all_done = false;
            ++s.phase;
        }
        return;
    }
    bool all = true;
    for (Action& a : s.actions) {
        all = update_action(s, a, dt) && all;
    }
    if (all) {
        s.all_done = true;
    }
    const Conditions::QuestScope scope(m_quests.conditions(), s.quest);
    const bool finished = p.completion_conditions.empty() ? s.all_done
                                                          : m_quests.conditions().evaluate(p.completion_conditions, k_player, 0);
    if (finished) {
        p.ending = true;
        Phase* phase = &p;
        fragment(s, oss::FragmentKind::scene_phase, index, 2, [phase] { phase->fragment_done = true; });
    }
}

void Scenes::update(float dt) {
    m_in_scene.clear();
    for (Scene& s : m_scenes) {
        if (s.playing && (s.phase >= 0 || s.speaker)) {
            for (const Actor& a : s.actors) {
                if (const uint32_t ref = alias_ref(s, a.alias)) {
                    m_in_scene.push_back(ref);
                }
            }
        }
    }
    for (Scene& s : m_scenes) {
        if (s.playing) {
            update_start(s, dt);
            update_scene(s, dt);
        }
    }
}

// Section 8.
void Scenes::set_playing(Scene& s, bool playing, const char* why) {
    if (playing) {
        s.playing = true;
        return;
    }
    if (!s.playing) {
        return;
    }
    if (m_quests.traced() == s.quest && !s.end_sent) {
        log::info("scene {:08X}: stops at phase {} ({})", s.form, s.phase, why);
    }
    s.phase = static_cast<int>(s.phases.size());
    Quest* q = m_quests.quest(s.quest);
    const bool quest_ok = q && q->is_running();
    if (!s.end_sent) {
        s.fragment_done = false;
        s.end_sent = true;
        Scene* p = &s;
        fragment(s, oss::FragmentKind::scene_end, 0, 0, [p] { p->fragment_done = true; });
        if (quest_ok) {
            return;
        }
    }
    if (quest_ok && !s.fragment_done) {
        return;
    }
    end_all_actions(s);
    for (Phase& p : s.phases) {
        p.started = p.fragment_pending = p.fragment_done = p.ending = false;
    }
    s.phase = -1;
    s.end_delay = std::numeric_limits<float>::max();
    s.hold = s.all_done = s.fragment_done = s.end_sent = false;
    s.playing = false;
    if (s.random) {
        m_random_busy = false;
        m_random_countdown = 0;
        if (s.pair[0] && m_quests.conversations) {
            m_quests.conversations->end_talk(s.pair[0], s.pair[1]);
        }
        s.speaker = s.spoken_to = 0;
        s.pair = {};
    }
    if ((s.flags & scene_stop_quest_on_end) && q) {
        m_quests.stop(*q);
    }
}

void Scenes::bind_natives() {
    using script::NativeCall;
    using script::Value;
    script::VirtualMachine& vm = m_quests.vm();
    auto add = [&vm](const char* name, script::NativeFunction fn) {
        vm.bind_native("Scene", name, std::move(fn), script::native_flags("Scene", name));
    };
    auto form = [](NativeCall& c) { return script::handle_form(c.self->handle); };
    add("Start", [this, form](NativeCall& c) {
        if (!start(form(c))) {
            c.vm.log_error(c.stack, "cannot start scene because its parent quest was not running");
        }
    });
    add("ForceStart", [this, form](NativeCall& c) {
        if (!force_start(form(c))) {
            c.vm.log_error(c.stack, "cannot force start scene because its parent quest was not running");
        }
    });
    add("Stop", [this, form](NativeCall& c) { stop(form(c)); });
    add("IsPlaying", [this, form](NativeCall& c) { c.result = Value::of(is_playing(form(c))); });
    add("IsActionComplete", [this, form](NativeCall& c) {
        c.result = Value::of(action_complete(form(c), static_cast<uint32_t>(c.args[0].integer)));
    });
    add("GetOwningQuest", [this, form](NativeCall& c) {
        if (const Scene* s = scene(form(c))) {
            c.result = c.vm.form_value(s->quest, "Quest");
        }
    });
}

} // namespace spud::game
