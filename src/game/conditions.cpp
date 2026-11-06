#include "game/conditions.hpp"

#include "game/quests.hpp"

#include <cmath>
#include <cstring>
#include <unordered_set>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace spud::game {

namespace {

constexpr forms::Sig sig(const char (&s)[5]) { return container::fourcc(s); }

// Function indices.
enum Function : uint16_t {
    GetDistance = 1, GetActorValue = 14, HasPerk = 448, HasSpell = 264, GetCurrentTime = 18, GetInSameCell = 32, SameFaction = 42, SameRace = 43,
    SameSex = 44, GetFactionRankDifference = 60, IsRaining = 62, GetInCell = 67, GetIsClass = 68, GetIsRace = 69,
    GetIsSex = 70, GetInFaction = 71, GetIsID = 72, GetFactionRank = 73, GetGlobalValue = 74, IsSnowing = 75,
    GetLevel = 80, GetPCIsClass = 129, GetPCIsRace = 130, GetPCIsSex = 131, GetPCInFaction = 132, SameFactionAsPC = 133,
    SameRaceAsPC = 134, SameSexAsPC = 135, GetIsReference = 136, GetWindSpeed = 147, GetCurrentWeatherPercent = 148,
    GetIsCurrentWeather = 149, GetInCellParam = 230, GetIsPlayableRace = 254, HasAssociationType = 258,
    HasFamilyRelationship = 259, HasParentRelationship = 261, IsPleasant = 266, IsCloudy = 267, GetBaseActorValue = 277,
    IsOwner = 278, IsInInterior = 300, GetInWorldspace = 310, IsActor = 353, IsEssential = 354, GetInCurrentLoc = 359, GetInCurrentLocAlias = 360, HasSameEditorLocAsRefAlias = 181,
    HasLinkedRef = 362, IsChild = 365, IsInList = 372, GetOffersServicesNow = 255, GetIsVoiceType = 426, GetRelationshipRank = 403, GetFactionCombatReaction = 410,
    Exists = 414, GetHealthPercentage = 430, IsProtected = 476, GetPermanentActorValue = 494, HasKeyword = 560,
    HasRefType = 561, LocationHasKeyword = 562, GetIsEditorLocation = 565, GetEventData = 576, GetActorValuePercent = 640, IsUnique = 641, IsLinkedTo = 650,
};

// Constant 0 in this build: stubs the callbacks keep. IsWin32 alone is 1.
constexpr uint16_t k_constant_zero[] = {36, 53, 64, 79, 112, 192, 225, 229, 282, 306, 309, 315, 437, 473, 488, 520, 530};
constexpr uint16_t k_is_win32 = 521;

// Functions answered here; the table in the design lists what the others wait for.
constexpr uint16_t k_available[] = {
    GetOffersServicesNow, GetIsVoiceType,
    GetDistance, GetActorValue, GetCurrentTime, GetInSameCell, SameFaction, SameRace, SameSex, GetFactionRankDifference,
    IsRaining, GetInCell, GetIsClass, GetIsRace, GetIsSex, GetInFaction, GetIsID, GetFactionRank, GetGlobalValue, IsSnowing,
    GetLevel, GetPCIsClass, GetPCIsRace, GetPCIsSex, GetPCInFaction, SameFactionAsPC, SameRaceAsPC, SameSexAsPC,
    GetIsReference, GetWindSpeed, GetCurrentWeatherPercent, GetIsCurrentWeather, GetInCellParam, GetIsPlayableRace,
    HasAssociationType, HasFamilyRelationship, HasParentRelationship, IsPleasant, IsCloudy, GetBaseActorValue, IsOwner,
    IsInInterior, GetInWorldspace, IsActor, IsEssential, GetInCurrentLoc, GetInCurrentLocAlias, HasSameEditorLocAsRefAlias, HasRefType, HasLinkedRef, IsChild, IsInList,
    GetRelationshipRank, GetFactionCombatReaction, Exists, GetHealthPercentage, IsProtected, GetPermanentActorValue,
    HasKeyword, LocationHasKeyword, GetIsEditorLocation, GetEventData, GetActorValuePercent, IsUnique, IsLinkedTo,
};

// The entry's reference byte is clear: the callback runs with no subject.
bool runs_without_subject(uint16_t f) {
    switch (f) {
    case GetCurrentTime: case IsRaining: case GetGlobalValue: case IsSnowing: case GetPCIsClass: case GetPCIsRace:
    case GetPCIsSex: case GetPCInFaction: case GetWindSpeed: case GetCurrentWeatherPercent: case GetIsCurrentWeather:
    case IsPleasant: case IsCloudy: case GetFactionCombatReaction:
        return true;
    default:
        return false;
    }
}

// Parameter 1 of type reference or actor: a null one becomes the target (types 4, 6, 24, 26).
bool takes_target(uint16_t f) {
    switch (f) {
    case GetDistance: case GetInSameCell: case SameFaction: case SameRace: case SameSex: case GetIsReference:
    case HasAssociationType: case HasFamilyRelationship: case HasParentRelationship: case GetRelationshipRank:
    case Exists: case IsLinkedTo: case 584 /* GetRelativeAngle */: case 678 /* ShouldAttackKill */: case 522: case 523: case 524: case 525: // GetVATS...TargetVisible
        return true;
    default:
        return false;
    }
}

// Template flags of ACBS.
constexpr uint16_t k_template_traits = 0x01;
constexpr uint16_t k_template_stats = 0x02;
constexpr uint16_t k_template_factions = 0x04;
constexpr uint32_t k_player_base = 0x7;
constexpr uint16_t k_get_vm_quest_variable = 629;
constexpr uint32_t k_weather_pleasant = 0x01, k_weather_cloudy = 0x02, k_weather_rainy = 0x04, k_weather_snow = 0x08;

int64_t member_int(const forms::Value* v, std::string_view path) {
    const forms::Value* m = v ? v->path(path) : nullptr;
    return m ? m->integer : 0;
}

} // namespace

