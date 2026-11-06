#include "importer/records/convert.hpp"

#include "core/binary.hpp"
#include "core/error.hpp"
#include "core/path.hpp"
#include "importer/records/codecs.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <sstream>

namespace spud::records {

using forms::Kind;
using forms::TypeId;

std::string_view issue_name(Diagnostics::Issue issue) {
    switch (issue) {
    case Diagnostics::Issue::unknown_record:
        return "unknown record";
    case Diagnostics::Issue::unknown_field:
        return "unknown field";
    case Diagnostics::Issue::decode_error:
        return "decode error";
    case Diagnostics::Issue::leftover:
        return "leftover bytes";
    case Diagnostics::Issue::truncated:
        return "truncated";
    case Diagnostics::Issue::missing_string:
        return "missing string";
    }
    return "?";
}

void Diagnostics::add(forms::Sig record, forms::Sig field, Issue issue, std::string_view example) {
    std::lock_guard lock(m_mutex);
    auto& e = m_issues[{record, field, issue}];
    ++e.count;
    if (e.examples.size() < 4 && !example.empty()) {
        e.examples.emplace_back(example);
    }
}

void Diagnostics::field_seen(forms::Sig record, forms::Sig field, size_t size) {
    std::lock_guard lock(m_mutex);
    auto& f = m_fields[{record, field}];
    ++f.count;
    f.min_size = std::min(f.min_size, size);
    f.max_size = std::max(f.max_size, size);
}

std::map<std::tuple<forms::Sig, forms::Sig, Diagnostics::Issue>, Diagnostics::Entry> Diagnostics::issues() const {
    std::lock_guard lock(m_mutex);
    return m_issues;
}

std::map<std::pair<forms::Sig, forms::Sig>, Diagnostics::FieldStats> Diagnostics::fields() const {
    std::lock_guard lock(m_mutex);
    return m_fields;
}

void ConditionFunctions::parse(std::string_view text) {
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream ls(line);
        std::string what;
        ls >> what;
        if (what == "param") {
            uint32_t id;
            std::string kind;
            ls >> id >> kind;
            Param p = kind == "form"     ? Param::form
                      : kind == "int"    ? Param::integer
                      : kind == "float"  ? Param::real
                      : kind == "string" ? Param::string
                                         : Param::none;
            param_kinds[id] = p;
        } else if (what == "function") {
            uint32_t index;
            Function f;
            ls >> index >> f.name;
            for (uint32_t t; ls >> t;) {
                f.param_types.push_back(t);
            }
            functions[index] = std::move(f);
        }
    }
}

ConditionFunctions::Param ConditionFunctions::param(uint32_t function, int slot, uint8_t flags) const {
    // GetEventData packs its event function and member into parameter 1 (u16 each) and keeps the event data
    // form in parameter 2.
    if (function == 576) {
        return slot == 0 ? Param::integer : Param::form;
    }
    auto it = functions.find(function);
    if (it == functions.end() || slot >= static_cast<int>(it->second.param_types.size())) {
        return Param::none;
    }
    auto type = it->second.param_types[static_cast<size_t>(slot)];
    auto kind = param_kinds.contains(type) ? param_kinds.at(type) : Param::none;
    // References, actors and packages become alias or package-data indices under the "use aliases" (0x02)
    // and "use pack data" (0x08) flags.
    if (kind == Param::form && (type == 4 || type == 6 || type == 29) && (flags & 0x0A) != 0) {
        return Param::integer;
    }
    return kind;
}

namespace {

using forms::string_key;

std::string hex_preview(std::span<const std::byte> data) {
    std::string s;
    for (size_t i = 0; i < std::min<size_t>(data.size(), 48); ++i) {
        s += std::format("{:02x}", static_cast<unsigned>(data[i]));
    }
    if (data.size() > 48) {
        s += "...";
    }
    return s;
}

class Decoder {
public:
    Decoder(const ConvertContext& ctx, const RawRecord& record, std::vector<std::byte>& out, size_t base,
            std::vector<Fixup>& fixups, ConvertStats& stats, std::unordered_map<std::string, int64_t>& vars)
        : m_ctx(ctx), m_schema(ctx.schema), m_record(record), m_out(out), m_base(base), m_fixups(fixups),
          m_stats(stats), m_vars(vars) {}

