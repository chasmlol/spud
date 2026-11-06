#include "formats/forms_typed.hpp"

#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace spud::forms {

namespace {

std::optional<int64_t> as_int(const Value* v) {
    if (!v) {
        return std::nullopt;
    }
    switch (v->kind) {
    case Value::Kind::integer: return v->integer;
    case Value::Kind::form: return v->form;
    case Value::Kind::real: return static_cast<int64_t>(v->real);
    default: return std::nullopt;
    }
}

std::optional<double> as_real(const Value* v) {
    if (!v) {
        return std::nullopt;
    }
    switch (v->kind) {
    case Value::Kind::real: return v->real;
    case Value::Kind::integer: return static_cast<double>(v->integer);
    default: return std::nullopt;
    }
}

uint32_t as_form(const Value* v) {
    return v && v->kind == Value::Kind::form ? v->form : 0;
}

// The first FormID member of an object (the entry's target in LVLO, CNTO, ...).
uint32_t first_form(const Value& v) {
    for (const auto& [name, m] : v.members) {
        if (m.kind == Value::Kind::form) {
            return m.form;
        }
    }
    return 0;
}

} // namespace

namespace {

// Decoded records. Conditions, placements and packages ask for the same records over and over, a quest's alias fill for
// tens of thousands of them, and decoding one allocates all its fields. Any thread: sharded so workers don't queue.
struct DecodedForms {
    using Fields = std::shared_ptr<const std::vector<DecodedField>>;
    static constexpr size_t k_shards = 64;
    static constexpr size_t k_shard_limit = 8192; // past this a shard starts again: forms alive keep what they hold
    struct Shard {
        std::mutex mutex;
        std::unordered_map<const FormEntry*, Fields> forms;
    };
    std::array<Shard, k_shards> shards;

    Fields get(const Database& db, const FormEntry& entry) {
        Shard& shard = shards[(reinterpret_cast<uintptr_t>(&entry) / sizeof(FormEntry)) % k_shards];
        {
            std::lock_guard lock(shard.mutex);
            if (const auto it = shard.forms.find(&entry); it != shard.forms.end()) {
                return it->second;
            }
        }
        Fields fields = std::make_shared<const std::vector<DecodedField>>(decode_form(db, entry));
        std::lock_guard lock(shard.mutex);
        if (shard.forms.size() >= k_shard_limit) {
            shard.forms.clear();
        }
        return shard.forms.try_emplace(&entry, std::move(fields)).first->second;
    }
};

} // namespace

Form::Form(const Database& db, const FormEntry& entry)
    : m_db(&db), m_entry(&entry), m_fields(db.cache<DecodedForms>().get(db, entry)) {}

std::string_view Form::plugin() const {
    return m_db->plugin_name(m_entry->plugin);
}

const Value* Form::get(std::string_view path) const {
    auto dot = path.find('.');
    auto head = path.substr(0, dot);
    auto sig = parse_sig(head);
    auto match = [&](const DecodedField& f) { return (sig && f.sig == *sig) || (f.def && f.def->name == head); };
    auto pick = [&](const DecodedField& f) {
        return dot == std::string_view::npos ? &f.value : f.value.path(path.substr(dot + 1));
    };
    // A layered form holds several records loaded one on top of the other: the last one loaded sets a field.
    if (m_entry->layers > 1) {
        for (auto it = m_fields->rbegin(); it != m_fields->rend(); ++it) {
            if (match(*it)) {
                return pick(*it);
            }
        }
        return nullptr;
    }
    for (const auto& f : *m_fields) {
        if (match(f)) {
            return pick(f);
        }
    }
    return nullptr;
}

std::vector<const Value*> Form::all(std::string_view field) const {
    std::vector<const Value*> out;
    auto sig = parse_sig(field);
    for (const auto& f : *m_fields) {
        if ((sig && f.sig == *sig) || (f.def && f.def->name == field)) {
            out.push_back(&f.value);
        }
    }
    return out;
}

