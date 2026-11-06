#include "anim/graph.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "formats/osn.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <mutex>
#include <unordered_set>

namespace spud::anim {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::unordered_map<std::string, NodeFactory>& factories() {
    static std::unordered_map<std::string, NodeFactory> f;
    return f;
}

void register_all() {
    static std::once_flag once;
    std::call_once(once, [] {
        register_structural_nodes();
        register_state_machine_nodes();
        register_generator_nodes();
        register_modifier_nodes();
    });
}

const osb::Object* follow(const osb::Graph& g, const osb::Value* v) {
    return v && v->kind == osb::Kind::object && v->object < g.objects.size() ? &g.objects[v->object] : nullptr;
}

const osb::Object* variant(const osb::Graph& g, std::string_view cls) {
    if (g.root >= g.objects.size()) {
        return nullptr;
    }
    const auto* variants = g.objects[g.root].get("namedVariants");
    if (!variants) {
        return nullptr;
    }
    for (const auto& v : variants->list) {
        const osb::Object* entry = follow(g, &v);
        const osb::Object* target = entry ? follow(g, entry->get("variant")) : nullptr;
        if (target && target->cls == cls) {
            return target;
        }
    }
    return nullptr;
}

std::vector<std::string> texts(const osb::Object* o, std::string_view name) {
    std::vector<std::string> out;
    if (const auto* v = o ? o->get(name) : nullptr) {
        out.reserve(v->list.size());
        for (const auto& e : v->list) {
            out.push_back(e.text);
        }
    }
    return out;
}

// Reads a member list off an object.
class Loader final : public Describer {
public:
    using MakeNode = std::function<Node*(uint32_t)>;
    Loader(const osb::Graph& g, const osb::Object& o, MakeNode make) : m_g(g), m_o(o), m_make(std::move(make)) {}

    void member(const char* n, bool& v, bool) override { if (auto* x = get(n)) v = x->integer != 0; }
    void member(const char* n, int8_t& v, bool) override { integer(n, v); }
    void member(const char* n, uint8_t& v, bool) override { integer(n, v); }
    void member(const char* n, int16_t& v, bool) override { integer(n, v); }
    void member(const char* n, uint16_t& v, bool) override { integer(n, v); }
    void member(const char* n, int32_t& v, bool) override { integer(n, v); }
    void member(const char* n, uint32_t& v, bool) override { integer(n, v); }
    void member(const char* n, float& v, bool) override { if (auto* x = get(n)) v = x->real; }
    void member(const char* n, glm::vec4& v, bool) override {
        if (auto* x = get(n); x && x->floats.size() >= 4) {
            v = {x->floats[0], x->floats[1], x->floats[2], x->floats[3]};
        }
    }
    void member(const char* n, glm::quat& v, bool) override {
        if (auto* x = get(n); x && x->floats.size() >= 4) {
            v = {x->floats[3], x->floats[0], x->floats[1], x->floats[2]};
        }
    }
    void member(const char* n, std::string& v) override { if (auto* x = get(n)) v = x->text; }
    void member(const char* n, Qs& v) override {
        if (auto* x = get(n); x && x->floats.size() >= 12) {
            const auto& f = x->floats;
            v.t = {f[0], f[1], f[2]};
            v.r = {f[7], f[4], f[5], f[6]};
            v.s = {f[8], f[9], f[10]};
        }
    }
    void member(const char* n, EventProperty& v) override {
        const osb::Object* e = follow(m_g, get(n));
        if (!e) {
            return;
        }
        if (const auto* id = e->get("id")) {
            v.id = static_cast<int32_t>(id->integer);
        }
        if (const osb::Object* payload = follow(m_g, e->get("payload"))) {
            if (const auto* data = payload->get("data")) {
                v.payload = std::make_shared<const std::string>(data->text);
            }
        }
    }
    void member(const char* n, std::vector<float>& v) override { if (auto* x = get(n)) v = x->floats; }
    void member(const char* n, std::vector<int32_t>& v) override { if (auto* x = get(n)) v = x->ints; }
    void member(const char* n, std::vector<uint8_t>& v) override { if (auto* x = get(n)) v = x->bytes; }
    void member(const char* n, std::vector<std::string>& v) override {
        if (auto* x = get(n)) {
            v.clear();
            for (const auto& e : x->list) {
                v.push_back(e.text);
            }
        }
    }
    void node(const char* n, Node*& v) override {
        if (auto* x = get(n); x && x->kind == osb::Kind::object) {
            v = m_make(x->object);
        }
    }
    void nodes(const char* n, std::vector<Node*>& v) override {
        if (auto* x = get(n)) {
            v.clear();
            for (const auto& e : x->list) {
                v.push_back(e.kind == osb::Kind::object ? m_make(e.object) : nullptr);
            }
        }
    }
    void object(const char* n, Bindable& v, bool* exists) override {
        const osb::Object* o = follow(m_g, get(n));
        if (exists) {
            *exists = o != nullptr;
        }
        if (o) {
            load_bindable(m_g, *o, v, m_make);
        }
    }

    static void load_bindable(const osb::Graph& g, const osb::Object& o, Bindable& b, const MakeNode& make) {
        Loader l(g, o, make);
        b.describe(l);
        const osb::Object* set = follow(g, o.get("variableBindingSet"));
        if (!set) {
            return;
        }
        auto bindings = std::make_shared<std::vector<Binding>>();
        if (const auto* list = set->get("bindings")) {
            for (const auto& e : list->list) {
                const osb::Object* entry = follow(g, &e);
                if (!entry) {
                    continue;
                }
                Binding x;
                if (const auto* p = entry->get("memberPath")) {
                    x.path = p->text;
                }
                if (const auto* i = entry->get("variableIndex")) {
                    x.variable = static_cast<int32_t>(i->integer);
                }
                if (const auto* t = entry->get("bindingType")) {
                    x.character_property = t->integer == 1;
                }
                bindings->push_back(std::move(x));
            }
        }
        if (const auto* i = set->get("indexOfBindingToEnable")) {
            b.enable_binding = static_cast<int32_t>(i->integer);
        }
        b.bindings = std::move(bindings);
    }

protected:
    void object_list(const char* n, std::function<void(size_t)> resize, std::function<Bindable&(size_t)> at) override {
        const auto* x = get(n);
        if (!x) {
            return;
        }
        resize(x->list.size());
        for (size_t i = 0; i < x->list.size(); ++i) {
            if (const osb::Object* o = follow(m_g, &x->list[i])) {
                load_bindable(m_g, *o, at(i), m_make);
            }
        }
    }

private:
    const osb::Value* get(const char* n) const { return m_o.get(n); }
    template <typename T>
    void integer(const char* n, T& v) {
        if (auto* x = get(n)) {
            v = static_cast<T>(x->integer);
        }
    }

