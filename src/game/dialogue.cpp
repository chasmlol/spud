#include "game/dialogue.hpp"

#include "core/path.hpp"
#include "script/natives.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace spud::game {

namespace {

constexpr uint32_t k_player = 0x14;
// Skyrim.esm's values, which override the image (0.3): read from the record when the game settings are at hand.
constexpr float k_speech_mult = 0.15f, k_padding = 0.5f;

uint32_t member_u32(const forms::Value& v, std::string_view name) {
    const forms::Value* m = v.member(name);
    return !m ? 0 : m->kind == forms::Value::Kind::form ? m->form : static_cast<uint32_t>(m->integer);
}

// The subtype table in index order.
constexpr std::string_view k_subtype_codes =
    "CUSTPFGTRUMOFVDLINTIFLATBRIBASKGGIFFASKFFAVOSHREFOLLFRJTSCENSHOWAGREREFUFEXTMREFFMLXFMXLFMATFMRTFMNTFMDR"
    "ATCKPOATBASHHIT_FLEEBLEDAVTHDETHGRSTBLOCTAUTALKLSTEAYIELACYIPICCASSAMURDASNCMUNCPICNSTFNTRANTRESWTCR"
    "VPSSVPSLVPESVPELALILLOILNOTAALTCNOTCALTNCOTNCOLOLOTNLOTCDFDASERUREPATRAVTRAIBAEXREEXRECHRCEXTREX"
    "OBCONOTITITGGBYEHELOSWMWFIWEZKEYJUMPKNOODEOBSTOFLOOBPICTPURSIDATPCPSPCSSPCSHIDLEBREAENBZEXBZACACPIRN"
    "OUTBGRNTLWBS";
constexpr uint16_t k_subtype_unknown = 103;
static_assert(k_subtype_codes.size() == k_subtype_unknown * 4);

uint8_t subtype_category(uint16_t subtype) {
    return subtype < 3 ? 0 : subtype < 14 ? 1 : subtype < 15 ? 2 : subtype < 26 ? 4 : subtype < 55 ? 3 : subtype < 66 ? 5
           : subtype < 75 ? 6 : 7;
}

} // namespace

Dialogue::Dialogue(QuestSystem& quests, DialogueOutput& output) : m_quests(quests), m_output(output) {
    load_records();
    bind_natives();
}

void Dialogue::load_records() {
    const forms::Database& db = m_quests.db();
    for (const forms::FormEntry* e : db.of_type(forms::sig("DLBR"))) {
        Branch b;
        b.form = e->form_id;
        const forms::Form form(db, *e);
        for (const forms::DecodedField& f : form.fields()) {
            switch (f.sig) {
            case forms::sig("DNAM"): b.flags = static_cast<uint32_t>(f.value.integer); break;
            case forms::sig("QNAM"): b.quest = f.value.form; break;
            case forms::sig("SNAM"): b.start = f.value.form; break;
            case forms::sig("TNAM"): b.type = static_cast<uint32_t>(f.value.integer); break;
            default: break;
            }
        }
        m_branches.emplace(b.form, b);
    }
    for (const forms::FormEntry* e : db.of_type(forms::sig("DIAL"))) {
        auto t = std::make_unique<Topic>();
        t->form = e->form_id;
        uint32_t tag = 0;
        const forms::Form form(db, *e);
        for (const forms::DecodedField& f : form.fields()) {
            const forms::Value& v = f.value;
            switch (f.sig) {
            case forms::sig("EDID"): t->editor_id = v.text; break;
            case forms::sig("FULL"): t->text = v.text; break;
            case forms::sig("PNAM"): {
                const float p = static_cast<float>(v.real);
                t->priority = p > 100.0f ? 255 : p < 1.0f ? 0 : static_cast<uint8_t>(p);
                break;
            }
            case forms::sig("BNAM"): t->branch = v.form; break;
            case forms::sig("QNAM"): t->quest = v.form; break;
            case forms::sig("DATA"):
                t->flags = static_cast<uint8_t>(member_u32(v, "do_all_before_repeating"));
                t->category = static_cast<uint8_t>(member_u32(v, "category"));
                t->subtype = static_cast<uint16_t>(member_u32(v, "subtype"));
                break;
            case forms::sig("SNAM"): tag = uint32_t(v.integer); break;
            default: break;
            }
        }
        // The number in DATA changed between versions of the form (HIT_ is 23, 28 and 29 in the masters, and
        // Skyrim.esm's 95 is GRNT, not BREA): the game takes subtype and category from SNAM.
        if (tag) {
            const char name[4] = {char(tag), char(tag >> 8), char(tag >> 16), char(tag >> 24)};
            t->subtype = k_subtype_unknown;
            for (uint16_t i = 0; i < k_subtype_unknown; ++i) {
                if (k_subtype_codes.substr(size_t(i) * 4, 4) == std::string_view(name, 4)) {
                    t->subtype = i;
                    t->category = subtype_category(i);
                    break;
                }
            }
        }
        // A topic's BNAM sets the branch's starting topic if it has none.
        if (auto b = m_branches.find(t->branch); b != m_branches.end() && !b->second.start) {
            b->second.start = t->form;
        }
        m_quest_topics[t->quest].push_back(t.get());
        m_topics.emplace(t->form, std::move(t));
    }
    for (auto& [form, b] : m_branches) {
        m_quest_branches[b.quest].push_back(&b);
    }
    for (const forms::FormEntry* e : db.of_type(forms::sig("GMST"))) {
        if (db.editor_id(*e) == "sTopicSubtypeTextPlayerDialogueRumors") {
            const forms::Form setting(db, *e);
            if (const forms::Value* v = setting.get("DATA")) {
                m_rumors_text = v->text;
            }
        }
    }
    // Within a quest the game's order comes from a pointer-keyed map (2.4). Rows are pushed to the front, so going
    // downwards leaves the lowest form first, which is how Hulda's list reads.
    for (auto& [quest, list] : m_quest_branches) {
        std::ranges::sort(list, std::greater<>{}, &Branch::form);
    }
    std::vector<const forms::FormEntry*> infos = db.of_type(forms::sig("INFO"));
    std::ranges::stable_sort(infos, {}, [](const forms::FormEntry* e) { return std::pair{e->plugin, e->file_order}; });
    for (const forms::FormEntry* e : infos) {
        if (auto t = m_topics.find(e->parent); t != m_topics.end()) {
            t->second->info_forms.push_back(e->form_id);
        }
    }
}