std::string_view Form::editor_id() const {
    auto v = get("EDID");
    return v && v->kind == Value::Kind::text ? std::string_view(v->text) : std::string_view();
}

std::string_view Form::name() const {
    auto v = get("FULL");
    return v && v->kind == Value::Kind::text ? std::string_view(v->text) : std::string_view();
}

std::string_view Form::description() const {
    auto v = get("DESC");
    return v && v->kind == Value::Kind::text ? std::string_view(v->text) : std::string_view();
}

std::vector<uint32_t> Form::keywords() const {
    std::vector<uint32_t> out;
    for (const auto* v : all("KWDA")) {
        for (const auto& item : v->items) {
            out.push_back(as_form(&item));
        }
    }
    return out;
}

std::optional<int64_t> item_value(const Form& f) {
    if (f.type() == sig("ALCH")) {
        return as_int(f.get("ENIT.value"));
    }
    return as_int(f.get("DATA.value"));
}

std::optional<double> item_weight(const Form& f) {
    if (f.type() == sig("ALCH")) {
        return as_real(f.get("DATA")); // ALCH DATA is the weight alone
    }
    return as_real(f.get("DATA.weight"));
}

std::vector<ItemCount> container_items(const Form& f) {
    std::vector<ItemCount> out;
    for (const auto* v : f.all("CNTO")) {
        out.push_back({as_form(v->member("item")), as_int(v->member("count")).value_or(0)});
    }
    return out;
}

std::optional<LeveledList> leveled_list(const Form& f) {
    if (f.type() != sig("LVLI") && f.type() != sig("LVLN") && f.type() != sig("LVSP")) {
        return std::nullopt;
    }
    LeveledList l;
    l.chance_none = static_cast<uint8_t>(as_int(f.get("LVLD")).value_or(0));
    l.flags = static_cast<uint8_t>(as_int(f.get("LVLF")).value_or(0));
    l.chance_none_global = as_form(f.get("LVLG"));
    for (const auto* v : f.all("LVLO")) {
        l.entries.push_back({first_form(*v), static_cast<uint16_t>(as_int(v->member("level")).value_or(0)),
                             static_cast<uint16_t>(as_int(v->member("count")).value_or(0))});
    }
    return l;
}

std::vector<EffectEntry> effects(const Form& f) {
    std::vector<EffectEntry> out;
    for (const auto& d : f.fields()) {
        if (d.sig == sig("EFID")) {
            out.push_back({as_form(&d.value), 0.0f, 0, 0});
        } else if (d.sig == sig("EFIT") && !out.empty()) {
            auto& e = out.back();
            e.magnitude = static_cast<float>(as_real(d.value.member("magnitude")).value_or(0));
            e.area = static_cast<uint32_t>(as_int(d.value.member("area")).value_or(0));
            e.duration = static_cast<uint32_t>(as_int(d.value.member("duration")).value_or(0));
        }
    }
    return out;
}

std::vector<uint32_t> form_list(const Form& f) {
    std::vector<uint32_t> out;
    for (const auto* v : f.all("LNAM")) {
        out.push_back(as_form(v));
    }
    return out;
}

std::optional<float> global_value(const Form& f) {
    auto v = as_real(f.get("FLTV"));
    return v ? std::optional<float>(static_cast<float>(*v)) : std::nullopt;
}

std::optional<Value> setting_value(const Form& f) {
    if (auto v = f.get("DATA")) {
        return *v;
    }
    return std::nullopt;
}

uint32_t placed_base(const Database& db, const FormEntry& entry) {
    if (entry.layers > 1) {
        const Value* name = Form(db, entry).get("NAME");
        return name ? name->form : 0;
    }
    uint32_t base = 0;
    if (const auto name = db.field(entry, sig("NAME")); name && name->data.size() >= 4) {
        std::memcpy(&base, name->data.data(), 4);
    }
    return base;
}