    const osb::Graph& m_g;
    const osb::Object& m_o;
    MakeNode m_make;
};

enum class MemberKind : uint8_t { none, boolean, i8, u8, i16, u16, i32, u32, real, vec4, quat, bone_weights };

// A binding made concrete for one object: where its member is, which variable it reads.
struct ResolvedBinding {
    MemberKind kind = MemberKind::none;
    void* address = nullptr;
    int32_t variable = -1; // the root's index, or the character property's
    VariableType type = VariableType::real;
    bool output = false;
    bool character_property = false;
    bool enable = false;
};

// Walks a member path over describe. Segments are separated by /, each a member
// name with an optional :index.
class PathResolver final : public Describer {
public:
    explicit PathResolver(std::string_view path) : m_rest(path) { next(); }

    MemberKind kind = MemberKind::none;
    void* address = nullptr;
    bool output = false;

    void member(const char* n, bool& v, bool o) override { leaf(n, MemberKind::boolean, &v, o); }
    void member(const char* n, int8_t& v, bool o) override { leaf(n, MemberKind::i8, &v, o); }
    void member(const char* n, uint8_t& v, bool o) override { leaf(n, MemberKind::u8, &v, o); }
    void member(const char* n, int16_t& v, bool o) override { leaf(n, MemberKind::i16, &v, o); }
    void member(const char* n, uint16_t& v, bool o) override { leaf(n, MemberKind::u16, &v, o); }
    void member(const char* n, int32_t& v, bool o) override { leaf(n, MemberKind::i32, &v, o); }
    void member(const char* n, uint32_t& v, bool o) override { leaf(n, MemberKind::u32, &v, o); }
    void member(const char* n, float& v, bool o) override { leaf(n, MemberKind::real, &v, o); }
    void member(const char* n, glm::vec4& v, bool o) override { leaf(n, MemberKind::vec4, &v, o); }
    void member(const char* n, glm::quat& v, bool o) override { leaf(n, MemberKind::quat, &v, o); }
    void member(const char*, std::string&) override {}
    void member(const char*, Qs&) override {}
    void member(const char*, EventProperty&) override {}
    void member(const char*, std::vector<float>&) override {}
    void member(const char*, std::vector<int32_t>&) override {}
    void member(const char*, std::vector<uint8_t>&) override {}
    void member(const char*, std::vector<std::string>&) override {}
    void node(const char*, Node*&) override {}
    void nodes(const char*, std::vector<Node*>&) override {}
    void object(const char* n, Bindable& v, bool*) override {
        if (!match(n)) {
            return;
        }
        if (m_last) {
            if (auto* w = dynamic_cast<BoneWeightArray*>(&v)) {
                kind = MemberKind::bone_weights;
                address = w;
            }
            return;
        }
        descend(v);
    }

protected:
    void object_list(const char* n, std::function<void(size_t)>, std::function<Bindable&(size_t)> at) override {
        if (match(n) && !m_last) {
            descend(at(m_index));
        }
    }

private:
    void next() {
        const size_t slash = m_rest.find('/');
        std::string_view segment = m_rest.substr(0, slash);
        m_last = slash == std::string_view::npos;
        m_rest = m_last ? std::string_view{} : m_rest.substr(slash + 1);
        const size_t colon = segment.find(':');
        m_name = segment.substr(0, colon);
        m_index = 0;
        if (colon != std::string_view::npos) {
            for (char c : segment.substr(colon + 1)) {
                m_index = m_index * 10 + size_t(c - '0');
            }
        }
    }
    bool match(const char* n) const { return kind == MemberKind::none && m_name == n; }
    void leaf(const char* n, MemberKind k, void* a, bool o) {
        if (match(n) && m_last) {
            kind = k;
            address = a;
            output = o;
        }
    }
    void descend(Bindable& v) {
        const auto name = m_name;
        const bool last = m_last;
        const auto rest = m_rest;
        next();
        v.describe(*this);
        m_name = name;
        m_last = last;
        m_rest = rest;
    }

    std::string_view m_rest, m_name;
    size_t m_index = 0;
    bool m_last = true;
};

float real_of(int32_t word) { return std::bit_cast<float>(word); }

int32_t read_word(const ResolvedBinding& b, const void* p, bool& is_real, float& real) {
    is_real = false;
    switch (b.kind) {
    case MemberKind::boolean: return *static_cast<const bool*>(p);
    case MemberKind::i8: return *static_cast<const int8_t*>(p);
    case MemberKind::u8: return *static_cast<const uint8_t*>(p);
    case MemberKind::i16: return *static_cast<const int16_t*>(p);
    case MemberKind::u16: return *static_cast<const uint16_t*>(p);
    case MemberKind::i32: return *static_cast<const int32_t*>(p);
    case MemberKind::u32: return static_cast<int32_t>(*static_cast<const uint32_t*>(p));
    case MemberKind::real:
        is_real = true;
        real = *static_cast<const float*>(p);
        return 0;
    default: return 0;
    }
}

} // namespace

void register_node(std::string_view cls, NodeFactory make) { factories()[std::string(cls)] = make; }

float Character::random_float() {
    random_state = random_state * 1664525u + 1013904223u;
    return float(random_state >> 8) / 16777216.0f;
}