ConditionItem condition_item(const forms::Value& ctda) {
    ConditionItem item;
    item.function = uint16_t(member_int(&ctda, "function"));
    item.flags = uint8_t(member_int(&ctda, "flags") | member_int(&ctda, "operator") << 5);
    item.run_on = RunOn(member_int(&ctda, "run_on"));
    if (const forms::Value* v = ctda.member("value")) {
        item.value = float(v->real);
    }
    if (const forms::Value* g = ctda.member("global")) {
        item.value_global = g->form;
    }
    item.param1 = uint32_t(member_int(&ctda, "param1"));
    item.param2 = uint32_t(member_int(&ctda, "param2"));
    item.member = uint16_t(member_int(&ctda, "param3"));
    if (const forms::Value* r = ctda.member("reference")) {
        item.reference = r->form;
    }
    return item;
}

void condition_text(ConditionItem& item, std::string_view text) {
    static std::unordered_set<std::string> strings; // a handful of variable names
    item.text = &*strings.emplace(text).first;
}

std::vector<ConditionItem> condition_list(const forms::Form& form) {
    std::vector<ConditionItem> list;
    for (const forms::DecodedField& f : form.fields()) {
        if (f.sig == forms::sig("CTDA")) {
            list.push_back(condition_item(f.value));
        } else if ((f.sig == forms::sig("CIS1") || f.sig == forms::sig("CIS2")) && !list.empty()) {
            condition_text(list.back(), f.value.text);
        }
    }
    return list;
}

bool Conditions::available(uint16_t function) {
    return function == k_is_win32 ||
           std::find(std::begin(k_available), std::end(k_available), function) != std::end(k_available) ||
           std::find(std::begin(k_constant_zero), std::end(k_constant_zero), function) != std::end(k_constant_zero);
}

const forms::Form* Conditions::form(uint32_t id) {
    if (id == 0) {
        return nullptr;
    }
    auto it = m_forms.find(id);
    if (it == m_forms.end()) {
        const forms::FormEntry* e = m_db.find(id);
        it = m_forms.emplace(id, e ? std::make_unique<forms::Form>(m_db, *e) : nullptr).first;
    }
    return it->second.get();
}

uint32_t Conditions::base_of(uint32_t reference) {
    if (reference == k_player) {
        return k_player_base; // no record places the player: the game creates PlayerRef
    }
    const forms::FormEntry* e = m_db.find(reference);
    if (!e) {
        return created_base ? created_base(reference) : 0;
    }
    if (const uint32_t base = forms::placed_base(m_db, *e)) {
        return base;
    }
    // A bound object a perk's tab runs on: the game wraps it in a scratch reference with it as the base.
    return e->type == sig("REFR") || e->type == sig("ACHR") ? 0 : e->form_id;
}

uint32_t Conditions::npc_of(uint32_t reference) {
    const uint32_t base = base_of(reference);
    const forms::Form* f = form(base);
    return f && f->type() == sig("NPC_") ? base : 0;
}

