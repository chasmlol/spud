#pragma once

// The game's own texts for the menus: string game settings from the load order (docs/formats/osu.md).

#include "formats/osu.hpp"
#include "importer/legacy/esm.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace spud::legacy {
class DataSource;
} // namespace spud::legacy

namespace spud::importer {

// The plugins' string tables (Strings/<plugin>_<language>.STRINGS, .DLSTRINGS, .ILSTRINGS), loaded on first use.
class GameStrings {
public:
    GameStrings(const legacy::EsmDatabase& db, const legacy::DataSource& data, std::string_view language);
    ~GameStrings();
    GameStrings(const GameStrings&) = delete;
    GameStrings& operator=(const GameStrings&) = delete;

    // A string-table entry of a plugin, "" if absent.
    std::string lookup(size_t plugin, uint32_t id) const;
    // A text subrecord (FULL, DESC, ...) of a record as UTF-8: through the tables for localized plugins.
    std::string text(const legacy::Record& rec, legacy::RecordType type) const;

private:
    struct Tables;
    const legacy::EsmDatabase& m_db;
    const legacy::DataSource& m_data;
    std::string m_language;
    std::unique_ptr<Tables> m_tables;
};

// GMST records (winning versions) -> name -> text: strings through the plugins' string tables, numbers (i, u, f, b)
// as decimal text.
osu::Strings import_game_strings(const legacy::EsmDatabase& db, const legacy::DataSource& data,
                                 std::string_view language);
// GLOB records (winning versions) -> name -> starting value as decimal text: the calendar (GameYear, GameMonth,
// GameDay, GameHour, GameDaysPassed) and the other globals, as a new game begins.
osu::Strings import_globals(const legacy::EsmDatabase& db);
// SNCT records with the "show in menu" flag (FNAM bit 1), in the data handler's order, as the Journal's Audio page
// lists them: "NNN.id" (form ID, hex), "NNN.name" (FULL), "NNN.default" (UNAM, the default menu value, 0..65535).
osu::Strings import_sound_categories(const legacy::EsmDatabase& db, const legacy::DataSource& data);
// Writes <cache>/interface/gamesettings.osu, globals.osu and soundcategories.osu.
void write_game_strings(const legacy::EsmDatabase& db, const legacy::DataSource& data, const std::string& cache_dir);

} // namespace spud::importer