int32_t Behavior::event_id(std::string_view name) const {
    for (size_t i = 0; i < event_names.size(); ++i) {
        if (event_names[i].size() == name.size() &&
            std::equal(name.begin(), name.end(), event_names[i].begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            })) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// Everything that turns a project's files into a Project.
class ProjectLoader {
public:
    ProjectLoader(const std::filesystem::path& cache, Project& p) : m_cache(cache), m_p(p) {}

    void run(const std::string& project) {
        m_p.m_path = project;
        const auto dir = parent(project);
        const osb::Graph pg = osb::read(read_file(m_cache / (project + ".osb")));
        const osb::Object* data = variant(pg, "hkbProjectData");
        const osb::Object* strings = data ? follow(pg, data->get("stringData")) : nullptr;
        const auto characters = texts(strings, "characterFilenames");
        if (characters.empty()) {
            throw ParseError(project + ": project names no character");
        }
        const auto character_path = relative(dir, characters[0]);
        const osb::Graph cg = osb::read(read_file(m_cache / (character_path + ".osb")));
        const osb::Object* character = variant(cg, "hkbCharacterData");
        const osb::Object* cs = character ? follow(cg, character->get("stringData")) : nullptr;
        if (!cs) {
            throw ParseError(character_path + ": no character data");
        }
        auto axis = [](const osb::Value* v, glm::vec3& out) {
            if (v && v->floats.size() >= 3) {
                out = {v->floats[0], v->floats[1], v->floats[2]};
            }
        };
        axis(data->get("worldUpWS"), m_p.m_world_up);
        axis(character->get("modelUpMS"), m_p.m_model_up);
        axis(character->get("modelForwardMS"), m_p.m_model_forward);
        axis(character->get("modelRightMS"), m_p.m_model_right);
        if (const auto* mode = data->get("defaultEventMode")) {
            m_p.default_event_mode = static_cast<int8_t>(mode->integer);
        }
        const auto rig = relative(dir, cs->get("rigName")->text);
        m_p.m_skeleton = std::make_shared<Skeleton>(Skeleton::from(osk::read(read_file(m_cache / (rig + ".osk")))));
        std::error_code missing;
        if (const auto file = m_cache / (rig + ".ragdoll.osb"); std::filesystem::exists(file, missing)) {
            m_p.m_ragdoll = read_ragdoll(osb::read(read_file(file)));
        }
        read_character(cg, *character, *cs);
        if (const osb::Object* mirror = follow(cg, character->get("mirroredSkeletonInfo"))) {
            glm::vec4 axis_value(0.0f);
            if (const auto* v = mirror->get("mirrorAxis"); v && v->floats.size() >= 4) {
                axis_value = {v->floats[0], v->floats[1], v->floats[2], v->floats[3]};
            }
            std::vector<int16_t> pairs;
            if (const auto* v = mirror->get("bonePairMap")) {
                pairs.assign(v->ints.begin(), v->ints.end());
            }
            m_p.m_mirrored_skeleton = make_mirrored_skeleton(*m_p.m_skeleton, axis_value, std::move(pairs));
        }
        if (const osb::Object* foot = follow(cg, character->get("footIkDriverInfo"))) {
            m_p.m_foot_ik_info = make_foot_ik_info();
            Loader::load_bindable(cg, *foot, *m_p.m_foot_ik_info, [](uint32_t) -> Node* { return nullptr; });
        }

        load_behavior(dir, cs->get("behaviorFilename")->text);
        link();
        for (const auto& name : texts(cs, "animationNames")) {
            load_clip(dir, name);
        }
        load_motion(project);
    }

private:
    static std::string parent(const std::string& p) {
        const size_t slash = p.rfind('/');
        return slash == std::string::npos ? std::string() : p.substr(0, slash);
    }
    static std::string relative(const std::string& dir, const std::string& file) {
        return path::replace_extension(dir + "/" + path::normalize(file), "");
    }

    void read_character(const osb::Graph& g, const osb::Object& character, const osb::Object& strings) {
        const auto names = texts(&strings, "characterPropertyNames");
        const auto* infos = character.get("characterPropertyInfos");
        const osb::Object* values = follow(g, character.get("characterPropertyValues"));
        for (size_t i = 0; i < names.size(); ++i) {
            m_p.m_property_ids.emplace(lower(names[i]), static_cast<int32_t>(i));
            VariableType type = VariableType::real;
            if (infos && i < infos->list.size()) {
                if (const osb::Object* info = follow(g, &infos->list[i])) {
                    if (const auto* t = info->get("type")) {
                        type = static_cast<VariableType>(t->integer);
                    }
                }
            }
            m_p.m_character_types.push_back(type);
        }
        if (values) {
            read_values(g, *values, m_p.m_character_values);
            m_p.m_character_bone_weights.resize(names.size());
            if (const auto* variants = values->get("variantVariableValues")) {
                for (size_t i = 0; i < names.size() && i < m_p.m_character_values.words.size(); ++i) {
                    const size_t slot = size_t(m_p.m_character_values.words[i]);
                    if (m_p.m_character_types[i] == VariableType::pointer && slot < variants->list.size()) {
                        if (const osb::Object* w = follow(g, &variants->list[slot])) {
                            if (const auto* f = w->get("boneWeights")) {
                                m_p.m_character_bone_weights[i] = f->floats;
                            }
                        }
                    }
                }
            }
        }
    }

    static void read_values(const osb::Graph& g, const osb::Object& set, VariableValues& out) {
        if (const auto* words = set.get("wordVariableValues")) {
            for (const auto& w : words->list) {
                const osb::Object* o = follow(g, &w);
                const auto* v = o ? o->get("value") : nullptr;
                out.words.push_back(v ? static_cast<int32_t>(v->integer) : 0);
            }
        }
        if (const auto* quads = set.get("quadVariableValues")) {
            for (size_t i = 0; i + 3 < quads->floats.size(); i += 4) {
                out.quads.emplace_back(quads->floats[i], quads->floats[i + 1], quads->floats[i + 2], quads->floats[i + 3]);
            }
        }
    }

    Behavior* load_behavior(const std::string& dir, const std::string& file) {
        const auto path = relative(dir, file);
        if (auto it = m_loaded.find(path); it != m_loaded.end()) {
            return it->second;
        }
        auto owned = std::make_unique<Behavior>();
        Behavior* b = owned.get();
        b->path = path;
        m_p.m_behaviors.push_back(std::move(owned));
        m_loaded.emplace(path, b);

        const auto g = std::make_shared<osb::Graph>(osb::read(read_file(m_cache / (path + ".osb"))));
        const osb::Object* root = variant(*g, "hkbBehaviorGraph");
        if (!root) {
            throw ParseError(path + ": no behaviour graph");
        }
        const osb::Object* data = follow(*g, root->get("data"));
        if (data) {
            const osb::Object* strings = follow(*g, data->get("stringData"));
            b->event_names = texts(strings, "eventNames");
            b->variable_names = texts(strings, "variableNames");
            if (const auto* infos = data->get("variableInfos")) {
                for (const auto& v : infos->list) {
                    const osb::Object* info = follow(*g, &v);
                    const auto* t = info ? info->get("type") : nullptr;
                    b->variable_types.push_back(t ? static_cast<VariableType>(t->integer) : VariableType::real);
                }
            }
            if (const auto* infos = data->get("eventInfos")) {
                for (const auto& v : infos->list) {
                    const osb::Object* info = follow(*g, &v);
                    const auto* f = info ? info->get("flags") : nullptr;
                    b->event_flags.push_back(f ? static_cast<uint32_t>(f->integer) : 0);
                }
            }
            if (const osb::Object* set = follow(*g, data->get("variableInitialValues"))) {
                read_values(*g, *set, b->initial_values);
            }
        }
        std::unordered_map<uint32_t, Node*> made;
        Loader::MakeNode make = [&, this](uint32_t index) -> Node* {
            if (auto it = made.find(index); it != made.end()) {
                return it->second;
            }
            const osb::Object& o = g->objects[index];
            auto f = factories().find(o.cls);
            if (f == factories().end()) {
                log::debug("behaviour {}: class {} has no node", path, o.cls);
                made.emplace(index, nullptr);
                return nullptr;
            }
            std::unique_ptr<Node> node = f->second();
            Node* n = node.get();
            m_p.m_nodes.push_back(std::move(node));
            made.emplace(index, n);
            if (const auto* v = o.get("name")) {
                n->name = v->text;
            }
            if (const auto* v = o.get("userData")) {
                n->user_data = static_cast<uint32_t>(v->integer);
            }
            Loader::load_bindable(*g, o, *n, make);
            if (o.cls == "hkbBehaviorReferenceGenerator") {
                n->user_data = 1; // the loader writes it when it resolves the reference
                pending_references.emplace_back(n, o.get("behaviorName")->text);
            }
            return n;
        };
        const auto* rg = root->get("rootGenerator");
        b->root_generator = rg && rg->kind == osb::Kind::object ? make(rg->object) : nullptr;
        if (data) {
            const osb::Object* strings = follow(*g, data->get("stringData"));
            for (const auto& name : texts(strings, "characterPropertyNames")) {
                auto it = m_p.m_property_ids.find(lower(name));
                b->property_to_character.push_back(it != m_p.m_property_ids.end() ? it->second : -1);
            }
        }
        // Referenced behaviours, depth first across files.
        auto references = std::move(pending_references);
        pending_references.clear();
        for (const auto& [node, name] : references) {
            set_reference(*node, load_behavior(dir, name));
        }
        return b;
    }

    // First-seen wins: a project's external ids, and the root's variables growing by what the others add.
    void link() {
        Behavior& root = *m_p.m_behaviors.front();
        for (size_t i = 0; i < root.variable_names.size(); ++i) {
            m_p.m_variable_ids.emplace(lower(root.variable_names[i]), static_cast<int32_t>(i));
        }
        for (auto& owned : m_p.m_behaviors) {
            Behavior& b = *owned;
            b.event_to_external.resize(b.event_names.size());
            for (size_t i = 0; i < b.event_names.size(); ++i) {
                const auto key = lower(b.event_names[i]);
                auto [it, fresh] = m_p.m_event_ids.try_emplace(key, static_cast<int32_t>(m_p.m_external_events.size()));
                if (fresh) {
                    m_p.m_external_events.push_back(b.event_names[i]);
                }
                b.event_to_external[i] = it->second;
                b.event_from_external.try_emplace(it->second, static_cast<int32_t>(i));
            }
            b.variable_to_root.resize(b.variable_names.size());
            for (size_t i = 0; i < b.variable_names.size(); ++i) {
                const auto key = lower(b.variable_names[i]);
                auto it = m_p.m_variable_ids.find(key);
                if (it == m_p.m_variable_ids.end()) {
                    const auto index = static_cast<int32_t>(root.variable_names.size());
                    root.variable_names.push_back(b.variable_names[i]);
                    const VariableType type = i < b.variable_types.size() ? b.variable_types[i] : VariableType::real;
                    root.variable_types.push_back(type);
                    int32_t word = i < b.initial_values.words.size() ? b.initial_values.words[i] : 0;
                    if (type == VariableType::vector3 || type == VariableType::vector4 || type == VariableType::quaternion) {
                        const glm::vec4 quad = size_t(word) < b.initial_values.quads.size() ? b.initial_values.quads[size_t(word)]
                                                                                            : glm::vec4(0.0f);
                        word = static_cast<int32_t>(root.initial_values.quads.size());
                        root.initial_values.quads.push_back(quad);
                    }
                    root.initial_values.words.push_back(word);
                    it = m_p.m_variable_ids.emplace(key, index).first;
                }
                b.variable_to_root[i] = it->second;
            }
        }
        link_state_machines(m_p);
    }

    void load_motion(const std::string& project) {
        const size_t slash = project.rfind('/');
        const auto file = m_cache / "animations" / "motion" / (project.substr(slash + 1) + ".osb");
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) {
            return;
        }
        const osb::Graph g = osb::read(read_file(file));
        for (const osb::Object& o : g.objects) {
            ClipMotion m;
            if (const auto* t = o.get("translation")) {
                for (size_t i = 0; i + 3 < t->floats.size(); i += 4) {
                    m.translation.push_back({t->floats[i], {t->floats[i + 1], t->floats[i + 2], t->floats[i + 3]}});
                }
            }
            if (const auto* r = o.get("rotation")) {
                for (size_t i = 0; i + 4 < r->floats.size(); i += 5) {
                    m.rotation.push_back({r->floats[i], glm::quat(r->floats[i + 4], r->floats[i + 1], r->floats[i + 2], r->floats[i + 3])});
                }
            }
            m_p.m_motion.emplace(lower(o.get("name")->text), std::move(m));
        }
    }

    void load_clip(const std::string& dir, const std::string& name) {
        const auto path = relative(dir, name);
        const auto file = m_cache / (path + ".osn");
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) {
            return;
        }
        auto clip = std::make_unique<Clip>();
        clip->data = osn::read(read_file(file));
        clip->name = name;
        const Clip* c = clip.get();
        const auto key = lower(path::normalize(name));
        m_p.m_clips_by_file.emplace(key.substr(key.rfind('/') + 1), c);
        m_p.m_clips.emplace(key, std::move(clip));
        m_p.m_track_names[c] = c->data.track_names;
        m_p.m_binding_triggers[c] = bind_annotations(m_p, *c);
    }

    std::filesystem::path m_cache;
    Project& m_p;
    std::unordered_map<std::string, Behavior*> m_loaded;
    std::vector<std::pair<Node*, std::string>> pending_references;
};