Topic* Dialogue::topic(uint32_t form) {
    auto it = m_topics.find(form);
    return it == m_topics.end() ? nullptr : it->second.get();
}

Info* Dialogue::load_info(uint32_t form_id, Topic& t) {
    if (auto it = m_infos.find(form_id); it != m_infos.end()) {
        return it->second.get();
    }
    const forms::Database& db = m_quests.db();
    const auto* e = db.find(form_id);
    if (!e) {
        return nullptr;
    }
    auto i = std::make_unique<Info>();
    i->form = form_id;
    i->topic = &t;
    Response* r = nullptr;
    const forms::Form form(db, *e);
    for (const forms::DecodedField& f : form.fields()) {
        const forms::Value& v = f.value;
        switch (f.sig) {
        case forms::sig("ENAM"):
            i->flags = member_u32(v, "flags");
            i->reset = member_u32(v, "reset_hours");
            break;
        case forms::sig("DATA"): i->flags = member_u32(v, "flags"); break;
        case forms::sig("PNAM"): i->previous = v.form; break;
        case forms::sig("DNAM"): i->shared = v.form; break;
        case forms::sig("ANAM"): i->speaker = v.form; break;
        case forms::sig("TWAT"): i->walk_away = v.form; break;
        case forms::sig("TCLT"): i->links.push_back(v.form); break;
        case forms::sig("RNAM"): i->prompt = v.text; break;
        case forms::sig("CTDA"): i->conditions.push_back(condition_item(v)); break;
        case forms::sig("TRDT"):
            i->responses.emplace_back();
            r = &i->responses.back();
            r->emotion = member_u32(v, "emotion_type");
            r->emotion_value = member_u32(v, "emotion_value");
            r->number = member_u32(v, "response_number");
            r->sound = member_u32(v, "sound");
            r->use_emotion = member_u32(v, "use_emotion_animation") != 0;
            break;
        case forms::sig("NAM1"):
            if (r) {
                r->text = v.text;
            }
            break;
        case forms::sig("SNAM"):
            if (r) {
                r->speaker_idle = v.form;
            }
            break;
        case forms::sig("LNAM"):
            if (r) {
                r->listener_idle = v.form;
            }
            break;
        default: break;
        }
    }
    Info* raw = i.get();
    m_infos.emplace(form_id, std::move(i));
    return raw;
}