std::optional<Placement> placement(const Database& db, const FormEntry& entry) {
    if (entry.layers > 1) {
        return placement(Form(db, entry));
    }
    Placement p{};
    bool name = false, data = false;
    for (const auto& field : db.fields(entry)) {
        if (field.sig == sig("NAME") && field.data.size() >= 4) {
            std::memcpy(&p.base, field.data.data(), 4);
            name = true;
        } else if (field.sig == sig("DATA") && field.data.size() >= 24) {
            std::memcpy(p.position.data(), field.data.data(), 12);
            std::memcpy(p.rotation.data(), field.data.data() + 12, 12);
            data = true;
        } else if (field.sig == sig("XSCL") && field.data.size() >= 4) {
            std::memcpy(&p.scale, field.data.data(), 4);
        }
    }
    if (!name || !data) {
        return std::nullopt;
    }
    return p;
}

std::optional<Placement> placement(const Form& f) {
    auto base = f.get("NAME");
    auto data = f.get("DATA");
    if (!base || !data || base->kind != Value::Kind::form) {
        return std::nullopt;
    }
    Placement p{};
    p.base = base->form;
    for (int i = 0; i < 3; ++i) {
        const char* axis[] = {"x", "y", "z"};
        p.position[i] = static_cast<float>(as_real(data->path(std::string("position.") + axis[i])).value_or(0));
        p.rotation[i] = static_cast<float>(as_real(data->path(std::string("rotation.") + axis[i])).value_or(0));
    }
    if (auto s = as_real(f.get("XSCL"))) {
        p.scale = static_cast<float>(*s);
    }
    return p;
}

std::vector<Response> responses(const Form& f) {
    std::vector<Response> out;
    for (const auto& d : f.fields()) {
        if (d.sig == sig("TRDT")) {
            Response r;
            r.emotion_type = static_cast<uint32_t>(as_int(d.value.member("emotion_type")).value_or(0));
            r.emotion_value = static_cast<uint32_t>(as_int(d.value.member("emotion_value")).value_or(0));
            r.number = static_cast<uint8_t>(as_int(d.value.member("response_number")).value_or(0));
            r.sound = as_form(d.value.member("sound"));
            out.push_back(std::move(r));
        } else if (d.sig == sig("NAM1")) {
            if (out.empty()) {
                out.emplace_back();
            }
            out.back().text = d.value.text;
        }
    }
    return out;
}

uint32_t shared_info(const Form& f) {
    return as_form(f.get("DNAM"));
}

std::vector<QuestStage> quest_stages(const Form& f) {
    std::vector<QuestStage> out;
    for (const auto& d : f.fields()) {
        if (d.sig == sig("INDX") && d.def && d.def->name == "stage") {
            QuestStage s;
            s.index = static_cast<uint16_t>(as_int(d.value.member("index")).value_or(0));
            s.flags = static_cast<uint8_t>(as_int(d.value.member("flags")).value_or(0));
            out.push_back(std::move(s));
        } else if (d.sig == sig("CNAM") && d.def && d.def->name == "log_entry" && !out.empty()) {
            out.back().log_entries.push_back(d.value.text);
        }
    }
    return out;
}

std::vector<FactionRank> faction_ranks(const Form& f) {
    std::vector<FactionRank> out;
    for (const auto& d : f.fields()) {
        if (d.sig == sig("RNAM")) {
            out.push_back({static_cast<uint32_t>(as_int(&d.value).value_or(0)), {}, {}});
        } else if (d.sig == sig("MNAM") && !out.empty()) {
            out.back().male_title = d.value.text;
        } else if (d.sig == sig("FNAM") && !out.empty()) {
            out.back().female_title = d.value.text;
        }
    }
    return out;
}

