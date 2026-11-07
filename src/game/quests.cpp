#include "game/quests.hpp"

#include "core/binary.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "game/scenes.hpp"
#include "game/story_manager.hpp"
#include "script/natives.hpp"

#include <future>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace spud::game {

namespace {

constexpr uint32_t k_player = 0x14;

const forms::Value* member(const forms::Value& v, std::string_view name) {
    return v.member(name);
}

uint32_t member_u32(const forms::Value& v, std::string_view name, uint32_t fallback = 0) {
    const forms::Value* m = member(v, name);
    if (!m) {
        return fallback;
    }
    return m->kind == forms::Value::Kind::form ? m->form : static_cast<uint32_t>(m->integer);
}

uint32_t as_u32(const forms::Value& v) {
    return v.kind == forms::Value::Kind::form ? v.form : static_cast<uint32_t>(v.integer);
}

} // namespace

Stage* Quest::stage(uint16_t index) {
    for (Stage& s : stages) {
        if (s.index == index) {
            return &s;
        }
    }
    return nullptr;
}

Alias* Quest::alias(uint32_t id) {
    for (Alias& a : aliases) {
        if (a.id == id) {
            return &a;
        }
    }
    return nullptr;
}

Objective* Quest::objective(uint16_t index) {
    for (Objective& o : objectives) {
        if (o.index == index) {
            return &o;
        }
    }
    return nullptr;
}

// ---- the record-only world ------------------------------------------------------------------------------------------

QuestWorld::QuestWorld(const forms::Database& db) : m_db(db) {}

void QuestWorld::index_refs() {
    if (m_indexed) {
        return;
    }
    m_indexed = true;
    // Decoded records are shared (forms_typed.cpp): a worker has them ready for the fills to come, which ask for thousands.
    const auto warm = [this] {
        // What thinking and conditions read first, or an actor's first thought pays for it in the tick.
        for (const forms::Sig type : {forms::sig("ACHR"), forms::sig("PACK"), forms::sig("NPC_"), forms::sig("FACT"), forms::sig("RACE"),
                                      forms::sig("CLAS"), forms::sig("LCTN"), forms::sig("FLST"), forms::sig("KYWD"),
                                      forms::sig("CSTY"), forms::sig("LVLN"),
                                      forms::sig("DIAL"), forms::sig("INFO")}) {
            for (const forms::FormEntry* e : m_db.of_type(type)) {
                const forms::Form f(m_db, *e);
            }
        }
        for (const uint32_t ref : m_persistent) {
            if (const forms::FormEntry* e = m_db.find(ref)) {
                const forms::Form f(m_db, *e);
                if (const forms::Value* name = f.get("NAME"); name && name->form) {
                    if (const forms::FormEntry* base = m_db.find(name->form)) {
                        const forms::Form b(m_db, *base);
                    }
                }
            }
        }
    };
    const struct Warm {
        QuestWorld* world;
        decltype(warm)& fn;
        ~Warm() { world->m_warming = std::async(std::launch::async, fn); }
    } start_warming{this, warm};
    constexpr uint32_t k_persistent = 0x400;
    // A persistent reference outside sits in its world's persistent cell, with no location: the cell at its position has it.
    std::unordered_map<uint64_t, uint32_t> grid;
    auto grid_key = [](uint32_t world, int32_t x, int32_t y) {
        return uint64_t(world) << 32 | uint64_t(uint16_t(x)) << 16 | uint16_t(y);
    };
    for (const forms::FormEntry* e : m_db.of_type(forms::sig("CELL"))) {
        const auto xclc = m_db.field(*e, forms::sig("XCLC"));
        const auto xlcn = m_db.field(*e, forms::sig("XLCN"));
        if (e->parent && xclc && xlcn && xclc->data.size() >= 8 && xlcn->data.size() >= 4) {
            int32_t at[2];
            uint32_t location = 0;
            std::memcpy(at, xclc->data.data(), 8);
            std::memcpy(&location, xlcn->data.data(), 4);
            grid[grid_key(e->parent, at[0], at[1])] = location;
        }
    }
    for (const forms::Sig type : {forms::sig("REFR"), forms::sig("ACHR")}) {
        for (const forms::FormEntry* e : m_db.of_type(type)) {
            m_by_cell[e->parent].push_back(e->form_id);
            if (e->flags & k_persistent) {
                m_persistent.push_back(e->form_id);
                m_persistent_set.insert(e->form_id);
                if (type == forms::sig("ACHR")) {
                    m_persistent_actors.push_back(e->form_id);
                }
            }
            if (auto name = m_db.field(*e, forms::sig("NAME")); name && name->data.size() >= 4) {
                uint32_t base = 0;
                std::memcpy(&base, name->data.data(), 4);
                m_by_base[base].push_back(e->form_id);
            }
            if (auto f = m_db.field(*e, forms::sig("XLRT"))) {
                for (size_t at = 0; at + 4 <= f->data.size(); at += 4) {
                    uint32_t ref_type = 0;
                    std::memcpy(&ref_type, f->data.data() + at, 4);
                    m_by_ref_type[ref_type].push_back(e->form_id);
                }
                uint32_t location = 0;
                if (const auto* cell = m_db.find(e->parent)) {
                    if (auto x = m_db.field(*cell, forms::sig("XLCN")); x && x->data.size() >= 4) {
                        std::memcpy(&location, x->data.data(), 4);
                    }
                    if (!location && cell->parent) {
                        if (const auto placed = forms::placement(forms::Form(m_db, *e))) {
                            const auto x = int32_t(std::floor(placed->position[0] / 4096.0f));
                            const auto y = int32_t(std::floor(placed->position[1] / 4096.0f));
                            if (const auto it = grid.find(grid_key(cell->parent, x, y)); it != grid.end()) {
                                location = it->second;
                            }
                        }
                    }
                }
                for (size_t i = 0; location && i + 4 <= f->data.size(); i += 4) {
                    uint32_t ref_type = 0;
                    std::memcpy(&ref_type, f->data.data() + i, 4);
                    m_typed_refs[uint64_t{location} << 32 | ref_type].push_back(e->form_id);
                }
            }
            for (const forms::Field& f : m_db.fields(*e)) {
                uint32_t link[2] = {};
                if (f.sig == forms::sig("XLKR") && f.data.size() >= 8) {
                    std::memcpy(link, f.data.data(), 8);
                    if (link[0] == 0 && link[1] != 0) {
                        m_linked_children[link[1]].push_back(e->form_id);
                    }
                }
            }
            if (type == forms::sig("ACHR")) {
                if (auto f = m_db.field(*e, forms::sig("NAME"))) {
                    uint32_t base = 0;
                    std::memcpy(&base, f->data.data(), std::min<size_t>(4, f->data.size()));
                    m_npc_refs.emplace(base, e->form_id);
                }
            }
        }
    }
}

RefState QuestWorld::ref_state(uint32_t ref) {
    RefState s;
    if (ref == k_player) { // no record: the game makes it
        s.exists = true;
        return s;
    }
    if (const auto* e = m_db.find(ref)) {
        s.exists = true;
        s.deleted = (e->flags & forms::form_deleted) != 0;
        constexpr uint32_t k_initially_disabled = 0x800;
        s.disabled = (e->flags & k_initially_disabled) != 0;
    }
    if (live.ref_state) {
        live.ref_state(ref, s);
    }
    return s;
}

std::vector<uint32_t> QuestWorld::persistent_refs() {
    index_refs();
    return m_persistent;
}

const std::vector<uint32_t>& QuestWorld::refs_in_cell(uint32_t cell) {
    static const std::vector<uint32_t> none;
    index_refs();
    const auto it = m_by_cell.find(cell);
    return it != m_by_cell.end() ? it->second : none;
}

const std::vector<uint32_t>& QuestWorld::persistent_actors() {
    index_refs();
    return m_persistent_actors;
}

bool QuestWorld::is_persistent(uint32_t ref) {
    index_refs();
    return m_persistent_set.contains(ref);
}

const std::vector<uint32_t>& QuestWorld::refs_of_type(uint32_t ref_type) {
    static const std::vector<uint32_t> none;
    index_refs();
    const auto it = m_by_ref_type.find(ref_type);
    return it != m_by_ref_type.end() ? it->second : none;
}

const std::vector<uint32_t>& QuestWorld::refs_of_base(uint32_t base) {
    static const std::vector<uint32_t> none;
    index_refs();
    const auto it = m_by_base.find(base);
    return it != m_by_base.end() ? it->second : none;
}

const std::vector<uint32_t>& QuestWorld::bases_in_faction(uint32_t faction) {
    static const std::vector<uint32_t> none;
    if (m_faction_bases.empty()) {
        m_faction_bases.try_emplace(0);
        for (const forms::FormEntry* e : m_db.of_type(forms::sig("NPC_"))) {
            for (const forms::FactionMembership& m : forms::npc_factions(forms::Form(m_db, *e))) {
                m_faction_bases[m.faction].push_back(e->form_id);
            }
        }
    }
    const auto it = m_faction_bases.find(faction);
    return it != m_faction_bases.end() ? it->second : none;
}

