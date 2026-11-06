#pragma once

// Localized plugin strings:.STRINGS,.DLSTRINGS and.ILSTRINGS tables, looked up the way the game does.
// Facts and their evidence, "Localized strings".
//
// A plugin with the TES4 "localized" flag stores u32 string IDs instead of text in its localized fields. The
// text lives in Strings/<plugin stem>_<sLanguage>.<table>, loose or in any archive. Which table a field uses
// is fixed per field (StringTableKind), and a missing ID behaves differently per table.

#include "importer/legacy/data_source.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::legacy {

enum class StringTableKind : uint8_t { strings = 0, dlstrings = 1, ilstrings = 2 };

std::string_view table_extension(StringTableKind kind); // ".strings", ".dlstrings", ".ilstrings"

// One string table file. The directory is kept sorted by ID; text is returned as stored (the plugin's
// code page, see to_utf8()).
class StringTable {
public:
    StringTable() = default;
    // Throws ParseError on a malformed table.
    StringTable(std::vector<std::byte> bytes, StringTableKind kind);

    StringTableKind kind() const { return m_kind; }
    size_t size() const { return m_ids.size(); }
    std::optional<std::string_view> find(uint32_t id) const;
    // Every (id, text) in ID order.
    template <typename F>
    void for_each(F&& f) const {
        for (size_t i = 0; i < m_ids.size(); ++i) {
            f(m_ids[i], text_at(m_offsets[i]));
        }
    }

private:
    std::string_view text_at(uint32_t offset) const;

    StringTableKind m_kind = StringTableKind::strings;
    std::vector<std::byte> m_bytes;
    size_t m_data_start = 0;
    std::vector<uint32_t> m_ids;     // sorted
    std::vector<uint32_t> m_offsets; // parallel to m_ids, relative to the data block
};

// What a lookup produced, mirroring the game's three outcomes.
enum class LookupResult : uint8_t {
    found,     // text from the table (ID 0 counts as found, with empty text)
    unchanged, // .STRINGS miss: the game leaves the field as it was (empty for a form's first load)
    failed,    // .DLSTRINGS/.ILSTRINGS miss: the game shows the literal text "LOOKUP FAILED!"
};

inline constexpr std::string_view k_lookup_failed = "LOOKUP FAILED!";

// The string tables of every localized plugin, for one language. Thread-safe; tables load on first use.
class Localization {
public:
    // `language` as in sLanguage:General (e.g. "ENGLISH"); file names are matched case-insensitively.
    Localization(const DataSource& data, std::string language);

    const std::string& language() const { return m_language; }

    struct PluginTables {
        std::array<StringTable, 3> tables;
        std::array<bool, 3> present{};      // false: no such file, so every lookup misses
        std::array<std::string, 3> paths;   // normalised data paths
        std::array<std::string, 3> origins; // "loose" or the archive the file came from
    };
    const PluginTables& tables(std::string_view plugin_filename) const;

    // Resolves a localized field's ID. `text` receives the table's raw bytes (or k_lookup_failed).
    LookupResult lookup(std::string_view plugin_filename, StringTableKind kind, uint32_t id,
                        std::string_view& text) const;

    // Strings/<stem>_<language><ext>, normalised.
    std::string table_path(std::string_view plugin_filename, StringTableKind kind) const;

private:
    const DataSource& m_data;
    std::string m_language;
    mutable std::mutex m_mutex;
    mutable std::unordered_map<std::string, std::unique_ptr<PluginTables>> m_plugins;
};

// sLanguage:General as the game resolves it: Skyrim.ini in the documents folder (or an MO2 profile's local
// copy), then Skyrim_Default.ini, then "ENGLISH".
std::string read_language(const std::filesystem::path& skyrim_ini, const std::filesystem::path& default_ini);

// Text encoding of plugin strings for a language: Windows-1252 for English, UTF-8 otherwise (with a
// per-language legacy code page used when the bytes are not valid UTF-8). Returns UTF-8.
std::string to_utf8(std::string_view bytes, std::string_view language);

// The game drops the first 13 characters of an unlocalized description that starts with "<ID=" (a leftover
// "<ID=XXXXXXXX>" debug prefix).
std::string_view strip_id_prefix(std::string_view text);

} // namespace spud::legacy
