#include "game/character.hpp"

#include "formats/forms_typed.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace spud::game {

namespace {

using forms::Value;

// Condition functions the character answers.
enum Function : uint16_t {
    GetActorValue = 14, GetDisease = 39, GetIsRace = 69, GetIsSex = 70, GetLevel = 80, GetStaminaPercentage = 128,
    GetPCIsRace = 130, GetPCIsSex = 131,
    HasMagicEffect = 214, HasSpell = 264, GetBaseActorValue = 277, GetHealthPercentage = 430, HasPerk = 448,
    GetPermanentActorValue = 494, GetActorValuePercent = 640, EPModSkillUsage_IsAdvanceSkill = 681,
    EPModSkillUsage_AdvanceObjectHasKeyword = 691, EPModSkillUsage_IsAdvanceAction = 692, HasMagicEffectKeyword = 699,
};

} // namespace

Character::Character(Game& game, const forms::Database& db, const Settings& settings, const Magic& magic, Conditions& conditions)
    : m_db(&db), m_settings(&settings), m_magic(&magic), m_conditions(&conditions), m_perks(db), m_values(db, settings, true),
      m_target(game, k_player_ref, m_values) {
    const ActorRecord record = actor_record(db, settings, k_player_base, 0, 0);
    m_values.set_record(record);
    m_level = record.level;
    m_race = record.race;
    m_female = (record.acbs_flags & 1) != 0;
    if (const forms::FormEntry* e = db.find(k_player_base)) {
        name = forms::Form(db, *e).name();
    }
    m_values.on_change = [this](int av, float old_current, float delta) { on_value_change(av, old_current, delta); };
    m_xp_threshold = settings.number("fXPLevelUpBase", 75.0f) + settings.number("fXPLevelUpMult", 25.0f);
    refresh_thresholds();
}

void Character::start() {
    if (const forms::FormEntry* e = m_db->find(k_player_base)) {
        const forms::Form f(*m_db, *e);
        for (const Value* p : f.all("PRKR")) {
            const Value* perk = p->member("perk");
            const Value* rank = p->member("rank");
            if (perk && perk->form) {
                set_perk_rank(perk->form, rank ? int(rank->integer) : 1);
            }
        }
    }
    form_spells();
    race_spells(m_race, true);
}

// ---- skills and levels -------------------------------------------------------------------------------------------

float Character::skill_threshold(int av, float level) const {
    const SkillData& s = m_perks.skill(av);
    return level >= 100.0f ? 0.0f : std::pow(level, m_settings->number("fSkillUseCurve", 1.95f)) * s.improve_mult + s.improve_offset;
}

void Character::refresh_thresholds() {
    for (int i = 0; i < k_skill_value_count; ++i) {
        const int av = k_first_skill_value + i;
        m_skills[size_t(i)].level = m_values.permanent(av);
        m_skills[size_t(i)].threshold = skill_threshold(av, m_values.base(av));
    }
}

float Character::skill_percent(int av) const {
    const SkillProgress& p = progress(av);
    return p.xp / p.threshold * 100.0f; // NaN at 100, where the threshold is 0: what the game sends
}

void Character::advance_skill(int av, float xp, bool quiet) {
    SkillProgress& s = m_skills[size_t(av - k_first_skill_value)];
    if (m_values.base(av) >= 100.0f) {
        return;
    }
    s.xp += entry_point(ep_skill_use, xp);
    bool levelled = false;
    while (s.threshold > 0.0f && s.xp >= s.threshold) {
        s.xp -= s.threshold;
        const float next = m_values.base(av) + 1.0f;
        m_values.set_base(av, next);
        s.threshold = skill_threshold(av, next);
        m_xp += next * m_settings->number("fXPPerSkillRank", 1.0f);
        levelled = true;
        if (next >= 100.0f) {
            s.xp = 0;
            break;
        }
    }
    if (levelled && !quiet && events.skill_notice) {
        events.skill_notice(m_perks.skill(av).name, int(m_values.base(av)), m_level, m_xp);
    }
}

void Character::use_skill(int av, float amount, uint32_t object, int action) {
    if (amount <= 0.0f || av < k_first_skill_value || av >= k_first_skill_value + k_skill_value_count) {
        return;
    }
    const SkillData& s = m_perks.skill(av);
    m_advance_skill = av;
    m_advance_object = object;
    m_advance_action = action;
    advance_skill(av, s.use_mult * amount + s.use_offset, false);
    m_advance_skill = -1;
    m_advance_object = 0;
    m_advance_action = -1;
}