uint32_t QuestWorld::ref_of_unique_npc(uint32_t npc) {
    constexpr uint32_t k_player_npc = 0x7;
    if (npc == k_player_npc) {
        return k_player;
    }
    index_refs();
    auto it = m_npc_refs.find(npc);
    return it == m_npc_refs.end() ? 0 : it->second;
}

std::vector<uint32_t> QuestWorld::linked_children(uint32_t ref) {
    index_refs();
    const auto it = m_linked_children.find(ref);
    return it == m_linked_children.end() ? std::vector<uint32_t>{} : it->second;
}

std::vector<uint32_t> QuestWorld::location_refs(uint32_t location, uint32_t ref_type) {
    // An overriding LCTN carries only what its own plugin added, while a plugin's references say where they stand and
    // what they are, so the references answer too.
    index_refs();
    std::vector<uint32_t> out;
    if (auto it = m_typed_refs.find(uint64_t{location} << 32 | ref_type); it != m_typed_refs.end()) {
        out = it->second;
    }
    if (const auto* e = m_db.find(location)) {
        const forms::Form form(m_db, *e);
        for (const char* field : {"LCSR", "ACSR"}) {
            for (const forms::Value* list : form.all(field)) {
                for (const forms::Value& item : list->items) {
                    const uint32_t ref = member_u32(item, "ref");
                    if (member_u32(item, "location_ref_type") == ref_type && std::ranges::find(out, ref) == out.end()) {
                        out.push_back(ref);
                    }
                }
            }
        }
    }
    return out;
}

uint32_t QuestWorld::location_parent_with_keyword(uint32_t location, uint32_t keyword) {
    for (int depth = 0; location && depth < 32; ++depth) {
        const auto* e = m_db.find(location);
        if (!e) {
            return 0;
        }
        const forms::Form form(m_db, *e);
        const auto words = form.keywords();
        if (!keyword || std::ranges::find(words, keyword) != words.end()) {
            return location;
        }
        const forms::Value* parent = form.get("PNAM");
        location = parent ? parent->form : 0;
    }
    return 0;
}

// ---- construction ---------------------------------------------------------------------------------------------------

QuestSystem::QuestSystem(const forms::Database& db, const oss::Scripts& scripts, script::VirtualMachine& vm,
                         Conditions& conditions, QuestWorld& world)
    : m_db(db), m_scripts(scripts), m_vm(vm), m_conditions(conditions), m_world(world) {
    load_records();
    m_story = std::make_unique<StoryManager>(*this);
    m_scenes = std::make_unique<Scenes>(*this);
    bind_natives();
}

QuestSystem::~QuestSystem() = default;

void QuestSystem::parse(Quest& q, const forms::Form& form) {
    enum class Section { top, story, stage, objective, target, alias } section = Section::top;
    Stage* stage = nullptr;
    StageItem* item = nullptr;
    Objective* objective = nullptr;
    ObjectiveTarget* target = nullptr;
    Alias* alias = nullptr;
    for (const forms::DecodedField& f : form.fields()) {
        const forms::Value& v = f.value;
        switch (f.sig) {
        case forms::sig("EDID"): q.editor_id = v.text; break;
        case forms::sig("FULL"):
            if (section == Section::top) {
                q.name = v.text;
            }
            break;
        case forms::sig("DNAM"):
            q.flags = static_cast<uint16_t>(member_u32(v, "flags"));
            q.priority = static_cast<uint8_t>(member_u32(v, "priority"));
            q.type = static_cast<uint8_t>(member_u32(v, "type"));
            break;
        case forms::sig("ENAM"): q.story_event = as_u32(v); break;
        case forms::sig("QTGL"): q.text_globals.push_back(v.form); break;
        case forms::sig("NEXT"): section = Section::story; break;
        case forms::sig("CTDA"): {
            const ConditionItem c = condition_item(v);
            switch (section) {
            case Section::top: q.dialogue_conditions.push_back(c); break;
            case Section::story: q.story_conditions.push_back(c); break;
            case Section::stage:
                if (item) {
                    item->conditions.push_back(c);
                }
                break;
            case Section::target: target->conditions.push_back(c); break;
            case Section::alias: alias->conditions.push_back(c); break;
            default: break;
            }
            break;
        }
        case forms::sig("INDX"):
            section = Section::stage;
            q.stages.emplace_back();
            stage = &q.stages.back();
            item = nullptr;
            stage->index = static_cast<uint16_t>(member_u32(v, "index"));
            stage->flags = static_cast<uint8_t>(member_u32(v, "flags") & ~stage_done);
            break;
        case forms::sig("QSDT"):
            stage->items.emplace_back();
            item = &stage->items.back();
            item->flags = static_cast<uint8_t>(as_u32(v));
            break;
        case forms::sig("CNAM"):
            if (item) {
                item->log = v.text;
            }
            break;
        case forms::sig("NAM0"):
            if (item) {
                item->next_quest = v.form;
            }
            break;
        case forms::sig("QOBJ"):
            section = Section::objective;
            q.objectives.emplace_back();
            objective = &q.objectives.back();
            objective->index = static_cast<uint16_t>(as_u32(v));
            break;
        case forms::sig("NNAM"):
            if (objective) {
                objective->text = v.text;
            }
            break;
        case forms::sig("QSTA"):
            section = Section::target;
            objective->targets.emplace_back();
            target = &objective->targets.back();
            target->alias = static_cast<int32_t>(member_u32(v, "alias"));
            target->ignores_locks = member_u32(v, "compass_marker_ignores_locks") & 1;
            break;
        case forms::sig("ALST"):
        case forms::sig("ALLS"):
            section = Section::alias;
            q.aliases.emplace_back();
            alias = &q.aliases.back();
            alias->id = as_u32(v);
            alias->location = f.sig == forms::sig("ALLS");
            break;
        case forms::sig("FNAM"):
            if (section == Section::alias) {
                alias->flags = as_u32(v) & ~alias_filled_by_other;
            } else if (section == Section::objective || section == Section::target) {
                objective->ored = as_u32(v) & 1;
            }
            break;
        case forms::sig("ALID"):
            if (alias) {
                alias->name = v.text;
            }
            break;
        case forms::sig("ALFI"): alias->force_into = static_cast<int32_t>(v.integer); break;
        case forms::sig("ALFR"): alias->specific = v.form; alias->fill = 1; break;
        case forms::sig("ALUA"): alias->unique_actor = v.form; alias->fill = 6; break;
        case forms::sig("ALFA"): alias->other_alias = static_cast<int32_t>(v.integer); alias->fill = 2; break;
        case forms::sig("ALRT"): alias->ref_type = v.form; break;
        case forms::sig("KNAM"):
            if (alias) {
                alias->keyword = v.form;
            }
            break;
        case forms::sig("ALEQ"): alias->external_quest = v.form; alias->fill = alias->location ? 4 : 5; break;
        case forms::sig("ALEA"): alias->external_alias = static_cast<int32_t>(v.integer); break;
        case forms::sig("ALCO"): alias->create_object = v.form; alias->fill = 4; break;
        case forms::sig("ALCA"): {
            alias->create_at = static_cast<int16_t>(member_u32(v, "alias"));
            alias->create_in = (member_u32(v, "create") & 0x8000) != 0;
            break;
        }
        case forms::sig("ALCL"): alias->create_level = as_u32(v); break;
        case forms::sig("ALNA"): alias->near_alias = static_cast<int32_t>(v.integer); alias->fill = 7; break;
        case forms::sig("ALFE"): alias->event = as_u32(v); alias->fill = 3; break;
        case forms::sig("ALFD"): alias->event_member = static_cast<uint16_t>(as_u32(v)); break;
        case forms::sig("ALFL"): alias->specific = v.form; alias->fill = 1; break;
        case forms::sig("ALSP"): alias->spells.push_back(v.form); break;
        case forms::sig("ALFC"): alias->factions.push_back(v.form); break;
        case forms::sig("ALPC"): alias->packages.push_back(v.form); break;
        case forms::sig("ALDN"): alias->display_name = v.form; break;
        case forms::sig("CNTO"):
            if (alias) {
                const forms::Value* item = v.member("item");
                const forms::Value* count = v.member("count");
                if (item && item->kind == forms::Value::Kind::form) {
                    alias->items.emplace_back(item->form, count ? int32_t(count->integer) : 1);
                }
            }
            break;
        case forms::sig("KWDA"):
            if (alias) {
                for (const forms::Value& k : v.items) {
                    alias->keywords.push_back(k.form);
                }
            }
            break;
        default: break;
        }
    }
    for (Alias& a : q.aliases) {
        if (a.fill < 0 && !a.conditions.empty()) {
            a.fill = 0;
        }
    }
    // InitItem: 0x800 marks the targets of ALFI, whatever the record says.
    for (const Alias& a : q.aliases) {
        if (a.force_into >= 0) {
            if (Alias* target_alias = q.alias(static_cast<uint32_t>(a.force_into))) {
                target_alias->flags |= alias_filled_by_other;
            }
        }
    }
    // The record's start-game bit is the runtime's copy too; an event-scoped quest never starts that way (1.3).
    if (q.flags & quest_running) {
        q.flags |= quest_start_game;
    } else {
        q.flags &= ~quest_start_game;
    }
    if (q.story_event != 0) {
        q.flags &= ~(quest_running | quest_start_game);
    }
    q.flags &= ~(quest_running | quest_completed | quest_failed | quest_shown | quest_wait | quest_active | quest_keep_instance);
}