Info* Dialogue::info(uint32_t form) {
    auto it = m_infos.find(form);
    return it == m_infos.end() ? nullptr : it->second.get();
}

// 0.1: the game, in file order.
void Dialogue::load_infos(Topic& t) {
    if (t.loaded) {
        return;
    }
    t.loaded = true;
    for (uint32_t form : t.info_forms) {
        Info* i = load_info(form, t);
        if (!i) {
            continue;
        }
        if (i->previous == 0) {
            t.infos.insert(t.infos.begin(), i);
            continue;
        }
        auto after = std::find_if(t.infos.rbegin(), t.infos.rend(), [i](const Info* x) { return x->form == i->previous; });
        if (after == t.infos.rend()) {
            t.infos.insert(t.infos.begin(), i);
        } else {
            t.infos.insert(after.base(), i);
        }
    }
}

bool Dialogue::quest_open(const Quest* q) const {
    return q && q->running_bit() && !q->wait_bit();
}

// 2.6: the player's table of when reset-hour INFOs ran.
bool Dialogue::remembered(const Info& i) const {
    if (i.reset == 0) {
        return false;
    }
    auto it = m_reset_stamps.find(i.form);
    if (it == m_reset_stamps.end()) {
        return false;
    }
    const double hours = i.reset * 24.0 / 65535.0;
    return (m_days - it->second) * 24.0 < hours;
}

bool Dialogue::info_passes(const Quest& q, const Info& i, uint32_t speaker, uint32_t target) {
    if (remembered(i) || ((i.flags & info_say_once) && i.said)) {
        return false;
    }
    Conditions& conditions = m_quests.conditions();
    const Conditions::QuestScope scope(conditions, q.form);
    return conditions.evaluate(q.dialogue_conditions, speaker, target) && conditions.evaluate(i.conditions, speaker, target);
}

// 3.1.
Info* Dialogue::pick_info(Topic& t, uint32_t speaker, uint32_t target, bool walking, const Topic* previous) {
    Quest* q = m_quests.quest(t.quest);
    const Conditions::QuestScope scope(m_quests.conditions(), t.quest);
    if (!quest_open(q) || !m_quests.conditions().evaluate(q->dialogue_conditions, speaker, target)) {
        return nullptr;
    }
    load_infos(t);
    const bool chain = walking || (previous && previous->category == category_topic);
    std::vector<Info*> candidates, used;
    for (Info* i : t.infos) {
        if (!info_passes(*q, *i, speaker, target)) {
            continue;
        }
        if (chain && std::ranges::find(m_history, &t) != m_history.end()) {
            continue;
        }
        const bool end = (i->flags & info_random_end) != 0;
        if (!(i->flags & info_random)) {
            if (candidates.empty()) {
                return i;
            }
            break;
        }
        ((t.flags & 1) && (i->flags & info_used_this_cycle) ? used : candidates).push_back(i);
        if (end) {
            break;
        }
    }
    if ((t.flags & 1) && candidates.empty()) {
        for (Info* i : used) {
            i->flags &= ~info_used_this_cycle;
        }
        candidates = std::move(used);
    }
    if (candidates.empty()) {
        return nullptr;
    }
    if (candidates.size() == 1) {
        return candidates[0];
    }
    return candidates[std::uniform_int_distribution<size_t>(0, candidates.size() - 1)(m_random)];
}

// 3.4.
Info* Dialogue::pick_by_subtype(uint8_t category, uint16_t subtype, uint32_t speaker, uint32_t target) {
    std::vector<Info*> pool;
    for (Quest* q : m_quests.running()) {
        const Conditions::QuestScope scope(m_quests.conditions(), q->form);
        if (!quest_open(q) || !m_quests.conditions().evaluate(q->dialogue_conditions, speaker, target)) {
            continue;
        }
        auto list = m_quest_topics.find(q->form);
        if (list == m_quest_topics.end()) {
            continue;
        }
        for (Topic* t : list->second) {
            if (t->category != category || t->subtype != subtype) {
                continue;
            }
            load_infos(*t);
            for (Info* i : t->infos) {
                if (!info_passes(*q, *i, speaker, target)) {
                    continue;
                }
                const bool random = (i->flags & info_random) != 0, end = (i->flags & info_random_end) != 0;
                if (pool.empty() || random || end) {
                    pool.push_back(i);
                }
                if (!random || end) {
                    return pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(m_random)];
                }
            }
        }
    }
    return pool.empty() ? nullptr : pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(m_random)];
}