std::shared_ptr<const Project> Project::load(const std::filesystem::path& cache, const std::string& project) {
    register_all();
    auto p = std::make_shared<Project>();
    ProjectLoader(cache, *p).run(project);
    return p;
}

Project::~Project() = default;

int32_t Project::external_event(std::string_view name) const {
    auto it = m_event_ids.find(lower(name));
    return it != m_event_ids.end() ? it->second : -1;
}
int32_t Project::root_variable(std::string_view name) const {
    auto it = m_variable_ids.find(lower(name));
    return it != m_variable_ids.end() ? it->second : -1;
}
int32_t Project::character_property(std::string_view name) const {
    auto it = m_property_ids.find(lower(name));
    return it != m_property_ids.end() ? it->second : -1;
}
const Clip* Project::clip(std::string_view name) const {
    const auto key = lower(path::normalize(name));
    if (auto it = m_clips.find(key); it != m_clips.end()) {
        return it->second.get();
    }
    // Shared behaviours name the male folder's clips; the female character lists the same files in its own folder.
    auto it = m_clips_by_file.find(key.substr(key.rfind('/') + 1));
    return it != m_clips_by_file.end() ? it->second : nullptr;
}

const ClipMotion* Project::motion(std::string_view clip_generator) const {
    auto it = m_motion.find(lower(clip_generator));
    return it != m_motion.end() ? &it->second : nullptr;
}

Graph::Graph(std::shared_ptr<const Project> project, Character& character)
    : m_project(std::move(project)), m_character(character) {
    character.project = m_project.get();
    character.skeleton = &m_project->skeleton();
    activate();
}

Graph::~Graph() {
    Context ctx = context(nullptr);
    for (auto& r : m_records) {
        if (r->activated && r->clone) {
            ctx.behavior = r->behavior;
            r->clone->deactivate(ctx);
        }
    }
}

Context Graph::context(Behavior* behavior) {
    Context ctx{*this, m_character};
    ctx.behavior = behavior;
    return ctx;
}

void Graph::activate() {
    m_values = m_project->root().initial_values;
    m_active = true;
    m_update_active_nodes = true;
    update_active_nodes();
}

Node* Graph::clone_of(const Node* t) const {
    auto it = m_index.find(t);
    return it != m_index.end() ? m_records[it->second]->clone.get() : nullptr;
}

Record* Graph::record_of(const Node* t) {
    auto it = m_index.find(t);
    return it != m_index.end() ? m_records[it->second].get() : nullptr;
}

Record* Graph::sync_record(Record* r) {
    for (int guard = 0; r && r->forwards_sync && r->sync_target >= 0 && guard < 64; ++guard) {
        r = m_records[size_t(r->sync_target)].get();
    }
    return r;
}