void QuestSystem::load_records() {
    for (const forms::FormEntry* e : m_db.of_type(forms::sig("QUST"))) {
        auto q = std::make_unique<Quest>();
        q->form = e->form_id;
        parse(*q, forms::Form(m_db, *e));
        m_by_form.emplace(q->form, q.get());
        m_by_name.emplace(path::to_lower(q->editor_id), q.get());
        m_quests.push_back(std::move(q));
    }
}

Quest* QuestSystem::quest(uint32_t form) {
    auto it = m_by_form.find(form);
    return it == m_by_form.end() ? nullptr : it->second;
}

Quest* QuestSystem::quest(std::string_view editor_id) {
    auto it = m_by_name.find(path::to_lower(editor_id));
    return it == m_by_name.end() ? nullptr : it->second;
}

bool QuestSystem::in_lists(const Quest& q) const {
    auto has = [&q](const std::vector<Quest*>& list) { return std::ranges::find(list, &q) != list.end(); };
    return has(m_starting) || has(m_running) || has(m_stopping);
}

bool QuestSystem::reserved(uint32_t form) const {
    if (!m_ready || form == k_player) {
        return false;
    }
    auto it = m_reservations.find(form);
    return it != m_reservations.end() && it->second > 0;
}

script::Handle QuestSystem::alias_handle(const Quest& q, const Alias& a) const {
    return script::alias_handle(q.form, a.id, a.location);
}

void QuestSystem::bind_alias_scripts(Quest& q) {
    for (const Alias& a : q.aliases) {
        m_vm.bind_scripts(q.form, static_cast<int32_t>(a.id), alias_handle(q, a));
    }
}

// 1.2: a new game.
void QuestSystem::new_game(uint32_t first) {
    // Behind the loading screen: the first fill would otherwise pay for the indexes in play.
    m_world.refs_of_type(0);
    m_world.bases_in_faction(0);
    for (const auto& q : m_quests) {
        m_vm.bind_scripts(q->form);
        bind_alias_scripts(*q);
    }
    // The opening starts before the rest: an actor's aliases count in the order they were filled, and its packages
    // have to beat what the other quests give the same actors.
    if (Quest* q = first ? quest(first) : nullptr) {
        start(*q, true);
    }
    for (const auto& q : m_quests) {
        if (q->flags & quest_start_game) {
            if (fill_aliases(*q, nullptr) == Fill::ok) {
                add_starting(*q);
            } else {
                q->flags |= quest_running; // the record's bit stays with the quest in no list (1.2)
            }
        }
    }
    m_ready = true;
}

// ---- start and stop -------------------------------------------------------------------------------------------------

bool QuestSystem::start(Quest& q, bool immediate, bool* pending) {
    if (pending) {
        *pending = false;
    }
    if (q.running_bit()) {
        if (pending) {
            *pending = q.promoting && !q.wait_bit();
        }
        return true;
    }
    if ((q.flags & quest_run_once) && q.has_run) {
        return false;
    }
    if (immediate) {
        if (fill_aliases(q, nullptr) != Fill::ok) {
            return false;
        }
        add_starting(q);
        if (pending) {
            *pending = true;
        }
        return true;
    }
    StoryEvent e;
    e.type = event_code("QSTR");
    e.add("Q1", q.form);
    m_story->queue(e);
    if (pending) {
        *pending = true;
    }
    return true;
}

void QuestSystem::reset_progress(Quest& q) {
    for (Stage& s : q.stages) {
        s.flags &= ~stage_done;
    }
    q.current_stage = 0;
    for (Objective& o : q.objectives) {
        o.state = 0;
    }
    q.flags &= ~(quest_completed | quest_failed | quest_shown);
}

void QuestSystem::add_starting(Quest& q) {
    reset_progress(q);
    q.has_run = true;
    q.promoting = true;
    q.flags |= quest_running;
    m_starting.push_back(&q);
}

// 1.4 step 4.
void QuestSystem::finish_promotion(Quest& q) {
    update_reservations(q, -1);
    auto apply = [&](Alias& a) {
        if (a.location || a.fill == 4) {
            return;
        }
        if (auto it = q.filled_refs.find(a.id); it != q.filled_refs.end()) {
            m_world.alias_applied(q, a, it->second);
        }
    };
    for (Alias& a : q.aliases) {
        if (!(a.flags & alias_filled_by_other)) {
            apply(a);
        }
    }
    for (Alias& a : q.aliases) {
        if (a.fill == 4 && !a.location && q.filled_refs.contains(a.id)) {
            auto anchor = q.filled_refs.find(static_cast<uint32_t>(a.create_at));
            const uint32_t ref = anchor == q.filled_refs.end()
                                     ? 0
                                     : m_world.create_ref(a.create_object, anchor->second, a.create_in, a.create_level);
            if (ref) {
                q.filled_refs[a.id] = ref;
                if (a.force_into >= 0) {
                    q.filled_refs[static_cast<uint32_t>(a.force_into)] = ref;
                }
                m_world.alias_applied(q, a, ref);
            } else {
                q.filled_refs.erase(a.id);
            }
        }
    }
    for (Alias& a : q.aliases) {
        if (a.flags & alias_filled_by_other) {
            apply(a);
        }
    }
    update_reservations(q, 1);
    create_instance_text(q);
    q.promoting = false;
}

// 1.4 step 5.
void QuestSystem::init_quest(Quest& q) {
    if (!(q.flags & quest_run_once)) {
        for (script::Object* o : m_vm.objects(script::form_handle(q.form))) {
            m_vm.reset(o);
        }
        for (const Alias& a : q.aliases) {
            for (script::Object* o : m_vm.objects(alias_handle(q, a))) {
                m_vm.reset(o);
            }
        }
    }
    if (q.event) {
        m_story->send_quest_event(q, *q.event);
    }
    q.flags |= quest_wait;
    for (Stage& s : q.stages) {
        if (s.flags & stage_start_up) {
            if (s.index > q.current_stage) {
                q.current_stage = s.index;
            }
            run_stage(q, s);
        }
    }
}

void QuestSystem::begin_stop(Quest& q) {
    q.flags |= quest_wait;
    for (Stage& s : q.stages) {
        if (s.flags & stage_shut_down) {
            if (s.index > q.current_stage) {
                q.current_stage = s.index;
            }
            run_stage(q, s);
        }
    }
    m_stopping.push_back(&q);
}

void QuestSystem::finish_stop(Quest& q) {
    q.flags &= ~(quest_running | quest_wait);
    m_scenes->quest_stopped(q);
    release_aliases(q);
    m_vm.unregister_update(script::form_handle(q.form));
    m_vm.unregister_game_time_update(script::form_handle(q.form));
    for (const Alias& a : q.aliases) {
        m_vm.unregister_update(alias_handle(q, a));
        m_vm.unregister_game_time_update(alias_handle(q, a));
    }
    finish_waiters(q, false, false);
    release_instance_text(q);
    q.flags &= ~quest_keep_instance;
}

void QuestSystem::stop(Quest& q) {
    q.flags &= ~quest_running;
}

void QuestSystem::reset(Quest& q) {
    reset_progress(q);
    const bool start_game = (q.flags & quest_start_game) != 0;
    if (q.running_bit() == start_game) {
        return;
    }
    if (start_game) {
        add_starting(q);
        return;
    }
    // the wait bit, Shut Down stages, stopping; the running bit stays until FinishStop.
    std::erase(m_running, &q);
    std::erase(m_starting, &q);
    begin_stop(q);
}