// 1.5.
Info* Dialogue::pick_greeting(uint32_t speaker) {
    Branch* found = nullptr;
    if (m_forced_branch) {
        if (auto b = m_branches.find(m_forced_branch); b != m_branches.end()) {
            found = &b->second;
        }
    }
    Info* picked = nullptr;
    if (!found) {
        Branch* fallback = nullptr;
        Info* fallback_info = nullptr;
        for (Quest* q : m_quests.running()) {
            auto list = m_quest_branches.find(q->form);
            if (list == m_quest_branches.end()) {
                continue;
            }
            for (Branch* b : list->second) {
                if (!(b->flags & branch_blocking)) {
                    continue;
                }
                Topic* t = topic(b->start);
                Info* i = t ? pick_info(*t, speaker, k_player) : nullptr;
                if (!i) {
                    continue;
                }
                if (b->type == 0) {
                    return i;
                }
                if (!fallback) {
                    fallback = b;
                    fallback_info = i;
                }
            }
        }
        if (fallback) {
            return fallback_info;
        }
    } else if (Topic* t = topic(found->start)) {
        picked = pick_info(*t, speaker, k_player);
    }
    return picked ? picked : pick_by_subtype(category_misc, k_subtype_hello, speaker, k_player);
}

// 3.5.
std::string Dialogue::voice_path(const Info& info, const Response& response, uint32_t speaker) {
    const std::string voice = m_output.voice_type(info.speaker ? info.speaker : speaker);
    if (voice.empty()) {
        return {};
    }
    const Info* source = &info;
    if (info.shared) {
        if (const Info* s = this->info(info.shared)) {
            source = s;
        } else if (const auto* e = m_quests.db().find(info.shared)) {
            if (Topic* owner = topic(e->parent)) {
                source = load_info(info.shared, *owner);
            }
        }
    }
    const Topic& t = *source->topic;
    const Quest* q = m_quests.quest(t.quest);
    std::string quest_id = q ? q->editor_id : std::string(), topic_id = t.editor_id;
    if (quest_id.size() + topic_id.size() > 25) {
        if (quest_id.size() <= 10) {
            topic_id.resize(25 - quest_id.size());
        } else {
            quest_id.resize(10);
            topic_id.resize(std::min<size_t>(15, topic_id.size()));
        }
    }
    const forms::FormEntry* entry = m_quests.db().find(source->form);
    if (!entry) {
        return {};
    }
    const auto sources = m_quests.db().sources(*entry);
    const size_t plugin_index = sources.empty() ? entry->plugin : sources.front();
    const std::string_view plugin = m_quests.db().plugin_name(plugin_index);
    const bool light = plugin.ends_with(".esl");
    const uint32_t id = source->form & (light ? 0xFFFu : 0xFFFFFFu);
    const std::string name = response.number == 0 ? std::string("New Response")
                                                  : std::format("{}_{}_{:08X}_{}", quest_id, topic_id, id, response.number);
    return std::format("sound/voice/{}/{}/{}", path::to_lower(plugin), path::to_lower(voice), path::to_lower(name));
}

// 3.6.
float Dialogue::silent_seconds(const Response& response) const {
    return static_cast<float>(response.text.size()) * k_speech_mult + k_padding;
}

const Response* Dialogue::response(const Info& i, size_t index) {
    const Info* source = &i;
    if (i.shared) {
        if (const auto* e = m_quests.db().find(i.shared)) {
            if (Topic* owner = topic(e->parent)) {
                source = load_info(i.shared, *owner);
            }
        }
    }
    return source && index < source->responses.size() ? &source->responses[index] : nullptr;
}

// ---- the topic list (2) ---------------------------------------------------------------------------------------------

void Dialogue::collect_branches() {
    m_top_level.clear();
    m_blocking.clear();
    for (Quest* q : m_quests.running()) {
        auto list = m_quest_branches.find(q->form);
        if (list == m_quest_branches.end()) {
            continue;
        }
        for (Branch* b : list->second) {
            if (b->type > 1) {
                continue;
            }
            if (b->flags & branch_top_level) {
                m_top_level.push_back(b);
            } else if (b->flags & branch_blocking) {
                m_blocking.push_back(b);
            }
        }
    }
}