std::vector<FactionMembership> npc_factions(const Form& f) {
    std::vector<FactionMembership> out;
    for (const auto* v : f.all("SNAM")) {
        out.push_back({as_form(v->member("faction")), static_cast<int8_t>(as_int(v->member("rank")).value_or(0))});
    }
    return out;
}

std::optional<NavmeshGeometry> navmesh(const Form& f) {
    const Value* g = f.get("NVNM");
    if (!g || g->kind != Value::Kind::object) {
        return std::nullopt;
    }
    NavmeshGeometry n;
    if (const Value* pc = g->member("pathing_cell")) {
        n.world = as_form(pc->member("world"));
        if (const Value* c = pc->member("cell")) {
            if (c->kind == Value::Kind::form) {
                n.cell = c->form;
            } else {
                n.grid_x = static_cast<int16_t>(as_int(c->member("grid_x")).value_or(0));
                n.grid_y = static_cast<int16_t>(as_int(c->member("grid_y")).value_or(0));
            }
        }
    }
    if (const Value* vs = g->member("vertices")) {
        for (const auto& v : vs->items) {
            n.vertices.push_back({static_cast<float>(as_real(v.member("x")).value_or(0)),
                                  static_cast<float>(as_real(v.member("y")).value_or(0)),
                                  static_cast<float>(as_real(v.member("z")).value_or(0))});
        }
    }
    auto f32 = [](const Value* v) { return static_cast<float>(as_real(v).value_or(0)); };
    auto i16 = [](const Value* v) { return static_cast<int16_t>(as_int(v).value_or(0)); };
    if (const Value* ts = g->member("triangles")) {
        for (const auto& t : ts->items) {
            NavmeshTriangle tri;
            const Value* vi = t.member("vertices");
            const Value* ei = t.member("edges");
            for (size_t k = 0; k < 3; ++k) {
                if (vi && k < vi->items.size()) {
                    tri.vertices[k] = static_cast<uint16_t>(as_int(&vi->items[k]).value_or(0));
                }
                if (ei && k < ei->items.size()) {
                    tri.edges[k] = i16(&ei->items[k]);
                }
            }
            tri.flags = static_cast<uint16_t>(as_int(t.member("flags")).value_or(0));
            tri.cover_flags = static_cast<uint16_t>(as_int(t.member("cover_flags")).value_or(0));
            n.triangles.push_back(tri);
        }
    }
    if (const Value* ls = g->member("edge_links")) {
        for (const auto& l : ls->items) {
            n.edge_links.push_back({static_cast<uint32_t>(as_int(l.member("type")).value_or(0)),
                                    as_form(l.member("navmesh")), i16(l.member("triangle"))});
        }
    }
    if (const Value* ds = g->member("door_links")) {
        for (const auto& d : ds->items) {
            n.doors.push_back({i16(d.member("triangle")), as_form(d.member("door"))});
        }
    }
    if (const Value* cs = g->member("cover_triangles")) {
        for (const auto& c : cs->items) {
            n.cover_triangles.push_back(i16(&c));
        }
    }
    if (const Value* gr = g->member("grid")) {
        n.grid_divisor = static_cast<uint32_t>(as_int(gr->member("divisor")).value_or(0));
        if (const Value* s = gr->member("cell_size")) {
            n.grid_cell_size = {f32(s->member("x")), f32(s->member("y"))};
        }
        for (auto [key, out] : {std::pair{"bounds_min", &n.grid_min}, std::pair{"bounds_max", &n.grid_max}}) {
            if (const Value* b = gr->member(key)) {
                *out = {f32(b->member("x")), f32(b->member("y")), f32(b->member("z"))};
            }
        }
        if (const Value* cells = gr->member("cells")) {
            for (const auto& c : cells->items) {
                auto& cell = n.grid_cells.emplace_back();
                for (const auto& t : c.items) {
                    cell.push_back(i16(&t));
                }
            }
        }
    }
    return n;
}

} // namespace spud::forms
