#include "game/caster.hpp"

#include "game/game.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

namespace spud::game {

namespace {

constexpr int k_right_charge = 64, k_left_charge = 82, k_shout_recovery_mult = 86;
constexpr int ep_spell_cost = 38, ep_cast_noise = 55, ep_can_dual_cast = 75, ep_spell_range = 88;
constexpr uint32_t k_time_scale = 0x3A;
constexpr uint32_t k_explosion_no_targets = 0x80;
constexpr uint32_t k_projectile_ignores_aim = 0x400;
constexpr int k_help_no_charge = 258;
constexpr int k_topic_word_1 = 51, k_topic_held = 52, k_topic_word_2 = 53, k_topic_word_3 = 54;

float units_between(const Units3& a, const Units3& b) {
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

// An effect's cost through a skill: the game.
float skill_cost(Game& game, float cost, int skill, uint32_t actor) {
    if (skill < 6 || skill > 23) {
        return cost;
    }
    const Settings& s = game.settings();
    const bool player = actor == k_player_ref;
    const float base = player ? s.number("fMagicCasterPCSkillCostBase", 0.0025f) : s.number("fMagicCasterSkillCostBase", 0.005f);
    const float mult = player ? s.number("fMagicCasterPCSkillCostMult", 1.0f) : s.number("fMagicCasterSkillCostMult", 0.5f);
    const float scale = player ? s.number("fMagicPCSkillCostScale", 0.65f) : s.number("fMagicSkillCostScale", 0.5f);
    const float level = std::floor(game.actor_values(actor).clamped(skill));
    return cost * mult * (1.0f - std::pow(base * level, scale));
}

} // namespace

Caster::Caster(Game& game, Casters& casters, uint32_t actor, uint8_t source)
    : m_game(&game), m_casters(&casters), m_actor(actor), m_source(source) {}

Casters::Casters(Game& game) : m_game(game) {}
Casters::~Casters() = default;

Casters::ActorMagic::ActorMagic(Game& game, Casters& c, uint32_t r)
    : ref(r), casters{Caster(game, c, r, source_left), Caster(game, c, r, source_right), Caster(game, c, r, source_voice),
                      Caster(game, c, r, source_instant)} {}

Casters::ActorMagic& Casters::actor(uint32_t ref) {
    auto& slot = m_actors[ref];
    if (!slot) {
        slot = std::make_unique<ActorMagic>(m_game, *this, ref);
        m_order.push_back(slot.get());
    }
    return *slot;
}

const Casters::ActorMagic* Casters::find(uint32_t ref) const {
    const auto it = m_actors.find(ref);
    return it == m_actors.end() ? nullptr : it->second.get();
}

Caster& Casters::caster(uint32_t ref, uint8_t source) { return actor(ref).casters[source < 4 ? source : source_instant]; }

void Casters::select(uint32_t ref, uint8_t source, uint32_t form, uint32_t object) {
    if (source <= source_voice) {
        actor(ref).selected[source] = {form, object};
    }
}

uint32_t Casters::selected(uint32_t ref, uint8_t source) const {
    const ActorMagic* a = find(ref);
    return a && source <= source_voice ? a->selected[source].form : 0;
}

void Casters::forget(uint32_t ref, uint32_t spell) {
    const auto it = m_actors.find(ref);
    if (it == m_actors.end()) {
        return;
    }
    ActorMagic& a = *it->second;
    for (Selected& s : a.selected) {
        if (s.form == spell) {
            s = {};
        }
    }
    for (Caster& c : a.casters) {
        if (c.item() && c.item()->form == spell) {
            c.interrupt(false);
        }
    }
    std::erase_if(a.cooldowns, [spell](const Cooldown& c) { return c.spell == spell; });
}

bool Casters::casting(uint32_t ref, const Spell& item) const {
    const ActorMagic* a = find(ref);
    return a && std::ranges::any_of(a->casters, [&](const Caster& c) { return c.busy() && c.item() == &item; });
}

bool Casters::dual_casting(uint32_t ref) const {
    const ActorMagic* a = find(ref);
    return a && a->casters[source_left].dual() && a->casters[source_left].busy();
}

bool Casters::on_cooldown(uint32_t ref, uint32_t spell) const {
    const ActorMagic* a = find(ref);
    return a && std::ranges::any_of(a->cooldowns, [spell](const Cooldown& c) { return c.spell == spell; });
}

float Casters::voice_recovery(uint32_t ref) const {
    const ActorMagic* a = find(ref);
    return a ? a->shout.recovery : 0.0f;
}

bool Casters::shouting(uint32_t ref) const {
    const ActorMagic* a = find(ref);
    return a && a->shout.state != 0;
}

const Shout* Casters::selected_shout(uint32_t ref) const {
    const ActorMagic* a = find(ref);
    return a ? m_game.magic().shout(a->selected[source_voice].form) : nullptr;
}

bool Casters::word_unlocked(uint32_t word) const { return std::ranges::find(m_words, word) != m_words.end(); }

std::vector<uint32_t> Casters::actors_near(const Units3& at, float radius, uint32_t except) {
    CombatWorld& world = m_game.combat().world();
    std::vector<uint32_t> near = world.high_actors();
    near.push_back(k_player_ref); // high_actors leaves the player out
    std::erase_if(near, [&](uint32_t ref) {
        CombatBody body;
        return ref == except || !world.body(ref, body) || !body.loaded || units_between(body.position, at) > radius;
    });
    return near;
}

// ---- costs -------------------------------------------------------------------------------------------------------

float Casters::item_cost(const Spell& item, uint32_t actor) const {
    const Magic& magic = m_game.magic();
    const bool skilled = item.type != spell_power && item.type != spell_lesser_power;
    float cost = 0;
    if (item.manual_cost) {
        cost = item.base_cost;
        const bool enchantment = item.type == spell_enchantment || item.type == spell_staff_enchantment;
        const SpellEffect* top = skilled && !enchantment ? magic.costliest(item) : nullptr;
        if (const MagicEffect* m = top ? magic.effect(top->effect) : nullptr) {
            cost = skill_cost(m_game, cost, m->magic_skill, actor);
        }
    } else {
        for (const SpellEffect& e : item.effects) {
            const MagicEffect* m = magic.effect(e.effect);
            if (m && !(m->flags & effect_no_cost)) {
                const float one = magic.effect_cost(e);
                cost += skilled ? skill_cost(m_game, one, m->magic_skill, actor) : one;
            }
        }
    }
    if (actor == k_player_ref) {
        cost = m_game.character().entry_point(ep_spell_cost, cost, {item.form});
    }
    return std::max(cost, 0.0f);
}

float Casters::charge_time(const Spell& item) const {
    if (item.manual_cost) {
        return item.charge_time;
    }
    float time = 0;
    for (const SpellEffect& e : item.effects) {
        const MagicEffect* m = m_game.magic().effect(e.effect);
        if (m && !(m->flags & effect_no_cost)) {
            time = std::max(time, m->cast_time);
        }
    }
    return time;
}

void Casters::use_skill(uint32_t actor, const Spell& item, const SpellEffect* effect, float scale) {
    if (actor != k_player_ref) {
        return;
    }
    switch (item.type) {
    case spell_power: case spell_lesser_power: case spell_voice: case spell_enchantment: case spell_staff_enchantment:
    case spell_scroll:
        return;
    default: break;
    }
    const Magic& magic = m_game.magic();
    const SpellEffect* e = effect ? effect : magic.costliest(item, false, true);
    const MagicEffect* m = e ? magic.effect(e->effect) : nullptr;
    if (!m || m->magic_skill < 6 || m->magic_skill > 23) {
        return;
    }
    float amount = magic.effect_cost(*e);
    if (item.manual_cost) {
        amount = std::min(amount, item.base_cost);
    }
    m_game.character().use_skill(m->magic_skill, amount * m->skill_mult * scale, item.form);
}

float Caster::cost_of(const Spell& item) const {
    const float cost = m_casters->item_cost(item, m_actor);
    if (!m_dual) {
        return cost;
    }
    const Settings& s = m_game->settings();
    return s.number("fMagicDualCastingCostMult", 2.8f) * cost + s.number("fMagicDualCastingCostBase", 0.0f);
}

int Caster::cost_av(const Spell& item) const {
    switch (item.type) {
    case spell_plain: case spell_lesser_power: case spell_poison: return av_magicka;
    case spell_enchantment: case spell_staff_enchantment:
        return item.cast_type == cast_constant ? -1 : m_source == source_left ? k_left_charge : k_right_charge;
    default: return -1;
    }
}

void Caster::pay(float amount) {
    const Spell& item = *m_item;
    const int av = cost_av(item);
    const bool charge = item.type == spell_enchantment || item.type == spell_staff_enchantment;
    if (av < 0 || amount <= 0.0f || m_skip_checks || (charge && m_source == source_instant)) {
        return;
    }
    m_game->actor_values(m_actor).mod(Modifier::damage, av, -amount);
    m_paid += amount;
    if (charge) {
        m_casters->store_charge(m_actor, m_source == source_left);
    }
}

void Caster::pay_charging(float dt) {
    // a charged item pays as it charges.
    const Spell& item = *m_item;
    if (item.cast_type != cast_fire_and_forget || m_charge_time <= 0.0f) {
        return;
    }
    const int av = cost_av(item);
    if (av >= 0 && !m_skip_checks && m_cost - m_paid > m_game->actor_values(m_actor).current(av)) {
        interrupt(false);
        return;
    }
    pay(std::min(m_cost * dt / m_charge_time, m_cost - m_paid));
}

// ---- checks ------------------------------------------------------------------------------------------------------

bool Caster::busy_elsewhere() const {
    const auto* a = m_casters->find(m_actor);
    return a && std::ranges::any_of(a->casters, [this](const Caster& c) { return &c != this && c.busy(); });
}

bool Caster::archetype_refuses(const Spell& item) const {
    MagicWorld& world = m_game->magic_world();
    for (const SpellEffect& e : item.effects) {
        const MagicEffect* m = m_game->magic().effect(e.effect);
        if (!m) {
            continue;
        }
        // a caster already updating its targets is holding what it took, so that's only asked at the start.
        if (((m->archetype == archetype_telekinesis || m->archetype == archetype_grab_actor) && m_next_update == 0.0f && world.holding(m_actor)) ||
            (m->archetype == archetype_slow_time && world.time_slowed()) ||
            (m->archetype == archetype_enhance_weapon && !m_game->combat().fighter(m_actor).weapon_drawn)) {
            return true;
        }
    }
    return false;
}

CastFailure Caster::check_cast(const Spell& item) const {
    if (m_skip_checks) {
        return cast_passes;
    }
    const bool concentration = item.cast_type == cast_concentration;
    const auto short_of_cost = [&] {
        bool tested = false;
        switch (m_state) {
        case CastState::idle: case CastState::requested: tested = true; break;
        case CastState::released: tested = concentration; break;
        case CastState::applying: tested = !concentration && m_charge_time <= 0.0f; break;
        default: break;
        }
        const int av = cost_av(item);
        if (!tested || av < 0) {
            return false;
        }
        const float current = m_game->actor_values(m_actor).current(av);
        return concentration ? current <= 0.0f : current < m_cost;
    };
    const auto alone_refused = [&] {
        const bool flagged = std::ranges::any_of(item.effects, [this](const SpellEffect& e) {
            const MagicEffect* m = m_game->magic().effect(e.effect);
            return m && (archetype_info(m->archetype).flags & archetype_self_alone);
        });
        return item.delivery == delivery_self && flagged && busy_elsewhere();
    };
    const auto* a = m_casters->find(m_actor);
    const bool voice_busy = a && a->casters[source_voice].busy() && m_source != source_voice;
    const bool hand_busy = a && (a->casters[source_left].busy() || a->casters[source_right].busy());
    const CastFailure no_cost = cost_av(item) == av_magicka ? cast_no_magicka : cast_no_charge;
    switch (item.type) {
    case spell_plain: case spell_scroll:
        if (short_of_cost()) {
            return no_cost;
        }
        if (alone_refused()) {
            return cast_multi_cast;
        }
        if (voice_busy && m_source != source_instant) {
            return cast_while_shouting;
        }
        break;
    case spell_power:
        if (m_casters->on_cooldown(m_actor, item.form)) {
            return cast_power_used;
        }
        break;
    case spell_lesser_power:
        if (m_casters->on_cooldown(m_actor, item.form)) {
            return cast_silent;
        }
        break;
    case spell_poison: case spell_staff_enchantment:
        if (short_of_cost()) {
            return no_cost;
        }
        if (alone_refused()) {
            return cast_multi_cast;
        }
        break;
    case spell_voice:
        if (m_casters->voice_recovery(m_actor) > 0.0f) {
            return cast_voice_recovering;
        }
        if (alone_refused()) {
            return cast_multi_cast;
        }
        if (hand_busy) {
            return cast_shout_while_casting;
        }
        // A shout with "treat spells as powers" is used up for the day as a power is; only Dragon Aspect has it.
        if (m_casters->on_cooldown(m_actor, item.form)) {
            return cast_power_used;
        }
        break;
    default: return cast_passes;
    }
    if (archetype_refuses(item)) {
        return cast_archetype;
    }
    return m_game->magic_world().mount_forbids(m_actor, item) ? cast_silent : cast_passes;
}

void Caster::cast_failed(CastFailure reason, bool in_release) {
    const Spell* item = m_item;
    if (m_actor == k_player_ref) {
        if (reason == cast_no_magicka || reason == cast_voice_recovering) {
            m_game->combat().world().blink_meter(reason == cast_no_magicka ? av_magicka : av_voice_points);
        }
        const char* text = nullptr;
        switch (reason) {
        case cast_no_magicka: text = "sMagicCastInsufficientMagicka"; break;
        case cast_power_used: text = "sMagicCastPowerUsed"; break;
        case cast_multi_cast: text = "sMagicCastMultiCast"; break;
        case cast_no_charge: text = "sMagicCastInsufficientItemCharge"; break;
        case cast_while_shouting: text = "sMagicCastCastWhileShouting"; break;
        case cast_shout_while_casting: text = "sMagicCastShoutWhileCasting"; break;
        default: break;
        }
        if (text) {
            m_game->notify_once(m_game->settings().text(text));
        }
        if (reason == cast_no_charge && item && item->type == spell_staff_enchantment) {
            m_game->magic_world().help_message(k_help_no_charge);
        }
    }
    if (!in_release) {
        finish();
    } else if (m_state != CastState::sustained || reason != cast_voice_recovering) {
        m_game->magic_world().fail_art(m_actor, *item);
        interrupt(false);
    }
}

// ---- the states --------------------------------------------------------------------------------------------------

namespace {
constexpr float k_graph_patience = 1.0f; // seconds a cast waits on a graph that may never answer; its own events come within frames
}

bool Caster::perform(std::string_view name) const {
    // Default objects 45 to 54 and the voice's; false with no graph to play it, and the caster goes on at once.
    const std::string_view hand = m_source == source_voice ? "Voice" : m_dual ? "Dual" : m_source == source_left ? "Left" : "Right";
    const bool voice_start = m_source == source_voice && name == "Attack";
    return m_game->combat().world().perform_action(m_actor, std::format("Action{}{}", hand, voice_start ? "" : name));
}

bool Caster::item_is_power() const {
    return m_item && m_item->type != spell_voice;
}

bool Caster::queue(std::string_view name) const {
    const std::string_view hand = m_dual ? "Dual" : m_source == source_left ? "Left" : "Right";
    return m_game->combat().world().queue_action(m_actor, std::format("Action{}{}", hand, name));
}

bool Caster::hand_wanted(bool on) const {
    CombatWorld& world = m_game->combat().world();
    const bool taken = world.want_cast(m_actor, m_source == source_left ? "bWantCastLeft" : "bWantCastRight", on);
    if (m_dual) {
        world.want_cast(m_actor, "bWantCastRight", on);
    }
    return taken;
}

bool Caster::request(const Spell& item, uint32_t target, uint32_t object, bool dual) {
    if (busy()) {
        return false;
    }
    if (item.type == spell_staff_enchantment && m_source <= source_right) {
        const Gear gear = m_game->combat().gear(m_actor);
        const Held& held = m_source == source_left ? gear.left : gear.right;
        if (held.item) {
            m_casters->prime_charge(m_actor, m_source == source_left, held);
        }
    }
    m_item = &item;
    m_target = target;
    m_object = object;
    m_dual = dual;
    m_paid = 0;
    m_cost = cost_of(item);
    m_charge_time = item.type == spell_voice ? 0.0f : m_casters->charge_time(item);
    if (dual) {
        const Settings& s = m_game->settings();
        m_charge_time = s.number("fMagicDualCastingTimeMult", 1.0f) * m_charge_time + s.number("fMagicDualCastingTimeBase", 0.0f);
    }
    if (const CastFailure reason = check_cast(item); reason != cast_passes) {
        cast_failed(reason, false);
        return false;
    }
    m_game->magic_world().cast_sound(m_actor, item.form, true);
    if (item.type == spell_voice) {
        m_state = CastState::ready; // a word has no charge, and the shout's own states lead the graph
        return true;
    }
    m_state = CastState::requested;
    // RequestCastImpl: the graph bool. The stance raises BeginCast from it and the charge action rides on that event
    // (StartChargeImpl): queued, it lands as the graph enters the casting state.
    m_graph_wait = 0;
    if (m_source != source_voice && hand_wanted(true)) {
        m_following_graph = queue("Attack");
    } else {
        perform("Attack"); // a power: the voice's own states don't lead it, so it goes on with or without the graph
        m_following_graph = false;
        m_casters->animation_event(m_actor, m_source == source_left ? "BeginCastLeft" : m_source == source_right ? "BeginCastRight" : "BeginCastVoice");
    }
    return true;
}

// StartReadyImpl.
void Caster::ready_action() {
    // Concentration goes straight on to release; the game's graph sees Start and Release in one frame (measured).
    if (m_source != source_voice && m_following_graph && m_item->cast_type != cast_concentration) {
        queue("Ready");
    }
}

void Caster::begin_cast() {
    if (m_state == CastState::requested) {
        m_timer = m_charge_time;
        m_state = m_timer > 0.0f ? CastState::charging : CastState::ready;
        if (m_state == CastState::ready) {
            ready_action();
        }
    }
}

void Caster::release() {
    if (m_state != CastState::ready) {
        return;
    }
    m_state = CastState::released;
    m_graph_wait = 0;
    if (m_source != source_voice && m_following_graph) {
        queue("Release");
    } else if (!perform("Release") || item_is_power()) {
        m_casters->animation_event(m_actor, m_source == source_left ? "MLh_SpellFire_Event" : m_source == source_right ? "MRh_SpellFire_Event" : "Voice_SpellFire_Event");
    }
}

bool Caster::release_cast() {
    if (!m_item || (m_state != CastState::released && m_state != CastState::sustained)) {
        return false;
    }
    const bool sustaining = m_state == CastState::sustained;
    if (!sustaining) {
        m_state = CastState::applying;
    }
    if (const CastFailure reason = check_cast(*m_item); reason != cast_passes) {
        cast_failed(reason, true);
        return false;
    }
    bool took = false;
    if (!apply_to_targets(took)) {
        m_game->magic_world().fail_art(m_actor, *m_item);
        interrupt(false);
        return false;
    }
    if (sustaining) {
        return true;
    }
    spell_cast(took);
    if (m_item->cast_type != cast_concentration) {
        finish();
        return true;
    }
    m_state = CastState::sustained;
    m_timer = 0;
    m_next_update = m_game->settings().number("fMagicCasterTargetUpdateInterval", 0.25f);
    return true;
}

void Caster::finish() {
    if (m_item) {
        m_game->magic_world().cast_sound(m_actor, m_item->form, false);
        if (m_source != source_voice) {
            hand_wanted(false);
        }
    }
    m_item = nullptr;
    m_target = m_object = m_blame = 0;
    m_state = CastState::idle;
    m_timer = m_cost = m_next_update = m_relaunch = m_charge_time = m_paid = 0;
    m_dual = m_skip_checks = false;
}

void Caster::interrupt(bool refund) {
    if (!m_item) {
        return;
    }
    // running out while charging passes no refund.
    if (refund && m_paid > 0.0f && m_state != CastState::applying && m_state != CastState::idle) {
        const int av = cost_av(*m_item);
        m_game->actor_values(m_actor).mod(Modifier::damage, av, m_paid);
        if (av != av_magicka) {
            m_casters->store_charge(m_actor, m_source == source_left);
        }
    }
    m_state = CastState::interrupting;
    m_game->combat().world().clear_actions(m_actor);
    perform("Interrupt");
    finish();
}

bool Caster::cast_immediate(const Spell& item, uint32_t target, uint32_t object, uint32_t blame) {
    finish();
    m_item = &item;
    m_target = target;
    m_object = object;
    m_blame = blame;
    m_skip_checks = true;
    bool took = false;
    const bool cast = apply_to_targets(took);
    if (cast) {
        spell_cast(took);
    }
    finish();
    return cast;
}

bool Caster::cast_on_target(const Spell& item, uint32_t target, uint32_t object) {
    finish();
    m_item = &item;
    m_target = target;
    m_object = object;
    m_cost = cost_of(item);
    m_state = CastState::released;
    return release_cast();
}

void Caster::sustain(float dt) {
    const Spell& item = *m_item;
    const Magic& magic = m_game->magic();
    if (const int av = cost_av(item); av >= 0 && !m_skip_checks) {
        const float current = m_game->actor_values(m_actor).current(av);
        pay(std::min(m_cost * dt, current));
        if (current <= m_cost * dt) {
            cast_failed(av == av_magicka ? cast_no_magicka : cast_no_charge, true);
            return;
        }
    }
    m_timer += dt;
    const bool aimed = item.delivery == delivery_aimed;
    if (aimed) {
        m_relaunch -= dt;
        if (m_relaunch <= 0.0f) {
            fire_projectile(nullptr);
        }
    }
    if (item.cast_duration > 0.0f && m_timer >= item.cast_duration) {
        interrupt(false);
        return;
    }
    if (!aimed) {
        const SpellEffect* top = magic.costliest(item, false, true);
        const MagicEffect* m = top ? magic.effect(top->effect) : nullptr;
        if (m && !(archetype_info(m->archetype).flags & archetype_custom_skill)) {
            m_casters->use_skill(m_actor, item, nullptr, dt);
        }
        if (m_timer >= m_next_update) {
            m_next_update = m_timer + m_game->settings().number("fMagicCasterTargetUpdateInterval", 0.25f);
            release_cast();
        }
    }
}

void Caster::update(float dt) {
    // ActorMagicCaster::Update, the game.
    if (!m_item) {
        return;
    }
    switch (m_state) {
    case CastState::requested:
        // A graph that isn't in a stance raises no BeginCast: the spell doesn't wait for it.
        if ((m_graph_wait += dt) > k_graph_patience) {
            m_following_graph = false;
            m_casters->animation_event(m_actor, m_source == source_left ? "BeginCastLeft" : m_source == source_right ? "BeginCastRight" : "BeginCastVoice");
        }
        break;
    case CastState::charging:
        pay_charging(dt);
        if (m_state == CastState::charging && (m_timer -= dt) <= 0.0f) {
            m_state = CastState::ready;
            ready_action();
        }
        break;
    case CastState::released:
        if (m_source != source_voice && (m_graph_wait += dt) > k_graph_patience) {
            m_casters->animation_event(m_actor, m_source == source_left ? "MLh_SpellFire_Event" : "MRh_SpellFire_Event");
        }
        break;
    case CastState::ready:
        if (m_item->cast_type == cast_concentration) {
            release();
        }
        break;
    case CastState::sustained: sustain(dt); break;
    default: break;
    }
}

// ---- delivery ----------------------------------------------------------------------------------------------------

AddTarget Caster::target_data(const SpellEffect& effect) const {
    AddTarget t;
    t.caster = m_blame ? m_blame : m_actor;
    t.item = m_item;
    t.effect = &effect;
    t.object = m_object;
    t.cast_cost = m_cost;
    t.casting_source = m_source;
    t.dual = m_dual;
    return t;
}

uint32_t Caster::touch_target() const {
    // `fMagicDefaultTouchDistance` has no reader: the reach is the race's.
    if (m_target) {
        return m_target;
    }
    Combat& combat = m_game->combat();
    CombatBody self;
    if (!m_game->is_actor(m_actor) || !combat.world().body(m_actor, self)) {
        return 0;
    }
    const forms::FormEntry* race = m_game->db().find(combat.fighter(m_actor).race);
    const forms::Value* v = race ? forms::Form(m_game->db(), *race).get("DATA.unarmed_reach") : nullptr;
    const float reach = (v ? float(v->real) : 0.0f) * self.full_scale;
    if (m_actor == k_player_ref) {
        return combat.world().swing_pick(m_actor, reach);
    }
    uint32_t nearest = 0;
    float best = reach;
    for (uint32_t ref : m_casters->actors_near(self.position, reach, m_actor)) {
        CombatBody body;
        if (combat.fighter(ref).dead() || !combat.world().line_of_sight(m_actor, ref) || !combat.world().body(ref, body)) {
            continue;
        }
        if (const float d = units_between(body.position, self.position); d <= best) {
            best = d;
            nearest = ref;
        }
    }
    return nearest;
}

float Caster::view_range() const {
    const float range = m_game->character().entry_point(ep_spell_range, m_item->range, {m_item->form});
    return range > 0.0f ? range * m_game->settings().number("fMagicUnitsPerFoot", 22.0f) : 5000.0f;
}

bool Caster::pick_direct(uint32_t& direct, std::optional<Units3>& location) {
    const Spell& item = *m_item;
    MagicWorld& world = m_game->magic_world();
    Combat& combat = m_game->combat();
    switch (item.delivery) {
    case delivery_self: direct = m_target ? m_target : m_actor; return true;
    case delivery_touch: direct = touch_target(); return true;
    case delivery_target_actor: case delivery_target_location: {
        const bool wants_location = item.delivery == delivery_target_location;
        if (m_target) {
            direct = m_target;
        } else if (m_actor != k_player_ref) {
            direct = m_game->is_actor(m_actor) ? combat.fighter(m_actor).combat_target : 0;
        } else {
            MagicPick pick;
            const bool hit = world.view_ray(m_actor, view_range(), pick);
            direct = wants_location ? pick.ref : world.crosshair_actor(m_actor);
            direct = direct ? direct : pick.ref;
            if (wants_location && !hit) {
                return false;
            }
            if (hit) {
                location = pick.point;
            }
        }
        CombatBody body;
        if (!location && direct && combat.world().body(direct, body)) {
            location = body.position;
        }
        return !wants_location || location.has_value();
    }
    default: return true;
    }
}

bool Caster::apply_to_targets(bool& took) {
    const Spell& item = *m_item;
    const Magic& magic = m_game->magic();
    MagicWorld& world = m_game->magic_world();
    CombatWorld& bodies = m_game->combat().world();
    if (item.delivery == delivery_aimed && item.cast_type == cast_fire_and_forget) {
        fire_projectile(nullptr);
        return true;
    }
    uint32_t direct = 0;
    std::optional<Units3> location;
    if (item.delivery != delivery_aimed && !pick_direct(direct, location)) {
        return false;
    }
    const SpellEffect* top = magic.costliest(item);
    const MagicEffect* costliest = top ? magic.effect(top->effect) : nullptr;
    if (costliest && (costliest->flags & effect_snap_to_navmesh) && item.delivery == delivery_target_location && location) {
        world.snap_to_navmesh(*location);
    }
    const bool summons = std::ranges::any_of(item.effects, [&](const SpellEffect& s) {
        const MagicEffect* m = magic.effect(s.effect);
        return m && m->archetype == archetype_summon_creature;
    });
    if (summons) {
        direct = m_actor;
    }
    // MagicItemDataCollector: the largest area decides whether anyone's gathered at all.
    const SpellEffect* largest = nullptr;
    for (const SpellEffect& s : item.effects) {
        if (!largest || s.area > largest->area) {
            largest = &s;
        }
    }
    const float units_per_foot = m_game->settings().number("fMagicUnitsPerFoot", 22.0f);
    std::vector<uint32_t> gathered;
    Units3 centre{};
    if (const MagicEffect* m = largest && largest->area ? magic.effect(largest->effect) : nullptr; m && m->delivery != delivery_aimed) {
        CombatBody body;
        const bool placed = location.has_value() || (direct && bodies.body(direct, body));
        centre = location ? *location : body.position;
        const forms::FormEntry* e = m_game->db().find(m->explosion);
        const forms::Value* flags = e ? forms::Form(m_game->db(), *e).get("DATA.flags") : nullptr;
        if (placed && !(flags && (flags->integer & k_explosion_no_targets))) {
            gathered = m_casters->actors_near(centre, float(largest->area) * units_per_foot, m_actor);
            if (!(item.flags & spell_area_ignores_los)) {
                std::erase_if(gathered, [&](uint32_t ref) { return !bodies.line_of_sight(m_actor, ref); });
            }
            if (m->explosion) {
                world.place_explosion(m->explosion, centre, m_actor);
            }
        }
    }
    for (const SpellEffect& s : item.effects) {
        const MagicEffect* m = magic.effect(s.effect);
        if (!m || m->delivery == delivery_aimed || (summons && m->archetype != archetype_summon_creature)) {
            continue;
        }
        AddTarget t = target_data(s);
        if (s.area > 0) {
            t.area_target = true;
            t.location = centre;
            for (uint32_t ref : gathered) {
                CombatBody body;
                MagicTarget& target = m_game->magic_target(ref);
                if (!target.ghost() && bodies.body(ref, body) && units_between(body.position, centre) <= float(s.area) * units_per_foot) {
                    took = target.add_target(t) || took;
                }
            }
        } else if (direct) {
            t.location = location;
            took = m_game->magic_target(direct).add_target(t) || took;
        }
        if (item.type == spell_ingredient) {
            break;
        }
    }
    // The aimed effects fly, once per projectile, even from a non-aimed item.
    std::vector<uint32_t> launched;
    for (const SpellEffect& s : item.effects) {
        const MagicEffect* m = magic.effect(s.effect);
        if (!m || !m->projectile || (m->delivery != delivery_aimed && m->delivery != delivery_target_location) ||
            std::ranges::find(launched, m->projectile) != launched.end()) {
            continue;
        }
        Units3 ground{};
        if (m->delivery == delivery_target_location && m_actor == k_player_ref && !world.ray_on_ground(m_actor, view_range(), ground)) {
            return false;
        }
        launched.push_back(m->projectile);
        fire_projectile(m);
    }
    return true;
}

void Caster::fire_projectile(const MagicEffect* effect) {
    // launched by.
    const Spell& item = *m_item;
    const Magic& magic = m_game->magic();
    MagicWorld& world = m_game->magic_world();
    Combat& combat = m_game->combat();
    if (!effect) {
        const SpellEffect* top = magic.costliest(item, item.delivery == delivery_aimed);
        effect = top ? magic.effect(top->effect) : nullptr;
    }
    if (!effect) {
        return;
    }
    const uint32_t projectile = m_dual && effect->dual_projectile ? effect->dual_projectile : effect->projectile;
    if (!projectile) {
        return;
    }
    SpellLaunch launch;
    launch.projectile = projectile;
    launch.shooter = m_actor;
    launch.actor_cause = m_blame ? m_blame : m_actor;
    launch.desired_target = m_target;
    launch.weapon = m_object;
    launch.spell = item.form;
    launch.casting_source = m_source;
    launch.dual = m_dual;
    launch.concentration = item.cast_type == cast_concentration;
    launch.power = m_dual ? magic.dual_effectiveness(m_cost) : 1.0f;
    launch.scale = m_dual && !effect->dual_projectile ? effect->dual_scale : 1.0f;
    for (const SpellEffect& s : item.effects) {
        launch.area = std::max(launch.area, float(s.area));
    }
    CombatBody self;
    const bool placed = combat.world().body(m_actor, self);
    if (!world.magic_node(m_actor, m_source, launch.origin)) {
        const bool chest = m_game->is_actor(m_actor) && !combat.fighter(m_actor).mounted;
        launch.origin = self.position;
        launch.origin[2] += self.height * (chest ? 0.7f : 0.5f);
    }
    if (!world.look_angles(m_actor, launch.heading, launch.pitch) && placed) {
        launch.heading = self.heading * std::numbers::pi_v<float> / 180.0f;
    }
    CombatBody target;
    const Fighter* fighter = m_game->is_actor(m_actor) ? &combat.fighter(m_actor) : nullptr;
    if (m_target && combat.world().body(m_target, target)) {
        const float dx = target.position[0] - launch.origin[0], dy = target.position[1] - launch.origin[1];
        const float dz = target.position[2] + target.height * 0.5f - launch.origin[2];
        launch.heading = std::atan2(dx, dy);
        launch.pitch = -std::atan2(dz, std::hypot(dx, dy));
    } else if (m_actor != k_player_ref && fighter && fighter->in_combat) {
        const forms::FormEntry* e = m_game->db().find(projectile);
        const forms::Value* flags = e ? forms::Form(m_game->db(), *e).get("DATA.flags") : nullptr;
        if (!(flags && (flags->integer & k_projectile_ignores_aim))) {
            world.combat_aim(m_actor, fighter->combat_target, launch.heading, launch.pitch);
        }
    }
    if (!world.launch_spell(launch)) {
        LaunchData data;
        data.origin = launch.origin;
        data.projectile = launch.projectile;
        data.shooter = launch.shooter;
        data.weapon = launch.weapon ? m_game->items().find(launch.weapon) : nullptr;
        data.heading = launch.heading;
        data.pitch = launch.pitch;
        data.actor_cause = launch.actor_cause;
        data.desired_target = launch.desired_target;
        data.spell = launch.spell;
        data.casting_source = launch.casting_source;
        data.area = launch.area;
        data.power = launch.power;
        data.scale = launch.scale;
        data.dual_cast = launch.dual;
        data.concentration = launch.concentration;
        m_game->projectiles().launch_spell(data);
    }
    m_relaunch = magic.relaunch_interval(projectile);
}

void Caster::projectile_hit(const SpellImpact& hit) {
    if (!hit.item) {
        return;
    }
    const Spell& item = *hit.item;
    const Magic& magic = m_game->magic();
    const float cost = m_casters->item_cost(item, m_actor);
    const float ward_passes = std::exchange(m_casters->ward_passes, 1.0f);
    if (hit.target) {
        MagicTarget& target = m_game->magic_target(hit.target);
        for (const SpellEffect& s : item.effects) {
            AddTarget t;
            t.caster = m_actor;
            t.item = &item;
            t.effect = &s;
            t.object = hit.object;
            t.effectiveness = hit.power * ward_passes;
            t.cast_cost = cost;
            t.casting_source = m_source;
            t.dual = hit.dual;
            t.area_target = hit.area;
            t.location = hit.location;
            target.add_target(t);
        }
        return;
    }
    const SpellEffect* top = magic.costliest(item, false, true);
    const MagicEffect* m = top ? magic.effect(top->effect) : nullptr;
    const uint16_t flags = m ? archetype_info(m->archetype).flags : uint16_t(0);
    if ((flags & archetype_miss_skill) && !(flags & archetype_custom_skill)) {
        const bool concentration = item.cast_type == cast_concentration;
        m_casters->use_skill(m_actor, item, top, concentration ? magic.relaunch_interval(m->projectile) : 1.0f);
    }
}

void Caster::spell_cast(bool took) {
    const Spell& item = *m_item;
    const Magic& magic = m_game->magic();
    MagicWorld& world = m_game->magic_world();
    m_casters->start_cooldown(m_actor, item);
    const bool on_release = item.cast_type == cast_fire_and_forget && m_charge_time <= 0.0f;
    if (on_release && !(item.type == spell_enchantment && item.delivery == delivery_touch && !took)) {
        pay(m_cost);
    }
    if (m_game->is_actor(m_actor)) {
        m_game->magic_target(m_actor).break_invisibility(item);
    }
    const SpellEffect* top = magic.costliest(item);
    const MagicEffect* m = top ? magic.effect(top->effect) : nullptr;
    if (m && m->image_space && m_actor == k_player_ref) {
        world.image_space(m->image_space);
    }
    if (item.type == spell_scroll && m_source <= source_right) {
        m_casters->consume_scroll(m_actor, m_source, item.form);
    }
    world.spell_cast_event(m_actor, item.form);
    if (m && m_actor == k_player_ref) {
        // entry point 55 can quiet a cast.
        if (m_game->character().entry_point(ep_cast_noise, 1.0f, {item.form}) != 0.0f) {
            world.cast_noise(m_actor, m->sound_level);
        }
    } else if (m) {
        world.cast_noise(m_actor, m->sound_level);
    }
}

// ---- what the rest of the game casts -----------------------------------------------------------------------------

bool Casters::instant_cast(uint32_t caster_ref, uint32_t spell, uint32_t target, uint32_t object, uint32_t blame) {
    const Spell* s = m_game.magic().spell(spell);
    return s && caster(caster_ref, source_instant).cast_immediate(*s, target, object, blame);
}

int Casters::prime_charge(uint32_t ref, bool left, const Held& held) {
    const int av = left ? k_left_charge : k_right_charge;
    ActorValues& values = m_game.actor_values(ref);
    ActorMagic& a = actor(ref);
    float charge = -1;
    if (held.stack) {
        const ItemExtra& extra = held.stack->extra;
        const float full = extra.max_charge ? float(extra.max_charge) : float(held.item->enchantment_amount);
        charge = extra.charge < 0.0f ? full : extra.charge;
    } else if (a.charged[left] != held.item->form) {
        a.charged[left] = held.item->form;
        charge = float(held.item->enchantment_amount);
    }
    if (charge >= 0.0f) {
        values.restore(av, -values.modifier(av, Modifier::damage));
        values.set_base(av, charge);
    }
    return av;
}

void Casters::store_charge(uint32_t ref, bool left) {
    const Gear gear = m_game.combat().gear(ref);
    const Held& held = left ? gear.left : gear.right;
    if (held.stack) {
        held.stack->extra.charge = m_game.actor_values(ref).current(left ? k_left_charge : k_right_charge);
    }
}

void Casters::weapon_enchantment(uint32_t attacker, const Held& held, uint32_t enchantment, bool left, uint32_t victim) {
    const Spell* s = m_game.magic().spell(enchantment);
    if (!s) {
        return;
    }
    ActorMagic& a = actor(attacker);
    if (!s->constant_enchantment()) {
        const int av = prime_charge(attacker, left, held);
        if (m_game.actor_values(attacker).current(av) < std::floor(item_cost(*s, attacker))) {
            if (attacker == k_player_ref && !a.charge_told[left]) {
                a.charge_told[left] = true;
                m_game.notify(std::string(m_game.settings().text("sEnchantInsufficientCharge")));
                m_game.magic_world().help_message(k_help_no_charge);
            }
            return;
        }
        a.charge_told[left] = false;
    }
    a.casters[left ? source_left : source_right].cast_on_target(*s, victim, held.item->form);
}

void Casters::start_cooldown(uint32_t ref, const Spell& item) {
    // The game; time.
    if (item.type == spell_power || (item.type == spell_lesser_power && ref == k_player_ref)) {
        const float time = item.type == spell_power ? 86400.0f / time_scale()
                                                    : m_game.settings().number("fMagicLesserPowerCooldownTimer", 3.0f);
        actor(ref).cooldowns.push_back({item.form, time});
    }
}

float Casters::time_scale() const {
    const CharacterEvents& events = m_game.character().events;
    const float scale = events.read_global ? events.read_global(k_time_scale) : 0.0f;
    return scale > 0.0f ? scale : 20.0f;
}

void Casters::consume_scroll(uint32_t ref, uint8_t source, uint32_t scroll) {
    // the last ones leave the hand first.
    ActorMagic& a = actor(ref);
    Inventory& inventory = m_game.inventory_of(ref);
    const int hands = int(a.selected[source_left].form == scroll) + int(a.selected[source_right].form == scroll);
    if (inventory.count(scroll) <= hands) {
        a.selected[source] = {};
    }
    inventory.remove(scroll, 1);
}

bool Casters::can_dual_cast(uint32_t ref, const Spell& item) {
    const ActorMagic& a = actor(ref);
    if (item.type != spell_plain || std::ranges::any_of(a.casters, [](const Caster& c) { return c.busy(); }) ||
        a.selected[source_left].form != item.form || a.selected[source_right].form != item.form) {
        return false;
    }
    return ref != k_player_ref || m_game.character().entry_point(ep_can_dual_cast, 0.0f, {item.form}) != 0.0f;
}

// ---- graph events and keys ---------------------------------------------------------------------------------------

void Casters::animation_event(uint32_t ref, std::string_view tag) {
    ActorMagic& a = actor(ref);
    if (tag == "BeginCastLeft") {
        a.casters[source_left].begin_cast();
    } else if (tag == "BeginCastRight") {
        a.casters[source_right].begin_cast();
    } else if (tag == "BeginCastVoice") {
        if (a.shout.state == 1) {
            a.shout.state = 2;
            m_game.combat().world().perform_action(a.ref, "ActionVoice");
        } else {
            a.casters[source_voice].begin_cast();
        }
    } else if (tag == "MLh_SpellFire_Event" || tag == "MRh_SpellFire_Event") {
        // A spell that applies at once is idle again before the art looks: it hears of the release now.
        const uint8_t hand = tag[1] == 'L' ? source_left : source_right;
        if (a.casters[hand].release_cast()) {
            m_game.magic_world().hand_cast(a.ref, hand, CastPhase::released);
        }
    } else if (tag == "Voice_SpellFire_Event") {
        const bool fired = a.casters[source_voice].release_cast();
        if (a.shout.state == 4) {
            if (fired) {
                shout_fired(a);
            } else {
                a.shout.state = 0;
            }
        }
    } else if (tag == "InterruptCast") {
        for (Caster& c : a.casters) {
            c.interrupt(false);
        }
    }
}

void Casters::set_keys(bool left, bool right, bool voice) { m_keys = {left, right, voice}; }

void Casters::request_selected(ActorMagic& a, uint8_t hand, bool dual) {
    const Selected& s = a.selected[hand];
    const Spell* spell = m_game.magic().spell(s.form);
    if (!spell) {
        return;
    }
    if (dual) {
        // on the left caster, a plain cast when the item won't take the change.
        a.casters[source_left].request(*spell, 0, s.object, !(spell->flags & spell_no_dual_modification));
    } else {
        a.casters[hand].request(*spell, 0, s.object);
    }
}

void Casters::hand_key(uint8_t hand, bool pressed, bool edge, float dt) {
    // on the hand's caster, the left when dual casting.
    ActorMagic& a = actor(k_player_ref);
    Caster& left = a.casters[source_left];
    Caster& c = left.dual() ? left : a.casters[hand];
    const Settings& settings = m_game.settings();
    switch (c.state()) {
    case CastState::idle: {
        if (!pressed) {
            m_requested[hand] = false;
            m_chord[hand] = 0;
            break;
        }
        const bool due = edge || m_chord[hand] > 0.0f || m_held[hand] >= settings.number("fMagicPostDrawCastDelay", 1.5f);
        const Spell* spell = m_game.magic().spell(a.selected[hand].form);
        if (m_requested[hand] || !due || !spell) {
            break;
        }
        const size_t other = hand == source_left ? source_right : source_left;
        if (can_dual_cast(k_player_ref, *spell)) {
            if (m_keys[other]) {
                m_requested = {true, true};
                request_selected(a, hand, true);
                break;
            }
            m_chord[hand] += std::max(dt, 0.0001f);
            if (m_chord[hand] < settings.number("fDualCastChordTime", 0.05f)) {
                break; // the other hand may yet come
            }
        }
        m_requested[hand] = true;
        request_selected(a, hand, false);
        break;
    }
    case CastState::requested: case CastState::charging:
        if (!pressed) {
            c.interrupt(true);
        }
        break;
    case CastState::ready:
        if (!pressed) {
            c.release();
        }
        break;
    case CastState::released:
        if (edge) {
            c.interrupt(true);
        }
        break;
    case CastState::sustained:
        if (!pressed && c.item()->cast_duration <= 0.0f) {
            c.interrupt(false);
        }
        break;
    default: break;
    }
}

void Casters::voice_key(bool pressed, bool edge) {
    ActorMagic& a = actor(k_player_ref);
    const Selected& held = a.selected[source_voice];
    if (const Shout* shout = m_game.magic().shout(held.form)) {
        if (edge && !a.shout.state) {
            start_shout(a, *shout);
        }
        return;
    }
    // A power holds the voice slot the way a spell holds a hand.
    Caster& c = a.casters[source_voice];
    switch (c.state()) {
    case CastState::idle:
        if (const Spell* spell = m_game.magic().spell(held.form); edge && spell) {
            c.request(*spell);
        }
        break;
    case CastState::requested: case CastState::charging:
        if (!pressed) {
            c.interrupt(true);
        }
        break;
    case CastState::ready:
        if (!pressed) {
            c.release();
        }
        break;
    case CastState::released:
        if (edge) {
            c.interrupt(true);
        }
        break;
    case CastState::sustained:
        if (!pressed && c.item()->cast_duration <= 0.0f) {
            c.interrupt(false);
        }
        break;
    default: break;
    }
}

void Casters::update_keys(float dt) {
    for (uint8_t hand : {source_left, source_right}) {
        m_held[hand] = m_keys[hand] ? m_held[hand] + dt : 0.0f;
        hand_key(hand, m_keys[hand], m_keys[hand] && !m_was[hand], dt);
    }
    voice_key(m_keys[source_voice], m_keys[source_voice] && !m_was[source_voice]);
    m_was = m_keys;
}

// ---- shouts ------------------------------------------------------------------------------------------------------

void Casters::start_shout(ActorMagic& a, const Shout& shout) {
    // one unlocked word must pass the flying mount check.
    const bool can = a.ref != k_player_ref || std::ranges::any_of(shout.words, [this, &a](const ShoutWord& w) {
        const Spell* s = m_game.magic().spell(w.spell);
        return s && word_unlocked(w.word) && !m_game.magic_world().mount_forbids(a.ref, *s);
    });
    if (!can) {
        return;
    }
    ShoutState& s = a.shout;
    s.state = 1;
    s.shout = &shout;
    s.held = s.waited = 0;
    // RequestCastImpl: the bool alone. Each stance's the game raises BeginCastVoice from it, and
    // the action comes on that event (StartChargeImpl): before it, the action leaves the modifier's state.
    if (!m_game.combat().world().want_cast(a.ref, "bWantCastVoice", true)) {
        animation_event(a.ref, "BeginCastVoice"); // a graph without one
    }
}

void Casters::npc_shout(uint32_t ref, uint32_t shout, int variation) {
    ActorMagic& a = actor(ref);
    const Shout* s = m_game.magic().shout(shout);
    if (s && !a.shout.state) {
        a.shout.variation = std::clamp(variation, 0, 2);
        start_shout(a, *s);
    }
}

int Casters::player_variation(ActorMagic& a, bool key, int state) {
    if (!key) {
        return state == 2 ? 0 : 1;
    }
    for (int i = 2; i > 0; --i) {
        const ShoutWord& w = a.shout.shout->words[size_t(i)];
        const Spell* s = m_game.magic().spell(w.spell);
        if (s && word_unlocked(w.word) && a.casters[source_voice].check_cast(*s) == cast_passes) {
            return i;
        }
    }
    return 0;
}

void Casters::fire_shout(ActorMagic& a, int variation, int topic) {
    ShoutState& s = a.shout;
    MagicWorld& world = m_game.magic_world();
    s.variation = variation;
    world.say(a.ref, topic);
    Caster& voice = a.casters[source_voice];
    const Spell* spell = m_game.magic().spell(s.shout->words[size_t(variation)].spell);
    if (!spell || !voice.request(*spell)) {
        m_game.combat().world().want_cast(a.ref, "bWantCastVoice", false);
        s.state = 0;
        return;
    }
    s.state = 4;
    s.waited = 0;
    voice.release();
}

void Casters::shout_fired(ActorMagic& a) {
    ShoutState& s = a.shout;
    m_game.combat().world().want_cast(a.ref, "bWantCastVoice", false);
    s.state = 5;
    s.recovery = s.shout->words[size_t(s.variation)].recovery * m_game.actor_values(a.ref).current(k_shout_recovery_mult);
    if (s.shout->spells_as_powers) {
        for (const ShoutWord& w : s.shout->words) {
            a.cooldowns.push_back({w.spell, 86400.0f / time_scale()});
        }
    }
}

void Casters::update_shout(ActorMagic& a, float dt) {
    ShoutState& s = a.shout;
    const Settings& settings = m_game.settings();
    const bool player = a.ref == k_player_ref;
    const bool key = player ? m_keys[source_voice] : true;
    switch (s.state) {
    case 1: case 4:
        if ((s.waited += dt) > settings.number("fShoutTimeout", 8.0f)) {
            a.casters[source_voice].interrupt(false);
            m_game.combat().world().want_cast(a.ref, "bWantCastVoice", false);
            s.state = 0;
        }
        break;
    case 2:
        s.held += dt;
        if (s.held >= settings.number("fShoutTime1", 0.2f) || !key) {
            const int variation = player ? player_variation(a, key, 2) : s.variation;
            if (variation) {
                s.state = 3;
                m_game.magic_world().say(a.ref, k_topic_held);
            } else {
                fire_shout(a, 0, k_topic_word_1);
            }
        }
        break;
    case 3:
        s.held += dt;
        if (s.held >= settings.number("fShoutTime2", 0.9f) || !key) {
            const int variation = player ? player_variation(a, key, 3) : s.variation;
            fire_shout(a, variation, variation == 1 ? k_topic_word_2 : k_topic_word_3);
        }
        break;
    case 5:
        if (!a.casters[source_voice].busy() && !m_game.combat().world().graph_bool(a.ref, "IsShouting")) {
            s.state = player && key ? 6 : 0;
        }
        break;
    case 6:
        if (!key) {
            s.state = 0;
        }
        break;
    default: break;
    }
}

// The game calls them at the
// selecting, the drawing and the load (); asking each frame comes to the same.
void Casters::update_hand_art(ActorMagic& a) {
    MagicWorld& world = m_game.magic_world();
    const bool out = m_game.combat().fighter(a.ref).weapon_drawn; // actor & 0xE0 >= 0x60
    if (!out && !a.hand_art[0] && !a.hand_art[1] && !a.weapon_art[0] && !a.weapon_art[1]) {
        return;
    }
    const Magic& magic = m_game.magic();
    const Gear gear = out ? m_game.combat().gear(a.ref) : Gear{};
    for (const uint8_t hand : {source_left, source_right}) {
        const MagicEffect* art = nullptr;
        const MagicEffect* enchant = nullptr;
        const Caster& caster = a.casters[hand];
        if (const Spell* item = !out                ? nullptr
                                : caster.busy()     ? caster.item()
                                : a.selected[hand].form ? magic.spell(a.selected[hand].form)
                                                    : nullptr) {
            const SpellEffect* top = magic.costliest(*item, item->delivery == delivery_aimed);
            art = top ? magic.effect(top->effect) : nullptr;
        }
        const Held& held = hand == source_left ? gear.left : gear.right;
        if (const Spell* item = held.item && held.item->enchantment ? magic.spell(held.item->enchantment) : nullptr;
            item && item->type == spell_enchantment && !item->constant_enchantment()) {
            const float full = float(held.item->enchantment_amount);
            const float charge = held.stack && held.stack->extra.charge >= 0.0f ? held.stack->extra.charge : full;
            const SpellEffect* top = magic.costliest(*item);
            if (top && charge >= std::floor(item_cost(*item, a.ref))) {
                enchant = magic.effect(top->effect);
            }
        }
        const auto phase = [&] {
            switch (caster.state()) {
            case CastState::requested:
            case CastState::charging: return CastPhase::charging;
            case CastState::ready: return CastPhase::ready;
            case CastState::released:
            case CastState::applying: return CastPhase::released;
            case CastState::sustained: return CastPhase::sustained;
            default: return CastPhase::none;
            }
        }();
        if (a.hand_phase[hand] != phase) {
            a.hand_phase[hand] = phase;
            world.hand_cast(a.ref, hand, phase);
        }
        if (a.hand_art[hand] != art) {
            a.hand_art[hand] = art;
            world.hand_art(a.ref, hand, art);
        }
        if (a.weapon_art[hand] != enchant) {
            a.weapon_art[hand] = enchant;
            world.weapon_art(a.ref, hand, enchant);
        }
    }
}

void Casters::update(float dt) {
    update_keys(dt);
    bool holding = false;
    for (size_t i = 0; i < m_order.size(); ++i) {
        ActorMagic& a = *m_order[i];
        a.shout.recovery = std::max(a.shout.recovery - dt, 0.0f);
        for (Cooldown& c : a.cooldowns) {
            c.left -= dt;
        }
        std::erase_if(a.cooldowns, [](const Cooldown& c) { return c.left <= 0.0f; });
        update_shout(a, dt);
        for (Caster& c : a.casters) {
            c.update(dt);
        }
        update_hand_art(a);
        if (a.ref == k_player_ref) {
            holding = std::ranges::any_of(a.casters, [](const Caster& c) { return c.busy(); });
        }
    }
    m_game.character().hold_magicka = holding;
}

} // namespace spud::game