void Dialogue::add_topic(Topic& t, const Topic* previous, bool links) {
    Info* i = pick_info(t, m_speaker, k_player, false, previous);
    if (!i) {
        return;
    }
    if (auto b = m_branches.find(t.branch); b != m_branches.end() && (b->second.flags & branch_blocking)) {
        m_blocking_found = true;
    }
    if (!response(*i, 0)) {
        return;
    }
    // A topic with no text of its own shows its subtype's.
    std::string row = i->prompt.empty() ? t.text : i->prompt;
    if (row.empty() && t.subtype == k_subtype_rumors) {
        row = m_rumors_text;
    }
    row = replace_tags(row, t);
    Entry e{&t, i, std::move(row), false};
    if (links || t.category == category_favor) {
        m_list.push_back(std::move(e));
    } else {
        m_list.insert(m_list.begin(), std::move(e));
    }
}

// MenuTopicEntry: the topic's quest, its current instance; the speaker's bribe and crime tags.
std::string Dialogue::replace_tags(std::string text, const Topic& t) {
    if (text.find('<') == text.npos) {
        return text;
    }
    for (const char* tag : {"BribeCost", "CrimeGold", "CrimeGoldViolent", "CrimeGoldNonviolent"}) {
        const std::string pattern = std::format("<{}>", tag);
        const int value = text.find(pattern) != text.npos ? m_output.prompt_tag(tag) : -1;
        for (size_t at; value >= 0 && (at = text.find(pattern)) != text.npos;) {
            text.replace(at, pattern.size(), std::to_string(value));
        }
    }
    const Quest* q = m_quests.quest(t.quest);
    return q ? m_quests.quest_text(*q, q->instance, std::move(text)) : text;
}

// 2.4: highest priority first; ties keep insertion order up to nine rows.
void Dialogue::sort_rows() {
    std::ranges::stable_sort(m_list, std::greater<>{}, [](const Entry& e) { return e.topic->priority; });
}

// 2.1.
void Dialogue::build_list() {
    const Entry* source = m_current ? &*m_current : nullptr;
    m_link_source = source && !source->info->links.empty() ? source->info : nullptr;
    const Topic* previous = source ? source->topic : nullptr;
    Info* links = m_link_source;
    m_list.clear();
    m_blocking_found = false;
    m_rumors = false;
    if (links) {
        for (uint32_t form : links->links) {
            if (Topic* t = topic(form)) {
                add_topic(*t, previous, true);
                if (!m_list.empty() && m_list.back().topic->form == links->walk_away && (m_list.back().info->flags & info_walk_away_hidden)) {
                    m_list.back().hidden = true;
                }
            }
        }
        return;
    }
    if (m_forced_branch) {
        if (auto b = m_branches.find(m_forced_branch); b != m_branches.end()) {
            if (Topic* t = topic(b->second.start)) {
                add_topic(*t, nullptr, false);
            }
        }
        return;
    }
    auto add_all = [this](const std::vector<Branch*>& list) {
        for (const Branch* b : list) {
            Topic* t = topic(b->start);
            if (!t) {
                continue;
            }
            if (t->subtype == k_subtype_rumors) {
                if (m_rumors) {
                    continue;
                }
                const size_t before = m_list.size();
                add_topic(*t, nullptr, false);
                m_rumors = m_list.size() > before;
            } else {
                add_topic(*t, nullptr, false);
            }
        }
    };
    add_all(m_blocking);
    if (m_list.empty()) {
        add_all(m_top_level);
    }
    if (m_list.size() > 1) {
        sort_rows();
    }
}

void Dialogue::send_list(bool opening) {
    if (!opening && (m_goodbye || m_list.empty())) {
        close();
        return;
    }
    m_shown.clear();
    for (size_t i = 0; i < m_list.size(); ++i) {
        const Entry& e = m_list[i];
        if (!e.hidden) {
            m_shown.push_back({e.text, !e.info->said, static_cast<int>(i)});
        }
    }
    if (m_shown.empty()) {
        if (!opening) {
            close();
        }
        return;
    }
    m_output.show_topics(m_shown, !opening);
    if (!opening) {
        m_output.voice_ready();
    }
}

