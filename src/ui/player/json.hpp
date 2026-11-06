#pragma once

// A small JSON value and parser: menu data captured from the game (Ui::capture), script arguments. Numbers are
// doubles, as in ActionScript; object members keep their order. The same reader as the game's
// so what the game writes reads back unchanged.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace spud::ui::player::json {

struct Value {
    enum Kind { null, boolean, number, string, array, object, undefined } kind = null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> items;                          // array
    std::vector<std::pair<std::string, Value>> members; // object
};

class Parser {
public:
    explicit Parser(const std::string& text) : m_text(text) {}

    bool parse(Value& out, std::string& error) {
        skip();
        if (!value(out)) {
            error = m_error.empty() ? "invalid JSON" : m_error;
            return false;
        }
        skip();
        if (m_pos != m_text.size()) {
            error = "unexpected text after JSON value at offset " + std::to_string(m_pos);
            return false;
        }
        return true;
    }

private:
    const std::string& m_text;
    size_t m_pos = 0;
    std::string m_error;

    void skip() {
        while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\r' ||
                                         m_text[m_pos] == '\n')) {
            ++m_pos;
        }
    }
    bool literal(const char* word) {
        const size_t n = std::char_traits<char>::length(word);
        if (m_text.compare(m_pos, n, word) == 0) {
            m_pos += n;
            return true;
        }
        return false;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) {
            o += static_cast<char>(cp);
        } else if (cp < 0x800) {
            o += static_cast<char>(0xC0 | (cp >> 6));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18));
            o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool str(std::string& o) {
        ++m_pos; // opening quote
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                o += c;
                continue;
            }
            if (m_pos >= m_text.size()) {
                break;
            }
            const char e = m_text[m_pos++];
            switch (e) {
            case 'n': o += '\n'; break;
            case 't': o += '\t'; break;
            case 'r': o += '\r'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'u': {
                if (m_pos + 4 > m_text.size()) {
                    m_error = "bad \\u escape";
                    return false;
                }
                unsigned cp = std::strtoul(m_text.substr(m_pos, 4).c_str(), nullptr, 16);
                m_pos += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && m_text.compare(m_pos, 2, "\\u") == 0 && m_pos + 6 <= m_text.size()) {
                    const unsigned lo = std::strtoul(m_text.substr(m_pos + 2, 4).c_str(), nullptr, 16);
                    m_pos += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(o, cp);
                break;
            }
            default: o += e; break;
            }
        }
        m_error = "unterminated string";
        return false;
    }
    bool value(Value& v) {
        skip();
        if (m_pos >= m_text.size()) {
            m_error = "missing JSON value";
            return false;
        }
        const char c = m_text[m_pos];
        if (c == '"') {
            v.kind = Value::string;
            return str(v.s);
        }
        if (c == '[') {
            v.kind = Value::array;
            ++m_pos;
            skip();
            if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            for (;;) {
                Value item;
                if (!value(item)) {
                    return false;
                }
                v.items.push_back(std::move(item));
                skip();
                if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                    ++m_pos;
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == ']') {
                    ++m_pos;
                    return true;
                }
                m_error = "expected , or ] in array";
                return false;
            }
        }
        if (c == '{') {
            v.kind = Value::object;
            ++m_pos;
            skip();
            if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            for (;;) {
                skip();
                std::string key;
                if (m_pos >= m_text.size() || m_text[m_pos] != '"' || !str(key)) {
                    m_error = "expected a member name";
                    return false;
                }
                skip();
                if (m_pos >= m_text.size() || m_text[m_pos] != ':') {
                    m_error = "expected :";
                    return false;
                }
                ++m_pos;
                Value item;
                if (!value(item)) {
                    return false;
                }
                v.members.emplace_back(std::move(key), std::move(item));
                skip();
                if (m_pos < m_text.size() && m_text[m_pos] == ',') {
                    ++m_pos;
                    continue;
                }
                if (m_pos < m_text.size() && m_text[m_pos] == '}') {
                    ++m_pos;
                    return true;
                }
                m_error = "expected , or } in object";
                return false;
            }
        }
        if (literal("true")) {
            v.kind = Value::boolean;
            v.b = true;
            return true;
        }
        if (literal("false")) {
            v.kind = Value::boolean;
            return true;
        }
        if (literal("null")) {
            v.kind = Value::null;
            return true;
        }
        if (literal("undefined")) { // ActionScript's undefined, as an extension
            v.kind = Value::undefined;
            return true;
        }
        const char* start = m_text.c_str() + m_pos;
        char* end = nullptr;
        v.n = std::strtod(start, &end);
        if (end == start) {
            m_error = "invalid JSON value at offset " + std::to_string(m_pos);
            return false;
        }
        v.kind = Value::number;
        m_pos += static_cast<size_t>(end - start);
        return true;
    }
};

inline bool parse(const std::string& text, Value& out, std::string& error) {
    return Parser(text).parse(out, error);
}

inline std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (const char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                o += buf;
            } else {
                o += c;
            }
        }
    }
    return o;
}

inline std::string quote(const std::string& s) {
    return "\"" + escape(s) + "\"";
}

inline std::string number(double d) {
    if (!std::isfinite(d)) {
        return "null";
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    // Prefer the shortest form that round-trips.
    for (int p = 6; p <= 17; ++p) {
        char t[40];
        std::snprintf(t, sizeof(t), "%.*g", p, d);
        if (std::strtod(t, nullptr) == d) {
            return t;
        }
    }
    return buf;
}

} // namespace spud::ui::player::json

namespace spud::ui::player {

class Vm;
struct Value;

// JSON -> an ActionScript value in `vm`: objects become plain Objects (members in order), arrays Arrays.
Value json_to_value(Vm& vm, const json::Value& v);

} // namespace spud::ui::player