uint32_t Conditions::npc_trait(uint32_t npc, uint16_t template_flag) {
    // A template can be a leveled list, which only a placed actor resolves: then the NPC_ itself is what is known.
    for (int depth = 0; depth < 16; ++depth) {
        const forms::Form* f = form(npc);
        if (!f) {
            return 0;
        }
        const forms::Value* acbs = f->get("ACBS");
        const forms::Value* t = f->get("TPLT");
        if (!t || !t->form || !(member_int(acbs, "template_flags") & template_flag)) {
            return npc;
        }
        const forms::Form* next = form(t->form);
        if (!next || next->type() != sig("NPC_")) {
            return npc;
        }
        npc = t->form;
    }
    return npc;
}

uint32_t Conditions::race_of(uint32_t npc) {
    if (npc == k_player_base && m_world.player_race) {
        return m_world.player_race;
    }
    const forms::Form* f = form(npc_trait(npc, k_template_traits));
    const forms::Value* v = f ? f->get("RNAM") : nullptr;
    return v ? v->form : 0;
}

uint32_t Conditions::cell_of(uint32_t reference) {
    if (reference == k_player && m_world.player_cell) {
        return m_world.player_cell;
    }
    const forms::FormEntry* e = m_db.find(reference);
    return e ? e->parent : 0;
}

uint32_t Conditions::worldspace_of(uint32_t cell) {
    const forms::Form* f = form(cell);
    if (!f || f->type() != sig("CELL")) {
        return 0;
    }
    const forms::Form* parent = form(f->entry().parent);
    return parent && parent->type() == sig("WRLD") ? parent->id() : 0;
}

uint32_t Conditions::location_of(uint32_t reference) {
    if (reference == k_player && m_world.player_location) {
        return m_world.player_location;
    }
    if (ref_location) {
        if (const uint32_t location = ref_location(reference)) {
            return location;
        }
    }
    return editor_location_of(reference);
}

// Where the record puts it, wherever it has gone since.
uint32_t Conditions::editor_location_of(uint32_t reference) {
    const forms::Form* c = form(cell_of(reference));
    const forms::Value* v = c ? c->get("XLCN") : nullptr;
    if (v) {
        return v->form;
    }
    // In the world's persistent cell: the cell its position is in.
    const forms::FormEntry* r = m_db.find(reference);
    const auto placed = r ? forms::placement(m_db, *r) : std::nullopt;
    if (!c || !placed || !c->entry().parent) {
        return 0;
    }
    if (!m_read_exteriors) {
        m_read_exteriors = true;
        for (const forms::FormEntry* e : m_db.of_type(sig("CELL"))) {
            if (const auto xclc = m_db.field(*e, sig("XCLC")); xclc && xclc->data.size() >= 8) {
                int32_t grid[2];
                std::memcpy(grid, xclc->data.data(), 8);
                m_exterior_cells[uint64_t(e->parent) << 32 | uint64_t(uint16_t(grid[0])) << 16 | uint16_t(grid[1])] = e->form_id;
            }
        }
    }
    const auto gx = int32_t(std::floor(placed->position[0] / 4096.0f)), gy = int32_t(std::floor(placed->position[1] / 4096.0f));
    const auto it = m_exterior_cells.find(uint64_t(c->entry().parent) << 32 | uint64_t(uint16_t(gx)) << 16 | uint16_t(gy));
    const forms::Form* exterior = it == m_exterior_cells.end() ? nullptr : form(it->second);
    const forms::Value* location = exterior ? exterior->get("XLCN") : nullptr;
    return location ? location->form : 0;
}

int32_t Conditions::faction_rank(uint32_t reference, uint32_t faction) {
    const forms::Form* f = form(npc_trait(npc_of(reference), k_template_factions));
    if (!f) {
        return -2;
    }
    for (const forms::FactionMembership& m : forms::npc_factions(*f)) {
        if (m.faction == faction) {
            return m.rank;
        }
    }
    return -2;
}

double Conditions::actor_value(uint32_t reference, uint32_t av) {
    // Skills (6-23) and the three attributes (24-26); every other value has no record here.
    const forms::Form* f = form(npc_trait(npc_of(reference), k_template_stats));
    const forms::Value* d = f ? f->get("DNAM") : nullptr;
    if (!d) {
        return 0.0;
    }
    if (av >= 6 && av <= 23) {
        const forms::Value* s = d->member("skill_values");
        return s && av - 6 < s->items.size() ? double(s->items[av - 6].integer) : 0.0;
    }
    const char* names[3] = {"health", "magicka", "stamina"};
    return av >= 24 && av <= 26 ? double(member_int(d, names[av - 24])) : 0.0;
}

bool Conditions::has_keyword(uint32_t form_id, uint32_t keyword) {
    const forms::Form* f = form(form_id);
    if (!f) {
        return false;
    }
    const auto list = f->keywords();
    return std::find(list.begin(), list.end(), keyword) != list.end();
}