// 4.6.
void Dialogue::run_fragment(Info& i, oss::FragmentKind kind, uint32_t speaker, std::function<void()> done) {
    if (!(i.flags & info_refusal)) {
        i.said = true;
    }
    if (i.reset) {
        m_reset_stamps[i.form] = m_days;
    }
    const oss::Scripts& scripts = m_quests.scripts();
    const auto& list = scripts.fragments;
    auto it = std::ranges::lower_bound(list, i.form, {}, &oss::Fragment::form_id);
    for (; it != list.end() && it->form_id == i.form; ++it) {
        if (it->kind != kind) {
            continue;
        }
        script::VirtualMachine& vm = m_quests.vm();
        vm.bind_scripts(i.form);
        script::Class* type = vm.find_class(scripts.string(it->script));
        script::Object* object = type ? vm.object_for(script::form_handle(i.form), type) : nullptr;
        if (object) {
            std::vector<script::Value> args{vm.form_value(speaker, "ObjectReference")};
            vm.call_method(object, scripts.string(it->function), std::move(args), [done](const script::Value&) {
                if (done) {
                    done();
                }
            });
            return;
        }
        break;
    }
    if (done) {
        done();
    }
}

void Dialogue::speak_current() {
    if (!m_current) {
        return;
    }
    Line line;
    line.speaker = m_speaker;
    line.target = k_player;
    line.info = m_current->info;
    line.in_menu = true;
    m_lines.push_back(line);
}

// 2.2.
void Dialogue::use_entry(const Entry& e, bool speak) {
    if (auto b = m_branches.find(e.topic->branch); b != m_branches.end() && (b->second.flags & branch_exclusive)) {
        m_forced_branch = b->second.form;
        m_blocking_found = false;
    } else if (m_forced_branch && !m_blocking_found) {
        m_forced_branch = 0;
    }
    if (e.topic->flags & 1) {
        e.info->flags |= info_used_this_cycle;
    }
    run_fragment(*e.info, oss::FragmentKind::info_begin, m_speaker, nullptr);
    m_goodbye = (e.info->flags & info_goodbye) != 0;
    collect_branches();
    m_history.push_back(e.topic);
    m_current = e;
    if (speak) {
        speak_current();
    }
}

// 1.3, 1.4.
bool Dialogue::start_conversation(uint32_t speaker, Info* forced) {
    if (m_speaker && m_speaker != speaker) {
        end_conversation();
    }
    Info* greeting = forced ? forced : pick_greeting(speaker);
    // 1.3 steps 10, 11: the greeting they're already saying carries on into the menu; anything else stops.
    const auto said = [&](const Line& l) { return l.speaker == speaker && !l.in_menu; };
    const auto playing = std::ranges::find_if(m_lines, said);
    const bool resume = greeting && playing != m_lines.end() && playing->info == greeting;
    if (playing != m_lines.end() && !resume) {
        m_output.stop_voice(speaker);
        std::erase_if(m_lines, said);
    }
    if (greeting && (greeting->flags & info_goodbye) && response(*greeting, 0) && !response(*greeting, 1)) {
        Line line;
        line.speaker = speaker;
        line.target = k_player;
        line.info = greeting;
        m_lines.push_back(line);
        return false;
    }
    m_speaker = speaker;
    m_list.clear();
    m_history.clear();
    m_current.reset();
    m_goodbye = false;
    collect_branches();
    if (greeting) {
        Entry e{greeting->topic, greeting, {}, false};
        use_entry(e, false);
    }
    build_list();
    if (m_list.empty() && !greeting) {
        m_speaker = 0;
        return false;
    }
    std::string name;
    const forms::Database& db = m_quests.db();
    if (const forms::FormEntry* entry = db.find(speaker)) {
        const forms::Form ref(db, *entry);
        name = std::string(ref.name());
        const forms::Value* base = ref.get("NAME");
        if (const forms::FormEntry* npc = base ? db.find(base->form) : nullptr; npc && name.empty()) {
            name = std::string(forms::Form(db, *npc).name());
        }
    }
    m_output.open_menu(name);
    if (const auto line = std::ranges::find_if(m_lines, said); resume && line != m_lines.end()) {
        line->in_menu = true;
        m_output.remove_subtitle(speaker);
        if (const Response* r = response(*line->info, line->cursor)) {
            m_output.show_text(r->text);
        }
        m_output.voice_ready();
    } else if (greeting) {
        speak_current();
    } else {
        send_list();
        return true;
    }
    // 1.4 step 10: the list is up during the greeting, unless a Blocking or Exclusive one says it first.
    const auto branch = m_branches.find(greeting->topic->branch);
    if (resume || branch == m_branches.end() || !(branch->second.flags & (branch_blocking | branch_exclusive))) {
        send_list(true);
    }
    return true;
}