    // Decodes one field's payload; returns the value to give a context variable (first scalar / string key).
    void field(const forms::FieldDef& def, std::span<const std::byte> data) {
        m_field_size = data.size();
        m_first_value.reset();
        m_have_char = false;
        m_first_char = 0;
        m_scopes.clear();
        m_truncated = false;
        BinaryReader in(data);
        decode(def.type, in, true, 0);
        if (in.remaining() != 0) {
            throw ParseError(std::format("{} bytes left over", in.remaining()));
        }
        if (!def.sets_var.empty()) {
            int64_t value = 0;
            if (def.sets_value) {
                value = *def.sets_value;
            } else if (def.sets_first_char) {
                value = m_first_char;
            } else if (!def.sets_from.empty()) {
                value = m_top_values.contains(def.sets_from) ? m_top_values.at(def.sets_from) : 0;
            } else if (m_first_value) {
                value = *m_first_value;
            }
            m_vars[def.sets_var] = value;
        }
    }

    bool truncated() const { return m_truncated; }
    std::string_view dropped() const { return m_dropped; }

private:
    using Scope = std::vector<std::pair<const std::string*, int64_t>>;

    void write_bytes(std::span<const std::byte> b) { m_out.insert(m_out.end(), b.begin(), b.end()); }
    template <typename T>
    void write(const T& v) {
        auto p = reinterpret_cast<const std::byte*>(&v);
        m_out.insert(m_out.end(), p, p + sizeof(T));
    }
    void write_text(std::string_view text) {
        auto p = reinterpret_cast<const std::byte*>(text.data());
        m_out.insert(m_out.end(), p, p + text.size());
        m_out.push_back(std::byte{0});
    }

    void note_char(std::string_view text) {
        if (!m_have_char) {
            m_have_char = true;
            m_first_char = text.empty() ? 0 : static_cast<unsigned char>(text[0]);
        }
    }

    void note_value(int64_t v) {
        if (!m_first_value) {
            m_first_value = v;
        }
        m_last_value = v;
    }