bool Conditions::location_within(uint32_t location, uint32_t ancestor) {
    for (int depth = 0; location && depth < 32; ++depth) {
        if (location == ancestor) {
            return true;
        }
        const forms::Form* f = form(location);
        const forms::Value* p = f ? f->get("PNAM") : nullptr;
        location = p ? p->form : 0;
    }
    return false;
}

bool Conditions::weather_flag(uint32_t weather, uint32_t flag) {
    const forms::Form* f = form(weather);
    const forms::Value* d = f ? f->get("DATA") : nullptr;
    return (member_int(d, "flags") & flag) != 0;
}

uint32_t Conditions::relationship(uint32_t a, uint32_t b, uint32_t* association) {
    // RELA records, between two bases, either way round; the first in the database's order if there are several.
    const auto pair_key = [](uint32_t x, uint32_t y) { return uint64_t(std::min(x, y)) << 32 | std::max(x, y); };
    if (m_relationships.empty()) {
        for (const forms::FormEntry* e : m_db.of_type(sig("RELA"))) {
            const forms::Form* f = form(e->form_id);
            const forms::Value* d = f ? f->get("DATA") : nullptr;
            const forms::Value* parent = d ? d->member("parent") : nullptr;
            const forms::Value* child = d ? d->member("child") : nullptr;
            if (parent && child) {
                const forms::Value* t = d->member("association_type");
                m_relationships.try_emplace(pair_key(parent->form, child->form), e->form_id, t ? t->form : 0);
            }
        }
    }
    const auto it = m_relationships.find(pair_key(a, b));
    if (it == m_relationships.end()) {
        return 0;
    }
    if (association) {
        *association = it->second.second;
    }
    return it->second.first;
}

uint32_t Conditions::linked_ref(uint32_t reference, uint32_t keyword) {
    const forms::Form* f = form(reference);
    if (!f) {
        return 0;
    }
    for (const forms::Value* v : f->all("XLKR")) {
        const forms::Value *k = v->member("keyword"), *r = v->member("reference");
        if (r && (k ? k->form : 0) == keyword) {
            return r->form;
        }
    }
    return 0;
}

