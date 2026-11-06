// The game's own texts the menus show: string game settings (GMST "s..." records, e.g. sConfirmNew) from the whole
// load order, resolved through the plugins' string tables. Output: interface/gamesettings.osu (docs/formats/osu.md).
//
// String tables (Strings/<plugin>_<language>.STRINGS, .DLSTRINGS, .ILSTRINGS): uint32 count, uint32 data size, then
// count x {uint32 id, uint32 offset}, then the data; .STRINGS entries are NUL-terminated, the other two carry a
// uint32 length first (UESP "Skyrim Mod:String Table File Format"). English tables are Windows-1252.

#include "importer/convert/ui_strings.hpp"

#include "core/binary.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "formats/container.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/esm.hpp"

#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <unordered_map>

namespace spud::importer {

namespace {

// Windows-1252 -> UTF-8 (0x80..0x9F per the code page, the rest map to the same code point).
std::string cp1252_to_utf8(std::string_view s) {
    static constexpr char16_t k_high[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                                            0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
    std::string out;
    for (unsigned char c : s) {
        uint32_t cp = c;
        if (c >= 0x80 && c < 0xA0) {
            cp = k_high[c - 0x80];
        }
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 63));
        } else {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 63));
            out += char(0x80 | (cp & 63));
        }
    }
    return out;
}

using Table = std::unordered_map<uint32_t, std::string>;

Table read_table(std::span<const std::byte> file, bool length_prefixed, bool utf8) {
    Table out;
    BinaryReader r(file);
    const uint32_t count = r.read<uint32_t>();
    const uint32_t data_size = r.read<uint32_t>();
    const size_t data_start = 8 + size_t(count) * 8;
    if (data_start + data_size > file.size()) {
        throw ParseError("string table shorter than its header says");
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = r.read<uint32_t>();
        const uint32_t offset = r.read<uint32_t>();
        size_t at = data_start + offset;
        if (at >= file.size()) {
            continue;
        }
        if (length_prefixed) {
            at += 4;
        }
        const auto* p = reinterpret_cast<const char*>(file.data()) + at;
        size_t n = 0;
        while (at + n < file.size() && p[n] != '\0') {
            ++n;
        }
        const std::string_view s(p, n);
        out.emplace(id, utf8 ? std::string(s) : cp1252_to_utf8(s));
    }
    return out;
}

} // namespace

struct GameStrings::Tables {
    std::map<size_t, std::array<Table, 3>> by_plugin;
};

GameStrings::GameStrings(const legacy::EsmDatabase& db, const legacy::DataSource& data, std::string_view language)
    : m_db(db), m_data(data), m_language(path::to_lower(language)), m_tables(std::make_unique<Tables>()) {}

GameStrings::~GameStrings() = default;

std::string GameStrings::lookup(size_t plugin, uint32_t id) const {
    auto& tables = m_tables->by_plugin;
    auto it = tables.find(plugin);
    if (it == tables.end()) {
        // Each localized plugin's three tables, loaded on first use.
        std::array<Table, 3> t;
        const bool utf8 = m_language != "english";
        const std::string stem = path::to_lower(std::filesystem::path(m_db.plugins()[plugin].filename).stem().string());
        const char* exts[3] = {".strings", ".dlstrings", ".ilstrings"};
        for (int k = 0; k < 3; ++k) {
            const std::string file = "strings/" + stem + "_" + m_language + exts[k];
            if (auto bytes = m_data.read(file)) {
                try {
                    t[size_t(k)] = read_table(*bytes, k != 0, utf8);
                } catch (const std::exception& e) {
                    log::warn("ui strings: {}: {}", file, e.what());
                }
            }
        }
        it = tables.emplace(plugin, std::move(t)).first;
    }
    for (const auto& t : it->second) {
        if (auto s = t.find(id); s != t.end()) {
            return s->second;
        }
    }
    return {};
}

std::string GameStrings::text(const legacy::Record& rec, legacy::RecordType type) const {
    const legacy::Subrecord* d = rec.find(type);
    if (!d) {
        return {};
    }
    if (rec.plugin().localized) {
        return d->data.size() >= 4 ? lookup(rec.entry().plugin, d->as<uint32_t>()) : std::string();
    }
    return cp1252_to_utf8(d->zstring());
}