    std::optional<int64_t> lookup_member(const std::string& name) const {
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
    void scalar(BinaryReader& in) {
        auto v = in.read<T>();
        write(v);
        if constexpr (std::is_integral_v<T>) {
            note_value(static_cast<int64_t>(v));
        } else {
            note_value(0);
        }
    }

    std::string_view read_zstring(BinaryReader& in, bool last) {
        auto rest = in.rest();
        std::string_view s(reinterpret_cast<const char*>(rest.data()), rest.size());
        auto nul = s.find('\0');
        if (nul == std::string_view::npos) {
            // Some fields end without a terminator; the game reads them to the end of the subrecord.
            in.skip(rest.size());
            return s;
        }
        in.skip(nul + 1);
        if (last && in.remaining() != 0) {
            // Bytes after the terminator of a trailing string are never read by the game (authoring tools
            // leave stale buffer contents there).
            m_dropped = std::string_view(s.substr(nul + 1));
            in.skip(in.remaining());
        }
        return s.substr(0, nul);
    }

    void decode(TypeId id, BinaryReader& in, bool last, int depth) {
        const auto& t = m_schema.type(id);
        switch (t.kind) {
        case Kind::i8: scalar<int8_t>(in); break;
        case Kind::i16: scalar<int16_t>(in); break;
        case Kind::i32: scalar<int32_t>(in); break;
        case Kind::i64: scalar<int64_t>(in); break;
        case Kind::u8: scalar<uint8_t>(in); break;
        case Kind::u16: scalar<uint16_t>(in); break;
        case Kind::u32: scalar<uint32_t>(in); break;
        case Kind::u64: scalar<uint64_t>(in); break;
        case Kind::f32: scalar<float>(in); break;
        case Kind::f64: scalar<double>(in); break;
        case Kind::formid: {
            auto v = in.read<uint32_t>();
            if (v != 0) {
                m_fixups.push_back({static_cast<uint32_t>(m_out.size() - m_base), id});
            }
            write(v);
            note_value(v);
            break;
        }
        case Kind::zstring: {
            if (t.sized) {
                // A fixed buffer: the text up to its NUL (the rest is stale), re-padded with zeros.
                auto b = in.take(t.count);
                std::string_view raw(reinterpret_cast<const char*>(b.data()), b.size());
                auto text = legacy::to_utf8(raw.substr(0, raw.find('\0')), m_ctx.language);
                if (text.size() >= t.count) {
                    throw ParseError("fixed string too long after conversion");
                }
                note_value(string_key(text));
                note_char(text);
                write_bytes(std::as_bytes(std::span(text.data(), text.size())));
                m_out.resize(m_out.size() + (t.count - text.size()), std::byte{0});
                break;
            }
            auto s = read_zstring(in, last);
            auto text = legacy::to_utf8(s, m_ctx.language);
            note_value(string_key(text));
            note_char(text);
            write_text(text);
            break;
        }
        case Kind::lstring: {
            std::string text;
            if (m_ctx.localized) {
                auto string_id = in.read<uint32_t>();
                std::string_view found;
                auto result = legacy::LookupResult::unchanged;
                if (m_ctx.strings) {
                    result = m_ctx.strings->lookup(m_ctx.plugin_filename, static_cast<legacy::StringTableKind>(t.table),
                                                   string_id, found);
                }
                if (result != legacy::LookupResult::found) {
                    ++m_stats.missing_strings;
                    m_missing_string = string_id;
                }
                text = result == legacy::LookupResult::failed ? std::string(found) : legacy::to_utf8(found, m_ctx.language);
            } else {
                auto s = read_zstring(in, last);
                if (t.table == 1) {
                    s = legacy::strip_id_prefix(s);
                }
                text = legacy::to_utf8(s, m_ctx.language);
            }
            note_value(string_key(text));
            write_text(text);
            break;
        }
        case Kind::wstring:
        case Kind::wstring32: {
            size_t n = t.kind == Kind::wstring ? in.read<uint16_t>() : in.read<uint32_t>();
            auto b = in.take(n);
            auto text = legacy::to_utf8(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), m_ctx.language);
            if (t.kind == Kind::wstring) {
                if (text.size() > 0xFFFF) {
                    throw ParseError("wstring too long after conversion");
                }
                write(static_cast<uint16_t>(text.size()));
            } else {
                write(static_cast<uint32_t>(text.size()));
            }
            write_bytes(std::as_bytes(std::span(text.data(), text.size())));
            break;
        }
        case Kind::bytes:
            write_bytes(t.sized ? in.take(t.count) : in.take(in.remaining()));
            break;
        case Kind::empty:
            break;
        case Kind::structure: {
            m_scopes.emplace_back();
            for (size_t i = 0; i < t.members.size(); ++i) {
                const auto& m = t.members[i];
                if (in.remaining() == 0 && depth == 0) {
                    // Older forms store shorter versions of many structs; the game reads what is there.
                    m_truncated = i < t.members.size();
                    break;
                }
                bool member_last = last && i + 1 == t.members.size();
                m_last_value.reset();
                decode(m.type, in, member_last, depth + 1);
                if (m_last_value) {
                    m_scopes.back().emplace_back(&m.name, *m_last_value);
                    if (depth == 0) {
                        m_top_values[m.name] = *m_last_value;
                    }
                }
            }
            m_scopes.pop_back();
            break;
        }
        case Kind::array: {
            const auto& e = m_schema.type(t.element);
            int64_t count = -1;
            switch (t.count_mode) {
            case forms::CountMode::to_end:
                break;
            case forms::CountMode::fixed:
                count = t.count;
                break;
            case forms::CountMode::prefix_u8: {
                auto c = in.read<uint8_t>();
                write(c);
                count = c;
                break;
            }
            case forms::CountMode::prefix_u16: {
                auto c = in.read<uint16_t>();
                write(c);
                count = c;
                break;
            }
            case forms::CountMode::prefix_u32: {
                auto c = in.read<uint32_t>();
                write(c);
                count = c;
                break;
            }
            case forms::CountMode::member: {
                auto c = lookup_member(t.count_member);
                if (!c) {
                    throw ParseError(std::format("array count member '{}' not found", t.count_member));
                }
                count = *c;
                break;
            }
            }
            if (count < 0) {
                if (e.fixed_size && *e.fixed_size > 0 && in.remaining() % *e.fixed_size != 0) {
                    throw ParseError(std::format("{} bytes is not a whole number of {}-byte elements",
                                                 in.remaining(), *e.fixed_size));
                }
                while (in.remaining() > 0) {
                    decode(t.element, in, false, depth + 1);
                }
            } else {
                if (e.fixed_size && *e.fixed_size > 0 && uint64_t(count) * *e.fixed_size > in.remaining()) {
                    throw ParseError(std::format("array of {} x {} bytes overruns the field", count, *e.fixed_size));
                }
                for (int64_t i = 0; i < count; ++i) {
                    decode(t.element, in, last && i + 1 == count, depth + 1);
                }
            }
            break;
        }
        case Kind::union_: {
            int64_t key = 0;
            bool have = true;
            switch (t.selector) {
            case forms::SelectorMode::size:
                key = static_cast<int64_t>(m_field_size);
                break;
            case forms::SelectorMode::formver:
                key = m_record.form_version;
                break;
            case forms::SelectorMode::member: {
                auto v = lookup_member(t.selector_name);
                have = v.has_value();
                key = v.value_or(0);
                break;
            }
            case forms::SelectorMode::var: {
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
            if (chosen == forms::k_no_type) {
                throw ParseError(std::format("no union case for {} = {}", t.selector_name.empty() ? "selector" : t.selector_name, key));
            }
            decode(chosen, in, last, depth);
            break;
        }
        case Kind::codec: {
            CodecContext c{m_ctx, m_record, m_out, m_base, m_fixups};
            run_codec(t.codec, c, in);
            break;
        }
        }
    }

public:
    std::optional<uint32_t> m_missing_string;

private:
    const ConvertContext& m_ctx;
    const forms::Schema& m_schema;
    const RawRecord& m_record;
    std::vector<std::byte>& m_out;
    size_t m_base;
    std::vector<Fixup>& m_fixups;
    ConvertStats& m_stats;
    std::unordered_map<std::string, int64_t>& m_vars;
    std::vector<Scope> m_scopes;
    std::unordered_map<std::string, int64_t> m_top_values;
    std::optional<int64_t> m_first_value;
    std::optional<int64_t> m_last_value;
    size_t m_field_size = 0;
    bool m_have_char = false;
    int64_t m_first_char = 0;
    bool m_truncated = false;
    std::string_view m_dropped;
};

bool conditions_hold(const forms::FieldDef& f, const std::unordered_map<std::string, int64_t>& vars) {
    for (const auto& c : f.when) {
        auto it = vars.find(c.var);
        bool match = false;
        if (it != vars.end()) {
            match = std::find(c.values.begin(), c.values.end(), it->second) != c.values.end();
        }
        if (match == c.negate) {
            return false;
        }
    }
    return true;
}

} // namespace

void convert_record(const ConvertContext& ctx, const RawRecord& record, std::span<const RawSubrecord> fields,
                    std::vector<std::byte>& out, std::vector<Fixup>& fixups, ConvertStats& stats) {
    const auto base = out.size();
    const auto* def = ctx.schema.record(record.type);
    auto* diag = ctx.diagnostics;
    if (!def && diag) {
        diag->add(record.type, 0, Diagnostics::Issue::unknown_record, std::format("{:08X}", record.form_id));
    }
    std::unordered_map<std::string, int64_t> vars;
    for (const auto& f : fields) {
        ++stats.fields;
        if (diag) {
            diag->field_seen(record.type, f.type, f.data.size());
        }
        const auto field_start = out.size();
        const auto fixup_start = fixups.size();
        uint32_t header[2] = {f.type, 0};
        out.insert(out.end(), reinterpret_cast<const std::byte*>(header), reinterpret_cast<const std::byte*>(header) + 8);

        const forms::FieldDef* fd = nullptr;
        if (def) {
            for (const auto& candidate : def->fields) {
                if (candidate.sig == f.type && conditions_hold(candidate, vars)) {
                    fd = &candidate;
                    break;
                }
            }
        }
        bool raw = true;
        if (fd) {
            Decoder d(ctx, record, out, base, fixups, stats, vars);
            try {
                d.field(*fd, f.data);
                raw = false;
                if (diag) {
                    if (d.truncated()) {
                        diag->add(record.type, f.type, Diagnostics::Issue::truncated,
                                  std::format("{:08X} size {}", record.form_id, f.data.size()));
                    }
                    if (d.m_missing_string) {
                        diag->add(record.type, f.type, Diagnostics::Issue::missing_string,
                                  std::format("{:08X} id {:08X}", record.form_id, *d.m_missing_string));
                    }
                    if (!d.dropped().empty() &&
                        std::any_of(d.dropped().begin(), d.dropped().end(), [](char c) { return c != 0; })) {
                        diag->add(record.type, f.type, Diagnostics::Issue::leftover,
                                  std::format("{:08X} after string: {}", record.form_id,
                                              hex_preview(std::as_bytes(std::span(d.dropped().data(), d.dropped().size())))));
                    }
                }
            } catch (const ParseError& e) {
                if (diag) {
                    diag->add(record.type, f.type, Diagnostics::Issue::decode_error,
                              std::format("{:08X} size {}: {} [{}]", record.form_id, f.data.size(), e.what(),
                                          hex_preview(f.data)));
                }
            }
        } else if (def && diag) {
            diag->add(record.type, f.type, Diagnostics::Issue::unknown_field,
                      std::format("{:08X} size {} [{}]", record.form_id, f.data.size(), hex_preview(f.data)));
        }
        if (raw) {
            // Keep the bytes exactly; no FormIDs in them are resolved.
            out.resize(field_start + 8);
            fixups.resize(fixup_start);
            out.insert(out.end(), f.data.begin(), f.data.end());
            ++stats.raw_fields;
        }
        uint32_t size = static_cast<uint32_t>(out.size() - field_start - 8);
        std::memcpy(out.data() + field_start + 4, &size, 4);
    }
}

} // namespace spud::records
