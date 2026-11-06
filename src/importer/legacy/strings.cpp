#include "importer/legacy/strings.hpp"

#include "core/binary.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/path.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <numeric>

#ifdef _WIN32
#include <windows.h>
#endif

namespace spud::legacy {

namespace {

// Windows-1252 0x80..0x9F. The five undefined bytes map to the C1 controls, as MultiByteToWideChar does,
// so every byte round-trips.
constexpr char16_t k_cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool is_ascii(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

bool valid_utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 4;
        if (n == 4 || i + n >= s.size() + (n == 0 ? 1 : 0)) {
            return false;
        }
        for (size_t k = 1; k <= n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += n + 1;
    }
    return true;
}

std::string from_cp1252(std::string_view s) {
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        append_utf8(out, c < 0x80 ? c : c < 0xA0 ? k_cp1252_high[c - 0x80] : c);
    }
    return out;
}

std::string from_code_page(std::string_view s, unsigned code_page) {
    if (code_page == 1252) {
        return from_cp1252(s);
    }
#ifdef _WIN32
    int wide = MultiByteToWideChar(code_page, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(code_page, 0, s.data(), static_cast<int>(s.size()), w.data(), wide);
    int narrow = WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(narrow), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, out.data(), narrow, nullptr, nullptr);
    return out;
#else
    return from_cp1252(s);
#endif
}

// Legacy code pages of the languages Skyrim ships, for strings that are not valid UTF-8.
unsigned legacy_code_page(std::string_view language) {
    auto l = path::to_lower(language);
    if (l == "polish" || l == "czech" || l == "hungarian") {
        return 1250;
    }
    if (l == "russian") {
        return 1251;
    }
    if (l == "greek") {
        return 1253;
    }
    if (l == "turkish") {
        return 1254;
    }
    if (l == "arabic") {
        return 1256;
    }
    return 1252;
}

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::optional<std::string> ini_value(const std::filesystem::path& ini, std::string_view section,
                                     std::string_view key) {
    std::ifstream in(ini);
    std::string current;
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.empty() || line[0] == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            current = path::to_lower(line.substr(1, line.size() - 2));
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos || current != path::to_lower(section)) {
            continue;
        }
        // GetPrivateProfileString semantics: the first matching key wins.
        if (path::to_lower(trim(line.substr(0, eq))) == path::to_lower(key)) {
            return trim(line.substr(eq + 1));
        }
    }
    return std::nullopt;
}

} // namespace

std::string_view table_extension(StringTableKind kind) {
    switch (kind) {
    case StringTableKind::strings:
        return ".strings";
    case StringTableKind::dlstrings:
        return ".dlstrings";
    case StringTableKind::ilstrings:
        return ".ilstrings";
    }
    return ".strings";
}

StringTable::StringTable(std::vector<std::byte> bytes, StringTableKind kind) : m_kind(kind), m_bytes(std::move(bytes)) {
    BinaryReader r(m_bytes);
    auto count = r.read<uint32_t>();
    auto data_size = r.read<uint32_t>();
    if (count > (r.remaining() / 8)) {
        throw ParseError(std::format("string table directory of {} entries overruns the file", count));
    }
    m_data_start = 8 + size_t(count) * 8;
    if (data_size > m_bytes.size() - m_data_start) {
        throw ParseError(std::format("string table data size {} overruns the file", data_size));
    }
    std::vector<std::pair<uint32_t, uint32_t>> dir(count);
    for (auto& [id, offset] : dir) {
        id = r.read<uint32_t>();
        offset = r.read<uint32_t>();
        if (offset >= data_size) {
            throw ParseError(std::format("string {} at offset {} is past the data block", id, offset));
        }
    }
    // The game inserts entries into a map in file order, so a repeated ID keeps its last text.
    std::stable_sort(dir.begin(), dir.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < dir.size(); ++i) {
        if (i + 1 < dir.size() && dir[i + 1].first == dir[i].first) {
            continue;
        }
        m_ids.push_back(dir[i].first);
        m_offsets.push_back(dir[i].second);
    }
}