osu::Strings import_game_strings(const legacy::EsmDatabase& db, const legacy::DataSource& data,
                                 std::string_view language) {
    const GameStrings strings_of(db, data, language);
    auto lookup = [&](size_t plugin, uint32_t id) { return strings_of.lookup(plugin, id); };

    std::map<std::string, std::string> settings;
    for (const legacy::RecordEntry* e : db.of_type(legacy::record_type("GMST"))) {
        const legacy::Record rec = db.read(*e);
        const std::string_view edid = rec.edid();
        const legacy::Subrecord* d = rec.find(legacy::record_type("DATA"));
        if (edid.empty() || !d) {
            continue;
        }
        // The name's first letter is the setting's type: i int32, u uint32, f float, b bool (a uint32), s string.
        const char type = char(std::tolower(uint8_t(edid[0])));
        if (type != 's') {
            if (d->data.size() < 4) {
                continue;
            }
            switch (type) {
            case 'i': settings[std::string(edid)] = std::to_string(d->as<int32_t>()); break;
            case 'u': settings[std::string(edid)] = std::to_string(d->as<uint32_t>()); break;
            case 'b': settings[std::string(edid)] = d->as<uint32_t>() ? "1" : "0"; break;
            case 'f': settings[std::string(edid)] = std::format("{}", d->as<float>()); break;
            default: break;
            }
            continue;
        }
        if (rec.plugin().localized) {
            if (d->data.size() >= 4) {
                settings[std::string(edid)] = lookup(e->plugin, d->as<uint32_t>());
            }
        } else {
            settings[std::string(edid)] = cp1252_to_utf8(d->zstring());
        }
    }
    container::StringTableBuilder strings;
    osu::Strings out;
    for (const auto& [k, v] : settings) {
        out.entries.push_back({strings.add(k), strings.add(v)});
    }
    const auto bytes = strings.bytes();
    out.strings.resize(bytes.size());
    std::memcpy(out.strings.data(), bytes.data(), bytes.size());
    log::info("ui strings: {} game settings", settings.size());
    return out;
}

osu::Strings import_globals(const legacy::EsmDatabase& db) {
    // GLOB: EDID, FNAM (type 's', 'l' or 'f'), FLTV (the value, a float whatever the type).
    std::map<std::string, std::string> globals;
    for (const legacy::RecordEntry* e : db.of_type(legacy::record_type("GLOB"))) {
        const legacy::Record rec = db.read(*e);
        const legacy::Subrecord* v = rec.find(legacy::record_type("FLTV"));
        if (rec.edid().empty() || !v || v->data.size() < 4) {
            continue;
        }
        globals[std::string(rec.edid())] = std::format("{}", v->as<float>());
    }
    container::StringTableBuilder strings;
    osu::Strings out;
    for (const auto& [k, value] : globals) {
        out.entries.push_back({strings.add(k), strings.add(value)});
    }
    const auto bytes = strings.bytes();
    out.strings.resize(bytes.size());
    std::memcpy(out.strings.data(), bytes.data(), bytes.size());
    log::info("ui strings: {} globals", globals.size());
    return out;
}

osu::Strings import_sound_categories(const legacy::EsmDatabase& db, const legacy::DataSource& data) {
    // SNCT: FNAM flags (bit 1: shown in the Audio menu), PNAM parent, VNAM static volume, UNAM default menu value
    // (both uint16 over 65535). the game's loader keeps FNAM, VNAM and UNAM
    // the game writes UNAM / 65535 as the entry's defaultVal.
    // The data handler keeps forms in the order they were first created: plugins in load order, file order within
    // each, so the first entry of each form ID decides the position and its winning version the values.
    const GameStrings strings_of(db, data, "english");
    const legacy::RecordType snct = legacy::record_type("SNCT");
    std::map<std::string, std::string> keys;
    std::unordered_map<uint32_t, bool> seen;
    int n = 0;
    for (const legacy::RecordEntry& e : db.entries()) {
        if (e.type != snct || !seen.emplace(e.form_id, true).second) {
            continue;
        }
        const auto rec = db.get(e.form_id);
        if (!rec) {
            continue;
        }
        const legacy::Subrecord* flags = rec->find(legacy::record_type("FNAM"));
        if (!flags || flags->data.size() < 4 || !(flags->as<uint32_t>() & 2u)) {
            continue;
        }
        const legacy::Subrecord* unam = rec->find(legacy::record_type("UNAM"));
        const std::string key = std::format("{:03}", n++);
        keys[key + ".id"] = std::format("{:08X}", rec->form_id());
        keys[key + ".name"] = strings_of.text(*rec, legacy::record_type("FULL"));
        keys[key + ".default"] = std::to_string(unam && unam->data.size() >= 2 ? unam->as<uint16_t>() : 0);
    }
    container::StringTableBuilder strings;
    osu::Strings out;
    for (const auto& [k, v] : keys) {
        out.entries.push_back({strings.add(k), strings.add(v)});
    }
    const auto bytes = strings.bytes();
    out.strings.resize(bytes.size());
    std::memcpy(out.strings.data(), bytes.data(), bytes.size());
    log::info("ui strings: {} sound categories in the Audio menu", n);
    return out;
}

void write_game_strings(const legacy::EsmDatabase& db, const legacy::DataSource& data, const std::string& cache_dir) {
    const auto table = import_game_strings(db, data, "english");
    write_file(std::filesystem::path(cache_dir) / "interface" / "gamesettings.osu", osu::write(table));
    write_file(std::filesystem::path(cache_dir) / "interface" / "globals.osu", osu::write(import_globals(db)));
    write_file(std::filesystem::path(cache_dir) / "interface" / "soundcategories.osu",
               osu::write(import_sound_categories(db, data)));
}

} // namespace spud::importer
