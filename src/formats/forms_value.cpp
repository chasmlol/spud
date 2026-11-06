#include "formats/forms_value.hpp"

#include "core/binary.hpp"
#include "core/error.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <map>

namespace spud::forms {

const Value* Value::member(std::string_view name) const {
    for (const auto& [k, v] : members) {
        if (k == name) {
            return &v;
        }
    }
    return nullptr;
}

const Value* Value::path(std::string_view dotted) const {
    const Value* v = this;
    while (v && !dotted.empty()) {
        auto dot = dotted.find('.');
        v = v->member(dotted.substr(0, dot));
        dotted = dot == std::string_view::npos ? std::string_view() : dotted.substr(dot + 1);
    }
    return v;
}

namespace {

void json_string(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void json_value(std::string& out, const Value& v) {
    switch (v.kind) {
    case Value::Kind::none: out += "null"; break;
    case Value::Kind::integer: out += std::to_string(v.integer); break;
    case Value::Kind::real:
        if (std::isfinite(v.real)) {
            out += std::format("{}", v.real);
        } else {
            json_string(out, std::format("{}", v.real));
        }
        break;
    case Value::Kind::form: out += std::format("\"{:08X}\"", v.form); break;
    case Value::Kind::text: json_string(out, v.text); break;
    case Value::Kind::bytes: json_string(out, v.text); break;
    case Value::Kind::object: {
        out += '{';
        bool first = true;
        for (const auto& [k, m] : v.members) {
            if (k == "_") {
                continue;
            }
            if (!first) {
                out += ',';
            }
            first = false;
            json_string(out, k);
            out += ':';
            json_value(out, m);
        }
        out += '}';
        break;
    }
    case Value::Kind::list: {
        out += '[';
        for (size_t i = 0; i < v.items.size(); ++i) {
            if (i) {
                out += ',';
            }
            json_value(out, v.items[i]);
        }
        out += ']';
        break;
    }
    }
}

std::string hex(std::span<const std::byte> b) {
    std::string s;
    s.reserve(b.size() * 2);
    for (auto x : b) {
        s += std::format("{:02x}", static_cast<unsigned>(x));
    }
    return s;
}

Value integer(int64_t v) {
    Value x;
    x.kind = Value::Kind::integer;
    x.integer = v;
    return x;
}

Value real(double v) {
    Value x;
    x.kind = Value::Kind::real;
    x.real = v;
    return x;
}

Value form(uint32_t v) {
    Value x;
    x.kind = Value::Kind::form;
    x.form = v;
    return x;
}

Value text(std::string_view s) {
    Value x;
    x.kind = Value::Kind::text;
    x.text = std::string(s);
    return x;
}

Value bytes(std::span<const std::byte> b) {
    Value x;
    x.kind = Value::Kind::bytes;
    x.text = hex(b);
    return x;
}

class Reader {
public:
    Reader(const Schema& schema, Sig record, uint16_t form_version, std::unordered_map<std::string, int64_t>& vars)
        : m_schema(schema), m_record(record), m_form_version(form_version), m_vars(vars) {}

    Value field(const FieldDef& def, std::span<const std::byte> data) {
        m_size = data.size();
        m_first.reset();
        m_have_char = false;
        m_first_char = 0;
        m_scopes.clear();
        m_top.clear();
        BinaryReader in(data);
        auto v = decode(def.type, in, 0);
        if (!def.sets_var.empty()) {
            int64_t value = 0;
            if (def.sets_value) {
                value = *def.sets_value;
            } else if (def.sets_first_char) {
                value = m_first_char;
            } else if (!def.sets_from.empty()) {
                value = m_top.contains(def.sets_from) ? m_top.at(def.sets_from) : 0;
            } else if (m_first) {
                value = *m_first;
            }
            m_vars[def.sets_var] = value;
        }
        if (in.remaining() != 0) {
            throw ParseError(std::format("{} bytes left over", in.remaining()));
        }
        return v;
    }

private:
    void note(int64_t v) {
        if (!m_first) {
            m_first = v;
        }
        m_last = v;
    }

    std::optional<int64_t> lookup(const std::string& name) const {
        for (auto s = m_scopes.rbegin(); s != m_scopes.rend(); ++s) {
            for (auto it = s->rbegin(); it != s->rend(); ++it) {
                if (*it->first == name) {
                    return it->second;
                }
            }
        }
        return std::nullopt;
    }

    template <typename T>
    Value scalar(BinaryReader& in) {
        auto v = in.read<T>();
        if constexpr (std::is_floating_point_v<T>) {
            note(0);
            return real(v);
        } else {
            note(static_cast<int64_t>(v));
            return integer(static_cast<int64_t>(v));
        }
    }

    std::string_view zstring(BinaryReader& in) {
        auto rest = in.rest();
        std::string_view s(reinterpret_cast<const char*>(rest.data()), rest.size());
        auto nul = s.find('\0');
        if (nul == std::string_view::npos) {
            in.skip(rest.size());
            return s;
        }
        in.skip(nul + 1);
        return s.substr(0, nul);
    }

    std::string_view wstring(BinaryReader& in, bool wide = false) {
        size_t n = wide ? in.read<uint32_t>() : in.read<uint16_t>();
        auto b = in.take(n);
        return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
    }

    Value vmad_object(BinaryReader& in, int16_t format) {
        Value o;
        o.kind = Value::Kind::object;
        if (format == 1) {
            o.members.emplace_back("form", form(in.read<uint32_t>()));
            o.members.emplace_back("alias", integer(in.read<int16_t>()));
            in.skip(2);
        } else {
            in.skip(2);
            o.members.emplace_back("alias", integer(in.read<int16_t>()));
            o.members.emplace_back("form", form(in.read<uint32_t>()));
        }
        return o;
    }

    Value vmad_scripts(BinaryReader& in, int16_t version, int16_t format) {
        Value list;
        list.kind = Value::Kind::list;
        auto count = in.read<uint16_t>();
        for (uint16_t s = 0; s < count; ++s) {
            Value script;
            script.kind = Value::Kind::object;
            script.members.emplace_back("name", text(wstring(in)));
            if (version >= 4) {
                script.members.emplace_back("flags", integer(in.read<uint8_t>()));
            }
            Value props;
            props.kind = Value::Kind::list;
            auto n = in.read<uint16_t>();
            for (uint16_t p = 0; p < n; ++p) {
                Value prop;
                prop.kind = Value::Kind::object;
                prop.members.emplace_back("name", text(wstring(in)));
                auto type = in.read<uint8_t>();
                prop.members.emplace_back("type", integer(type));
                if (version >= 4) {
                    prop.members.emplace_back("flags", integer(in.read<uint8_t>()));
                }
                auto one = [&](uint8_t t) -> Value {
                    switch (t) {
                    case 1: return vmad_object(in, format);
                    case 2: return text(wstring(in));
                    case 3: return integer(in.read<int32_t>());
                    case 4: return real(in.read<float>());
                    case 5: return integer(in.read<uint8_t>());
                    default: return Value{};
                    }
                };
                if (type >= 11 && type <= 15) {
                    Value arr;
                    arr.kind = Value::Kind::list;
                    auto c = in.read<uint32_t>();
                    for (uint32_t i = 0; i < c; ++i) {
                        arr.items.push_back(one(static_cast<uint8_t>(type - 10)));
                    }
                    prop.members.emplace_back("value", std::move(arr));
                } else {
                    prop.members.emplace_back("value", one(type));
                }
                props.items.push_back(std::move(prop));
            }
            script.members.emplace_back("properties", std::move(props));
            list.items.push_back(std::move(script));
        }
        return list;
    }

    Value vmad_fragment(BinaryReader& in) {
        Value f;
        f.kind = Value::Kind::object;
        f.members.emplace_back("unknown", integer(in.read<uint8_t>()));
        f.members.emplace_back("script", text(wstring(in)));
        f.members.emplace_back("fragment", text(wstring(in)));
        return f;
    }

    Value vmad(BinaryReader& in) {
        Value v;
        v.kind = Value::Kind::object;
        auto version = in.read<int16_t>();
        auto format = in.read<int16_t>();
        v.members.emplace_back("version", integer(version));
        v.members.emplace_back("object_format", integer(format));
        v.members.emplace_back("scripts", vmad_scripts(in, version, format));
        if (in.remaining() == 0) {
            return v;
        }
        Value frag;
        frag.kind = Value::Kind::object;
        Value list;
        list.kind = Value::Kind::list;
        if (m_record == sig("INFO") || m_record == sig("PACK") || m_record == sig("SCEN")) {
            frag.members.emplace_back("version", integer(in.read<int8_t>()));
            auto flags = in.read<uint8_t>();
            frag.members.emplace_back("flags", integer(flags));
            frag.members.emplace_back("file", text(wstring(in)));
            int n = std::popcount(static_cast<unsigned>(m_record == sig("PACK") ? flags & 7 : flags & 3));
            for (int i = 0; i < n; ++i) {
                list.items.push_back(vmad_fragment(in));
            }
            if (m_record == sig("SCEN")) {
                Value phases;
                phases.kind = Value::Kind::list;
                auto c = in.read<uint16_t>();
                for (uint16_t i = 0; i < c; ++i) {
                    Value p;
                    p.kind = Value::Kind::object;
                    p.members.emplace_back("flags", integer(in.read<uint8_t>()));
                    p.members.emplace_back("phase", integer(in.read<uint32_t>()));
                    auto fr = vmad_fragment(in);
                    p.members.insert(p.members.end(), fr.members.begin(), fr.members.end());
                    phases.items.push_back(std::move(p));
                }
                frag.members.emplace_back("phases", std::move(phases));
            }
        } else if (m_record == sig("PERK")) {
            frag.members.emplace_back("version", integer(in.read<int8_t>()));
            frag.members.emplace_back("file", text(wstring(in)));
            auto c = in.read<uint16_t>();
            for (uint16_t i = 0; i < c; ++i) {
                auto index = in.read<uint32_t>();
                auto fr = vmad_fragment(in);
                fr.members.insert(fr.members.begin(), {"index", integer(index)});
                list.items.push_back(std::move(fr));
            }
        } else if (m_record == sig("QUST")) {
            frag.members.emplace_back("version", integer(in.read<int8_t>()));
            auto c = in.read<uint16_t>();
            frag.members.emplace_back("file", text(wstring(in)));
            for (uint16_t i = 0; i < c; ++i) {
                auto stage = in.read<uint32_t>();
                auto log = in.read<uint32_t>();
                auto fr = vmad_fragment(in);
                fr.members.insert(fr.members.begin(), {"log_entry", integer(log)});
                fr.members.insert(fr.members.begin(), {"stage", integer(stage)});
                list.items.push_back(std::move(fr));
            }
            Value aliases;
            aliases.kind = Value::Kind::list;
            auto a = in.read<uint16_t>();
            for (uint16_t i = 0; i < a; ++i) {
                Value al;
                al.kind = Value::Kind::object;
                al.members.emplace_back("object", vmad_object(in, format));
                auto av = in.read<int16_t>();
                auto af = in.read<int16_t>();
                al.members.emplace_back("scripts", vmad_scripts(in, av, af));
                aliases.items.push_back(std::move(al));
            }
            v.members.emplace_back("aliases", std::move(aliases));
        }
        frag.members.emplace_back("fragments", std::move(list));
        v.members.emplace_back("fragments", std::move(frag));
        return v;
    }

    Value ctda(BinaryReader& in) {
        Value v;
        v.kind = Value::Kind::object;
        auto rest = in.take(in.remaining());
        BinaryReader r(rest);
        auto u32 = [&](const char* name, bool as_form) {
            if (r.remaining() >= 4) {
                auto x = r.read<uint32_t>();
                v.members.emplace_back(name, as_form ? form(x) : integer(x));
            }
        };
        if (r.remaining() >= 1) {
            auto op = r.read<uint8_t>();
            v.members.emplace_back("operator", integer(op >> 5));
            v.members.emplace_back("flags", integer(op & 0x1F));
            r.skip(std::min<size_t>(3, r.remaining()));
            if (r.remaining() >= 4) {
                if (op & 0x04) {
                    v.members.emplace_back("global", form(r.read<uint32_t>()));
                } else {
                    v.members.emplace_back("value", real(r.read<float>()));
                }
            }
            if (r.remaining() >= 2) {
                v.members.emplace_back("function", integer(r.read<uint16_t>()));
                r.skip(std::min<size_t>(2, r.remaining()));
            }
            u32("param1", false);
            u32("param2", false);
            u32("run_on", false);
            u32("reference", true);
            u32("param3", false);
        }
        return v;
    }

    Value decode(TypeId id, BinaryReader& in, int depth) {
        const auto& t = m_schema.type(id);
        switch (t.kind) {
        case Kind::i8: return scalar<int8_t>(in);
        case Kind::i16: return scalar<int16_t>(in);
        case Kind::i32: return scalar<int32_t>(in);
        case Kind::i64: return scalar<int64_t>(in);
        case Kind::u8: return scalar<uint8_t>(in);
        case Kind::u16: return scalar<uint16_t>(in);
        case Kind::u32: return scalar<uint32_t>(in);
        case Kind::u64: return scalar<uint64_t>(in);
        case Kind::f32: return scalar<float>(in);
        case Kind::f64: return scalar<double>(in);
        case Kind::formid: {
            auto v = in.read<uint32_t>();
            note(v);
            return form(v);
        }
        case Kind::zstring:
        case Kind::lstring: {
            std::string_view s;
            if (t.kind == Kind::zstring && t.sized) {
                auto b = in.take(t.count);
                s = std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
                s = s.substr(0, s.find('\0'));
            } else {
                s = zstring(in);
            }
            note(string_key(s));
            if (!m_have_char) {
                m_have_char = true;
                m_first_char = s.empty() ? 0 : static_cast<unsigned char>(s[0]);
            }
            return text(s);
        }
        case Kind::wstring:
            return text(wstring(in));
        case Kind::wstring32:
            return text(wstring(in, true));
        case Kind::bytes:
            return bytes(t.sized ? in.take(t.count) : in.take(in.remaining()));
        case Kind::empty:
            return Value{};
        case Kind::structure: {
            Value v;
            v.kind = Value::Kind::object;
            m_scopes.emplace_back();
            for (const auto& m : t.members) {
                if (in.remaining() == 0 && depth == 0) {
                    break;
                }
                m_last.reset();
                v.members.emplace_back(m.name, decode(m.type, in, depth + 1));
                if (m_last) {
                    m_scopes.back().emplace_back(&m.name, *m_last);
                    if (depth == 0) {
                        m_top[m.name] = *m_last;
                    }
                }
            }
            m_scopes.pop_back();
            return v;
        }
        case Kind::array: {
            Value v;
            v.kind = Value::Kind::list;
            int64_t count = -1;
            switch (t.count_mode) {
            case CountMode::to_end: break;
            case CountMode::fixed: count = t.count; break;
            case CountMode::prefix_u8: count = in.read<uint8_t>(); break;
            case CountMode::prefix_u16: count = in.read<uint16_t>(); break;
            case CountMode::prefix_u32: count = in.read<uint32_t>(); break;
            case CountMode::member: count = lookup(t.count_member).value_or(0); break;
            }
            if (count < 0) {
                while (in.remaining() > 0) {
                    v.items.push_back(decode(t.element, in, depth + 1));
                }
            } else {
                for (int64_t i = 0; i < count; ++i) {
                    v.items.push_back(decode(t.element, in, depth + 1));
                }
            }
            return v;
        }
        case Kind::union_: {
            int64_t key = 0;
            bool have = true;
            switch (t.selector) {
            case SelectorMode::size: key = static_cast<int64_t>(m_size); break;
            case SelectorMode::formver: key = m_form_version; break;
            case SelectorMode::member: {
                auto x = lookup(t.selector_name);
                have = x.has_value();
                key = x.value_or(0);
                break;
            }
            case SelectorMode::var: {
                auto it = m_vars.find(t.selector_name);
                have = it != m_vars.end();
                key = have ? it->second : 0;
                break;
            }
            }
            TypeId chosen = t.fallback;
            if (have) {
                for (const auto& c : t.cases) {
                    if (c.value == key) {
                        chosen = c.type;
                        break;
                    }
                }
            }
            if (chosen == k_no_type) {
                throw ParseError("no union case");
            }
            return decode(chosen, in, depth);
        }
        case Kind::codec:
            if (t.codec == "vmad") {
                return vmad(in);
            }
            if (t.codec == "ctda") {
                return ctda(in);
            }
            if (t.codec == "coed") {
                Value v;
                v.kind = Value::Kind::object;
                v.members.emplace_back("owner", form(in.read<uint32_t>()));
                v.members.emplace_back("global_or_rank", integer(in.read<uint32_t>()));
                if (in.remaining() >= 4) {
                    v.members.emplace_back("condition", real(in.read<float>()));
                }
                return v;
            }
            return bytes(in.take(in.remaining()));
        }
        return Value{};
    }

    using Scope = std::vector<std::pair<const std::string*, int64_t>>;
    const Schema& m_schema;
    Sig m_record;
    uint16_t m_form_version;
    std::unordered_map<std::string, int64_t>& m_vars;
    std::vector<Scope> m_scopes;
    std::unordered_map<std::string, int64_t> m_top;
    std::optional<int64_t> m_first;
    std::optional<int64_t> m_last;
    size_t m_size = 0;
    bool m_have_char = false;
    int64_t m_first_char = 0;
};

bool holds(const FieldDef& f, const std::unordered_map<std::string, int64_t>& vars) {
    for (const auto& c : f.when) {
        auto it = vars.find(c.var);
        bool match = it != vars.end() && std::find(c.values.begin(), c.values.end(), it->second) != c.values.end();
        if (match == c.negate) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string Value::json() const {
    std::string out;
    json_value(out, *this);
    return out;
}

std::vector<DecodedField> decode_fields(const Schema& schema, Sig record, uint16_t form_version, FieldRange fields) {
    std::vector<DecodedField> out;
    std::unordered_map<std::string, int64_t> vars;
    const auto* def = schema.record(record);
    for (const auto& f : fields) {
        DecodedField d{f.sig, nullptr, {}};
        if (def) {
            for (const auto& c : def->fields) {
                if (c.sig == f.sig && holds(c, vars)) {
                    d.def = &c;
                    break;
                }
            }
        }
        if (d.def) {
            try {
                Reader r(schema, record, form_version, vars);
                d.value = r.field(*d.def, f.data);
            } catch (const std::exception&) {
                // Fields the importer kept raw (they didn't fit the schema) decode as bytes.
                d.def = nullptr;
                d.value = bytes(f.data);
            }
        } else {
            d.value = bytes(f.data);
        }
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<DecodedField> decode_form(const Database& db, const FormEntry& form) {
    return decode_fields(db.schema(), form.type, form.form_version, db.fields(form));
}

std::string form_json(const Database& db, const FormEntry& f) {
    std::string out = std::format("{{\"id\":\"{:08X}\",\"type\":\"{}\",\"flags\":{},\"plugin\":", f.form_id,
                                  sig_name(f.type), f.flags);
    json_string(out, db.plugin_name(f.plugin));
    out += ",\"sources\":[";
    auto src = db.sources(f);
    for (size_t i = 0; i < src.size(); ++i) {
        if (i) {
            out += ',';
        }
        json_string(out, db.plugin_name(src[i]));
    }
    out += std::format("],\"parent\":\"{:08X}\",\"fields\":{{", f.parent);
    // Group repeated signatures into lists, keeping first-appearance order.
    std::vector<std::pair<std::string, std::vector<const Value*>>> groups;
    auto decoded = decode_form(db, f);
    for (const auto& d : decoded) {
        auto key = d.def ? d.def->name : sig_name(d.sig);
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == key; });
        if (it == groups.end()) {
            groups.push_back({key, {&d.value}});
        } else {
            it->second.push_back(&d.value);
        }
    }
    for (size_t i = 0; i < groups.size(); ++i) {
        if (i) {
            out += ',';
        }
        json_string(out, groups[i].first);
        out += ':';
        if (groups[i].second.size() == 1) {
            json_value(out, *groups[i].second[0]);
        } else {
            out += '[';
            for (size_t k = 0; k < groups[i].second.size(); ++k) {
                if (k) {
                    out += ',';
                }
                json_value(out, *groups[i].second[k]);
            }
            out += ']';
        }
    }
    out += "}}";
    return out;
}

} // namespace spud::forms