void Dialogue::choose_topic(int index) {
    if (index < 0 || static_cast<size_t>(index) >= m_list.size()) {
        return;
    }
    const Entry e = m_list[static_cast<size_t>(index)];
    std::erase_if(m_lines, [this](Line& l) {
        if (l.in_menu) {
            m_output.stop_voice(l.speaker);
            return true;
        }
        return false;
    });
    use_entry(e, true);
}

void Dialogue::skip() {
    for (Line& l : m_lines) {
        if (l.in_menu) {
            l.timer = 0;
            m_output.stop_voice(l.speaker);
        }
    }
}

// 4.3: no response is left.
void Dialogue::line_finished() {
    if (!m_current) {
        return;
    }
    Info* i = m_current->info;
    if (i->flags & info_invisible_continue) {
        build_list();
        auto next = std::ranges::find_if(m_list, [](const Entry& e) { return e.topic->category != category_favor; });
        if (next != m_list.end()) {
            const Entry e = *next;
            use_entry(e, true);
            return;
        }
    }
    if (m_goodbye) {
        close();
        return;
    }
    build_list();
    send_list();
}

// 1.8.
void Dialogue::end_conversation() {
    const uint32_t speaker = m_speaker;
    if (!speaker) {
        return;
    }
    Info* current = m_current ? m_current->info : nullptr;
    // 1.8 step 3: the response being said finishes, the next ones don't come.
    std::erase_if(m_lines, [](const Line& l) { return l.in_menu && !l.begun; });
    for (Line& l : m_lines) {
        if (l.in_menu) {
            l.in_menu = false;
            l.last = true;
        }
    }
    if (m_link_source && current && !(current->flags & info_goodbye) && current->walk_away) {
        m_speaker = 0;
        if (is_saying_line(speaker)) { // step 4: a walk-away stops the line
            m_output.stop_voice(speaker);
            std::erase_if(m_lines, [speaker](const Line& l) { return l.speaker == speaker; });
        }
        say(speaker, current->walk_away, k_player);
    } else if (!current || !(current->flags & info_goodbye)) {
        m_speaker = 0;
        if (Info* goodbye = pick_by_subtype(category_misc, k_subtype_goodbye, speaker, k_player)) {
            Line line;
            line.speaker = speaker;
            line.target = k_player;
            line.info = goodbye;
            m_lines.push_back(line);
        }
    }
    m_speaker = 0;
    m_current.reset();
    m_list.clear();
    m_forced_branch = 0;
    m_output.remove_subtitle(speaker);
}

void Dialogue::close() {
    end_conversation();
    m_output.close_menu();
}

bool Dialogue::is_actor(uint32_t ref) {
    const auto* e = m_quests.db().find(ref);
    return ref == k_player || (e && e->type == forms::sig("ACHR"));
}

bool Dialogue::is_saying_line(uint32_t speaker) const {
    return std::ranges::any_of(m_lines, [speaker](const Line& l) { return l.speaker == speaker; });
}

bool Dialogue::say(uint32_t speaker, uint32_t topic_form, uint32_t target, bool no_padding) {
    if (is_saying_line(speaker)) {
        return false;
    }
    Topic* t = topic(topic_form);
    Info* i = t ? pick_info(*t, speaker, target) : nullptr;
    if (!i || !response(*i, 0)) {
        return false;
    }
    Line line;
    line.speaker = speaker;
    line.target = target;
    line.info = i;
    line.no_padding = no_padding;
    m_lines.push_back(line);
    return true;
}