void Character::increment_skill(int av, int levels) {
    // the amount to the next level, without the use rates, once per level; only the last says so.
    m_advance_skill = av;
    SkillProgress& s = m_skills[size_t(av - k_first_skill_value)];
    for (int i = 0; i < levels && m_values.base(av) < 100.0f; ++i) {
        const float ratio = s.threshold > 0.0f ? s.xp / s.threshold : 0.0f;
        advance_skill(av, s.threshold - s.xp, i + 1 < levels);
        s.xp = ratio * s.threshold;
    }
    m_advance_skill = -1;
}

void Character::lose_progress(int count) {
    for (; count > 0; --count) {
        SkillProgress* least = nullptr;
        for (SkillProgress& s : m_skills) {
            if (s.xp > 0.0f && (!least || s.xp < least->xp)) {
                least = &s;
            }
        }
        if (!least) {
            break;
        }
        least->xp = 0;
    }
}

void Character::count_level() {
    ++m_level;
    m_values.set_record([&] {
        ActorRecord r = m_values.record();
        r.level = m_level;
        return r;
    }());
    if (events.level_up) {
        events.level_up(m_level);
    }
}

void Character::choose_attribute(int attribute) {
    m_values.mod_base(attribute, m_settings->number("iAVDhmsLevelUp", 10.0f));
    if (attribute == av_stamina) {
        m_values.mod(Modifier::permanent, av_carry_weight, m_settings->number("fLevelUpCarryWeightMod", 5.0f));
    }
    // what's above the threshold carries over.
    m_xp -= m_xp_threshold;
    m_xp_threshold = float(m_level) * m_settings->number("fXPLevelUpMult", 25.0f) + m_settings->number("fXPLevelUpBase", 75.0f);
    for (int i = 0; i < k_skill_value_count; ++i) {
        m_skills[size_t(i)].level = m_values.permanent(k_first_skill_value + i);
    }
    m_values.mod_base(av_health, m_settings->number("fPCHealthLevelBonus", 0.0f));
    for (int av : {av_health, av_magicka, av_stamina}) {
        m_values.restore(av, -m_values.modifier(av, Modifier::damage));
    }
}

void Character::reset_legendary(int av) {
    if (!legendary_ready(av)) {
        return;
    }
    m_values.set_base(av, m_settings->number("fLegendarySkillResetValue", 15.0f));
    SkillProgress& s = m_skills[size_t(av - k_first_skill_value)];
    s.level = 0;
    s.xp = 0;
    s.threshold = skill_threshold(av, m_values.base(av));
    ++s.legendary;
    if (const PerkTree* tree = m_perks.tree(av)) {
        for (const PerkTreeNode& n : tree->nodes) {
            for (const Perk* p = m_perks.find(n.perk); p; p = m_perks.find(p->next)) {
                if (const int rank = perk_rank(p->form)) {
                    remove_perk(p->form);
                    add_perk_points(rank);
                }
            }
        }
    }
}

// ---- perks -------------------------------------------------------------------------------------------------------

void Character::add_perk_points(int count) {
    if (count > 0) {
        m_perk_points = std::min(m_perk_points + count, 255);
    }
}

int Character::perk_rank(uint32_t perk) const {
    const auto it = m_ranks.find(perk);
    return it == m_ranks.end() ? 0 : it->second;
}

void Character::add_perk(uint32_t perk, int rank) {
    const Perk* p = m_perks.find(perk);
    if (!p) {
        return;
    }
    const int next = rank == 0 ? perk_rank(perk) + 1 : rank;
    if (next <= p->ranks) {
        set_perk_rank(perk, next);
    }
}

void Character::set_perk_rank(uint32_t perk, int rank) {
    // the entries of the old rank go, those of the new one come.
    const Perk* p = m_perks.find(perk);
    const int old = perk_rank(perk);
    if (!p || old == rank) {
        return;
    }
    if (rank) {
        m_ranks[perk] = rank;
    } else {
        m_ranks.erase(perk);
    }
    for (const PerkEntry& e : p->entries) {
        if (e.rank + 1 == old) {
            remove_entry(e);
        } else if (e.rank + 1 == rank) {
            apply_entry(e);
        }
    }
}