void Graph::forward_sync(Node& node, const Node* child) {
    Record* r = node.record;
    auto it = m_index.find(child);
    if (r) {
        r->forwards_sync = it != m_index.end();
        r->sync_target = r->forwards_sync ? static_cast<int32_t>(it->second) : -1;
    }
}

void Graph::copy_sync(Node& node, const Node* child) {
    Record* r = node.record;
    Record* c = sync_record(record_of(child));
    if (r && c) {
        r->sync = c->sync;
        r->sync.mirrored = false;
    }
}

// Binding objects: built when a record's clone is made, since they hold addresses in it.
void Graph::resolve_bindings(Record& r) {
    r.bindings.clear();
    if (!r.clone) {
        return;
    }
    std::vector<Bindable*> sets{r.clone.get()};
    r.clone->collect_bindables(sets);
    for (Bindable* b : sets) {
        if (!b->bindings) {
            continue;
        }
        for (size_t i = 0; i < b->bindings->size(); ++i) {
            const Binding& x = (*b->bindings)[i];
            PathResolver path(x.path);
            b->describe(path);
            if (path.kind == MemberKind::none) {
                continue; // an unresolved path disables the binding silently
            }
            RecordBinding out;
            out.kind = static_cast<uint8_t>(path.kind);
            out.address = path.address;
            out.output = path.output;
            out.enable = int32_t(i) == b->enable_binding;
            out.character_property = x.character_property;
            if (x.character_property) {
                out.variable = x.variable; // behaviours' indices of the character's properties
                out.type = static_cast<uint8_t>(VariableType::real);
                if (r.behavior && size_t(x.variable) < r.behavior->property_to_character.size()) {
                    out.variable = r.behavior->property_to_character[size_t(x.variable)];
                }
                if (out.variable < 0 || size_t(out.variable) >= m_project->character_types().size()) {
                    continue;
                }
                out.type = static_cast<uint8_t>(m_project->character_types()[size_t(out.variable)]);
            } else {
                if (!r.behavior || size_t(x.variable) >= r.behavior->variable_to_root.size()) {
                    continue;
                }
                out.variable = r.behavior->variable_to_root[size_t(x.variable)];
                out.type = static_cast<uint8_t>(r.behavior->variable_types[size_t(x.variable)]);
            }
            r.has_outputs = r.has_outputs || out.output;
            r.bindings.push_back(out);
        }
    }
}

void Graph::copy_in(Record& r) {
    for (const RecordBinding& b : r.bindings) {
        if (b.output) {
            continue;
        }
        const auto kind = static_cast<MemberKind>(b.kind);
        const auto type = static_cast<VariableType>(b.type);
        const VariableValues& set = b.character_property ? m_project->character_values() : m_values;
        if (size_t(b.variable) >= set.words.size()) {
            continue;
        }
        const int32_t word = set.words[size_t(b.variable)];
        if (kind == MemberKind::bone_weights) {
            auto* target = static_cast<BoneWeightArray*>(b.address);
            const auto& all = m_project->character_bone_weights();
            if (b.character_property && size_t(b.variable) < all.size()) {
                target->weights = all[size_t(b.variable)];
            }
            continue;
        }
        if (kind == MemberKind::vec4 || kind == MemberKind::quat) {
            if (size_t(word) < set.quads.size()) {
                const glm::vec4 q = set.quads[size_t(word)];
                if (kind == MemberKind::vec4) {
                    *static_cast<glm::vec4*>(b.address) = q;
                } else {
                    *static_cast<glm::quat*>(b.address) = glm::quat(q.w, q.x, q.y, q.z);
                }
            }
            continue;
        }
        // Conversion table, "in".
        const bool from_real = type == VariableType::real;
        const float real = from_real ? real_of(word) : 0.0f;
        const int32_t integer = type == VariableType::int8 ? static_cast<int8_t>(word) : word;
        switch (kind) {
        case MemberKind::boolean: {
            bool& m = *static_cast<bool*>(b.address);
            const bool v = from_real ? real != 0.0f : type == VariableType::int8 ? (word & 0xFF) != 0 : word != 0;
            if (b.enable && m != v) {
                m_update_active_nodes = true;
            }
            m = v;
            break;
        }
        case MemberKind::real: *static_cast<float*>(b.address) = from_real ? real : float(integer); break;
        case MemberKind::i8: *static_cast<int8_t*>(b.address) = static_cast<int8_t>(from_real ? int32_t(real) : integer); break;
        case MemberKind::u8: *static_cast<uint8_t*>(b.address) = static_cast<uint8_t>(from_real ? int32_t(real) : integer); break;
        case MemberKind::i16: *static_cast<int16_t*>(b.address) = static_cast<int16_t>(from_real ? int32_t(real) : integer); break;
        case MemberKind::u16: *static_cast<uint16_t*>(b.address) = static_cast<uint16_t>(from_real ? int32_t(real) : integer); break;
        case MemberKind::i32: *static_cast<int32_t*>(b.address) = from_real ? int32_t(real) : integer; break;
        case MemberKind::u32: *static_cast<uint32_t*>(b.address) = static_cast<uint32_t>(from_real ? int32_t(real) : integer); break;
        default: break;
        }
    }
}

void Graph::copy_back(Record& r) {
    for (const RecordBinding& b : r.bindings) {
        if (!b.output || b.character_property || size_t(b.variable) >= m_values.words.size()) {
            continue;
        }
        const auto kind = static_cast<MemberKind>(b.kind);
        const auto type = static_cast<VariableType>(b.type);
        int32_t& word = m_values.words[size_t(b.variable)];
        if (kind == MemberKind::vec4 || kind == MemberKind::quat) {
            if (size_t(word) < m_values.quads.size()) {
                if (kind == MemberKind::vec4) {
                    m_values.quads[size_t(word)] = *static_cast<const glm::vec4*>(b.address);
                } else {
                    const glm::quat& q = *static_cast<const glm::quat*>(b.address);
                    m_values.quads[size_t(word)] = {q.x, q.y, q.z, q.w};
                }
            }
            continue;
        }
        ResolvedBinding view;
        view.kind = kind;
        bool is_real = false;
        float real = 0.0f;
        const int32_t integer = read_word(view, b.address, is_real, real);
        switch (type) {
        case VariableType::boolean: word = (is_real ? real != 0.0f : integer != 0) ? 1 : 0; break;
        case VariableType::int8: word = static_cast<int8_t>(is_real ? int32_t(real) : integer); break;
        case VariableType::int16: word = static_cast<int16_t>(is_real ? int32_t(real) : integer); break;
        case VariableType::int32: word = is_real ? int32_t(real) : integer; break;
        case VariableType::real:
            if (is_real) {
                if (std::isfinite(real)) { // Inf and NaN leave the variable as it is
                    word = std::bit_cast<int32_t>(real);
                }
            } else {
                word = std::bit_cast<int32_t>(float(integer));
            }
            break;
        default: break;
        }
    }
}

