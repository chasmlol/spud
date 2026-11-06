#include "game/crime.hpp"

#include "game/game.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>

namespace spud::game {

namespace {

using forms::Form;
using forms::Value;

constexpr uint32_t k_guard_faction = 0x00086EEE; // default object "Guard Faction"
constexpr uint32_t k_player_npc = 0x00000007;

uint32_t form_of(const Value* v) { return v && v->kind == Value::Kind::form ? v->form : 0; }
int64_t integer(const Value* v) { return v && v->kind == Value::Kind::integer ? v->integer : 0; }
float real(const Value* v) { return v && v->kind == Value::Kind::real ? float(v->real) : 0.0f; }

uint64_t pair_of(uint32_t a, uint32_t b) { return a < b ? uint64_t(a) << 32 | b : uint64_t(b) << 32 | a; }

bool violent(CrimeType t) { return t == CrimeType::attack || t == CrimeType::murder; }

} // namespace

const CrimeFaction* Crime::faction(uint32_t form) {
    if (const auto it = m_factions.find(form); it != m_factions.end()) {
        return &it->second;
    }
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = db.find(form);
    if (!e || e->type != forms::parse_sig("FACT").value()) {
        return nullptr;
    }
    const Form f(db, *e);
    CrimeFaction c;
    c.form = form;
    c.name = f.name();
    c.flags = uint32_t(integer(f.get("DATA")));
    c.jail_marker = form_of(f.get("JAIL"));
    c.follower_marker = form_of(f.get("WAIT"));
    c.stolen_container = form_of(f.get("STOL"));
    c.player_container = form_of(f.get("PLCN"));
    c.outfit = form_of(f.get("JOUT"));
    c.group = form_of(f.get("CRGR"));
    if (const Value* v = f.get("CRVA")) {
        c.murder = int32_t(integer(v->member("murder")));
        c.assault = int32_t(integer(v->member("assault")));
        c.trespass = int32_t(integer(v->member("trespass")));
        c.pickpocket = int32_t(integer(v->member("pickpocket")));
        c.escape = int32_t(integer(v->member("escape")));
        c.werewolf = int32_t(integer(v->member("werewolf")));
        c.steal_mult = real(v->member("steal_multiplier"));
    }
    if (c.flags & faction_default_gold) { // the werewolf value stays the record's
        const Settings& s = m_game.settings();
        c.murder = int32_t(s.number("iCrimeGoldMurder", 1000));
        c.assault = int32_t(s.number("iCrimeGoldAttack", 40));
        c.steal_mult = s.number("fCrimeGoldSteal", 0.5f);
        c.trespass = int32_t(s.number("iCrimeGoldTrespass", 5));
        c.pickpocket = int32_t(s.number("iCrimeGoldPickpocket", 25));
        c.escape = int32_t(s.number("iCrimeGoldEscape", 100));
    }
    return &m_factions.emplace(form, std::move(c)).first->second;
}

uint32_t Crime::crime_faction_of(uint32_t npc) const {
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = db.find(npc);
    return e ? form_of(Form(db, *e).get("CRIF")) : 0;
}

bool Crime::in_faction(uint32_t npc, uint32_t faction) const {
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = db.find(npc);
    if (!e) {
        return false;
    }
    for (const forms::FactionMembership& m : forms::npc_factions(Form(db, *e))) {
        if (m.faction == faction && m.rank >= 0) {
            return true;
        }
    }
    return false;
}

bool Crime::is_guard(uint32_t npc) const { return in_faction(npc, k_guard_faction); }

// The game: the actor's crime faction is it, or it's in that faction's group.
bool Crime::shares_group(uint32_t npc, uint32_t faction_form) {
    const uint32_t own = crime_faction_of(npc);
    if (own == faction_form) {
        return true;
    }
    const CrimeFaction* f = own ? faction(own) : nullptr;
    if (!f || !f->group) {
        return false;
    }
    const forms::Database& db = m_game.db();
    const forms::FormEntry* list = db.find(f->group);
    if (!list) {
        return false;
    }
    const std::vector<uint32_t> members = forms::form_list(Form(db, *list));
    return std::find(members.begin(), members.end(), faction_form) != members.end();
}

// The game, through the witness's crime faction: a type its faction doesn't care about, on a
// target outside it; or its members' crimes when it doesn't report those.
bool Crime::ignored(uint32_t witness_npc, CrimeType type, uint32_t target_npc) const {
    const uint32_t own = crime_faction_of(witness_npc);
    const auto it = m_factions.find(own);
    if (it == m_factions.end()) {
        return false;
    }
    const uint32_t flags = it->second.flags;
    const bool member = target_npc && in_faction(target_npc, own);
    if ((flags & faction_no_report_members) && member) {
        return true;
    }
    static constexpr uint32_t k_ignore[7] = {faction_ignore_steal,  faction_ignore_pickpocket, faction_ignore_trespass,
                                             faction_ignore_assault, faction_ignore_murder,     0,
                                             faction_ignore_werewolf};
    return (flags & k_ignore[size_t(type)]) && !member;
}

int32_t Crime::gold_of(const CrimeRecord& crime) {
    const CrimeFaction* f = faction(crime.faction);
    if (!f) {
        return 0;
    }
    switch (crime.type) {
    case CrimeType::steal: {
        if (f->steal_mult <= 0.0f) {
            return 0;
        }
        // The game: no form (a container's report) counts 0, so such a theft costs 1.
        const Item* item = crime.form ? m_game.items().find(crime.form) : nullptr;
        const float value = item ? float(crime.count) * float(m_game.base_value(*item)) : 0.0f;
        return std::max(int32_t(value * f->steal_mult), 1);
    }
    case CrimeType::pickpocket: return f->pickpocket;
    case CrimeType::trespass: return f->trespass;
    case CrimeType::attack: return f->assault;
    case CrimeType::murder: return f->murder;
    case CrimeType::escape: return f->escape;
    case CrimeType::werewolf: return f->werewolf;
    }
    return 0;
}

// The first witness who sees it and shares the group, charging for this crime, takes it over; a guard at once and for
// good, anyone else only if no guard does (section 1.2).
void Crime::take_over(CrimeRecord& crime, const std::vector<Witness>& around) {
    uint32_t fallback = 0;
    for (const Witness& w : around) {
        const uint32_t own = crime_faction_of(w.npc);
        if (!w.detects || !own || own == crime.faction || !shares_group(w.npc, crime.faction)) {
            continue;
        }
        CrimeRecord probe = crime;
        probe.faction = own;
        if (gold_of(probe) <= 0) {
            continue;
        }
        if (is_guard(w.npc)) {
            crime.faction = own;
            return;
        }
        fallback = fallback ? fallback : own;
    }
    if (fallback) {
        crime.faction = fallback;
    }
}

// The game: the gold, the enemy flag for a violent crime, the notification.
void Crime::charge(CrimeRecord& crime) {
    const CrimeFaction* f = faction(crime.faction);
    if (crime.charged || !f) {
        return;
    }
    crime.charged = true;
    crime.gold = gold_of(crime);
    mod_crime_gold(crime.faction, violent(crime.type), crime.gold);
    if (violent(crime.type)) {
        set_enemy(crime.faction, true);
    }
    if (crime.gold != 0) {
        m_game.notify(std::format("{} {} {}", crime.gold, m_game.settings().text("sAddCrimeGold"), f->name));
    }
}

void Crime::add_no_crime(uint32_t target, int32_t value) {
    m_stolen[m_game.location_crime_faction(target)][0] += value;
}

void Crime::report_theft(uint32_t target, uint32_t form, int32_t count, int32_t value, uint32_t owner, bool can_warn) {
    if (!reporting) {
        return;
    }
    const Item* item = form ? m_game.items().find(form) : nullptr;
    value = item ? count * m_game.base_value(*item) : value ? value : 1;
    const std::vector<Witness> around = witnesses ? witnesses() : std::vector<Witness>{};
    // The owner's representative: the owner, or a member of the owning faction, within sight of the player.
    const Witness* rep = nullptr;
    for (const Witness& w : around) {
        if (w.alive && (w.npc == owner || in_faction(w.npc, owner)) && !ignored(w.npc, CrimeType::steal, 0)) {
            rep = &w;
            break;
        }
    }
    m_stats.items_stolen += count;
    if (!rep) {
        add_no_crime(target, value);
        return;
    }
    CrimeRecord crime;
    crime.target = target;
    crime.owner = owner;
    crime.form = form;
    crime.count = count;
    crime.faction = crime_faction_of(rep->npc);
    const bool seen = rep->detects;
    // Attack on next theft: a second theft the same day loses the warning; a day later it starts again.
    const int day = int(m_hours / 24.0);
    bool may_warn = seen && can_warn && m_warning_count < int(m_game.settings().number("iStealWarnings", 2));
    if (m_attack_next_day == day) {
        may_warn = false;
    } else if (m_attack_next_day >= 0) {
        m_attack_next_day = -1;
    } else {
        m_attack_next_day = day;
    }
    if (seen) {
        crime.witnesses.push_back(rep->ref);
    }
    if (may_warn) {
        // The owner comes for it (procedure 34, AI's walk). With no walk the time runs out into the alarm.
        ++m_warning_count;
        m_warning_timer = m_game.settings().number("fWarningTimer", 5.0f);
        m_crimes.push_back(std::move(crime));
        m_warnings.push_back({m_crimes.size() - 1, m_game.settings().number("fTakeBackTimerSetting", 20.0f)});
        return;
    }
    bool reported = false;
    for (const Witness& w : around) {
        if (w.detects && w.alive && shares_group(w.npc, crime.faction) && !ignored(w.npc, CrimeType::steal, 0)) {
            reported = true;
            if (std::find(crime.witnesses.begin(), crime.witnesses.end(), w.ref) == crime.witnesses.end()) {
                crime.witnesses.push_back(w.ref);
            }
        }
    }
    if (reported) {
        take_over(crime, around);
        m_stolen[crime.faction][1] += value;
        charge(crime);
        m_crimes.push_back(std::move(crime));
    } else {
        add_no_crime(target, value);
    }
}

// A failed pickpocket always charges: the target is the first witness.
void Crime::report_pickpocket(uint32_t ref, uint32_t npc) {
    if (!reporting) {
        return;
    }
    if (ignored(npc, CrimeType::pickpocket, 0)) {
        return;
    }
    CrimeRecord crime;
    crime.type = CrimeType::pickpocket;
    crime.target = ref;
    crime.owner = npc;
    crime.faction = crime_faction_of(npc);
    crime.witnesses.push_back(ref);
    const std::vector<Witness> around = witnesses ? witnesses() : std::vector<Witness>{};
    take_over(crime, around);
    charge(crime);
    m_crimes.push_back(std::move(crime));
}

void Crime::report_trespass(uint32_t owner) {
    if (!reporting) {
        return;
    }
    CrimeRecord crime;
    crime.type = CrimeType::trespass;
    crime.owner = owner;
    const std::vector<Witness> around = witnesses ? witnesses() : std::vector<Witness>{};
    ++m_stats.trespasses;
    for (const Witness& w : around) {
        if (w.alive && (w.npc == owner || in_faction(w.npc, owner))) {
            crime.faction = crime_faction_of(w.npc);
            crime.witnesses.push_back(w.ref);
            charge(crime);
            break;
        }
    }
    m_crimes.push_back(std::move(crime));
}

void Crime::report_attack(uint32_t ref, uint32_t npc) {
    if (!reporting || is_guard(npc)) {
        return;
    }
    CrimeRecord crime;
    crime.type = CrimeType::attack;
    crime.target = ref;
    crime.owner = npc;
    crime.faction = crime_faction_of(npc);
    ++m_stats.assaults;
    if (ignored(npc, CrimeType::attack, 0)) {
        return;
    }
    crime.witnesses.push_back(ref);
    take_over(crime, witnesses ? witnesses() : std::vector<Witness>{});
    charge(crime);
    set_resisting_arrest(crime.faction);
    m_crimes.push_back(std::move(crime));
}

void Crime::report_murder(uint32_t ref, uint32_t npc) {
    if (!reporting) {
        return;
    }
    CrimeRecord crime;
    crime.type = CrimeType::murder;
    crime.target = ref;
    crime.owner = npc;
    crime.faction = crime_faction_of(npc);
    ++m_stats.murders;
    const std::vector<Witness> around = witnesses ? witnesses() : std::vector<Witness>{};
    bool seen = false;
    for (const Witness& w : around) {
        if (w.ref != ref && w.alive && w.detects && shares_group(w.npc, crime.faction)) {
            seen = true;
            crime.witnesses.push_back(w.ref);
        }
    }
    if (seen) {
        take_over(crime, around);
        charge(crime);
        set_resisting_arrest(crime.faction);
    }
    m_crimes.push_back(std::move(crime));
}

// SetCrimeGold: a non-zero value adds to the infamy of its class, whole.
void Crime::set_crime_gold(uint32_t f, bool is_violent, int32_t gold) {
    Bounty& b = m_bounty[f];
    const int32_t before = b.total();
    (is_violent ? b.violent : b.non_violent) = gold;
    if (gold != 0) {
        (is_violent ? b.infamy_violent : b.infamy_non_violent) += gold;
    }
    const CrimeFaction* c = faction(f);
    if (c && (c->flags & faction_tracks_crime) && b.total() > before) {
        m_stats.largest_bounty = std::max(m_stats.largest_bounty, b.total());
        m_stats.lifetime_bounty += b.total() - before;
    }
}

void Crime::mod_crime_gold(uint32_t f, bool is_violent, int32_t delta) {
    const Bounty& b = m_bounty[f];
    set_crime_gold(f, is_violent, std::max((is_violent ? b.violent : b.non_violent) + delta, 0));
}

void Crime::adjust_crime_gold(uint32_t f, bool is_violent, int32_t delta) {
    Bounty& b = m_bounty[f];
    int32_t& gold = is_violent ? b.violent : b.non_violent;
    int32_t& infamy = is_violent ? b.infamy_violent : b.infamy_non_violent;
    gold = std::max(gold + delta, 0);
    infamy = std::max(infamy + delta, 0);
}

void Crime::reset_crime_gold(uint32_t f) {
    Bounty& b = m_bounty[f];
    b.violent = b.non_violent = 0;
}

int32_t Crime::stolen_value(uint32_t f, bool with_crime) const {
    const auto it = m_stolen.find(f);
    return it == m_stolen.end() ? 0 : it->second[with_crime ? 1 : 0];
}

void Crime::set_enemy(uint32_t f, bool enemy) {
    if (enemy) {
        m_enemy_stamp[f] = m_hours;
    } else {
        m_enemy_stamp.erase(f);
    }
}

// The flag expires iCrimeEnemyCoolDownTimer game hours on.
bool Crime::is_enemy(uint32_t f) {
    const auto it = m_enemy_stamp.find(f);
    if (it == m_enemy_stamp.end()) {
        return false;
    }
    if (it->second + m_game.settings().number("iCrimeEnemyCoolDownTimer", 24) < m_hours) {
        m_enemy_stamp.erase(it);
        return false;
    }
    return true;
}

bool Crime::arrest_on_sight(uint32_t f) {
    const Settings& s = m_game.settings();
    const Bounty& b = bounty(f);
    if (m_days > 0 || resisting_arrest(f)) {
        return false;
    }
    return (is_enemy(f) && b.total() > 0) || b.non_violent > int32_t(s.number("iArrestOnSightNonViolent", 1000)) ||
           b.violent > int32_t(s.number("iArrestOnSightViolent", 999));
}

bool Crime::attack_on_sight(uint32_t f) {
    const Settings& s = m_game.settings();
    const Bounty& b = bounty(f);
    const auto non_violent = int32_t(s.number("iAttackOnSightNonViolent", 0));
    return b.violent > int32_t(s.number("iAttackOnSightViolent", 2999)) ||
           (non_violent > 0 && b.non_violent > non_violent);
}

void Crime::set_resisting_arrest(uint32_t f) {
    if (f) {
        set_enemy(f, true);
        m_resist_clock[f] = m_clock;
    }
}

bool Crime::resisting_arrest(uint32_t f) {
    const auto it = m_resist_clock.find(f);
    if (it == m_resist_clock.end()) {
        return false;
    }
    if (!is_enemy(f) || m_clock - it->second >= m_game.settings().number("fResistArrestTimer", 300.0f)) {
        m_resist_clock.erase(it);
        return false;
    }
    return true;
}

void Crime::set_expelled(uint32_t f, bool value) { m_expelled[f] = value; }

bool Crime::expelled(uint32_t f) {
    const auto it = m_expelled.find(f);
    if (it != m_expelled.end()) {
        return it->second;
    }
    const CrimeFaction* c = faction(f);
    return c && (c->flags & faction_expelled);
}

void Crime::move_all(Inventory& from, Inventory& to, const std::function<bool(const Stack&)>& which) {
    auto& stacks = from.stacks();
    for (size_t i = stacks.size(); i-- > 0;) {
        if (which(stacks[i])) {
            const Stack s = stacks[i];
            stacks.erase(stacks.begin() + std::ptrdiff_t(i));
            to.add(s.form, s.count, s.extra);
            for (Stack& moved : to.stacks()) {
                if (moved.form == s.form && moved.extra == s.extra) {
                    moved.worn = s.worn; // comes back as it went: re-equips
                    moved.favorite = s.favorite;
                }
            }
        }
    }
}

std::optional<JailMove> Crime::door_move(uint32_t door) const {
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = db.find(door);
    if (!e) {
        return std::nullopt;
    }
    const Form f(db, *e);
    const Value* t = f.get("XTEL");
    if (!t) {
        return std::nullopt;
    }
    JailMove m;
    m.door = form_of(t->member("door"));
    if (const Value* p = t->member("position")) {
        m.position = {real(p->member("x")), real(p->member("y")), real(p->member("z"))};
    }
    if (const Value* r = t->member("rotation")) {
        m.heading = real(r->member("z"));
    }
    return m;
}

void Crime::clear_jail_state() {
    set_enemy(m_jail_faction, false);
    m_days = 0;
    m_jail_faction = 0;
}

// PayFine: the gold, stolen goods to the hold's chest, the bounty gone.
void Crime::pay_fine(uint32_t f, bool go_to_jail, bool remove_stolen) {
    const CrimeFaction* c = faction(f);
    if (!c) {
        return;
    }
    Player& p = m_game.player();
    p.inventory.remove(k_gold, crime_gold(f));
    ++m_stats.fines_paid;
    m_jail_faction = f;
    if (remove_stolen && c->stolen_container) {
        move_all(p.inventory, m_game.container(c->stolen_container), [](const Stack& s) { return s.extra.owner != 0; });
    }
    reset_crime_gold(f);
    if (go_to_jail) {
        m_days = 0;
        release(); // thrown out by the jail's door
    } else {
        clear_jail_state();
    }
}

// GoToJail and Confiscate.
void Crime::go_to_jail(uint32_t f, bool remove_inventory, bool real_jail) {
    const CrimeFaction* c = faction(f);
    if (!c) {
        return;
    }
    m_jail_faction = f;
    m_escaping = false;
    m_trespass_owner = 0;
    ++m_stats.times_jailed;
    const Settings& s = m_game.settings();
    m_days = std::clamp(crime_gold(f) / int32_t(s.number("iCrimeDaysInPrisonMod", 100)), 1,
                        int32_t(s.number("iCrimeMaxNumberofDaysinJail", 7)));
    if (!real_jail) {
        reset_crime_gold(f);
        clear_jail_state();
        return;
    }
    if (remove_inventory) {
        Inventory& inv = m_game.player().inventory;
        if (c->stolen_container) {
            move_all(inv, m_game.container(c->stolen_container), [](const Stack& st) { return st.extra.owner != 0; });
        }
        if (c->player_container) {
            move_all(inv, m_game.container(c->player_container), [](const Stack&) { return true; });
        }
        inv.add(k_lockpick, 1);
        if (const forms::FormEntry* outfit = c->outfit ? m_game.db().find(c->outfit) : nullptr) {
            const Form o(m_game.db(), *outfit);
            if (const Value* items = o.get("INAM")) {
                for (const Value& item : items->items) {
                    for (const Rolled& r : m_game.roll(item.form)) {
                        inv.add(r.form, r.count, r.extra);
                        inv.stacks().back().worn = Worn::worn;
                    }
                }
            }
        }
    }
    m_move = door_move(c->jail_marker); // the door's destination: inside
}

// ServeSentence: the days pass, skill progress goes, the things come back.
void Crime::serve_sentence() {
    const CrimeFaction* c = faction(m_jail_faction);
    if (m_days <= 0 || !c) {
        return;
    }
    m_stats.days_jailed += m_days;
    if (pass_hours) {
        pass_hours(24.0 * m_days);
    }
    // The game: the smallest progress first, levels never drop.
    auto n = int32_t(float(crime_gold(m_jail_faction)) * m_game.settings().number("fCrimeGoldSkillPenaltyMult", 0.1f));
    Player& p = m_game.player();
    m_game.character().lose_progress(n);
    if (c->player_container) {
        move_all(m_game.container(c->player_container), p.inventory, [](const Stack&) { return true; });
    }
    release();
}

// Release: out by the door linked to the marker's, and the slate wiped.
void Crime::release() {
    const CrimeFaction* c = faction(m_jail_faction);
    if (c) {
        if (const auto inside = door_move(c->jail_marker)) {
            m_move = door_move(inside->door);
        }
        reset_crime_gold(m_jail_faction);
    }
    clear_jail_state();
}

// The game: only where the faction's escape value isn't 0. `confirmed`: out by a
// teleport door; the things stay in the evidence chest.
void Crime::set_escaping(bool escaping, bool confirmed) {
    const CrimeFaction* c = faction(m_jail_faction);
    if (!c || c->escape == 0 || m_days <= 0) {
        m_escaping = false;
        return;
    }
    m_escaping = escaping;
    if (confirmed) {
        mod_crime_gold(m_jail_faction, false, c->escape);
        m_game.notify(std::format("{} {} {}", c->escape, m_game.settings().text("sAddCrimeGold"), c->name));
        m_jail_faction = 0;
        m_days = 0;
        m_escaping = false;
        ++m_stats.jail_escapes;
    }
}

std::optional<JailMove> Crime::take_move() { return std::exchange(m_move, std::nullopt); }

int Crime::relationship(uint32_t a, uint32_t b) const {
    if (const auto it = m_relationship_changes.find(pair_of(a, b)); it != m_relationship_changes.end()) {
        return it->second;
    }
    return m_game.record_relationship(a, b);
}

void Crime::set_relationship(uint32_t a, uint32_t b, int rank) { m_relationship_changes[pair_of(a, b)] = rank; }

int Crime::favor_value(uint32_t npc) const {
    static constexpr const char* k_names[5] = {"", "iFavorFriendValue", "iFavorConfidantValue", "iFavorAllyValue",
                                               "iFavorLoverValue"};
    static constexpr float k_defaults[5] = {0, 25, 50, 100, 500};
    const int rank = relationship(k_player_npc, npc);
    return rank >= 1 && rank <= 4 ? int(m_game.settings().number(k_names[rank], k_defaults[rank])) : 0;
}

int32_t Crime::bribe_amount(uint32_t npc) const {
    const Settings& s = m_game.settings();
    const Player& p = m_game.player();
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = db.find(npc);
    float level = 1, morality = 0;
    if (e) {
        const Form f(db, *e);
        if (const Value* v = f.get("ACBS.level")) {
            level = float(v->integer);
        }
        if (const Value* v = f.get("AIDT.morality")) {
            morality = float(v->integer);
        }
    }
    const float speech = m_game.character().skill(speechcraft), other = m_game.npc_skill(npc, speechcraft);
    const float x = float(m_game.character().level()) + level * s.number("fBribeNPCLevelMult", 0.1f) -
                    (speech - other - 100.0f) * s.number("fBribeSpeechcraftMult", 0.1f);
    const float cost = std::pow(std::max(x, 0.0f), s.number("fBribeCostCurve", 1.4f)) * s.number("fBribeScale", 0.5f) *
                       (1.0f + morality * s.number("fBribeMoralityMult", 1.0f));
    return int32_t(m_game.perk_value(46, cost));
}

void Crime::set_faction_reaction(uint32_t a, uint32_t b, int reaction) { m_reactions[{a, b}] = reaction; }

int Crime::faction_reaction(uint32_t a, uint32_t b) const {
    if (const auto it = m_reactions.find({a, b}); it != m_reactions.end()) {
        return it->second;
    }
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = db.find(a);
    if (!e) {
        return 0;
    }
    const Form f(db, *e);
    for (const Value* v : f.all("XNAM")) {
        if (form_of(v->member("faction")) == b) {
            return int(integer(v->member("group_combat_reaction")));
        }
    }
    return 0;
}

// The game: an owned cell that isn't public or hand-changed, and isn't the player's to be in.
void Crime::enter_cell(uint32_t cell) {
    const forms::Database& db = m_game.db();
    const forms::FormEntry* e = cell ? db.find(cell) : nullptr;
    uint32_t owner = 0;
    if (e && reporting) {
        const Form f(db, *e);
        const int64_t flags = integer(f.get("DATA"));
        owner = form_of(f.get("XOWN"));
        if ((flags & 0x60) || m_game.may_take(owner, 0)) {
            owner = 0;
        }
    }
    m_trespass_owner = owner;
}

void Crime::update(float seconds, double game_hours) {
    m_hours = game_hours;
    m_clock += seconds;
    if (m_warning_timer > 0.0f) {
        m_warning_timer -= seconds;
        if (m_warning_timer <= 0.0f && m_warning_count > 0) {
            --m_warning_count;
        }
    }
    for (size_t i = m_warnings.size(); i-- > 0;) {
        Warning& w = m_warnings[i];
        w.seconds -= seconds;
        if (w.seconds <= 0.0f) {
            if (w.crime < m_crimes.size()) {
                charge(m_crimes[w.crime]);
            }
            m_warnings.erase(m_warnings.begin() + std::ptrdiff_t(i));
        }
    }
    for (CrimeRecord& c : m_crimes) {
        c.age += seconds;
    }
    m_sweep -= seconds;
    if (m_sweep > 0.0f) {
        return;
    }
    m_sweep = 1.0f;
    // The game: 30 seconds settle a crime; before that, with every witness dead it's undone.
    const auto around = witnesses ? witnesses() : std::vector<Witness>{};
    const float settle = m_game.settings().number("fKillWitnessesTimerSetting", 30.0f);
    if (!m_warnings.empty()) {
        return; // their indices hold until they've run out
    }
    std::erase_if(m_crimes, [&](CrimeRecord& c) {
        if (c.settled || c.age < 1.0) {
            return false;
        }
        if (c.age >= settle) {
            c.settled = true;
            return false;
        }
        const bool any_alive = std::any_of(c.witnesses.begin(), c.witnesses.end(), [&](uint32_t ref) {
            const auto w = std::find_if(around.begin(), around.end(), [&](const Witness& x) { return x.ref == ref; });
            return w == around.end() || w->alive; // out of sight isn't dead
        });
        if (any_alive) {
            return false;
        }
        if (c.charged && c.gold > 0) {
            adjust_crime_gold(c.faction, violent(c.type), -c.gold);
            const CrimeFaction* f = faction(c.faction);
            const Settings& s = m_game.settings();
            m_game.notify(std::format("{} {} {} {}", s.text("sWitnessKilled"), c.gold, s.text("sRemoveCrimeGold"),
                                      f ? f->name : std::string()));
        }
        return true;
    });
}

} // namespace spud::game