void Character::remove_perk(uint32_t perk) {
    if (!m_temporary_perks.contains(perk)) {
        set_perk_rank(perk, 0);
    }
}

void Character::add_temporary_perk(uint32_t perk) {
    if (m_temporary_perks[perk]++ == 0 && !has_perk(perk)) {
        set_perk_rank(perk, 1);
    }
}

void Character::remove_temporary_perk(uint32_t perk) {
    const auto it = m_temporary_perks.find(perk);
    if (it != m_temporary_perks.end() && --it->second == 0) {
        m_temporary_perks.erase(it);
        set_perk_rank(perk, 0);
    }
}

void Character::apply_entry(const PerkEntry& e) {
    switch (e.type) {
    case PerkEntry::entry_point: {
        // priority down, then rank up.
        auto& list = m_entries[size_t(e.point)];
        const auto at = std::lower_bound(list.begin(), list.end(), &e, [](const PerkEntry* a, const PerkEntry* b) {
            return a->priority != b->priority ? a->priority > b->priority : a->rank < b->rank;
        });
        list.insert(at, &e);
        break;
    }
    case PerkEntry::ability: add_spell(e.form); break;
    case PerkEntry::quest:
        if (events.start_quest) {
            events.start_quest(e.form, e.stage);
        }
        break;
    }
}

void Character::remove_entry(const PerkEntry& e) {
    if (e.type == PerkEntry::entry_point) {
        auto& list = m_entries[size_t(e.point)];
        list.erase(std::remove(list.begin(), list.end(), &e), list.end());
    } else if (e.type == PerkEntry::ability) {
        remove_spell(e.form);
    }
}

Character::PerkWalk Character::walk_perk(uint32_t first) const {
    PerkWalk w;
    bool found = false;
    for (const Perk* p = m_perks.find(first); p; p = m_perks.find(p->next)) {
        ++w.total;
        if (found) {
            continue;
        }
        if (has_perk(p->form)) {
            ++w.owned;
            w.prev = w.next;
            w.next = p;
        } else {
            w.prev = w.next;
            w.next = p;
            found = true;
        }
    }
    return w;
}

bool Character::perk_conditions(const Perk& perk) const { return m_conditions->evaluate(perk.conditions, k_player_ref, 0); }

int Character::perk_state(uint32_t first) const {
    const Perk* p = m_perks.find(first);
    if (!p) {
        return 0;
    }
    if (has_perk(first)) {
        return 2;
    }
    return p->level < m_level && perk_conditions(*p) ? 1 : 0;
}

bool Character::can_take(uint32_t first) const {
    const PerkWalk w = walk_perk(first);
    return w.next && w.owned < w.total && perk_conditions(*w.next);
}

bool Character::take_perk(uint32_t first) {
    if (!can_take(first)) {
        return false;
    }
    add_perk(walk_perk(first).next->form, 1);
    m_perk_points = std::max(m_perk_points - 1, 0);
    return true;
}

bool Character::entry_passes(const PerkEntry& e, int point, std::span<const uint32_t> args) const {
    // CheckConditionFilters: a tab per argument, the argument as the subject and no target.
    if (e.tab_count != entry_point_info(point).arguments) {
        return false;
    }
    for (size_t k = 0; k < e.tabs.size(); ++k) {
        const uint32_t subject = k == 0 ? k_player_ref : k - 1 < args.size() ? args[k - 1] : 0;
        if (subject ? !m_conditions->evaluate(e.tabs[k], subject, 0) : !e.tabs[k].empty()) {
            return false;
        }
    }
    return true;
}

void Character::run_function(const PerkEntry& e, float& r) const {
    const auto actor_value = [&] { return m_values.current(int(e.value1)) * e.value2; };
    switch (e.function) {
    case 1: r = e.value1; break;
    case 2: r += e.value1; break;
    case 3: r *= e.value1; break;
    case 4: {
        const float u = events.random_percent ? events.random_percent() * 0.01f : 0.5f;
        r += e.value1 + (e.value2 - e.value1) * u;
        break;
    }
    case 5: r += actor_value(); break;
    case 6: r = std::abs(r); break;
    case 7: r = -std::abs(r); break;
    case 12: r = actor_value(); break;
    case 13: r *= actor_value(); break;
    case 14: r *= 1.0f + actor_value(); break;
    default: break;
    }
}