// 1.4, 1.6.
void QuestSystem::update_quests(bool menu_paused, float dt) {
    bool changed = false;
    for (size_t i = 0; i < m_running.size();) {
        Quest* q = m_running[i];
        if (!q->running_bit()) {
            m_running.erase(m_running.begin() + static_cast<ptrdiff_t>(i));
            begin_stop(*q);
            changed = true;
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < m_stopping.size();) {
        Quest* q = m_stopping[i];
        if (q->outstanding.empty()) {
            m_stopping.erase(m_stopping.begin() + static_cast<ptrdiff_t>(i));
            finish_stop(*q);
            changed = true;
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < m_starting.size();) {
        Quest* q = m_starting[i];
        if (!q->running_bit()) {
            m_starting.erase(m_starting.begin() + static_cast<ptrdiff_t>(i));
            begin_stop(*q);
            continue;
        }
        if (!q->promoting && !q->wait_bit()) {
            init_quest(*q);
        }
        if (!q->promoting && q->wait_bit() && q->outstanding.empty()) {
            q->flags &= ~quest_wait;
            m_scenes->quest_started(*q);
            m_starting.erase(m_starting.begin() + static_cast<ptrdiff_t>(i));
            m_running.push_back(q);
            finish_waiters(*q, true, false);
            changed = true;
            continue;
        }
        ++i;
    }
    for (Quest* q : m_starting) {
        if (q->promoting) {
            finish_promotion(*q);
        }
    }
    if (changed) {
        std::ranges::stable_sort(m_running, std::greater<>{}, &Quest::priority);
    }
    if (!menu_paused) {
        m_scenes->update(dt);
    }
}

void QuestSystem::update(float dt_seconds, double game_days, bool menu_paused) {
    m_alias_packages_stale = true;
    m_days = game_days;
    m_story->process_events();
    update_quests(menu_paused, dt_seconds);
}

// ---- stages ---------------------------------------------------------------------------------------------------------

bool QuestSystem::set_stage(Quest& q, uint16_t index) {
    Stage* s = q.stage(index);
    if (!q.running_bit() || !s) {
        return false;
    }
    if (index > q.current_stage) {
        q.current_stage = index;
    }
    return run_stage(q, *s);
}

bool QuestSystem::run_stage(Quest& q, Stage& s) {
    if ((s.flags & stage_done) && !(q.flags & quest_repeat_stages)) {
        return false;
    }
    s.flags |= stage_done;
    if (q.form == m_traced) {
        log::info("quest {}: stage {}", q.editor_id, s.index);
    }
    if (s.flags & stage_keep_instance) {
        q.flags |= quest_keep_instance;
    }
    const uint32_t items = static_cast<uint32_t>(s.items.size());
    if (s.flags & (stage_start_up | stage_shut_down)) {
        for (uint32_t i = 0; i < items; ++i) {
            q.outstanding.emplace_back(s.index, i);
        }
    }
    for (uint32_t i = 0; i < items; ++i) {
        run_item(q, s, i);
    }
    return items > 0;
}

void QuestSystem::run_item(Quest& q, Stage& s, uint32_t index) {
    StageItem& item = s.items[index];
    const Conditions::QuestScope scope(m_conditions, q.form);
    if (!m_conditions.evaluate(item.conditions, k_player, 0)) {
        item_done(q.form, s.index, index);
        return;
    }
    if (item.flags & item_complete_quest) {
        set_completed(q, true);
    }
    if (item.flags & item_fail_quest) {
        set_failed(q, true);
    }
    if (!item.log.empty()) {
        if (InstanceText* record = q.instance_text(q.instance)) {
            record->log = item.log; // the instance's single last-entry slot (2.6)
        }
        m_world.log_entry(q, item);
        if ((item.flags & item_complete_quest) && item.next_quest) {
            if (Quest* next = quest(item.next_quest); next && !next->running_bit()) {
                start(*next, false);
            }
        }
    }
    // the fragment, or the callback at once (2.5).
    const auto& list = m_scripts.fragments;
    auto it = std::ranges::lower_bound(list, q.form, {}, &oss::Fragment::form_id);
    for (; it != list.end() && it->form_id == q.form; ++it) {
        if (it->kind != oss::FragmentKind::quest_stage || it->index != s.index || it->entry != static_cast<int32_t>(index)) {
            continue;
        }
        script::Class* type = m_vm.find_class(m_scripts.string(it->script));
        script::Object* object = type ? m_vm.object_for(script::form_handle(q.form), type) : nullptr;
        if (object) {
            const uint32_t form = q.form;
            const uint16_t stage = s.index;
            m_vm.call_method(object, m_scripts.string(it->function), {},
                             [this, form, stage, index](const script::Value&) { item_done(form, stage, index); });
            return;
        }
        break;
    }
    item_done(q.form, s.index, index);
}

void QuestSystem::item_done(uint32_t form, uint16_t stage, uint32_t item) {
    Quest* q = quest(form);
    if (auto it = std::ranges::find(q->outstanding, std::pair{stage, item}); it != q->outstanding.end()) {
        q->outstanding.erase(it);
    }
    for (size_t i = 0; i < m_waiters.size();) {
        Waiter& w = m_waiters[i];
        if (w.quest == form && w.stage == stage && w.items_left > 0 && --w.items_left == 0) {
            const script::StackId stack = w.stack;
            m_waiters.erase(m_waiters.begin() + static_cast<ptrdiff_t>(i));
            m_vm.return_from_latent(stack, script::Value::of(true));
        } else {
            ++i;
        }
    }
}

// Waiting Papyrus calls (1.4 step 7, 1.6, 2.2).
void QuestSystem::finish_waiters(Quest& q, bool started, bool failed) {
    std::vector<Waiter> waiting;
    std::erase_if(m_waiters, [&](const Waiter& w) {
        if (w.quest == q.form && w.items_left == 0) {
            waiting.push_back(w);
            return true;
        }
        return false;
    });
    for (const Waiter& w : waiting) {
        if (w.stage < 0 || !started || failed) {
            m_vm.return_from_latent(w.stack, script::Value::of(!failed));
            continue;
        }
        Stage* s = q.stage(static_cast<uint16_t>(w.stage));
        if (!s || s->items.empty()) {
            set_stage(q, static_cast<uint16_t>(w.stage));
            m_vm.return_from_latent(w.stack, script::Value::of(false));
            continue;
        }
        m_waiters.push_back({w.stack, q.form, w.stage, static_cast<uint32_t>(s->items.size()), false});
        if (!set_stage(q, static_cast<uint16_t>(w.stage))) {
            std::erase_if(m_waiters, [&](const Waiter& x) { return x.stack == w.stack; });
            m_vm.return_from_latent(w.stack, script::Value::of(false));
        }
    }
}

// ---- objectives -----------------------------------------------------------------------------------------------------

void QuestSystem::set_completed(Quest& q, bool completed) {
    if (completed) {
        q.flags |= quest_completed;
        if (q.type != k_quest_type_misc) {
            m_world.notify(QuestWorld::Notice::quest_completed, q, q.name);
            q.flags &= ~quest_active;
        }
    } else {
        q.flags &= ~quest_completed;
    }
}

void QuestSystem::set_failed(Quest& q, bool failed) {
    if (failed) {
        q.flags |= quest_failed | quest_completed;
        if (q.type != k_quest_type_misc) {
            m_world.notify(QuestWorld::Notice::quest_failed, q, q.name);
            q.flags &= ~quest_active;
        }
    } else {
        q.flags &= ~quest_failed;
    }
}

// 3.2.
void QuestSystem::set_objective_state(Quest& q, Objective& o, uint8_t state) {
    if (!q.running_bit() && !q.wait_bit()) {
        return;
    }
    if (state == 1) {
        if (!(q.flags & quest_shown)) {
            if (q.type != k_quest_type_misc) {
                m_world.notify(QuestWorld::Notice::quest_started, q, quest_text(q, q.instance, q.name));
                q.flags |= quest_shown;
            }
            const bool any_active = std::ranges::any_of(m_quests, [](const auto& x) { return (x->flags & quest_active) != 0; });
            if (!any_active) {
                q.flags |= quest_active;
            }
        }
        m_world.notify(QuestWorld::Notice::objective_shown, q, objective_notice(q, o));
    } else if (state >= 2) {
        if (o.ored) {
            const bool displayed = state == 3 || state == 5;
            for (Objective& other : q.objectives) {
                const bool other_displayed = other.state == 1 || other.state == 3 || other.state == 5;
                if (&other != &o && other.ored && other_displayed == displayed) {
                    other.state = state;
                }
            }
        }
        if (o.state == 1 && (!(q.flags & quest_completed) || q.type == k_quest_type_misc)) {
            if (state == 3) {
                m_world.notify(QuestWorld::Notice::objective_completed, q, objective_notice(q, o));
            } else if (state == 5) {
                m_world.notify(QuestWorld::Notice::objective_failed, q, objective_notice(q, o));
            }
        }
    }
    o.state = state;
    if (state != 0) {
        q.flags |= quest_keep_instance; // a quest the player has seen keeps its journal text (1.9)
        update_player_objective(q, o);
    }
}

// an "or" objective has the word in front.
std::string QuestSystem::objective_notice(const Quest& q, const Objective& o) {
    std::string text = const_cast<QuestSystem*>(this)->quest_text(q, q.instance, o.text);
    return o.ored ? std::format("{} {}", m_world.text_setting("sOr"), text) : text;
}

void QuestSystem::set_objective_displayed(Quest& q, uint16_t index, bool display, bool force) {
    Objective* o = q.objective(index);
    if (!o || (q.flags & quest_completed)) {
        return;
    }
    const bool shown = o->state == 1 || o->state == 3 || o->state == 5;
    if (!display) {
        set_objective_state(q, *o, 0);
    } else if (!shown || force) {
        set_objective_state(q, *o, 1);
    }
}

void QuestSystem::set_objective_completed(Quest& q, uint16_t index, bool complete) {
    Objective* o = q.objective(index);
    if (!o) {
        return;
    }
    if (!complete) {
        set_objective_state(q, *o, 0);
    } else if (o->state < 2) {
        set_objective_state(q, *o, o->state == 1 ? 3 : 2);
    }
}

void QuestSystem::set_objective_failed(Quest& q, uint16_t index, bool fail) {
    Objective* o = q.objective(index);
    if (!o || (q.flags & quest_completed)) {
        return;
    }
    if (!fail) {
        set_objective_state(q, *o, 0);
    } else if (o->state < 2) {
        set_objective_state(q, *o, o->state == 1 ? 5 : 4);
    }
}

void QuestSystem::all_objectives(Quest& q, bool complete) {
    for (Objective& o : q.objectives) {
        const bool shown = o.state == 1 || o.state == 3 || o.state == 5;
        set_objective_state(q, o, static_cast<uint8_t>(complete ? (shown ? 3 : 2) : (shown ? 5 : 4)));
    }
}

std::vector<QuestSystem::Target> QuestSystem::quest_targets() {
    std::vector<Target> out;
    for (Quest* q : m_running) {
        if (q->flags & quest_completed) {
            continue;
        }
        for (const Objective& o : q->objectives) {
            if (o.state != 1) {
                continue;
            }
            for (const ObjectiveTarget& t : o.targets) {
                auto it = q->filled_refs.find(static_cast<uint32_t>(t.alias));
                const Conditions::QuestScope scope(m_conditions, q->form);
                if (it != q->filled_refs.end() && m_conditions.evaluate(t.conditions, it->second, 0)) {
                    out.push_back({q->form, it->second, o.index});
                }
            }
        }
    }
    return out;
}

// ---- aliases --------------------------------------------------------------------------------------------------------

bool QuestSystem::in_aliases(const Quest& q, uint32_t ref) const {
    return std::ranges::any_of(q.filled_refs, [ref](const auto& kv) { return kv.second == ref; });
}

bool QuestSystem::usable(const Alias& a, uint32_t ref) {
    const RefState s = m_world.ref_state(ref);
    if (!s.exists || s.deleted) {
        return false;
    }
    if (!(a.flags & alias_allow_reserved) && reserved(ref)) {
        return false;
    }
    if ((s.disabled && !(a.flags & alias_allow_disabled)) || (s.destroyed && !(a.flags & alias_allow_destroyed))) {
        return false;
    }
    return !s.dead || (a.flags & alias_allow_dead);
}

// A uniform pick; a ref that breaks the reuse rule is removed and the pick repeats.
uint32_t QuestSystem::pick(Quest& q, const Alias& a, std::vector<uint32_t> candidates) {
    while (!candidates.empty()) {
        const size_t i = std::uniform_int_distribution<size_t>(0, candidates.size() - 1)(m_random);
        const uint32_t ref = candidates[i];
        if ((a.flags & alias_allow_reuse) || !in_aliases(q, ref)) {
            return ref;
        }
        candidates.erase(candidates.begin() + static_cast<ptrdiff_t>(i));
    }
    return 0;
}

// 4.3.
// Scanning every reference for an alias costs tens of milliseconds, and a location change tries dozens of aliases. Most
// conditions name what they need: a group of the list that only ANDs on HasRefType, GetInFaction or GetIsID == 1, on the
// subject, can only be true of those references. The conditions still decide; this only says whom to ask.
bool QuestSystem::alias_candidates(const Alias& a, std::vector<uint32_t>& out) {
    constexpr uint16_t k_get_in_cell = 67, k_get_in_faction = 71, k_get_is_id = 72, k_get_is_reference = 136,
                       k_has_ref_type = 561;
    bool found = false;
    std::vector<uint32_t> group;
    bool usable_group = true;
    const auto add = [&](const ConditionItem& c) {
        if (c.run_on != RunOn::subject || (c.flags & (condition_aliases | condition_global | condition_swap)) || (c.flags >> 5) != 0 ||
            c.value != 1.0f || !c.param1) {
            return false;
        }
        switch (c.function) {
        case k_has_ref_type: {
            const auto& refs = m_world.refs_of_type(c.param1);
            group.insert(group.end(), refs.begin(), refs.end());
            return true;
        }
        case k_get_is_reference:
            group.push_back(c.param1);
            return true;
        case k_get_in_cell: { // a reference's cell is its record's (Conditions::cell_of)
            const auto& refs = m_world.refs_in_cell(c.param1);
            group.insert(group.end(), refs.begin(), refs.end());
            return true;
        }
        case k_get_is_id: {
            const auto& refs = m_world.refs_of_base(c.param1);
            group.insert(group.end(), refs.begin(), refs.end());
            return true;
        }
        case k_get_in_faction:
            for (const uint32_t base : m_world.bases_in_faction(c.param1)) {
                const auto& refs = m_world.refs_of_base(base);
                group.insert(group.end(), refs.begin(), refs.end());
            }
            if (m_world.live.faction_changes) {
                const std::vector<uint32_t> changed = m_world.live.faction_changes(c.param1);
                group.insert(group.end(), changed.begin(), changed.end());
            }
            return true;
        default: return false;
        }
    };
    const auto close_group = [&] {
        if (usable_group && (!found || group.size() < out.size())) {
            out = group;
            found = true;
        }
        group.clear();
        usable_group = true;
    };
    for (const ConditionItem& c : a.conditions) {
        usable_group = add(c) && usable_group;
        if (!(c.flags & condition_or)) {
            close_group();
        }
    }
    // The list can end on an OR, which leaves its group open: it counts like any other (Conditions::evaluate).
    if (!a.conditions.empty() && (a.conditions.back().flags & condition_or)) {
        close_group();
    }
    if (found) {
        std::ranges::sort(out);
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
    return found;
}

// A condition of its own (not in an OR group) that only an actor can meet.
bool QuestSystem::only_actors(const Alias& a) {
    bool open = false;
    for (const ConditionItem& c : a.conditions) {
        if (!open && !(c.flags & condition_or) && Conditions::needs_actor(c)) {
            return true;
        }
        open = (c.flags & condition_or) != 0;
    }
    return false;
}

uint32_t QuestSystem::fill_ref(Quest& q, Alias& a, const StoryEvent* event) {
    const Conditions::QuestScope scope(m_conditions, q.form);
    auto candidate_ok = [&](uint32_t ref) { return m_conditions.evaluate(a.conditions, ref, 0); };
    auto gate = [&] { return m_conditions.evaluate(a.conditions, k_player, 0); };
    switch (a.fill) {
    case 0: {
        // A list that fails whoever is asked, as when a global is off, has no need of the tens of thousands of loaded refs.
        if (m_conditions.fails_for_all(a.conditions)) {
            return 0;
        }
        std::vector<uint32_t> list;
        auto scan = [&](const std::vector<uint32_t>& refs) {
            for (uint32_t ref : refs) {
                if (ref != k_player && usable(a, ref) && candidate_ok(ref)) {
                    list.push_back(ref);
                }
            }
        };
        std::vector<uint32_t> named;
        if (alias_candidates(a, named)) {
            // Those of them the two passes would have reached.
            std::vector<uint32_t> reached;
            const bool loaded_only = (a.flags & alias_in_loaded_area) != 0;
            bool have_loaded = false;
            std::unordered_set<uint32_t> loaded;
            for (const uint32_t ref : named) {
                if (!loaded_only && m_world.is_persistent(ref)) {
                    reached.push_back(ref);
                    continue;
                }
                if (!have_loaded) {
                    have_loaded = true;
                    const std::vector<uint32_t> refs = m_world.loaded_refs();
                    loaded.insert(refs.begin(), refs.end());
                }
                if (loaded.contains(ref)) {
                    reached.push_back(ref);
                }
            }
            scan(reached);
        } else if (only_actors(a)) {
            scan(m_world.loaded_actors());
            if (!(a.flags & alias_in_loaded_area)) {
                scan(m_world.persistent_actors());
            }
        } else {
            scan(m_world.loaded_refs());
            if (!(a.flags & alias_in_loaded_area)) {
                scan(m_world.persistent_refs());
            }
        }
        if (a.flags & alias_closest) {
            std::ranges::sort(list, {}, [this](uint32_t r) { return m_world.distance_to_player(r); });
            for (uint32_t ref : list) {
                if ((a.flags & alias_allow_reuse) || !in_aliases(q, ref)) {
                    return ref;
                }
            }
            return 0;
        }
        return pick(q, a, std::move(list));
    }
    case 1: {
        const RefState s = m_world.ref_state(a.specific);
        if (!a.specific || !s.exists || s.deleted || !candidate_ok(a.specific)) {
            return 0;
        }
        return !(a.flags & alias_allow_reserved) && reserved(a.specific) ? 0 : a.specific;
    }
    case 2: {
        if (!gate()) {
            return 0;
        }
        auto it = q.filled_locations.find(static_cast<uint32_t>(a.other_alias));
        if (it == q.filled_locations.end()) {
            return 0;
        }
        std::vector<uint32_t> list;
        for (uint32_t ref : m_world.location_refs(it->second, a.ref_type)) {
            if (usable(a, ref)) {
                list.push_back(ref);
            }
        }
        return pick(q, a, std::move(list));
    }
    case 3: {
        if (!event || event->type != a.event) {
            return 0;
        }
        const StoryEvent::Member* m = event->member(a.event_member);
        if (!m || !m->value || !candidate_ok(m->value)) {
            return 0;
        }
        return !(a.flags & alias_allow_reserved) && reserved(m->value) ? 0 : m->value;
    }
    case 4:
        // A stub until promotion (4.7): the anchor must hold a reference; the object stands in.
        return gate() && q.filled_refs.contains(static_cast<uint32_t>(a.create_at)) ? a.create_object : 0;
    case 5: {
        Quest* other = quest(a.external_quest);
        if (!other) {
            return 0;
        }
        auto it = other->filled_refs.find(static_cast<uint32_t>(a.external_alias));
        return it != other->filled_refs.end() && candidate_ok(it->second) ? it->second : 0;
    }
    case 6: {
        if (!gate()) {
            return 0;
        }
        const uint32_t ref = m_world.ref_of_unique_npc(a.unique_actor);
        const RefState s = m_world.ref_state(ref);
        if (!ref || s.deleted || (s.dead && !(a.flags & alias_allow_dead)) || (s.disabled && !(a.flags & alias_allow_disabled)) ||
            (!(a.flags & alias_allow_reserved) && reserved(ref))) {
            return 0;
        }
        return ref;
    }
    case 7: {
        auto it = q.filled_refs.find(static_cast<uint32_t>(a.near_alias));
        if (it == q.filled_refs.end()) {
            return 0;
        }
        std::vector<uint32_t> list;
        for (uint32_t ref : m_world.linked_children(it->second)) {
            if (ref != k_player && usable(a, ref) && candidate_ok(ref)) {
                list.push_back(ref);
            }
        }
        return pick(q, a, std::move(list));
    }
    default: return 0;
    }
}

// 4.5, and 6.1 of.
uint32_t QuestSystem::fill_location(Quest& q, Alias& a, const StoryEvent* event) {
    const Conditions::QuestScope scope(m_conditions, q.form);
    auto location_ok = [&](uint32_t location) {
        const auto* e = m_db.find(location);
        return e && !(e->flags & forms::form_deleted) && ((a.flags & alias_allow_reserved) || !reserved(location));
    };
    auto cleared_ok = [&](uint32_t location) { return (a.flags & alias_allow_cleared) || !m_world.location_cleared(location); };
    switch (a.fill) {
    case 0: {
        std::vector<uint32_t> fresh, cleared;
        for (const forms::FormEntry* e : m_db.of_type(forms::sig("LCTN"))) {
            const uint32_t location = e->form_id;
            if (location_ok(location) && cleared_ok(location) && m_conditions.evaluate(a.conditions, location, 0)) {
                (m_world.location_cleared(location) ? cleared : fresh).push_back(location);
            }
        }
        std::vector<uint32_t>& list = fresh.empty() ? cleared : fresh;
        while (!list.empty()) {
            const size_t i = std::uniform_int_distribution<size_t>(0, list.size() - 1)(m_random);
            const uint32_t location = list[i];
            const bool held = std::ranges::any_of(q.filled_locations, [location](const auto& kv) { return kv.second == location; });
            if ((a.flags & alias_allow_reuse) || !held) {
                return location;
            }
            list.erase(list.begin() + static_cast<ptrdiff_t>(i));
        }
        return 0;
    }
    case 1: return location_ok(a.specific) ? a.specific : 0;
    case 2: {
        auto it = q.filled_refs.find(static_cast<uint32_t>(a.other_alias));
        const uint32_t location = it == q.filled_refs.end() ? 0 : m_world.location_parent_with_keyword(m_world.ref_location(it->second), a.keyword);
        return location && location_ok(location) && cleared_ok(location) ? location : 0;
    }
    case 3: {
        if (!event || event->type != a.event) {
            return 0;
        }
        const StoryEvent::Member* m = event->member(a.event_member);
        return m && m->value && location_ok(m->value) && cleared_ok(m->value) ? m->value : 0;
    }
    case 4: {
        Quest* other = quest(a.external_quest);
        if (!other) {
            return 0;
        }
        auto it = other->filled_locations.find(static_cast<uint32_t>(a.external_alias));
        return it == other->filled_locations.end() ? 0 : it->second;
    }
    default: return 0;
    }
}

void QuestSystem::clear_filled(Quest& q) {
    q.filled_refs.clear();
    q.filled_locations.clear();
    q.event.reset();
}

// 4.2. Fills here finish in one call: nothing yet makes a fill slow enough to slice.
QuestSystem::Fill QuestSystem::fill_aliases(Quest& q, const StoryEvent* event) {
    if (!q.filled_refs.empty() || !q.filled_locations.empty() || q.event) {
        return Fill::ok;
    }
    struct Context {
        Conditions& conditions;
        ~Context() { conditions.set_aliases(nullptr, nullptr); }
    } context{m_conditions};
    m_conditions.set_aliases(&q.filled_refs, &q.filled_locations);
    q.fill_order = ++m_fills;
    for (Alias& a : q.aliases) {
        if (a.location) {
            if (const uint32_t location = fill_location(q, a, event)) {
                q.filled_locations[a.id] = location;
                if (a.force_into >= 0) {
                    q.filled_locations[static_cast<uint32_t>(a.force_into)] = location;
                }
            } else if (!(a.flags & alias_optional)) {
                q.fill_failure = std::format("{} (fill {})", a.name, a.fill);
                clear_filled(q);
                return Fill::failed;
            }
            continue;
        }
        if (q.filled_refs.contains(a.id) || (a.flags & alias_filled_by_other)) {
            continue;
        }
        if (const uint32_t ref = fill_ref(q, a, event)) {
            q.filled_refs[a.id] = ref;
            if (a.force_into >= 0) {
                q.filled_refs[static_cast<uint32_t>(a.force_into)] = ref;
            }
        } else if (!(a.flags & alias_optional)) {
            q.fill_failure = std::format("{} (fill {}, conditions", a.name, a.fill);
            for (const ConditionItem& c : a.conditions) {
                q.fill_failure += std::format(" {}{}", c.function, Conditions::available(c.function) ? "" : "?");
            }
            q.fill_failure += ")";
            clear_filled(q);
            return Fill::failed;
        }
    }
    // Aliases filled only by another's ALFI, and not optional, must be targets of a filled alias.
    for (const Alias& a : q.aliases) {
        if ((a.flags & alias_filled_by_other) && !(a.flags & alias_optional) && !a.location && !q.filled_refs.contains(a.id)) {
            clear_filled(q);
            return Fill::failed;
        }
    }
    update_reservations(q, 1);
    if (event) {
        q.event = *event;
    }
    return Fill::ok;
}

void QuestSystem::update_reservations(Quest& q, int delta) {
    auto adjust = [&](uint32_t form) {
        if (delta > 0) {
            ++m_reservations[form];
        } else if (auto it = m_reservations.find(form); it != m_reservations.end() && --it->second == 0) {
            m_reservations.erase(it);
        }
    };
    for (const Alias& a : q.aliases) {
        if (!(a.flags & alias_reserves) || a.fill == 4) {
            continue;
        }
        if (a.location) {
            if (auto it = q.filled_locations.find(a.id); it != q.filled_locations.end()) {
                adjust(it->second);
            }
        } else if (auto it = q.filled_refs.find(a.id); it != q.filled_refs.end()) {
            adjust(it->second);
        }
    }
}

void QuestSystem::release_aliases(Quest& q) {
    update_reservations(q, -1);
    for (const Alias& a : q.aliases) {
        if (!a.location) {
            if (auto it = q.filled_refs.find(a.id); it != q.filled_refs.end()) {
                m_world.alias_removed(q, a, it->second);
            }
        }
    }
    q.filled_refs.clear();
    q.filled_locations.clear();
    q.event.reset();
}

void QuestSystem::clear_ref(Quest& q, Alias& a) {
    auto it = q.filled_refs.find(a.id);
    if (it == q.filled_refs.end()) {
        return;
    }
    const uint32_t ref = it->second;
    q.filled_refs.erase(it);
    m_scenes->ref_left(q, ref);
    m_world.alias_removed(q, a, ref);
    if (a.flags & alias_reserves) {
        if (auto r = m_reservations.find(ref); r != m_reservations.end() && --r->second == 0) {
            m_reservations.erase(r);
        }
    }
}

void QuestSystem::force_ref(Quest& q, Alias& a, uint32_t ref) {
    auto it = q.filled_refs.find(a.id);
    if (a.location || !ref || (it != q.filled_refs.end() && it->second == ref)) {
        return;
    }
    clear_ref(q, a);
    q.filled_refs[a.id] = ref;
    update_alias_text(q, a);
    m_world.alias_applied(q, a, ref);
    if (a.flags & alias_reserves) {
        ++m_reservations[ref];
    }
    m_scenes->alias_changed(q, a.id);
}

void QuestSystem::clear_location(Quest& q, Alias& a) {
    auto it = q.filled_locations.find(a.id);
    if (it == q.filled_locations.end()) {
        return;
    }
    if (a.flags & alias_reserves) {
        if (auto r = m_reservations.find(it->second); r != m_reservations.end() && --r->second == 0) {
            m_reservations.erase(r);
        }
    }
    q.filled_locations.erase(it);
}

void QuestSystem::force_location(Quest& q, Alias& a, uint32_t location) {
    if (!a.location || !location) {
        return;
    }
    clear_location(q, a);
    q.filled_locations[a.id] = location;
    update_alias_text(q, a);
    if (a.flags & alias_reserves) {
        ++m_reservations[location];
    }
}

// ---- natives (section 6) --------------------------------------------------------------------------------------------

void QuestSystem::bind_natives() {
    using script::NativeCall;
    using script::Value;
    auto add = [this](const char* cls, const char* name, script::NativeFunction fn) {
        m_vm.bind_native(cls, name, std::move(fn), script::native_flags(cls, name));
    };
    auto self = [this](NativeCall& c) { return quest(script::handle_form(c.self->handle)); };
    auto quest_of = [this](const Value& v) -> Quest* {
        return v.tag == script::Tag::object && v.object ? quest(script::handle_form(v.object->handle)) : nullptr;
    };
    auto form_of = [](const Value& v) -> uint32_t {
        return v.tag == script::Tag::object && v.object ? script::handle_form(v.object->handle) : 0;
    };
    auto alias_of = [this](NativeCall& c, Quest*& q) -> Alias* {
        q = quest(script::handle_form(c.self->handle));
        return q ? q->alias(script::handle_alias(c.self->handle)) : nullptr;
    };

    auto flag_query = [&add, self](const char* name, bool (*test)(const Quest&)) {
        add("Quest", name, [self, test](NativeCall& c) {
            const Quest* q = self(c);
            c.result = Value::of(q && test(*q));
        });
    };
    flag_query("IsRunning", [](const Quest& q) { return q.is_running(); });
    flag_query("IsStarting", [](const Quest& q) { return q.is_starting(); });
    flag_query("IsStopping", [](const Quest& q) { return q.is_stopping(); });
    flag_query("IsStopped", [](const Quest& q) { return q.is_stopped(); });
    flag_query("IsActive", [](const Quest& q) { return (q.flags & quest_active) != 0; });
    flag_query("IsCompleted", [](const Quest& q) { return (q.flags & quest_completed) != 0; });
    add("Quest", "GetCurrentStageID", [self](NativeCall& c) {
        const Quest* q = self(c);
        c.result = Value::of(q ? static_cast<int32_t>(q->current_stage) : 0);
    });
    add("Quest", "IsStageDone", [self](NativeCall& c) {
        Quest* q = self(c);
        const int32_t i = c.args[0].integer;
        if (i < 0 || i > 65535) {
            c.vm.log_error(c.stack, std::format("stage index {} is out of range", i));
            return;
        }
        const Stage* s = q ? q->stage(static_cast<uint16_t>(i)) : nullptr;
        c.result = Value::of(s && (s->flags & stage_done));
    });
    add("Quest", "CompleteQuest", [this, self](NativeCall& c) { set_completed(*self(c), true); });
    add("Quest", "CompleteAllObjectives", [this, self](NativeCall& c) { all_objectives(*self(c), true); });
    add("Quest", "FailAllObjectives", [this, self](NativeCall& c) { all_objectives(*self(c), false); });
    add("Quest", "SetActive", [self](NativeCall& c) {
        Quest* q = self(c);
        q->flags = static_cast<uint16_t>(c.args[0].boolean ? q->flags | quest_active : q->flags & ~quest_active);
    });
    add("Quest", "Stop", [this, self](NativeCall& c) { stop(*self(c)); });
    add("Quest", "Reset", [this, self](NativeCall& c) { reset(*self(c)); });
    add("Quest", "SetObjectiveDisplayed", [this, self](NativeCall& c) {
        set_objective_displayed(*self(c), static_cast<uint16_t>(c.args[0].integer), c.args[1].boolean, c.args[2].boolean);
    });
    add("Quest", "SetObjectiveCompleted", [this, self](NativeCall& c) {
        set_objective_completed(*self(c), static_cast<uint16_t>(c.args[0].integer), c.args[1].boolean);
    });
    add("Quest", "SetObjectiveFailed", [this, self](NativeCall& c) {
        set_objective_failed(*self(c), static_cast<uint16_t>(c.args[0].integer), c.args[1].boolean);
    });
    auto objective_query = [&add, self](const char* name, bool (*test)(uint8_t)) {
        add("Quest", name, [self, test, name](NativeCall& c) {
            const Quest* q = self(c);
            const auto index = static_cast<uint16_t>(c.args[0].integer);
            const Objective* o = nullptr;
            if (q) {
                for (const Objective& x : q->objectives) {
                    if (x.index == index) {
                        o = &x;
                    }
                }
            }
            if (!o) {
                c.vm.log_error(c.stack, std::format("unknown quest objective {}", c.args[0].integer));
            }
            c.result = Value::of(o && test(o->state));
        });
    };
    objective_query("IsObjectiveDisplayed", [](uint8_t s) { return s == 1 || s == 3 || s == 5; });
    objective_query("IsObjectiveCompleted", [](uint8_t s) { return s == 2 || s == 3; });
    objective_query("IsObjectiveFailed", [](uint8_t s) { return s == 4 || s == 5; });
    add("Quest", "GetAlias", [this, self](NativeCall& c) {
        Quest* q = self(c);
        const Alias* a = q ? q->alias(static_cast<uint32_t>(c.args[0].integer)) : nullptr;
        if (a) {
            script::Class* type = m_vm.find_class(a->location ? "LocationAlias" : "ReferenceAlias");
            if (script::Object* o = m_vm.object_for(alias_handle(*q, *a), type)) {
                c.result = Value::of_object(o, m_vm.find_class("Alias"));
            }
        }
    });
    add("Quest", "Start", [this, self](NativeCall& c) {
        Quest* q = self(c);
        if (q->story_event != 0) {
            c.vm.log_error(c.stack, "attempting to start event scoped quest outside of story manager");
            c.vm.return_from_latent(c.stack, Value::of(false));
            return;
        }
        bool pending = false;
        if (!start(*q, false, &pending)) {
            c.vm.return_from_latent(c.stack, Value::of(false));
        } else if (!pending) {
            c.vm.return_from_latent(c.stack, Value::of(true));
        } else {
            m_waiters.push_back({c.stack, q->form, -1, 0, true});
        }
    });
    add("Quest", "SetCurrentStageID", [this, self](NativeCall& c) {
        Quest* q = self(c);
        const auto index = static_cast<uint16_t>(c.args[0].integer);
        if (!q->running_bit() && q->story_event != 0) {
            c.vm.log_error(c.stack, "attempting to start event scoped quest outside of story manager");
            c.vm.return_from_latent(c.stack, Value::of(false));
            return;
        }
        bool pending = false;
        if (!start(*q, false, &pending)) {
            c.vm.return_from_latent(c.stack, Value::of(false));
            return;
        }
        if (pending || q->wait_bit() || q->promoting) {
            if (pending) {
                m_waiters.push_back({c.stack, q->form, index, 0, false});
                return;
            }
        }
        const Stage* s = q->stage(index);
        const uint32_t items = s ? static_cast<uint32_t>(s->items.size()) : 0;
        if (items == 0) {
            set_stage(*q, index);
            c.vm.return_from_latent(c.stack, Value::of(false));
            return;
        }
        m_waiters.push_back({c.stack, q->form, index, items, false});
        if (!set_stage(*q, index)) {
            std::erase_if(m_waiters, [&](const Waiter& w) { return w.stack == c.stack; });
            c.vm.return_from_latent(c.stack, Value::of(false));
        }
    });

    add("Alias", "GetOwningQuest", [this](NativeCall& c) { c.result = c.vm.form_value(script::handle_form(c.self->handle), "Quest"); });
    add("ReferenceAlias", "GetReference", [this, alias_of](NativeCall& c) {
        Quest* q = nullptr;
        if (const Alias* a = alias_of(c, q)) {
            if (auto it = q->filled_refs.find(a->id); it != q->filled_refs.end()) {
                c.result = c.vm.form_value(it->second, "ObjectReference");
            }
        }
    });
    add("ReferenceAlias", "ForceRefTo", [this, alias_of, form_of](NativeCall& c) {
        Quest* q = nullptr;
        Alias* a = alias_of(c, q);
        const uint32_t ref = form_of(c.args[0]);
        if (!ref) {
            c.vm.log_error(c.stack, "Cannot force the alias's reference to a None reference");
        } else if (a) {
            force_ref(*q, *a, ref);
        }
    });
    add("ReferenceAlias", "Clear", [this, alias_of](NativeCall& c) {
        Quest* q = nullptr;
        if (Alias* a = alias_of(c, q)) {
            if (a->flags & alias_optional) {
                clear_ref(*q, *a);
            } else {
                c.vm.log_error(c.stack, "Cannot clear a non-optional alias");
            }
        }
    });
    add("LocationAlias", "GetLocation", [this, alias_of](NativeCall& c) {
        Quest* q = nullptr;
        if (const Alias* a = alias_of(c, q)) {
            if (auto it = q->filled_locations.find(a->id); it != q->filled_locations.end()) {
                c.result = c.vm.form_value(it->second, "Location");
            }
        }
    });
    add("LocationAlias", "ForceLocationTo", [this, alias_of, form_of](NativeCall& c) {
        Quest* q = nullptr;
        Alias* a = alias_of(c, q);
        const uint32_t location = form_of(c.args[0]);
        if (!location) {
            c.vm.log_error(c.stack, "Cannot force the alias's location to a None location");
        } else if (a) {
            force_location(*q, *a, location);
        }
    });
    add("LocationAlias", "Clear", [this, alias_of](NativeCall& c) {
        Quest* q = nullptr;
        if (Alias* a = alias_of(c, q)) {
            if (a->flags & alias_optional) {
                clear_location(*q, *a);
            } else {
                c.vm.log_error(c.stack, "Cannot clear a non-optional alias");
            }
        }
    });
    (void)quest_of;
    m_story->bind_natives();
    m_scenes->bind_natives();
}

// ---- save -----------------------------------------------------------------------------------------------------------

std::vector<std::byte> QuestSystem::save() const {
    BinaryWriter w;
    w.write<uint32_t>(1);
    w.write<uint8_t>(m_ready ? 1 : 0);
    w.write<uint32_t>(static_cast<uint32_t>(m_quests.size()));
    for (const auto& q : m_quests) {
        w.write<uint32_t>(q->form);
        w.write<uint16_t>(q->flags);
        w.write<uint16_t>(q->current_stage);
        w.write<uint8_t>(static_cast<uint8_t>(q->has_run | q->promoting << 1));
        w.write<uint32_t>(q->instance);
        w.write<uint32_t>(static_cast<uint32_t>(q->stages.size()));
        for (const Stage& s : q->stages) {
            w.write<uint8_t>(s.flags);
        }
        w.write<uint32_t>(static_cast<uint32_t>(q->objectives.size()));
        for (const Objective& o : q->objectives) {
            w.write<uint8_t>(o.state);
        }
        auto map = [&w](const std::unordered_map<uint32_t, uint32_t>& m) {
            w.write<uint32_t>(static_cast<uint32_t>(m.size()));
            for (const auto& [k, v] : m) {
                w.write<uint32_t>(k);
                w.write<uint32_t>(v);
            }
        };
        map(q->filled_refs);
        map(q->filled_locations);
        w.write<uint32_t>(static_cast<uint32_t>(q->outstanding.size()));
        for (const auto& [stage, item] : q->outstanding) {
            w.write<uint16_t>(stage);
            w.write<uint32_t>(item);
        }
    }
    auto list = [&w](const std::vector<Quest*>& l) {
        w.write<uint32_t>(static_cast<uint32_t>(l.size()));
        for (const Quest* q : l) {
            w.write<uint32_t>(q->form);
        }
    };
    list(m_starting);
    list(m_running);
    list(m_stopping);
    w.write<uint32_t>(static_cast<uint32_t>(m_reservations.size()));
    for (const auto& [k, v] : m_reservations) {
        w.write<uint32_t>(k);
        w.write<uint32_t>(v);
    }
    // What the journal reads: the player's objectives and the instance records. Older saves end above.
    w.write<uint32_t>(static_cast<uint32_t>(m_player_objectives.size()));
    for (const PlayerObjective& p : m_player_objectives) {
        w.write<uint32_t>(p.quest);
        w.write<uint16_t>(p.index);
        w.write<uint32_t>(p.instance);
        w.write<uint8_t>(p.state);
    }
    uint32_t kept = 0;
    for (const auto& q : m_quests) {
        kept += q->instances.empty() ? 0 : 1;
    }
    w.write<uint32_t>(kept);
    for (const auto& q : m_quests) {
        if (q->instances.empty()) {
            continue;
        }
        w.write<uint32_t>(q->form);
        w.write<uint32_t>(static_cast<uint32_t>(q->instances.size()));
        for (const InstanceText& t : q->instances) {
            w.write<uint32_t>(t.id);
            w.write<uint32_t>(static_cast<uint32_t>(t.forms.size()));
            for (const auto& [alias, form] : t.forms) {
                w.write<uint32_t>(alias);
                w.write<uint32_t>(form);
            }
            w.write<uint32_t>(static_cast<uint32_t>(t.globals.size()));
            for (const auto& [global, value] : t.globals) {
                w.write<uint32_t>(global);
                w.write<float>(value);
            }
            w.write<uint32_t>(static_cast<uint32_t>(t.log.size()));
            w.write_string(t.log);
        }
    }
    return w.take();
}

void QuestSystem::load(std::span<const std::byte> bytes) {
    BinaryReader r(bytes);
    if (r.read<uint32_t>() != 1) {
        throw ParseError("quest save: unknown version");
    }
    m_ready = r.read<uint8_t>() != 0;
    const uint32_t count = r.read<uint32_t>();
    for (uint32_t i = 0; i < count; ++i) {
        Quest scratch;
        Quest* q = quest(r.read<uint32_t>());
        Quest& t = q ? *q : scratch;
        t.flags = r.read<uint16_t>();
        t.current_stage = r.read<uint16_t>();
        const uint8_t bits = r.read<uint8_t>();
        t.has_run = bits & 1;
        t.promoting = bits & 2;
        t.instance = r.read<uint32_t>();
        const uint32_t stages = r.read<uint32_t>();
        for (uint32_t k = 0; k < stages; ++k) {
            const uint8_t flags = r.read<uint8_t>();
            if (k < t.stages.size()) {
                t.stages[k].flags = flags;
            }
        }
        const uint32_t objectives = r.read<uint32_t>();
        for (uint32_t k = 0; k < objectives; ++k) {
            const uint8_t state = r.read<uint8_t>();
            if (k < t.objectives.size()) {
                t.objectives[k].state = state;
            }
        }
        auto map = [&r](std::unordered_map<uint32_t, uint32_t>& m) {
            m.clear();
            const uint32_t n = r.read<uint32_t>();
            for (uint32_t k = 0; k < n; ++k) {
                const uint32_t key = r.read<uint32_t>();
                m[key] = r.read<uint32_t>();
            }
        };
        map(t.filled_refs);
        map(t.filled_locations);
        t.outstanding.clear();
        const uint32_t outstanding = r.read<uint32_t>();
        for (uint32_t k = 0; k < outstanding; ++k) {
            const uint16_t stage = r.read<uint16_t>();
            t.outstanding.emplace_back(stage, r.read<uint32_t>());
        }
    }
    auto list = [&](std::vector<Quest*>& l) {
        l.clear();
        const uint32_t n = r.read<uint32_t>();
        for (uint32_t k = 0; k < n; ++k) {
            if (Quest* q = quest(r.read<uint32_t>())) {
                l.push_back(q);
            }
        }
    };
    list(m_starting);
    list(m_running);
    list(m_stopping);
    m_reservations.clear();
    const uint32_t n = r.read<uint32_t>();
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t key = r.read<uint32_t>();
        m_reservations[key] = r.read<uint32_t>();
    }
    m_player_objectives.clear();
    for (const auto& q : m_quests) {
        q->instances.clear();
    }
    if (r.eof()) {
        return;
    }
    for (uint32_t count = r.read<uint32_t>(); count > 0; --count) {
        PlayerObjective p;
        p.quest = r.read<uint32_t>();
        p.index = r.read<uint16_t>();
        p.instance = r.read<uint32_t>();
        p.state = r.read<uint8_t>();
        m_player_objectives.push_back(p);
    }
    for (uint32_t count = r.read<uint32_t>(); count > 0; --count) {
        Quest scratch;
        Quest* q = quest(r.read<uint32_t>());
        Quest& t = q ? *q : scratch;
        for (uint32_t records = r.read<uint32_t>(); records > 0; --records) {
            InstanceText record;
            record.id = r.read<uint32_t>();
            for (uint32_t k = r.read<uint32_t>(); k > 0; --k) {
                const uint32_t alias = r.read<uint32_t>();
                record.forms.emplace_back(alias, r.read<uint32_t>());
            }
            for (uint32_t k = r.read<uint32_t>(); k > 0; --k) {
                const uint32_t global = r.read<uint32_t>();
                record.globals.emplace_back(global, r.read<float>());
            }
            const uint32_t length = r.read<uint32_t>();
            record.log.resize(length);
            for (char& c : record.log) {
                c = char(r.read<uint8_t>());
            }
            t.instances.push_back(std::move(record));
        }
    }
}

bool QuestSystem::alias_has_keyword(uint32_t ref, uint32_t keyword) const {
    for (const auto& q : m_quests) {
        if (!q->running_bit() || q->filled_refs.empty()) {
            continue;
        }
        for (const Alias& a : q->aliases) {
            const auto it = q->filled_refs.find(a.id);
            if (it != q->filled_refs.end() && it->second == ref &&
                std::find(a.keywords.begin(), a.keywords.end(), keyword) != a.keywords.end()) {
                return true;
            }
        }
    }
    return false;
}

// Every actor's thinking asks: one pass over the quests a tick, not one an actor. Quests change in update(), after the AI.
std::vector<uint32_t> QuestSystem::alias_packages(uint32_t ref) const {
    if (m_alias_packages_stale) {
        m_alias_packages_stale = false;
        m_alias_packages.clear();
        // An actor's aliases count in the order they were filled.
        std::vector<const Quest*> running;
        for (const auto& q : m_quests) {
            if (q->running_bit() && !q->filled_refs.empty()) {
                running.push_back(q.get());
            }
        }
        std::ranges::sort(running, {}, &Quest::fill_order);
        for (const Quest* q : running) {
            for (const Alias& a : q->aliases) {
                const auto it = q->filled_refs.find(a.id);
                if (it != q->filled_refs.end() && !a.packages.empty()) {
                    auto& out = m_alias_packages[it->second];
                    out.insert(out.end(), a.packages.begin(), a.packages.end());
                }
            }
        }
    }
    const auto it = m_alias_packages.find(ref);
    return it != m_alias_packages.end() ? it->second : std::vector<uint32_t>{};
}

} // namespace spud::game