// Collecting one node.
void Graph::collect(Node* node, Behavior* behavior, bool force) {
    if (!node) {
        return;
    }
    Record* old = nullptr;
    if (auto it = m_old_index.find(node); it != m_old_index.end()) {
        old = m_old_records[it->second].get();
    }
    auto owned = std::make_unique<Record>();
    Record& r = *owned;
    r.template_node = node;
    if (old) {
        r.clone = std::move(old->clone);
        r.bindings = std::move(old->bindings);
        r.has_outputs = old->has_outputs;
        r.moved = true;
        old->moved_to = &r;
    } else {
        r.clone = node->clone();
        r.fresh = true;
    }
    r.behavior = behavior;
    r.generator = node->is_generator();
    r.clone->record = &r;
    if (r.fresh) {
        resolve_bindings(r);
    }
    Context ctx = context(behavior);
    m_queue_map = behavior;
    bool was_active = old != nullptr;
    if (was_active && force) {
        r.clone->deactivate(ctx);
        was_active = false;
    }
    if (!was_active) {
        copy_in(r);
        r.clone->activate(ctx);
        const auto* m = r.generator ? nullptr : static_cast<const Modifier*>(r.clone.get());
        if (r.has_outputs && (r.generator || m->enable)) {
            copy_back(r);
        }
    }

    std::vector<Child> kids;
    r.clone->children(children_active_only, kids);
    std::vector<int32_t> indices;
    indices.reserve(kids.size());
    // Records are pushed in post-order; `r` stays put as it is owned by its unique_ptr until appended below.
    for (const Child& k : kids) {
        if (!k.node) {
            indices.push_back(-1);
            continue;
        }
        auto it = m_index.find(k.node);
        if (it == m_index.end()) {
            Behavior* below = r.clone->behavior_below();
            collect(k.node, below ? below : behavior, force || k.force_reactivate);
            it = m_index.find(k.node);
        }
        if (it == m_index.end()) {
            indices.push_back(-1);
            continue;
        }
        Record& c = *m_records[it->second];
        ++c.parents;
        c.frozen = c.frozen && k.frozen;
        indices.push_back(static_cast<int32_t>(it->second));
    }

    if (old) {
        r.activated = old->activated;
        if (r.generator && !old->forwards_sync) {
            r.sync = old->sync;
            r.sync.mirrored = false;
        }
    } else {
        r.activated = true;
    }
    const auto index = static_cast<uint32_t>(m_records.size());
    m_records.push_back(std::move(owned));
    m_index.emplace(node, index);
    r.first_child = static_cast<uint32_t>(m_children.size());
    r.child_count = static_cast<uint32_t>(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        m_children.push_back(indices[i]);
        if (indices[i] < 0) {
            continue;
        }
        Record& c = *m_records[size_t(indices[i])];
        c.ignore_events = kids[i].ignore_events;
        c.event_parent = static_cast<int32_t>(index);
        if (kids[i].sync_to_parent) {
            c.sync_parent = static_cast<int32_t>(index);
            c.has_sync_parent = true;
        } else if (c.sync_parent < 0) {
            c.sync_parent = static_cast<int32_t>(index);
        }
    }
    if (r.generator) {
        static_cast<Generator*>(r.clone.get())->update_sync(ctx);
    }
}

void Graph::update_active_nodes() {
    Behavior* saved = m_queue_map;
    m_old_records = std::move(m_records);
    m_old_index = std::move(m_index);
    m_records.clear();
    m_index.clear();
    m_children.clear();

    Behavior& root = m_project->root();
    collect(root.root_generator, &root, false);
    if (!m_records.empty()) {
        Record& last = *m_records.back();
        last.frozen = false;
        last.ignore_events = false;
        for (size_t i = m_records.size() - 1; i-- > 0;) {
            Record& r = *m_records[i];
            if (!r.ignore_events) {
                continue;
            }
            for (uint32_t k = 0; k < r.child_count; ++k) {
                const int32_t c = m_children[r.first_child + k];
                if (c >= 0 && m_records[size_t(c)]->event_parent == int32_t(i)) {
                    m_records[size_t(c)]->ignore_events = true;
                }
            }
        }
    }
    // New nodes activate before old ones deactivate.
    for (auto& old : m_old_records) {
        if (old->moved_to || !old->clone) {
            continue;
        }
        if (old->activated) {
            Context ctx = context(old->behavior);
            m_queue_map = old->behavior;
            copy_in(*old);
            old->clone->deactivate(ctx);
            if (old->has_outputs) {
                copy_back(*old);
            }
        }
    }
    m_old_records.clear();
    m_old_index.clear();
    m_update_active_nodes = false;
    m_queue_map = saved;
}

namespace {

// The sync interval of a sync info at time t ("Sync interval of a sync info").
struct Interval {
    int32_t first = -1, second = -1;
    float fraction = 0.0f;
};

Interval sync_interval(const SyncInfo& s, float t) {
    Interval out;
    const uint32_t n = s.point_count;
    if (n == 0) {
        out.fraction = std::clamp(t * s.base_frequency, 0.0f, 1.0f);
        return out;
    }
    const float period = s.base_frequency > 0.0f ? 1.0f / s.base_frequency : 0.0f;
    if (n == 1 && s.cyclic) {
        out.first = out.second = s.points[0].id;
        float x = t;
        if (x < s.points[0].time) {
            x += period;
        }
        out.fraction = std::clamp((x - s.points[0].time) * s.base_frequency, 0.0f, 1.0f);
        return out;
    }
    uint32_t chosen = 0;
    bool none_after = true;
    for (uint32_t i = 0; i < n; ++i) {
        if (s.points[i].time >= t) {
            chosen = i;
            none_after = false;
            break;
        }
    }
    float later = s.points[chosen].time;
    float earlier;
    int32_t later_id = s.points[chosen].id, earlier_id;
    if (chosen == 0) {
        const SyncInfo::Point& last = s.points[n - 1];
        if (s.cyclic) {
            earlier_id = last.id;
            earlier = last.time;
            if (none_after || t > later) {
                later += period;
            } else {
                earlier -= period;
            }
        } else if (none_after || t > later) {
            earlier_id = last.id;
            earlier = last.time;
            later_id = -1;
            later = period;
        } else if (later == 0.0f) {
            if (n == 1) {
                return out;
            }
            later_id = s.points[1].id;
            later = s.points[1].time;
            earlier_id = s.points[0].id;
            earlier = 0.0f;
        } else {
            earlier_id = -1;
            earlier = 0.0f;
        }
    } else {
        earlier_id = s.points[chosen - 1].id;
        earlier = s.points[chosen - 1].time;
    }
    out.first = earlier_id;
    out.second = later_id;
    out.fraction = later > earlier ? std::clamp((t - earlier) / (later - earlier), 0.0f, 1.0f) : 0.0f;
    return out;
}

// The time for an interval ("Time for an interval"); false when the ids aren't found.
bool time_for_interval(const SyncInfo& s, const Interval& v, float& out) {
    const float period = s.base_frequency > 0.0f ? 1.0f / s.base_frequency : 0.0f;
    auto find = [&](int32_t id) -> const SyncInfo::Point* {
        for (uint32_t i = 0; i < s.point_count; ++i) {
            if (s.points[i].id == id) {
                return &s.points[i];
            }
        }
        return nullptr;
    };
    if (v.first == -1 && v.second == -1) {
        out = s.base_frequency > 0.0f ? v.fraction / s.base_frequency : 0.0f;
        return true;
    }
    const SyncInfo::Point* a = v.first != -1 ? find(v.first) : nullptr;
    const SyncInfo::Point* b = v.second != -1 ? find(v.second) : nullptr;
    if (v.first == -1 && b) {
        out = b->time * v.fraction;
        return true;
    }
    if (v.second == -1 && a) {
        out = (period - a->time) * v.fraction + a->time;
        return true;
    }
    if (a && b && v.first != v.second) {
        out = b->time > a->time ? (b->time - a->time) * v.fraction + a->time
                                : (b->time + period - a->time) * v.fraction + a->time;
        if (b->time > a->time) {
            return true;
        }
    } else if (a || b) {
        out = period * v.fraction + (a ? a : b)->time;
    } else {
        return false;
    }
    if (out + 0.00001f >= period) {
        out = std::clamp(out - period, 0.0f, period);
    }
    return true;
}

} // namespace