// 4.4: each line's responses, one after the other.
void Dialogue::update(float dt, double game_days) {
    m_days = game_days;
    bool menu_line_ended = false;
    for (size_t n = 0; n < m_lines.size();) {
        Line& l = m_lines[n];
        const Response* r = response(*l.info, l.cursor);
        if (!l.begun) {
            // said once the actor is free.
            if (std::any_of(m_lines.begin(), m_lines.begin() + static_cast<ptrdiff_t>(n),
                            [&](const Line& o) { return o.speaker == l.speaker; })) {
                ++n;
                continue;
            }
            l.begun = true;
            if (!r) {
                l.timer = 0;
            } else {
                const std::string path = voice_path(*l.info, *r, l.speaker);
                const float length = path.empty() ? 0.0f : m_output.voice_length(path);
                if (length > 0) {
                    m_output.play_voice(l.speaker, path, l.in_menu, r->emotion, r->emotion_value);
                    l.timer = length + (l.no_padding ? 0.0f : k_padding);
                } else {
                    l.timer = silent_seconds(*r);
                }
                if (!l.in_menu && l.cursor == 0) {
                    run_fragment(*l.info, oss::FragmentKind::info_begin, l.speaker, nullptr);
                }
                if (l.in_menu) {
                    m_output.show_text(r->text);
                    m_output.voice_ready(); // 4.2 step 9: the movie takes a skip from here on
                } else {
                    m_output.add_subtitle(l.speaker, r->text, (l.info->flags & info_force_subtitle) != 0);
                }
            }
        }
        l.timer -= dt;
        if (l.timer > 0) {
            m_output.follow_voice(l.speaker);
            ++n;
            continue;
        }
        m_output.remove_subtitle(l.speaker);
        ++l.cursor;
        if (!l.last && response(*l.info, l.cursor)) {
            l.begun = false;
            ++n;
            continue;
        }
        const bool menu = l.in_menu;
        Info* finished = l.info;
        const uint32_t who = l.speaker;
        m_lines.erase(m_lines.begin() + static_cast<ptrdiff_t>(n));
        run_fragment(*finished, oss::FragmentKind::info_end, who, nullptr);
        menu_line_ended = menu_line_ended || menu;
    }
    if (menu_line_ended && m_speaker) {
        line_finished();
    }
}

std::vector<std::string> Dialogue::explain(uint32_t speaker) {
    std::vector<std::string> out;
    collect_branches();
    for (const Branch* b : m_top_level) {
        Topic* t = topic(b->start);
        if (!t) {
            continue;
        }
        const Quest* q = m_quests.quest(t->quest);
        load_infos(*t);
        std::string line = std::format("{} {} [{}] p{} c{} '{}':", q ? q->editor_id : "?", t->editor_id, t->infos.size(), t->priority, t->category, t->text);
        for (const Info* i : t->infos) {
            std::string blocked;
            if (q) {
                for (const ConditionItem& c : q->dialogue_conditions) {
                    if (!m_quests.conditions().evaluate(c, speaker, k_player)) {
                        blocked += std::format(" quest:{}{}/{}", c.function, Conditions::available(c.function) ? "" : "?", c.param1);
                    }
                }
            }
            for (const ConditionItem& c : i->conditions) {
                if (!m_quests.conditions().evaluate(c, speaker, k_player)) {
                    blocked += std::format(" {}{}", c.function, Conditions::available(c.function) ? "" : "?");
                }
            }
            line += std::format(" [{:08X} '{}'{}]", i->form, i->prompt, blocked);
        }
        out.push_back(std::move(line));
    }
    return out;
}

void Dialogue::bind_natives() {
    using script::NativeCall;
    using script::Value;
    script::VirtualMachine& vm = m_quests.vm();
    auto form_of = [](const Value& v) -> uint32_t {
        return v.tag == script::Tag::object && v.object ? script::handle_form(v.object->handle) : 0;
    };
    vm.bind_native("Topic", "Add", [this](NativeCall& c) {
        if (const Topic* t = topic(script::handle_form(c.self->handle))) {
            if (auto b = m_branches.find(t->branch); b != m_branches.end()) {
                b->second.flags |= branch_top_level;
            }
        }
    }, script::native_flags("Topic", "Add"));
    vm.bind_native("TopicInfo", "GetOwningQuest", [this](NativeCall& c) {
        const uint32_t form = script::handle_form(c.self->handle);
        if (const auto* e = m_quests.db().find(form)) {
            if (const Topic* t = topic(e->parent)) {
                c.result = c.vm.form_value(t->quest, "Quest");
            }
        }
    }, script::native_flags("TopicInfo", "GetOwningQuest"));
    vm.bind_native("ObjectReference", "Say", [this, form_of](NativeCall& c) {
        const uint32_t t = form_of(c.args[0]);
        if (!t) {
            c.vm.log_error(c.stack, "cannot speak a None topic info", 1);
            return;
        }
        say(script::handle_form(c.self->handle), t, form_of(c.args[1]));
    }, script::native_flags("ObjectReference", "Say"));
    vm.bind_native("ObjectReference", "IsInDialogueWithPlayer", [this](NativeCall& c) {
        c.result = Value::of(m_speaker != 0 && m_speaker == script::handle_form(c.self->handle));
    }, script::native_flags("ObjectReference", "IsInDialogueWithPlayer"));
    m_quests.scenes().set_actors(this);
}

} // namespace spud::game