float Character::entry_point(int point, float value, std::initializer_list<uint32_t> args) const {
    for (const PerkEntry* e : entries(point)) {
        if (entry_passes(*e, point, args)) {
            run_function(*e, value);
        }
    }
    return value;
}

uint32_t Character::entry_point_spell(int point, std::initializer_list<uint32_t> args) const {
    uint32_t spell = 0;
    for (const PerkEntry* e : entries(point)) {
        if (e->function == 10 && e->data_type == 5 && entry_passes(*e, point, args)) {
            spell = e->form; // the last wins
        }
    }
    return spell;
}

std::string Character::entry_point_text(int point, std::initializer_list<uint32_t> args) const {
    std::string text;
    for (const PerkEntry* e : entries(point)) {
        if (e->function == 15 && e->data_type == 7 && entry_passes(*e, point, args)) {
            text = e->text;
        }
    }
    return text;
}

std::vector<Character::ActivateChoice> Character::activate_choices(uint32_t target, bool& replace) {
    std::vector<ActivateChoice> out;
    const std::array<uint32_t, 1> args = {target};
    for (const PerkEntry* e : entries(ep_activate)) {
        if (e->function != 9 || e->data_type != 4 || !entry_passes(*e, ep_activate, args)) {
            continue;
        }
        if (e->script_flags & 1) {
            run_choice(*e, target);
        } else {
            out.push_back({e, e->text});
        }
        replace = replace || (e->script_flags & 2) != 0;
    }
    return out;
}

void Character::run_choice(const PerkEntry& e, uint32_t target) {
    if (e.fragment != -1 && events.perk_fragment) {
        events.perk_fragment(target, e.perk, e.fragment);
    }
    if (e.form && target == k_player_ref) {
        apply(e.form);
    }
}

// ---- race --------------------------------------------------------------------------------------------------------

void Character::form_spells() {
    if (const forms::FormEntry* e = m_db->find(k_player_base)) {
        for (const Value* s : forms::Form(*m_db, *e).all("SPLO")) {
            if (const Spell* spell = m_magic->spell(s->form); spell && spell->permanent()) {
                apply(s->form);
            }
        }
    }
}

void Character::race_spells(uint32_t race, bool add) {
    const forms::FormEntry* e = m_db->find(race);
    if (!e) {
        return;
    }
    for (const Value* s : forms::Form(*m_db, *e).all("SPLO")) {
        const Spell* spell = m_magic->spell(s->form);
        if (!spell || !spell->permanent()) {
            continue;
        }
        if (add && !m_target.has_effect_from(s->form)) {
            apply(s->form);
        } else if (!add) {
            dispel_spell(s->form);
        }
    }
}

void Character::reset_stats() {
    m_values.set_record(actor_record(*m_db, *m_settings, k_player_base, m_level, m_race));
    m_values.clear_skill_and_attribute_bases();
    refresh_thresholds();
}

void Character::switch_race(uint32_t race, bool keep_stats) {
    if (race == m_race || !m_db->find(race)) {
        return;
    }
    race_spells(m_race, false);
    const bool to_beast = [&] {
        const std::string_view id = m_db->editor_id(*m_db->find(race));
        return id == "WerewolfBeastRace" || id == "DLC1VampireBeastRace";
    }();
    if (to_beast && !m_original_race) {
        m_original_race = m_race;
    } else if (!to_beast) {
        m_original_race = 0;
    }
    m_race = race;
    ActorRecord r = m_values.record();
    r.race = race;
    m_values.set_record(r);
    m_values.clear_derived_bases();
    if (!keep_stats) {
        reset_stats();
    }
    race_spells(race, true);
    update(0.0f, false, true);
    if (events.race_changed) {
        events.race_changed(race);
    }
}

// ---- the frame ---------------------------------------------------------------------------------------------------