void Graph::timesteps(float dt) {
    if (m_records.empty()) {
        return;
    }
    m_records.back()->timestep = dt;
    for (size_t i = m_records.size() - 1; i-- > 0;) {
        Record& r = *m_records[i];
        if (dt == 0.0f || r.frozen) {
            r.timestep = 0.0f;
            continue;
        }
        Record& p = *m_records[size_t(r.sync_parent >= 0 ? r.sync_parent : int32_t(m_records.size() - 1))];
        const Record* ps = sync_record(&p);
        const Record* cs = sync_record(&r);
        if (!r.has_sync_parent || cs->sync.base_frequency == 0.0f || ps->sync.base_frequency == 0.0f) {
            r.timestep = r.generator && cs->sync.playback_speed == 0.0f ? 0.0f : p.timestep;
            continue;
        }
        const float period = 1.0f / ps->sync.base_frequency;
        float t = p.timestep, w = 0.0f;
        if (t >= period) {
            w = std::floor(t / period);
            t -= w * period;
        }
        float x = t + ps->sync.local_time;
        if (x > period) {
            x -= std::floor(x / period) * period;
            if (x >= ps->sync.local_time) {
                x = ps->sync.local_time - 0.00001f;
            }
        }
        const Interval interval = sync_interval(ps->sync, x);
        float y = 0.0f;
        if (!time_for_interval(cs->sync, interval, y)) {
            r.timestep = t;
            continue;
        }
        const float tc = 1.0f / cs->sync.base_frequency;
        if (y < cs->sync.local_time) {
            y += tc;
        }
        y += tc * w;
        r.timestep = y - cs->sync.local_time;
    }
}

void Graph::update_modifier(Context& ctx, Record& r, float timestep) {
    auto* m = static_cast<Modifier*>(r.clone.get());
    const bool was = m->enable;
    copy_in(r);
    if (m->enable) {
        if (!r.activated) {
            m->activate(ctx);
            r.activated = true;
            m_update_active_nodes = m_update_active_nodes || r.child_count > 0;
        }
        m->update(ctx, timestep);
        if (r.has_outputs) {
            copy_back(r);
        }
    } else {
        if (r.activated) {
            m->deactivate(ctx);
            r.activated = false;
            m_update_active_nodes = m_update_active_nodes || r.child_count > 0;
        }
        if (r.has_outputs && was) {
            copy_back(r);
        }
    }
}

void Graph::update(float dt) {
    if (m_update_active_nodes) {
        update_active_nodes();
    }
    timesteps(dt);
    Behavior* saved = m_queue_map;
    for (auto& owned : m_records) {
        Record& r = *owned;
        Context ctx = context(r.behavior);
        m_queue_map = r.behavior;
        m_discard_raised = r.ignore_events;
        if (r.template_node->user_data & 1) {
            continue;
        }
        if (r.generator) {
            copy_in(r);
            r.clone->update(ctx, r.timestep);
            if (r.has_outputs) {
                copy_back(r);
            }
        } else {
            update_modifier(ctx, r, r.timestep);
        }
    }
    m_discard_raised = false;
    m_queue_map = saved;
    if (m_update_active_nodes) {
        update_active_nodes();
    } else {
        for (auto& owned : m_records) {
            if (owned->generator) {
                Context ctx = context(owned->behavior);
                static_cast<Generator*>(owned->clone.get())->update_sync(ctx);
            }
        }
    }
    drain();
}

void Graph::raise(Context& ctx, int32_t id, const std::string* payload) {
    if (m_discard_raised || id < 0) {
        return;
    }
    Behavior* b = ctx.behavior ? ctx.behavior : m_queue_map;
    int32_t external = id;
    if (b && size_t(id) < b->event_to_external.size()) {
        external = b->event_to_external[size_t(id)];
    }
    m_queue.push_back({external, payload});
}

void Graph::raise(Context& ctx, const EventProperty& e) {
    raise(ctx, e.id, e.payload.get());
}

void Graph::deliver(int32_t external, const std::string* payload, uint8_t& flag) {
    Context ctx = context(nullptr);
    const size_t count = m_records.size();
    for (size_t i = 0; i < count; ++i) {
        Record& r = *m_records[i];
        if (r.ignore_events) {
            continue;
        }
        int32_t id = external;
        if (r.behavior && external >= 0) {
            id = r.behavior->event_internal(external);
            if (id < 0) {
                continue;
            }
        }
        ctx.behavior = r.behavior;
        m_queue_map = r.behavior;
        Event e{id, payload, nullptr};
        if (r.generator) {
            r.clone->handle_event(ctx, e);
        } else {
            auto* m = static_cast<Modifier*>(r.clone.get());
            if (!m->enable) {
                if (r.activated) {
                    m->deactivate(ctx);
                    r.activated = false;
                }
                continue;
            }
            if (!r.activated) {
                m->activate(ctx);
                r.activated = true;
            }
            m->handle_event(ctx, e);
        }
    }
    if (!(ctx.event_triggered_transition & 1)) {
        ctx.behavior = nullptr;
        global_transitions(ctx, external, payload);
    }
    if (m_update_active_nodes) {
        update_active_nodes();
    }
    flag |= ctx.event_triggered_transition;
}

void Graph::drain() {
    for (int round = 0; round < 5 && !m_queue.empty(); ++round) {
        std::vector<Queued> events(m_queue.begin(), m_queue.end());
        m_queue.clear();
        std::vector<Queued> unique;
        for (const Queued& e : events) {
            if (e.id < 0) {
                continue;
            }
            const bool seen = std::ranges::any_of(unique, [&](const Queued& u) { return u.id == e.id && u.payload == e.payload; });
            if (!seen) {
                unique.push_back(e);
            }
        }
        for (const Queued& e : unique) {
            uint8_t flag = 0;
            deliver(e.id, e.payload, flag);
            if (m_character.on_event && size_t(e.id) < m_project->external_events().size()) {
                m_character.on_event(m_project->external_events()[size_t(e.id)], e.payload);
            }
        }
    }
    m_queue.clear();
}

