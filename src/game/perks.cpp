#include "game/perks.hpp"

#include "game/actor_values.hpp"

#include <algorithm>

namespace spud::game {

namespace {

using forms::Value;

constexpr uint8_t k_arguments[k_entry_point_count] = {
    3, 3, 3, 2, 3, 1, 1, 2, 2, 2, 1, 1, 1, 1, 2, 1, 1, 3, 3, 1, 2, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 2, 2, 2, 3,
    3, 3, 2, 1, 1, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 3, 2, 3, 1, 2, 3, 2, 1, 2, 2, 3, 1, 1, 1, 1, 1, 3, 2, 1, 2, 2,
    2, 1, 2, 2, 2, 3, 3, 3, 1, 2, 1, 3, 3, 2, 2, 2, 2, 2, 2, 2,
};

EntryKind kind_of(int entry_point) {
    switch (entry_point) {
    case 9: return EntryKind::leveled_list;
    case 14: return EntryKind::activate;
    case 51: case 52: case 53: case 67: case 69: return EntryKind::spell;
    case 54: return EntryKind::graph_bool;
    case 81: return EntryKind::text;
    default: return EntryKind::value;
    }
}

int64_t integer(const Value* v, std::string_view path) {
    const Value* m = v ? v->path(path) : nullptr;
    return m ? m->integer : 0;
}

float real(const Value& v) { return float(v.kind == Value::Kind::real ? v.real : double(v.integer)); }

void read_parameters(const Value& v, PerkEntry& e) {
    switch (e.data_type) {
    case 1: e.value1 = real(v); break;
    case 2:
        if (const Value* a = v.member(e.function == 5 || e.function >= 12 ? "actor_value" : "value1")) {
            e.value1 = real(*a);
        }
        if (const Value* b = v.member(e.function == 5 || e.function >= 12 ? "value" : "value2")) {
            e.value2 = real(*b);
        }
        break;
    case 3: case 4: case 5: e.form = v.form; break;
    case 6: case 7: e.text = v.text; break;
    default: break;
    }
}

} // namespace

const EntryPointInfo& entry_point_info(int entry_point) {
    static const auto table = [] {
        std::array<EntryPointInfo, k_entry_point_count> t{};
        for (int i = 0; i < k_entry_point_count; ++i) {
            t[size_t(i)] = {k_arguments[i], kind_of(i)};
        }
        return t;
    }();
    return table[size_t(std::clamp(entry_point, 0, k_entry_point_count - 1))];
}

const Perk* Perks::find(uint32_t form) const {
    if (const auto it = m_perks.find(form); it != m_perks.end()) {
        return it->second.get();
    }
    std::unique_ptr<Perk> out;
    const forms::FormEntry* entry = m_db->find(form);
    if (entry && entry->type == forms::sig("PERK")) {
        const forms::Form f(*m_db, *entry);
        out = std::make_unique<Perk>();
        out->form = form;
        out->name = f.name();
        out->description = f.description();
        PerkEntry* e = nullptr;
        uint8_t tab = 0;
        // The fields in order: one record holds the perk's own conditions, then each entry's tabs.
        for (const forms::DecodedField& field : f.fields()) {
            const Value& v = field.value;
            switch (field.sig) {
            case forms::sig("PRKE"):
                e = &out->entries.emplace_back();
                e->perk = form;
                e->type = PerkEntry::Type(integer(&v, "type"));
                e->rank = uint8_t(integer(&v, "rank"));
                e->priority = uint8_t(integer(&v, "priority"));
                tab = 0;
                break;
            case forms::sig("DATA"):
                if (!e) {
                    out->level = int8_t(integer(&v, "level"));
                    out->ranks = uint8_t(integer(&v, "num_ranks"));
                    out->playable = integer(&v, "playable") != 0;
                    out->hidden = integer(&v, "hidden") != 0;
                } else if (e->type == PerkEntry::quest) {
                    if (const Value* q = v.member("quest")) {
                        e->form = q->form;
                    }
                    e->stage = uint16_t(integer(&v, "stage"));
                } else if (e->type == PerkEntry::ability) {
                    e->form = v.form;
                } else {
                    e->point = uint8_t(integer(&v, "entry_point"));
                    e->function = uint8_t(integer(&v, "function"));
                    e->tab_count = uint8_t(integer(&v, "condition_tab_count"));
                    e->tabs.resize(e->tab_count);
                }
                break;
            case forms::sig("NNAM"): out->next = v.form; break;
            case forms::sig("PRKC"): tab = uint8_t(v.integer); break;
            case forms::sig("CTDA"):
                if (!e) {
                    out->conditions.push_back(condition_item(v));
                } else if (tab < e->tabs.size()) {
                    e->tabs[tab].push_back(condition_item(v));
                }
                break;
            case forms::sig("CIS1"): case forms::sig("CIS2"):
                if (e && tab < e->tabs.size() && !e->tabs[tab].empty()) {
                    condition_text(e->tabs[tab].back(), v.text);
                } else if (!e && !out->conditions.empty()) {
                    condition_text(out->conditions.back(), v.text);
                }
                break;
            case forms::sig("EPFT"):
                if (e) {
                    e->data_type = uint8_t(v.integer);
                }
                break;
            case forms::sig("EPFD"):
                if (e) {
                    read_parameters(v, *e);
                }
                break;
            case forms::sig("EPF2"):
                if (e) {
                    e->text = v.text;
                }
                break;
            case forms::sig("EPF3"):
                if (e) {
                    e->script_flags = uint16_t(integer(&v, "flags"));
                    e->fragment = int32_t(integer(&v, "fragment_index"));
                }
                break;
            default: break;
            }
        }
        std::stable_sort(out->entries.begin(), out->entries.end(), [](const PerkEntry& a, const PerkEntry& b) {
            return a.rank != b.rank ? a.rank < b.rank : a.priority != b.priority ? a.priority > b.priority : a.type < b.type;
        });
    }
    return m_perks.emplace(form, std::move(out)).first->second.get();
}

uint32_t Perks::avif(int av) const {
    if (av >= k_first_skill_value && av < k_first_skill_value + k_skill_value_count) {
        return 0x446 + uint32_t(av); // AVOneHanded 0000044C
    }
    const char* name = av == av_werewolf_perks ? "AVWerewolfPerks" : av == av_vampire_perks ? "AVVampirePerks" : nullptr;
    if (name) {
        for (const forms::FormEntry* e : m_db->of_type(forms::sig("AVIF"))) {
            if (m_db->editor_id(*e) == name) {
                return e->form_id;
            }
        }
    }
    return 0;
}

const PerkTree* Perks::tree(int av) const {
    if (const auto it = m_trees.find(av); it != m_trees.end()) {
        return it->second.get();
    }
    std::unique_ptr<PerkTree> out;
    if (const forms::FormEntry* entry = m_db->find(avif(av))) {
        const forms::Form f(*m_db, *entry);
        out = std::make_unique<PerkTree>();
        out->actor_value = av;
        PerkTreeNode* n = nullptr;
        for (const forms::DecodedField& field : f.fields()) {
            const Value& v = field.value;
            switch (field.sig) {
            case forms::sig("PNAM"):
                n = &out->nodes.emplace_back();
                n->perk = v.form;
                break;
            case forms::sig("XNAM"):
                if (n) {
                    n->x = float(v.integer);
                }
                break;
            case forms::sig("YNAM"):
                if (n) {
                    n->y = float(v.integer);
                }
                break;
            case forms::sig("HNAM"):
                if (n) {
                    n->h = real(v);
                }
                break;
            case forms::sig("VNAM"):
                if (n) {
                    n->v = real(v);
                }
                break;
            case forms::sig("SNAM"):
                if (n) {
                    n->skill = v.form;
                }
                break;
            case forms::sig("CNAM"):
                if (n) {
                    n->children.push_back(uint32_t(v.integer));
                }
                break;
            case forms::sig("INAM"):
                if (n) {
                    n->index = uint32_t(v.integer);
                }
                break;
            default: break;
            }
        }
        for (size_t i = 1; i < out->nodes.size(); ++i) { // the root's grid bytes are stale
            out->max_grid_x = std::max(out->max_grid_x, int(out->nodes[i].x));
        }
    }
    return m_trees.emplace(av, std::move(out)).first->second.get();
}

const SkillData& Perks::skill(int av) const {
    if (const auto it = m_skills.find(av); it != m_skills.end()) {
        return it->second;
    }
    SkillData d;
    if (const forms::FormEntry* entry = m_db->find(avif(av))) {
        const forms::Form f(*m_db, *entry);
        d.name = f.name();
        d.description = f.description();
        if (const Value* s = f.get("AVSK")) {
            auto number = [s](const char* n, float fallback) {
                const Value* m = s->member(n);
                return m ? real(*m) : fallback;
            };
            d.use_mult = number("use_mult", 1);
            d.use_offset = number("offset_mult", 0);
            d.improve_mult = number("improve_mult", 1);
            d.improve_offset = number("improve_offset", 20);
        }
    }
    return m_skills.emplace(av, std::move(d)).first->second;
}

} // namespace spud::game