double Conditions::value(uint16_t fn, uint32_t subject, uint32_t target, uint32_t p1, uint32_t p2, bool* known) {
    if (known) {
        *known = available(fn);
    }
    if (double live = 0.0; m_facts && m_facts->condition_value(fn, subject, target, p1, p2, live)) {
        if (known) {
            *known = true;
        }
        return live;
    }
    const uint32_t npc = npc_of(subject);
    auto is = [](bool b) { return b ? 1.0 : 0.0; };
    switch (fn) {
    case GetDistance: {
        // Not in the same world: interiors must be one cell, exteriors one worldspace.
        if (!p1) {
            return 0.0;
        }
        const uint32_t ca = cell_of(subject), cb = cell_of(p1);
        const uint32_t wa = subject == k_player ? m_world.player_worldspace : worldspace_of(ca);
        const uint32_t wb = p1 == k_player ? m_world.player_worldspace : worldspace_of(cb);
        if ((!wa || !wb) ? ca != cb : wa != wb) {
            return double(FLT_MAX);
        }
        // The player's own position isn't in the records.
        auto position = [this](uint32_t ref) -> std::optional<std::array<float, 3>> {
            if (ref == k_player) {
                return m_world.player_position;
            }
            const forms::Form* f = form(ref);
            const auto placed = f ? forms::placement(*f) : std::nullopt;
            return placed ? std::optional(std::array{placed->position[0], placed->position[1], placed->position[2]}) : std::nullopt;
        };
        const auto pa = position(subject), pb = position(p1);
        if (!pa || !pb) {
            return 0.0;
        }
        return std::hypot((*pa)[0] - (*pb)[0], (*pa)[1] - (*pb)[1], (*pa)[2] - (*pb)[2]);
    }
    case HasPerk: {
        // The base's PRKR: only the player's perks change.
        const forms::Form* f = form(npc);
        if (f) {
            for (const forms::Value* r : f->all("PRKR")) {
                const forms::Value* perk = r->member("perk");
                if (perk && perk->form == p1) {
                    return 1.0;
                }
            }
        }
        return 0.0;
    }
    case HasSpell: {
        for (const forms::Form* f : {form(npc), form(race_of(npc))}) {
            if (f) {
                for (const forms::Value* s : f->all("SPLO")) {
                    if (s->form == p1) {
                        return 1.0;
                    }
                }
            }
        }
        return 0.0;
    }
    case GetActorValue: case GetBaseActorValue: case GetPermanentActorValue:
        return actor_value(subject, p1);
    case GetActorValuePercent: case GetHealthPercentage:
        return is(npc != 0); // current / permanent: nothing damages an actor yet
    case GetCurrentTime:
        return m_world.game_hour;
    case GetInSameCell:
        return is(p1 && cell_of(subject) && cell_of(subject) == cell_of(p1));
    case SameFaction: case SameFactionAsPC: {
        // The game's test: stops at a rank other than -1, and "not found" reads -2, so nearly anything matches.
        const uint32_t other = fn == SameFaction ? p1 : k_player;
        const forms::Form* f = form(npc_trait(npc, k_template_factions));
        if (!f || !npc_of(other)) {
            return 0.0;
        }
        for (const forms::FactionMembership& m : forms::npc_factions(*f)) {
            if (faction_rank(other, m.faction) != -1) {
                return 1.0;
            }
        }
        return 0.0;
    }
    case SameRace: case SameRaceAsPC: {
        const uint32_t other = npc_of(fn == SameRace ? p1 : k_player);
        return is(npc && other && race_of(npc) == race_of(other));
    }
    case SameSex: case SameSexAsPC: case GetIsSex: case GetPCIsSex: {
        auto sex = [&](uint32_t n) -> int64_t {
            if (n == k_player_base) {
                return m_world.player_female ? 1 : 0;
            }
            const forms::Form* f = form(npc_trait(n, k_template_traits));
            return member_int(f ? f->get("ACBS") : nullptr, "flags") & 1;
        };
        if (fn == GetIsSex || fn == GetPCIsSex) {
            const uint32_t who = fn == GetIsSex ? npc : npc_of(k_player);
            return is(who && sex(who) == int64_t(p1));
        }
        const uint32_t other = npc_of(fn == SameSex ? p1 : k_player);
        return is(npc && other && sex(npc) == sex(other));
    }
    case GetFactionRankDifference: {
        // "Not found" reads -2 and enters the subtraction; only rank -1 is left out.
        if (!npc || !npc_of(p2)) {
            return 0.0;
        }
        const int32_t a = faction_rank(subject, p1), b = faction_rank(p2, p1);
        return a != -1 && b != -1 ? double(a - b) : 0.0;
    }
    case IsRaining: case IsSnowing: {
        const uint32_t flag = fn == IsRaining ? k_weather_rainy : k_weather_snow;
        const float t = m_world.weather_transition;
        auto byte = [&](uint32_t weather, const char* name) {
            const forms::Form* f = form(weather);
            return float(member_int(f ? f->get("DATA") : nullptr, name));
        };
        return is((weather_flag(m_world.weather, flag) && byte(m_world.weather, "precipitation_fade_in") * 0.0039176475f < t) ||
                  (weather_flag(m_world.outgoing_weather, flag) &&
                   byte(m_world.outgoing_weather, "precipitation_fade_out") * 0.0039176471f + 0.001f > t));
    }
    case GetInCell: case GetInCellParam:
        return is(p1 && cell_of(p2 ? p2 : subject) == p1);
    case GetIsClass: case GetPCIsClass: {
        const forms::Form* f = form(npc_trait(fn == GetIsClass ? npc : npc_of(k_player), k_template_traits));
        const forms::Value* v = f ? f->get("CNAM") : nullptr;
        return is(v && v->form == p1);
    }
    case GetIsRace: case GetPCIsRace: {
        const uint32_t who = fn == GetIsRace ? npc : npc_of(k_player);
        return is(who && race_of(who) == p1);
    }
    case GetInFaction: case GetPCInFaction:
        return is(faction_rank(fn == GetInFaction ? subject : k_player, p1) >= 0); // rank -1 is listed, not a member
    case GetIsID:
        return is(base_of(subject) == p1);
    case GetFactionRank:
        return npc ? double(faction_rank(subject, p1)) : -1.0;
    case GetGlobalValue: {
        const forms::Form* f = form(p1);
        if (!f || f->type() != sig("GLOB")) {
            return -1.0;
        }
        if (float live = 0.0f; m_facts && m_facts->global_value(p1, live)) {
            return live;
        }
        return double(forms::global_value(*f).value_or(0.0f));
    }
    case GetLevel: {
        const forms::Form* f = form(npc_trait(npc, k_template_stats));
        const forms::Value* acbs = f ? f->get("ACBS") : nullptr;
        int64_t level = member_int(acbs, "level");
        if (member_int(acbs, "flags") & 0x80) { // PC level mult: x 1000, of the player's level, within the limits
            const forms::Form* p = form(npc_of(k_player));
            level = int64_t(double(member_int(p ? p->get("ACBS") : nullptr, "level")) * double(level) * 0.001);
            const int64_t lo = member_int(acbs, "calc_min_level"), hi = member_int(acbs, "calc_max_level");
            level = hi ? std::min(level, hi) : level;
            level = lo ? std::max(level, lo) : level;
        }
        return double(level);
    }
    case GetIsReference:
        return is(subject == p1);
    case GetWindSpeed:
        return m_world.wind_speed;
    case GetCurrentWeatherPercent:
        return m_world.weather_transition;
    case GetIsCurrentWeather:
        return is(m_world.weather && m_world.weather == p1);
    case GetIsPlayableRace: {
        const forms::Form* race = form(race_of(npc));
        return is((member_int(race ? race->get("DATA") : nullptr, "flags") & 1) != 0);
    }
    case HasAssociationType: {
        uint32_t association = 0;
        return is(relationship(npc, npc_of(p1), &association) && association == p2);
    }
    case HasFamilyRelationship: {
        uint32_t association = 0;
        if (!relationship(npc, npc_of(p1), &association)) {
            return 0.0;
        }
        const forms::Form* f = form(association);
        const forms::Value* d = f ? f->get("DATA") : nullptr;
        return is(d && (d->integer & 1));
    }
    case HasParentRelationship: {
        const forms::Form* f = form(relationship(npc, npc_of(p1), nullptr));
        const forms::Value* d = f ? f->get("DATA") : nullptr;
        const forms::Value* parent = d ? d->member("parent") : nullptr;
        return is(parent && parent->form == npc);
    }
    case GetRelationshipRank: {
        const forms::Form* f = form(relationship(npc, npc_of(p1), nullptr));
        const forms::Value* d = f ? f->get("DATA") : nullptr;
        // RELA rank 0 Lover .. 8 Archnemesis, as the rank numbers 4 .. -4.
        return d ? double(4 - member_int(d, "rank")) : 0.0;
    }
    case IsPleasant: case IsCloudy: {
        const uint32_t flag = fn == IsPleasant ? k_weather_pleasant : k_weather_cloudy;
        const double t = m_world.weather_transition;
        return (weather_flag(m_world.weather, flag) ? t : 0.0) + (weather_flag(m_world.outgoing_weather, flag) ? 1.0 - t : 0.0);
    }
    case IsOwner: {
        const forms::Form* f = form(subject);
        const forms::Value* v = f ? f->get("XOWN") : nullptr;
        return is(v && v->form == (p1 ? p1 : npc_of(k_player)));
    }
    case IsInInterior: {
        if (subject == k_player && m_world.player_cell) {
            return is(m_world.player_interior);
        }
        const forms::Form* c = form(cell_of(subject));
        const forms::Value* d = c ? c->get("DATA") : nullptr;
        return is(d && (d->integer & 1));
    }
    case GetInWorldspace: {
        const uint32_t who = p2 ? p2 : subject;
        const uint32_t world = who == k_player && m_world.player_worldspace ? m_world.player_worldspace : worldspace_of(cell_of(who));
        const forms::Form* list = form(p1);
        if (!world || !list) {
            return 0.0;
        }
        if (list->type() == sig("FLST")) {
            const auto ids = forms::form_list(*list);
            return is(std::find(ids.begin(), ids.end(), world) != ids.end());
        }
        return is(world == p1);
    }
    case IsActor: {
        const forms::Form* f = form(subject);
        return is(subject == k_player || (f && f->type() == sig("ACHR")));
    }
    case IsEssential: case IsProtected: case IsUnique: {
        const forms::Form* f = form(npc);
        const int64_t flags = member_int(f ? f->get("ACBS") : nullptr, "flags");
        return is((flags & (fn == IsEssential ? 0x02 : fn == IsUnique ? 0x20 : 0x800)) != 0);
    }
    case GetInCurrentLoc:
        return is(p1 && location_within(location_of(subject), p1));
    case GetIsEditorLocation:
        return is(p1 && location_within(editor_location_of(subject), p1));
    case GetInCurrentLocAlias: {
        if (!m_alias_locations) {
            return 0.0;
        }
        const auto it = m_alias_locations->find(p1);
        return is(it != m_alias_locations->end() && location_within(location_of(subject), it->second));
    }
    case HasSameEditorLocAsRefAlias: {
        const auto it = m_alias_refs ? m_alias_refs->find(p1) : decltype(m_alias_refs->find(p1)){};
        if (!m_alias_refs || it == m_alias_refs->end()) {
            return 0.0;
        }
        uint32_t a = location_of(subject), b = location_of(it->second);
        if (p2) { // to the first location each has with the keyword
            while (a && !has_keyword(a, p2)) {
                const forms::Form* f = form(a);
                const forms::Value* v = f ? f->get("PNAM") : nullptr;
                a = v ? v->form : 0;
            }
            while (b && !has_keyword(b, p2)) {
                const forms::Form* f = form(b);
                const forms::Value* v = f ? f->get("PNAM") : nullptr;
                b = v ? v->form : 0;
            }
        }
        return is(a && a == b);
    }
    case HasRefType: {
        const forms::Form* f = form(subject);
        const forms::Value* list = f ? f->get("XLRT") : nullptr;
        return is(p1 && list && std::ranges::any_of(list->items, [p1](const forms::Value& v) { return v.form == p1; }));
    }
    case HasLinkedRef:
        return is(linked_ref(subject, p1) != 0);
    case IsLinkedTo:
        return is(linked_ref(subject, p2) == p1);
    case IsChild: {
        const forms::Form* race = form(race_of(npc));
        return is((member_int(race ? race->get("DATA") : nullptr, "flags") & 4) != 0);
    }
    case GetIsVoiceType: {
        const forms::Form* f = form(npc_trait(npc, k_template_traits));
        const forms::Value* v = f ? f->get("VTCK") : nullptr;
        uint32_t voice = v ? v->form : 0;
        // the player's voice comes from default objects 14 to 17, which every master leaves empty, so it's
        // the race's for its sex.
        if (npc == k_player_base) {
            const forms::Form* race = form(race_of(npc));
            const forms::Value* voices = race ? race->get("VTCK") : nullptr;
            const forms::Value* own = voices ? voices->member(m_world.player_female ? "female" : "male") : nullptr;
            voice = own ? own->form : voice;
        }
        if (!voice) {
            return 0.0;
        }
        if (voice == p1) {
            return 1.0;
        }
        const forms::Form* list = form(p1);
        if (list) {
            for (uint32_t id : forms::form_list(*list)) {
                if (id == voice) {
                    return 1.0;
                }
            }
        }
        return 0.0;
    }
    case GetOffersServicesNow: {
        // The first of the actor's factions with vendor hours around now and passing vendor conditions. Whether the
        // vendor's location holds the actor needs a world position, so it isn't tested yet.
        const forms::Form* f = form(npc_trait(npc, k_template_factions));
        if (!f) {
            return 0.0;
        }
        for (const forms::FactionMembership& m : forms::npc_factions(*f)) {
            const forms::Form* faction = form(m.faction);
            const forms::Value* hours = faction ? faction->get("VENV") : nullptr;
            const forms::Value* chest = faction ? faction->get("VENC") : nullptr;
            if (hours && chest && chest->form) {
                const int64_t start = member_int(hours, "start_hour"), end = member_int(hours, "end_hour");
                if (start < m_world.game_hour && m_world.game_hour < end) {
                    return 1.0;
                }
            }
        }
        return 0.0;
    }
    case IsInList: {
        const forms::Form* list = form(p1);
        if (!list) {
            return 0.0;
        }
        const uint32_t base = base_of(subject);
        for (uint32_t id : forms::form_list(*list)) {
            if (id == base) {
                return 1.0;
            }
        }
        return 0.0;
    }
    case GetFactionCombatReaction: {
        const forms::Form* f = form(p1);
        if (!f || !p2) {
            return 0.0;
        }
        for (const forms::Value* v : f->all("XNAM")) {
            const forms::Value* other = v->member("faction");
            if (other && other->form == p2) {
                return double(member_int(v, "group_combat_reaction"));
            }
        }
        return 0.0;
    }
    case Exists:
        return is(subject != p1);
    case HasKeyword:
        return is(p1 && (has_keyword(base_of(subject), p1) || (npc && has_keyword(race_of(npc), p1))));
    case LocationHasKeyword:
        return is(p1 && has_keyword(location_of(subject), p1));
    case GetEventData:
        return event_data(p1, p2);
    case k_is_win32:
        return 1.0;
    default:
        return 0.0;
    }
}