std::vector<std::string> Graph::active_nodes() const {
    std::vector<std::string> out;
    for (const auto& r : m_records) {
        const Node* n = r->clone ? r->clone.get() : r->template_node;
        if (n && !n->name.empty()) {
            out.push_back(n->name);
        }
    }
    return out;
}

bool Graph::notify(std::string_view event) {
    const int32_t id = m_project->external_event(event);
    if (id < 0) {
        return false;
    }
    uint8_t flag = 0;
    deliver(id, nullptr, flag);
    return flag != 0;
}

void Graph::generate(Output& output) {
    const Skeleton& skeleton = m_project->skeleton();
    output.resize(skeleton.size(), output.floats.size());
    output.clear();
    if (m_outputs.size() < m_records.size()) {
        m_outputs.resize(m_records.size());
    }
    // Each generator has its own output; a pass-through copies its child (hkbGeneratorOutput's recycling is memory only).
    std::vector<const Output*> kids;
    for (size_t i = 0; i < m_records.size(); ++i) {
        Record& r = *m_records[i];
        if (!r.generator) {
            continue;
        }
        Output* out = i + 1 == m_records.size() ? &output : m_outputs[i].get();
        if (!out) {
            m_outputs[i] = std::make_unique<Output>();
            out = m_outputs[i].get();
        }
        if (out != &output) {
            out->resize(skeleton.size(), output.floats.size());
            out->clear();
        }
        kids.clear();
        for (uint32_t k = 0; k < r.child_count; ++k) {
            const int32_t c = m_children[r.first_child + k];
            if (c >= 0 && m_records[size_t(c)]->generator) {
                kids.push_back(size_t(c) + 1 == m_records.size() ? &output : m_outputs[size_t(c)].get());
            }
        }
        Context ctx = context(r.behavior);
        m_queue_map = r.behavior;
        copy_in(r);
        static_cast<Generator*>(r.clone.get())->generate(ctx, kids, *out, 0.0f);
    }
    // Post-generate: missing bones take the reference pose, rotations normalised.
    if (output.pose_fraction > 0.0f) {
        for (size_t b = 0; b < output.pose.size(); ++b) {
            if (!output.all_present && !output.present[b]) {
                output.pose[b] = skeleton.reference[b];
            } else {
                const float n = glm::length(output.pose[b].r);
                if (n > 0.0f) {
                    output.pose[b].r = output.pose[b].r / n;
                }
            }
        }
        output.all_present = true;
    }
    m_queue_map = nullptr;
}

bool Graph::root_motion(float dt, float scale, glm::vec3& translation, glm::vec3& angles) {
    std::vector<ActiveClip> clips;
    active_clips(clips);
    glm::vec3 sum_t(0.0f), sum_r(0.0f);
    float total = 0.0f;
    for (const ActiveClip& c : clips) {
        const ClipMotion* motion = c.name ? m_project->motion(*c.name) : nullptr;
        if (!motion || c.motion_weight <= 0.0f) {
            continue;
        }
        const float w = c.motion_weight;
        const float t1 = c.local_time, t0 = t1 - dt * c.playback_speed; // sampling clamps: before 0 is lost at a loop
        const glm::quat r0 = motion->rotation_at(t0), r1 = motion->rotation_at(t1);
        glm::vec3 delta = glm::conjugate(r0) * (motion->translation_at(t1) - motion->translation_at(t0)) * w;
        glm::vec3 turn = glm::eulerAngles(glm::conjugate(r0) * r1) * w;
        if (c.mirrored) {
            delta.x = -delta.x;
            turn.y = -turn.y;
            turn.z = -turn.z;
        }
        sum_t += delta;
        sum_r += turn;
        total += w;
    }
    if (total <= 0.0f) {
        return false;
    }
    translation = sum_t / total * scale;
    angles = sum_r / total;
    return true;
}

void Graph::modify_step(Context& ctx, Modifier* template_modifier, Output& output) {
    Record* r = record_of(template_modifier);
    if (!r || !r->clone) {
        m_update_active_nodes = true;
        return;
    }
    if (template_modifier->user_data & 2) {
        return;
    }
    auto* m = static_cast<Modifier*>(r->clone.get());
    const bool was = m->enable;
    copy_in(*r);
    Context inner = context(r->behavior);
    if (m->enable) {
        if (!r->activated) {
            m->activate(inner);
            r->activated = true;
        }
        m->modify(inner, output);
        if (r->has_outputs) {
            copy_back(*r);
        }
    } else {
        if (r->activated) {
            m->deactivate(inner);
            r->activated = false;
        }
        if (r->has_outputs && was) {
            copy_back(*r);
        }
    }
    ctx.event_triggered_transition |= inner.event_triggered_transition;
}

void Graph::modify_sync_step(Context& ctx, Modifier* template_modifier, SyncInfo& sync) {
    if (Record* r = record_of(template_modifier); r && r->clone) {
        auto* m = static_cast<Modifier*>(r->clone.get());
        if (m->enable) {
            m->modify_sync(ctx, sync);
        }
    }
}

namespace {

template <typename T>
bool write_word(VariableValues& v, int32_t index, T value) {
    if (index < 0 || size_t(index) >= v.words.size()) {
        return false;
    }
    v.words[size_t(index)] = std::bit_cast<int32_t>(value);
    return true;
}

} // namespace

bool Graph::set_bool(std::string_view name, bool v) { return write_word<int32_t>(m_values, m_project->root_variable(name), v ? 1 : 0); }
bool Graph::set_int(std::string_view name, int32_t v) { return write_word<int32_t>(m_values, m_project->root_variable(name), v); }
bool Graph::set_float(std::string_view name, float v) { return write_word<float>(m_values, m_project->root_variable(name), v); }
bool Graph::set_vector(std::string_view name, const glm::vec4& v) {
    const int32_t i = m_project->root_variable(name);
    if (i < 0 || size_t(i) >= m_values.words.size() || size_t(m_values.words[size_t(i)]) >= m_values.quads.size()) {
        return false;
    }
    m_values.quads[size_t(m_values.words[size_t(i)])] = v;
    return true;
}
bool Graph::get_bool(std::string_view name, bool& v) const {
    const int32_t i = m_project->root_variable(name);
    if (i < 0 || size_t(i) >= m_values.words.size()) {
        return false;
    }
    v = m_values.words[size_t(i)] != 0;
    return true;
}
bool Graph::get_int(std::string_view name, int32_t& v) const {
    const int32_t i = m_project->root_variable(name);
    if (i < 0 || size_t(i) >= m_values.words.size()) {
        return false;
    }
    v = m_values.words[size_t(i)];
    return true;
}
bool Graph::get_float(std::string_view name, float& v) const {
    const int32_t i = m_project->root_variable(name);
    if (i < 0 || size_t(i) >= m_values.words.size()) {
        return false;
    }
    v = real_of(m_values.words[size_t(i)]);
    return true;
}

} // namespace spud::anim