float Character::regen_rate(int attribute, bool in_combat) const {
    if (attribute == av_health) {
        float rate = m_values.permanent(av_health) * m_values.current(av_heal_rate) / 100.0f;
        if (rate > 0.0f) {
            if (in_combat) {
                rate *= m_values.current(av_combat_health_regen_mult);
            }
            rate = entry_point(ep_recovered_health, rate);
            rate *= m_values.current(av_heal_rate_mult) / 100.0f;
        }
        return rate;
    }
    const bool magicka = attribute == av_magicka;
    const float rate = m_values.permanent(attribute) * m_values.current(magicka ? av_magicka_rate : av_stamina_rate) / 100.0f;
    const float combat = in_combat ? (magicka ? m_settings->number("fCombatMagickaRegenRateMult", 0.33f)
                                              : m_settings->number("fCombatStaminaRegenRateMult", 0.35f))
                                   : 1.0f;
    return rate * combat * m_values.current(magicka ? av_magicka_rate_mult : av_stamina_rate_mult) / 100.0f;
}

void Character::on_value_change(int av, float old_current, float delta) {
    (void)old_current;
    if (av == av_magicka && delta < 0.0f) {
        const float delay = m_settings->number("fDamagedMagickaRegenDelay", 0.5f);
        m_magicka_delay = std::min(std::max(m_magicka_delay, delay), m_settings->number("fMagickaRegenDelayMax", 5.0f));
    }
}

void Character::update(float dt, bool in_combat, bool body) {
    if (m_magicka_delay > 0.0f) {
        m_magicka_delay = std::max(m_magicka_delay - dt, 0.0f);
    } else if (dt > 0.0f && !hold_magicka) {
        m_values.restore(av_magicka, regen_rate(av_magicka, in_combat) * dt);
    }
    if (!body && dt > 0.0f) {
        m_values.restore(av_health, regen_rate(av_health, in_combat) * dt);
        m_values.restore(av_stamina, regen_rate(av_stamina, in_combat) * dt);
    }
    m_target.update(dt);
    if (events.frame) {
        events.frame(dt);
    }
}

void Character::body_changed(float health, float stamina) {
    if (health != 0.0f) {
        m_values.mod(Modifier::damage, av_health, health);
    }
    if (stamina != 0.0f) {
        m_values.mod(Modifier::damage, av_stamina, stamina);
    }
}

// ---- conditions --------------------------------------------------------------------------------------------------

bool Character::condition_value(uint16_t fn, uint32_t subject, uint32_t target, uint32_t p1, uint32_t p2, double& out) {
    (void)target;
    (void)p2;
    auto is = [&](bool b) {
        out = b ? 1.0 : 0.0;
        return true;
    };
    // The context of a perk entry, whoever the subject is.
    switch (fn) {
    case EPModSkillUsage_IsAdvanceSkill: return is(int(p1) == m_advance_skill);
    case EPModSkillUsage_IsAdvanceAction: return is(int(p1) == m_advance_action);
    case EPModSkillUsage_AdvanceObjectHasKeyword: {
        if (const forms::FormEntry* e = m_db->find(m_advance_object)) {
            const auto k = forms::Form(*m_db, *e).keywords();
            return is(std::find(k.begin(), k.end(), p1) != k.end());
        }
        return is(false);
    }
    case GetPCIsRace: return is(m_race == p1);
    case GetPCIsSex: return is(int(m_female) == int(p1));
    default: break;
    }
    if (subject != k_player_ref) {
        return false;
    }
    switch (fn) {
    case GetActorValue: out = m_values.current(int(p1)); return true;
    case GetBaseActorValue: out = m_values.base(int(p1)); return true;
    case GetPermanentActorValue: out = m_values.permanent(int(p1)); return true;
    case GetActorValuePercent: out = m_values.percent(int(p1)); return true;
    case GetHealthPercentage: out = m_values.percent(av_health); return true;
    case GetStaminaPercentage: // not a percentage: only whether there is any
        return is(std::abs(m_values.percent(av_stamina)) > 0.0001f);
    case GetLevel: out = m_level; return true;
    case GetIsRace: return is(m_race == p1);
    case GetIsSex: return is(int(m_female) == int(p1));
    case HasPerk: return is(has_perk(p1));
    case HasSpell: return is(has_spell(p1));
    case HasMagicEffect: return is(has_magic_effect(p1));
    case HasMagicEffectKeyword: return is(has_magic_effect_keyword(p1));
    case GetDisease: return is(has_disease());
    default: return false;
    }
}

} // namespace spud::game