std::string_view StringTable::text_at(uint32_t offset) const {
    auto data = std::span<const std::byte>(m_bytes).subspan(m_data_start);
    auto at = data.subspan(offset);
    if (m_kind == StringTableKind::strings) {
        std::string_view s(reinterpret_cast<const char*>(at.data()), at.size());
        return s.substr(0, s.find('\0'));
    }
    // DL/IL: u32 length (including the terminator), then the text.
    BinaryReader r(at);
    auto length = r.read<uint32_t>();
    auto text = r.take(std::min<size_t>(length, r.remaining()));
    std::string_view s(reinterpret_cast<const char*>(text.data()), text.size());
    return s.substr(0, s.find('\0'));
}

std::optional<std::string_view> StringTable::find(uint32_t id) const {
    auto it = std::lower_bound(m_ids.begin(), m_ids.end(), id);
    if (it == m_ids.end() || *it != id) {
        return std::nullopt;
    }
    return text_at(m_offsets[static_cast<size_t>(it - m_ids.begin())]);
}

Localization::Localization(const DataSource& data, std::string language)
    : m_data(data), m_language(std::move(language)) {}

std::string Localization::table_path(std::string_view plugin_filename, StringTableKind kind) const {
    auto stem = std::filesystem::path(plugin_filename).stem().string();
    return path::normalize(std::format("strings/{}_{}{}", stem, m_language, table_extension(kind)));
}

const Localization::PluginTables& Localization::tables(std::string_view plugin_filename) const {
    auto key = path::to_lower(plugin_filename);
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_plugins.find(key); it != m_plugins.end()) {
            return *it->second;
        }
    }
    auto loaded = std::make_unique<PluginTables>();
    for (int k = 0; k < 3; ++k) {
        auto kind = static_cast<StringTableKind>(k);
        auto p = table_path(plugin_filename, kind);
        loaded->paths[k] = p;
        auto bytes = m_data.read(p);
        if (!bytes) {
            continue;
        }
        try {
            loaded->tables[k] = StringTable(std::move(*bytes), kind);
            loaded->present[k] = true;
            loaded->origins[k] = m_data.origin(p);
        } catch (const std::exception& e) {
            log::error("{}: {}", p, e.what());
        }
    }
    std::lock_guard lock(m_mutex);
    auto [it, inserted] = m_plugins.emplace(key, std::move(loaded));
    return *it->second;
}

LookupResult Localization::lookup(std::string_view plugin_filename, StringTableKind kind, uint32_t id,
                                  std::string_view& text) const {
    if (id == 0) {
        text = {};
        return LookupResult::found;
    }
    const auto& t = tables(plugin_filename);
    auto k = static_cast<size_t>(kind);
    if (t.present[k]) {
        if (auto s = t.tables[k].find(id)) {
            text = *s;
            return LookupResult::found;
        }
    }
    if (kind == StringTableKind::strings) {
        text = {};
        return LookupResult::unchanged;
    }
    text = k_lookup_failed;
    return LookupResult::failed;
}

std::string read_language(const std::filesystem::path& skyrim_ini, const std::filesystem::path& default_ini) {
    if (auto v = ini_value(skyrim_ini, "General", "sLanguage"); v && !v->empty()) {
        return *v;
    }
    if (auto v = ini_value(default_ini, "General", "sLanguage"); v && !v->empty()) {
        return *v;
    }
    return "ENGLISH";
}

std::string to_utf8(std::string_view bytes, std::string_view language) {
    if (is_ascii(bytes)) {
        return std::string(bytes);
    }
    if (path::to_lower(language) == "english") {
        return from_cp1252(bytes);
    }
    if (valid_utf8(bytes)) {
        return std::string(bytes);
    }
    return from_code_page(bytes, legacy_code_page(language));
}

std::string_view strip_id_prefix(std::string_view text) {
    if (text.starts_with("<ID=")) {
        return text.size() > 13 ? text.substr(13) : std::string_view();
    }
    return text;
}

} // namespace spud::legacy
