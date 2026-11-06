#include "formats/forms_schema.hpp"

#include "core/error.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace spud::forms {

std::string sig_name(Sig s) {
    std::string out(4, '?');
    for (int i = 0; i < 4; ++i) {
        auto c = static_cast<char>((s >> (8 * i)) & 0xFF);
        if (c >= 0x20 && c < 0x7F) {
            out[i] = c;
        }
    }
    return out;
}

uint32_t string_key(std::string_view s) {
    uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c)));
        h *= 16777619u;
    }
    return h;
}

std::optional<Sig> parse_sig(std::string_view s) {
    if (s.size() != 4) {
        return std::nullopt;
    }
    Sig out = 0;
    for (int i = 0; i < 4; ++i) {
        out |= static_cast<uint32_t>(static_cast<uint8_t>(s[i])) << (8 * i);
    }
    return out;
}

namespace {

bool is_sig_token(std::string_view t) {
    if (t.size() != 4) {
        return false;
    }
    bool upper = false;
    for (char c : t) {
        if (c >= 'A' && c <= 'Z') {
            upper = true;
        } else if (!((c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return upper || (t[0] >= '0' && t[0] <= '9');
}

struct Token {
    std::string text;
    int line;
};

std::vector<Token> tokenize(std::string_view text) {
    std::vector<Token> out;
    int line = 1;
    size_t i = 0;
    auto ident_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$' || c == '-';
    };
    while (i < text.size()) {
        char c = text[i];
        if (c == '\n') {
            ++line;
            ++i;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '#') {
            while (i < text.size() && text[i] != '\n') {
                ++i;
            }
        } else if (c == '"') {
            size_t end = text.find('"', i + 1);
            if (end == std::string_view::npos) {
                throw ParseError(std::format("line {}: unterminated string", line));
            }
            out.push_back({std::string(text.substr(i, end - i + 1)), line});
            i = end + 1;
        } else if (c == '-' && i + 1 < text.size() && text[i + 1] == '>') {
            out.push_back({"->", line});
            i += 2;
        } else if (c == '!' && i + 1 < text.size() && text[i + 1] == '=') {
            out.push_back({"!=", line});
            i += 2;
        } else if (ident_char(c)) {
            size_t start = i;
            while (i < text.size() && ident_char(text[i])) {
                ++i;
            }
            out.push_back({std::string(text.substr(start, i - start)), line});
        } else {
            out.push_back({std::string(1, c), line});
            ++i;
        }
    }
    return out;
}

std::optional<int64_t> parse_int(std::string_view s) {
    bool neg = !s.empty() && s[0] == '-';
    if (neg) {
        s.remove_prefix(1);
    }
    int base = 10;
    if (s.starts_with("0x") || s.starts_with("0X")) {
        base = 16;
        s.remove_prefix(2);
    }
    uint64_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (ec != std::errc() || p != s.data() + s.size() || s.empty()) {
        return std::nullopt;
    }
    return neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
}

struct ScalarInfo {
    const char* name;
    Kind kind;
    uint32_t size;
};
constexpr ScalarInfo k_scalars[] = {
    {"i8", Kind::i8, 1},   {"i16", Kind::i16, 2}, {"i32", Kind::i32, 4}, {"i64", Kind::i64, 8},
    {"u8", Kind::u8, 1},   {"u16", Kind::u16, 2}, {"u32", Kind::u32, 4}, {"u64", Kind::u64, 8},
    {"f32", Kind::f32, 4}, {"f64", Kind::f64, 8},
};

} // namespace

class SchemaParser {
public:
    SchemaParser(Schema& schema, std::string_view text, std::string_view file)
        : m_schema(schema), m_tokens(tokenize(text)), m_file(file) {}

    void run() {
        while (!done()) {
            auto& t = peek();
            if (t.text == "type") {
                next();
                auto name = expect_name();
                expect("=");
                auto id = parse_type();
                if (m_schema.m_named.contains(name)) {
                    fail(std::format("type {} defined twice", name));
                }
                // Named types alias their definition; keep the name for messages.
                if (m_schema.m_types[id].name.empty()) {
                    m_schema.m_types[id].name = name;
                }
                m_schema.m_named[name] = id;
            } else if (t.text == "record") {
                next();
                parse_record();
            } else if (t.text == "fields") {
                next();
                auto name = expect_name();
                std::vector<FieldDef> list;
                parse_fields(list);
                expect("end");
                m_fieldsets[name] = std::move(list);
            } else {
                fail(std::format("expected 'type', 'fields' or 'record', found '{}'", t.text));
            }
        }
    }

private:
    bool done() const { return m_pos >= m_tokens.size(); }
    const Token& peek() const {
        if (done()) {
            throw ParseError(std::format("{}: unexpected end of schema", m_file));
        }
        return m_tokens[m_pos];
    }
    bool peek_is(std::string_view s) const { return !done() && m_tokens[m_pos].text == s; }
    const Token& next() {
        auto& t = peek();
        ++m_pos;
        return t;
    }
    [[noreturn]] void fail(const std::string& message) const {
        int line = done() ? (m_tokens.empty() ? 0 : m_tokens.back().line) : m_tokens[m_pos].line;
        throw ParseError(std::format("{}:{}: {}", m_file, line, message));
    }
    void expect(std::string_view s) {
        if (!peek_is(s)) {
            fail(std::format("expected '{}', found '{}'", s, done() ? "end" : peek().text));
        }
        ++m_pos;
    }
    std::string expect_name() {
        auto& t = next();
        if (t.text.empty() || !(std::isalpha(static_cast<unsigned char>(t.text[0])) || t.text[0] == '_')) {
            --m_pos;
            fail(std::format("expected a name, found '{}'", t.text));
        }
        return t.text;
    }
    int64_t expect_int() {
        auto& t = next();
        auto v = parse_int(t.text);
        if (!v) {
            --m_pos;
            fail(std::format("expected a number, found '{}'", t.text));
        }
        return *v;
    }
    Sig expect_sig() {
        auto& t = next();
        // A signature with bytes outside A-Z0-9_ (WTHR ":0TX", IMAD "\0IAD") is written as the hex of its
        // little-endian u32: 0x44414900.
        if (t.text.size() == 10 && t.text.starts_with("0x")) {
            if (auto v = parse_int(t.text)) {
                return static_cast<Sig>(*v);
            }
        }
        if (!is_sig_token(t.text)) {
            --m_pos;
            fail(std::format("expected a four-character signature, found '{}'", t.text));
        }
        return *parse_sig(t.text);
    }

    void parse_record() {
        RecordDef rec;
        rec.sig = expect_sig();
        if (!done() && peek().text.starts_with('"')) {
            auto t = next().text;
            rec.description = t.substr(1, t.size() - 2);
        }
        parse_fields(rec.fields);
        expect("end");
        if (m_schema.m_record_index.contains(rec.sig)) {
            fail(std::format("record {} defined twice", sig_name(rec.sig)));
        }
        m_schema.m_record_index[rec.sig] = m_schema.m_records.size();
        m_schema.m_records.push_back(std::move(rec));
    }

    void parse_fields(std::vector<FieldDef>& fields) {
        while (!peek_is("end")) {
            if (peek_is("use")) {
                next();
                auto name = expect_name();
                auto it = m_fieldsets.find(name);
                if (it == m_fieldsets.end()) {
                    fail(std::format("unknown field set '{}'", name));
                }
                fields.insert(fields.end(), it->second.begin(), it->second.end());
                continue;
            }
            FieldDef f;
            f.sig = expect_sig();
            f.name = expect_name();
            f.type = parse_type();
            while (peek_is("when")) {
                next();
                Condition c;
                auto var = next().text;
                if (!var.starts_with('$')) {
                    --m_pos;
                    fail("expected $variable after 'when'");
                }
                c.var = var.substr(1);
                if (peek_is("!=")) {
                    next();
                    c.negate = true;
                } else {
                    expect("=");
                }
                c.values.push_back(expect_int());
                while (peek_is("|")) {
                    next();
                    c.values.push_back(expect_int());
                }
                f.when.push_back(std::move(c));
            }
            if (peek_is("->")) {
                next();
                auto var = next().text;
                if (!var.starts_with('$')) {
                    --m_pos;
                    fail("expected $variable after '->'");
                }
                f.sets_var = var.substr(1);
                if (peek_is("=")) {
                    next();
                    auto& v = peek().text;
                    if (v == "char0") {
                        next();
                        f.sets_first_char = true;
                    } else if (v.starts_with('.')) {
                        f.sets_from = next().text.substr(1);
                    } else {
                        f.sets_value = expect_int();
                    }
                }
            }
            fields.push_back(std::move(f));
        }
    }

    TypeId parse_type() {
        auto id = parse_base();
        while (peek_is("[")) {
            next();
            Type a;
            a.kind = Kind::array;
            a.element = id;
            if (peek_is("]")) {
                a.count_mode = CountMode::to_end;
            } else {
                auto& t = peek().text;
                if (t == "u8" || t == "u16" || t == "u32") {
                    a.count_mode = t == "u8" ? CountMode::prefix_u8 : t == "u16" ? CountMode::prefix_u16 : CountMode::prefix_u32;
                    next();
                } else if (t.starts_with('.')) {
                    a.count_mode = CountMode::member;
                    a.count_member = next().text.substr(1);
                } else {
                    a.count_mode = CountMode::fixed;
                    a.count = static_cast<uint32_t>(expect_int());
                }
            }
            expect("]");
            id = m_schema.add(std::move(a));
        }
        return id;
    }

    TypeId parse_base() {
        auto t = next().text;
        for (const auto& s : k_scalars) {
            if (t == s.name) {
                Type x;
                x.kind = s.kind;
                return m_schema.add(std::move(x));
            }
        }
        Type x;
        if (t == "formid") {
            x.kind = Kind::formid;
            if (peek_is("(")) {
                next();
                while (true) {
                    if (peek_is("*")) {
                        next();
                    } else {
                        x.targets.push_back(expect_sig());
                    }
                    if (peek_is(",")) {
                        next();
                        continue;
                    }
                    break;
                }
                expect(")");
            }
        } else if (t == "zstring") {
            x.kind = Kind::zstring;
            if (peek_is("(")) {
                // zstring(N): a fixed N-byte buffer holding a NUL-terminated string
                next();
                x.count = static_cast<uint32_t>(expect_int());
                x.sized = true;
                expect(")");
            }
        } else if (t == "lstring" || t == "lstring.dl" || t == "lstring.il") {
            x.kind = Kind::lstring;
            x.table = t == "lstring" ? 0 : t == "lstring.dl" ? 1 : 2;
        } else if (t == "wstring") {
            x.kind = Kind::wstring;
        } else if (t == "wstring32") {
            x.kind = Kind::wstring32;
        } else if (t == "bytes") {
            x.kind = Kind::bytes;
            if (peek_is("(")) {
                next();
                x.count = static_cast<uint32_t>(expect_int());
                x.sized = true;
                expect(")");
            }
        } else if (t == "empty") {
            x.kind = Kind::empty;
        } else if (t == "codec") {
            x.kind = Kind::codec;
            expect("(");
            x.codec = expect_name();
            expect(")");
        } else if (t == "{") {
            x.kind = Kind::structure;
            while (!peek_is("}")) {
                Member m;
                m.name = expect_name();
                expect(":");
                m.type = parse_type();
                x.members.push_back(std::move(m));
            }
            expect("}");
        } else if (t == "union") {
            x.kind = Kind::union_;
            expect("(");
            auto sel = next().text;
            if (sel == "size") {
                x.selector = SelectorMode::size;
            } else if (sel == "formver") {
                x.selector = SelectorMode::formver;
            } else if (sel.starts_with('.')) {
                x.selector = SelectorMode::member;
                x.selector_name = sel.substr(1);
            } else if (sel.starts_with('$')) {
                x.selector = SelectorMode::var;
                x.selector_name = sel.substr(1);
            } else {
                --m_pos;
                fail(std::format("bad union selector '{}'", sel));
            }
            expect(")");
            expect("{");
            while (!peek_is("}")) {
                std::vector<int64_t> values;
                bool fallback = false;
                while (true) {
                    if (peek_is("*")) {
                        next();
                        fallback = true;
                    } else if (!done() && peek().text.starts_with('"')) {
                        auto q = next().text;
                        values.push_back(string_key(q.substr(1, q.size() - 2)));
                    } else {
                        values.push_back(expect_int());
                    }
                    if (peek_is("|")) {
                        next();
                        continue;
                    }
                    break;
                }
                expect(":");
                auto ct = parse_type();
                for (auto v : values) {
                    x.cases.push_back({v, ct});
                }
                if (fallback) {
                    x.fallback = ct;
                }
            }
            expect("}");
        } else if (!t.empty() && std::isupper(static_cast<unsigned char>(t[0])) && !is_sig_token(t)) {
            // A named type, possibly defined later.
            if (auto it = m_schema.m_named.find(t); it != m_schema.m_named.end()) {
                return it->second;
            }
            Type placeholder;
            placeholder.kind = Kind::empty;
            auto id = m_schema.add(std::move(placeholder));
            m_schema.m_unresolved.emplace_back(id, t);
            return id;
        } else {
            --m_pos;
            fail(std::format("expected a type, found '{}'", t));
        }
        return m_schema.add(std::move(x));
    }

    Schema& m_schema;
    std::unordered_map<std::string, std::vector<FieldDef>>& m_fieldsets = m_schema.m_fieldsets;
    std::vector<Token> m_tokens;
    std::string m_file;
    size_t m_pos = 0;
};

TypeId Schema::add(Type t) {
    m_types.push_back(std::move(t));
    return static_cast<TypeId>(m_types.size() - 1);
}

void Schema::parse(std::string_view text, std::string_view file_name) {
    SchemaParser(*this, text, file_name).run();
}

void Schema::compute_size(TypeId id, std::vector<uint8_t>& state) {
    if (state[id] == 2) {
        return;
    }
    if (state[id] == 1) {
        throw ParseError(std::format("schema type {} contains itself", type_text(id)));
    }
    state[id] = 1;
    auto& t = m_types[id];
    std::optional<uint32_t> size;
    switch (t.kind) {
    case Kind::i8:
    case Kind::u8:
        size = 1;
        break;
    case Kind::i16:
    case Kind::u16:
        size = 2;
        break;
    case Kind::i32:
    case Kind::u32:
    case Kind::f32:
    case Kind::formid:
        size = 4;
        break;
    case Kind::i64:
    case Kind::u64:
    case Kind::f64:
        size = 8;
        break;
    case Kind::empty:
        size = 0;
        break;
    case Kind::bytes:
    case Kind::zstring:
        if (t.sized) {
            size = t.count;
        }
        break;
    case Kind::structure: {
        uint32_t total = 0;
        bool fixed = true;
        for (auto& m : t.members) {
            compute_size(m.type, state);
            if (auto s = m_types[m.type].fixed_size) {
                total += *s;
            } else {
                fixed = false;
            }
        }
        if (fixed) {
            size = total;
        }
        break;
    }
    case Kind::array:
        compute_size(t.element, state);
        if (t.count_mode == CountMode::fixed && m_types[t.element].fixed_size) {
            size = t.count * *m_types[t.element].fixed_size;
        }
        break;
    case Kind::union_: {
        std::optional<uint32_t> common;
        bool same = true;
        auto visit = [&](TypeId c) {
            compute_size(c, state);
            auto s = m_types[c].fixed_size;
            if (!s || (common && *common != *s)) {
                same = false;
            }
            common = s;
        };
        for (auto& c : t.cases) {
            visit(c.type);
        }
        if (t.fallback != k_no_type) {
            visit(t.fallback);
        }
        if (same && common) {
            size = common;
        }
        break;
    }
    default:
        break;
    }
    m_types[id].fixed_size = size;
    state[id] = 2;
}

void Schema::finish() {
    for (auto& [id, name] : m_unresolved) {
        auto it = m_named.find(name);
        if (it == m_named.end()) {
            throw ParseError(std::format("schema: unknown type '{}'", name));
        }
        // Copy the definition into the placeholder (types are small and immutable after finish()).
        auto copy = m_types[it->second];
        m_types[id] = std::move(copy);
    }
    m_unresolved.clear();
    std::vector<uint8_t> state(m_types.size(), 0);
    for (TypeId i = 0; i < m_types.size(); ++i) {
        compute_size(i, state);
    }
}

const RecordDef* Schema::record(Sig s) const {
    auto it = m_record_index.find(s);
    return it == m_record_index.end() ? nullptr : &m_records[it->second];
}

std::optional<TypeId> Schema::named(std::string_view name) const {
    auto it = m_named.find(std::string(name));
    if (it == m_named.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::string Schema::type_text(TypeId id) const {
    const auto& t = m_types[id];
    if (!t.name.empty()) {
        return t.name;
    }
    switch (t.kind) {
    case Kind::i8: return "i8";
    case Kind::i16: return "i16";
    case Kind::i32: return "i32";
    case Kind::i64: return "i64";
    case Kind::u8: return "u8";
    case Kind::u16: return "u16";
    case Kind::u32: return "u32";
    case Kind::u64: return "u64";
    case Kind::f32: return "f32";
    case Kind::f64: return "f64";
    case Kind::formid: return "formid";
    case Kind::zstring: return t.sized ? std::format("zstring({})", t.count) : "zstring";
    case Kind::lstring: return t.table == 0 ? "lstring" : t.table == 1 ? "lstring.dl" : "lstring.il";
    case Kind::wstring: return "wstring";
    case Kind::wstring32: return "wstring32";
    case Kind::bytes: return t.sized ? std::format("bytes({})", t.count) : "bytes";
    case Kind::empty: return "empty";
    case Kind::codec: return std::format("codec({})", t.codec);
    case Kind::structure: {
        std::string s = "{";
        for (auto& m : t.members) {
            s += std::format(" {}:{}", m.name, type_text(m.type));
        }
        return s + " }";
    }
    case Kind::array: {
        auto e = type_text(t.element);
        switch (t.count_mode) {
        case CountMode::to_end: return e + "[]";
        case CountMode::fixed: return std::format("{}[{}]", e, t.count);
        case CountMode::prefix_u8: return e + "[u8]";
        case CountMode::prefix_u16: return e + "[u16]";
        case CountMode::prefix_u32: return e + "[u32]";
        case CountMode::member: return std::format("{}[.{}]", e, t.count_member);
        }
        return e + "[]";
    }
    case Kind::union_:
        return "union(...)";
    }
    return "?";
}

} // namespace spud::forms