double Conditions::event_data(uint32_t param1, uint32_t param2) {
    const StoryEvent::Member* member = m_event ? m_event->member(uint16_t(param1 >> 16)) : nullptr;
    if (!member) {
        return 0.0;
    }
    const uint32_t id = member->value;
    switch (param1 & 0xFFFF) {
    case 0: // GetIsID
        return id == param2 ? 1.0 : 0.0;
    case 1: { // IsInList
        const forms::Form* list = form(param2);
        if (!list) {
            return 0.0;
        }
        const auto ids = forms::form_list(*list);
        return std::find(ids.begin(), ids.end(), id) != ids.end() ? 1.0 : 0.0;
    }
    case 2: // GetValue
        return double(id);
    case 3: // HasKeyword
        return has_keyword(id, param2) ? 1.0 : 0.0;
    case 4: { // GetItemValue
        const forms::Form* f = form(id);
        const auto value = f ? forms::item_value(*f) : std::nullopt;
        return double(uint32_t(value ? int32_t(*value) : -1));
    }
    default:
        return 0.0;
    }
}

bool Conditions::evaluate(const ConditionItem& item, uint32_t subject, uint32_t target) {
    uint32_t s = subject, t = target;
    switch (item.run_on) {
    case RunOn::subject:
        break;
    case RunOn::target:
        s = target;
        t = subject;
        break;
    case RunOn::reference:
        s = item.reference;
        break;
    case RunOn::linked_reference:
        s = subject ? linked_ref(subject, 0) : 0;
        t = 0;
        break;
    case RunOn::event: {
        const StoryEvent::Member* m = m_event ? m_event->member(item.member) : nullptr;
        s = m ? m->value : 0;
        t = 0;
        break;
    }
    case RunOn::combat_target: // nobody fights yet; aliases and packages have no owner here
    case RunOn::alias:
    case RunOn::package:
        s = 0;
        t = 0;
        break;
    }
    if ((item.flags & condition_swap) && s && t) {
        std::swap(s, t);
    }
    uint32_t p1 = item.param1;
    if (item.flags & condition_aliases) {
        // The parameter names an alias of the quest being filled.
        const auto it = m_alias_refs ? m_alias_refs->find(p1) : decltype(m_alias_refs->find(p1)){};
        p1 = m_alias_refs && it != m_alias_refs->end() ? it->second : 0;
    } else if (!p1 && takes_target(item.function)) {
        p1 = t;
    }
    if (!s && !runs_without_subject(item.function)) {
        return false; // the entry needs a reference: the callback isn't called, for any comparison
    }
    double v = 0.0;
    constexpr uint16_t k_get_vm_script_variable = 630, k_get_graph_variable_int = 675,
                       k_get_graph_variable_float = 447;
    if (item.function == k_get_vm_quest_variable || item.function == k_get_vm_script_variable) {
        if (m_facts && item.text) {
            m_facts->quest_variable(p1, *item.text, v);
        }
    } else if (item.function == k_get_graph_variable_int) {
        if (m_facts && item.text) {
            m_facts->graph_value(s, *item.text, v);
        }
    } else if (item.function == k_get_graph_variable_float) {
        if (m_facts && item.text) {
            m_facts->graph_real(s, *item.text, v);
        }
    } else {
        v = value(item.function, s, t, p1, item.param2);
    }
    double against = item.value;
    if (item.flags & condition_global) {
        const forms::Form* g = form(item.value_global);
        against = g ? double(forms::global_value(*g).value_or(0.0f)) : 0.0;
        if (float live = 0.0f; m_facts && m_facts->global_value(item.value_global, live)) {
            against = live;
        }
    }
    switch (item.flags >> 5) {
    case 0: return v == against;
    case 1: return v != against;
    case 2: return v > against;
    case 3: return v >= against;
    case 4: return v < against;
    case 5: return v <= against;
    default: return false;
    }
}

