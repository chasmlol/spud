// Quest text: the instance records and the tags resolved through them.

#include "game/quests.hpp"

#include "core/path.hpp"
#include "formats/forms_typed.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>

namespace spud::game {

namespace {

bool same_text(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
}

constexpr uint32_t k_player_npc = 7;
constexpr uint32_t k_player_reference = 0x14;
constexpr uint32_t k_template_base_data = 0x80; // TPLT "use base data"

// What the game shows for a form: its name, or for an NPC what the modifier asks of it.
struct NpcText {
    const char* modifier;
    const char *male, *female;
};
constexpr NpcText k_npc_text[] = {
    {"Pronoun", "sPronounMale", "sPronounFemale"},
    {"PronounObj", "sPronounObjMale", "sPronounObjFemale"},
    {"PronounPos", "sPronounPosMale", "sPronounPosFemale"},
    {"PronounPosObj", "sPronounPosObjMale", "sPronounPosObjFemale"},
    {"PronounRef", "sPronounRefMale", "sPronounRefFemale"},
    {"PronounInt", "sPronounIntMale", "sPronounIntFemale"},
    {"Gender", "sGenderMale", "sGenderFemale"},
};

constexpr std::array<int, 12> k_month_days = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
constexpr const char* k_months[12] = {"sMonthJanuary", "sMonthFebruary", "sMonthMarch", "sMonthApril", "sMonthMay", "sMonthJune",
                                      "sMonthJuly", "sMonthAugust", "sMonthSeptember", "sMonthOctober", "sMonthNovember", "sMonthDecember"};
constexpr const char* k_days[7] = {"sDaySunday", "sDayMonday", "sDayTuesday", "sDayWednesday", "sDayThursday", "sDayFriday", "sDaySaturday"};

} // namespace

std::string QuestWorld::text_setting(std::string_view name) { return live.text_setting ? live.text_setting(name) : std::string(); }

uint32_t QuestSystem::stored_form(const Alias& a, uint32_t ref) const {
    if (a.location) {
        return ref;
    }
    // the base form, an NPC taken back through templates that give it its base data.
    const forms::FormEntry* e = m_db.find(ref);
    if (ref == k_player_reference) {
        return k_player_npc;
    }
    if (!e) {
        return 0;
    }
    const auto name = m_db.field(*e, forms::sig("NAME"));
    uint32_t base = name ? name->as<uint32_t>() : ref;
    for (int depth = 0; depth < 16; ++depth) {
        const forms::FormEntry* b = m_db.find(base);
        if (!b || b->type != forms::sig("NPC_")) {
            return b && b->type == forms::sig("LVLN") ? 0 : base;
        }
        const forms::Form npc(m_db, *b);
        const forms::Value* acbs = npc.get("ACBS");
        const forms::Value* flags = acbs ? acbs->path("template_flags") : nullptr;
        const forms::Value* parent = npc.get("TPLT");
        if (!parent || !parent->form || !flags || !(flags->integer & k_template_base_data)) {
            return base;
        }
        base = parent->form;
    }
    return base;
}

void QuestSystem::create_instance_text(Quest& q) {
    const bool stores = std::ranges::any_of(q.aliases, [](const Alias& a) { return (a.flags & alias_stores_text) != 0; });
    const bool logs = std::ranges::any_of(q.stages, [](const Stage& s) {
        return std::ranges::any_of(s.items, [](const StageItem& i) { return !i.log.empty(); });
    });
    if (!stores && q.text_globals.empty() && !logs) {
        return;
    }
    ++q.instance;
    InstanceText record;
    record.id = q.instance;
    for (const Alias& a : q.aliases) {
        if (a.flags & alias_stores_text) {
            store_alias_text(q, record, a);
        }
    }
    for (const uint32_t global : q.text_globals) {
        record.globals.emplace_back(global, m_world.live.global_value ? m_world.live.global_value(global) : 0.0f);
    }
    q.instances.push_back(std::move(record));
}

void QuestSystem::store_alias_text(const Quest& q, InstanceText& record, const Alias& a) {
    std::erase_if(record.forms, [&](const auto& entry) { return entry.first == a.id; });
    const auto& filled = a.location ? q.filled_locations : q.filled_refs;
    if (const auto it = filled.find(a.id); it != filled.end() && it->second) {
        if (const uint32_t form = stored_form(a, it->second)) {
            record.forms.emplace_back(a.id, form);
        }
    }
}

void QuestSystem::update_alias_text(Quest& q, const Alias& a) {
    if (a.flags & alias_stores_text) {
        if (InstanceText* record = q.instance_text(q.instance)) {
            store_alias_text(q, *record, a);
        }
    }
}

// then 1.9 step 5: a record not kept goes at stop.
void QuestSystem::release_instance_text(Quest& q) {
    if (!(q.flags & quest_keep_instance)) {
        std::erase_if(q.instances, [&](const InstanceText& r) { return r.id == q.instance; });
    }
}

const std::string& QuestSystem::log_text(const Quest& q, uint32_t instance) const {
    static const std::string none;
    const InstanceText* record = const_cast<Quest&>(q).instance_text(instance);
    return record ? record->log : none;
}

std::string QuestSystem::form_text(uint32_t form, std::string_view modifier) {
    const forms::FormEntry* e = m_db.find(form);
    if (!e) {
        return {};
    }
    const forms::Form f(m_db, *e);
    if (e->type != forms::sig("NPC_")) {
        return std::string(f.name());
    }
    const forms::Value* acbs = f.get("ACBS");
    const forms::Value* flags = acbs ? acbs->path("flags") : nullptr;
    const bool female = flags && (flags->integer & 1);
    for (const NpcText& t : k_npc_text) {
        if (same_text(modifier, t.modifier)) {
            return m_world.text_setting(female ? t.female : t.male);
        }
    }
    if (same_text(modifier, "Race")) {
        const forms::Value* race = f.get("RNAM");
        const forms::FormEntry* r = race ? m_db.find(race->form) : nullptr;
        return r ? std::string(forms::Form(m_db, *r).name()) : std::string();
    }
    if (same_text(modifier, "ShortName")) {
        if (const forms::Value* s = f.get("SHRT"); s && !s->text.empty()) {
            return s->text;
        }
    }
    return std::string(f.name());
}

uint32_t QuestSystem::alias_form(const Quest& q, const InstanceText* record, std::string_view name, const Alias** out) const {
    if (same_text(name, "Player")) {
        return k_player_npc;
    }
    if (!record) {
        return 0;
    }
    const auto a = std::ranges::find_if(q.aliases, [&](const Alias& x) { return same_text(x.name, name); });
    if (a == q.aliases.end()) {
        return 0;
    }
    if (out) {
        *out = &*a;
    }
    const auto it = std::ranges::find_if(record->forms, [&](const auto& e) { return e.first == a->id; });
    return it == record->forms.end() ? 0 : it->second;
}

std::optional<std::string> QuestSystem::alias_tag(const Quest& q, const InstanceText* record, std::string_view value,
                                                  const std::string* modifier) {
    const Alias* alias = nullptr;
    const uint32_t form = alias_form(q, record, value, &alias);
    // A reference alias's display name message, its tags resolved in turn.
    if (!modifier && alias && !alias->location && alias->display_name) {
        if (const forms::FormEntry* m = m_db.find(alias->display_name)) {
            std::string text(forms::Form(m_db, *m).name());
            if (const size_t at = text.find("<BaseName>"); at != text.npos && form) {
                text.replace(at, 10, form_text(form, {}));
            }
            return replace_tags(q, record, std::move(text));
        }
    }
    if (!form) {
        return std::nullopt;
    }
    return form_text(form, modifier ? *modifier : std::string());
}

std::optional<std::string> QuestSystem::relationship_tag(const Quest& q, const InstanceText* record, std::string_view first,
                                                         std::string_view second) {
    const uint32_t a = alias_form(q, record, first, nullptr), b = alias_form(q, record, second, nullptr);
    if (!a || !b) {
        return std::nullopt;
    }
    for (const forms::FormEntry* e : m_db.of_type(forms::sig("RELA"))) {
        const forms::Form r(m_db, *e);
        const forms::Value* data = r.get("DATA");
        const forms::Value* parent = data ? data->path("parent") : nullptr;
        const forms::Value* child = data ? data->path("child") : nullptr;
        const forms::Value* type = data ? data->path("association_type") : nullptr;
        if (!parent || !child || !type || !((parent->form == a && child->form == b) || (parent->form == b && child->form == a))) {
            continue;
        }
        const forms::FormEntry* t = m_db.find(type->form);
        if (!t) {
            return std::nullopt;
        }
        const forms::Form association(m_db, *t);
        const forms::FormEntry* who = m_db.find(a);
        const forms::Form npc(m_db, *who);
        const forms::Value* acbs = npc.get("ACBS");
        const forms::Value* flags = acbs ? acbs->path("flags") : nullptr;
        const bool female = flags && (flags->integer & 1);
        const bool is_parent = parent->form == a;
        const forms::Value* male_title = association.get(is_parent ? "MPRT" : "MCHT");
        const forms::Value* female_title = association.get(is_parent ? "FPRT" : "FCHT");
        if (female && female_title && !female_title->text.empty()) {
            return female_title->text;
        }
        return male_title ? male_title->text : std::string();
    }
    return std::nullopt;
}

std::optional<std::string> QuestSystem::global_tag(const InstanceText* record, std::string_view editor_id, const std::string* modifier) {
    const forms::FormEntry* global = nullptr;
    for (const forms::FormEntry* e : m_db.of_type(forms::sig("GLOB"))) {
        if (same_text(m_db.editor_id(*e), editor_id)) {
            global = e;
            break;
        }
    }
    if (!global || !record) {
        return std::nullopt;
    }
    const auto it = std::ranges::find_if(record->globals, [&](const auto& g) { return g.first == global->form_id; });
    if (it == record->globals.end()) {
        return std::nullopt;
    }
    const double value = it->second;
    if (!modifier) {
        const forms::Form f(m_db, *global);
        const forms::Value* type = f.get("FNAM");
        return type && type->integer == 'f' ? std::format("{:.2f}", value) : std::format("{:.0f}", value);
    }
    if (same_text(*modifier, "Timer")) {
        return value <= 0.0 ? std::format("0 {}", m_world.text_setting("sTimeMinutes"))
               : value < 1.0 ? std::format("{:.0f} {}", value * 60.0, m_world.text_setting("sTimeMinutes"))
                             : std::format("{:.1f} {}", value, m_world.text_setting("sTimeHours"));
    }
    const double hours = (value - std::floor(value)) * 24.0;
    const int hour = int(hours);
    if (same_text(*modifier, "Hour")) {
        return std::format("{}", hour);
    }
    if (same_text(*modifier, "Hour12")) {
        return std::format("{}", hour > 12 ? hour - 12 : hour == 0 ? 12 : hour);
    }
    if (same_text(*modifier, "Minutes")) {
        return std::format("{:02}", int((hours - hour) * 60.0));
    }
    if (same_text(*modifier, "Meridiem")) {
        return m_world.text_setting(hour >= 12 ? "sTimePM" : "sTimeAM");
    }
    if (same_text(*modifier, "TimeSpan")) {
        // The table: the first range to hold the hour, one ending at or before its start running past midnight.
        for (const std::string_view span : {"Sunrise", "Morning", "Afternoon", "Evening", "Sunset", "Night", "Midnight"}) {
            const float start = m_world.number_setting(std::format("fTimeSpan{}Start", span));
            const float end = m_world.number_setting(std::format("fTimeSpan{}End", span));
            const bool in_span = end > start ? hours >= start && hours < end : hours >= start || hours < end;
            if (in_span) {
                return m_world.text_setting(std::format("sTimeSpan{}", span));
            }
        }
        return {};
    }
    // The date: the calendar now, moved by the whole days to the value.
    const std::array<float, 4> now = m_world.live.calendar ? m_world.live.calendar() : std::array<float, 4>{201, 7, 17, 1};
    int year = int(now[0]), month = int(now[1]), day = int(now[2]);
    int shift = int(std::floor(value)) - int(std::floor(now[3]));
    for (; shift > 0; --shift) {
        if (++day > k_month_days[size_t(month)]) {
            day = 1;
            if (++month == 12) {
                month = 0;
                ++year;
            }
        }
    }
    for (; shift < 0; ++shift) {
        if (--day < 1) {
            month = month == 0 ? 11 : month - 1;
            year -= month == 11 ? 1 : 0;
            day = k_month_days[size_t(month)];
        }
    }
    if (same_text(*modifier, "Day")) {
        return std::format("{}", day);
    }
    if (same_text(*modifier, "Month")) {
        return std::format("{}", month + 1);
    }
    if (same_text(*modifier, "Year")) {
        return std::format("{}", year);
    }
    if (same_text(*modifier, "MonthWord")) {
        return m_world.text_setting(k_months[month]);
    }
    if (same_text(*modifier, "Weekday")) {
        return m_world.text_setting(k_days[size_t(int(std::floor(value)) % 7)]);
    }
    return std::string(); // an unknown modifier writes nothing
}

// The scanner of and the quest pass.
std::string QuestSystem::replace_tags(const Quest& q, const InstanceText* record, std::string text) {
    if (text.find('<') == text.npos) {
        return text;
    }
    std::string out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t open = text.find('<', at);
        if (open == text.npos) {
            out.append(text, at, text.npos);
            break;
        }
        out.append(text, at, open - at);
        const size_t close = text.find('>', open);
        if (close == text.npos) {
            out.append(text, open, text.npos);
            break;
        }
        const std::string_view tag(text.data() + open + 1, close - open - 1);
        const size_t dot = tag.find('.'), equals = tag.find('=');
        const size_t first = std::min(dot, equals);
        const std::string_view name = tag.substr(0, first);
        std::string modifier;
        std::string_view value;
        bool has_modifier = false, has_value = false, cap = false;
        if (equals != tag.npos) {
            has_value = true;
            value = tag.substr(equals + 1);
            if (dot != tag.npos && dot > equals) {
                value = tag.substr(equals + 1, dot - equals - 1);
            }
        }
        if (dot != tag.npos) {
            has_modifier = true;
            modifier = tag.substr(dot + 1, equals != tag.npos && equals > dot ? equals - dot - 1 : tag.npos);
            if (const size_t c = modifier.find("Cap"); c != modifier.npos) {
                cap = true;
                modifier.resize(c);
            }
        }
        std::optional<std::string> result;
        bool known = false;
        if (has_value && same_text(name, "Alias")) {
            known = true;
            result = alias_tag(q, record, value, has_modifier ? &modifier : nullptr);
        } else if (has_value && same_text(name, "Global")) {
            known = true;
            result = global_tag(record, value, has_modifier ? &modifier : nullptr);
        } else if (has_value && has_modifier && same_text(name, "Relationship")) {
            known = true;
            result = relationship_tag(q, record, modifier, value);
        }
        if (!known) {
            // Left as it stands; an unknown tag with Cap cuts the text there, as the game's restore misses the C.
            out += '<';
            at = open + 1;
            continue;
        }
        std::string shown = result ? std::move(*result) : m_world.text_setting("sInvalidTagString");
        if (cap && !shown.empty()) {
            shown[0] = char(std::toupper(uint8_t(shown[0])));
        }
        out += shown;
        at = close + 1;
    }
    return out;
}

std::string QuestSystem::quest_text(const Quest& q, uint32_t instance, std::string text) {
    return replace_tags(q, const_cast<Quest&>(q).instance_text(instance), std::move(text));
}

void QuestSystem::update_player_objective(const Quest& q, const Objective& o) {
    const auto it = std::ranges::find_if(m_player_objectives, [&](const PlayerObjective& p) {
        return p.quest == q.form && p.index == o.index && p.instance == q.instance;
    });
    if (it != m_player_objectives.end()) {
        it->state = o.state;
    } else {
        m_player_objectives.push_back({q.form, o.index, q.instance, o.state});
        if (m_player_objectives.size() > 3000) { // the entries of the oldest quest go
            const uint32_t oldest = m_player_objectives.front().quest;
            std::erase_if(m_player_objectives, [&](const PlayerObjective& p) { return p.quest == oldest; });
        }
    }
}

} // namespace spud::game