bool Conditions::needs_actor(const ConditionItem& c) {
    if (c.run_on != RunOn::subject || (c.flags & (condition_global | condition_swap)) || (c.flags >> 5) != 0 ||
        c.value != 1.0f) {
        return false; // "equal to 1": what isn't an actor gets 0 from these
    }
    switch (c.function) {
    case GetIsVoiceType: case GetIsRace: case GetIsClass: case GetIsSex: case HasAssociationType: case IsActor:
    case IsChild: case GetInFaction:
        return true;
    default:
        return false;
    }
}

bool Conditions::fails_for_all(std::span<const ConditionItem> list) {
    bool group = false, free = true, open = false;
    for (const ConditionItem& item : list) {
        free = free && item.run_on == RunOn::subject && !(item.flags & condition_swap) &&
               runs_without_subject(item.function) && !takes_target(item.function);
        group = group || (free && evaluate(item, k_player, 0));
        open = (item.flags & condition_or) != 0;
        if (!open) {
            if (free && !group) {
                return true;
            }
            group = false;
            free = true;
        }
    }
    return open && free && !group;
}

bool Conditions::evaluate(std::span<const ConditionItem> list, uint32_t subject, uint32_t target) {
    // Items with the OR flag join the item after them into a group; groups are ANDed. An empty list is true.
    bool group = false, open = false;
    for (const ConditionItem& item : list) {
        group = group || evaluate(item, subject, target);
        open = (item.flags & condition_or) != 0;
        if (!open) {
            if (!group) {
                return false; // a group failed: nothing after it can change the answer
            }
            group = false;
        }
    }
    return !open || group;
}

} // namespace spud::game
