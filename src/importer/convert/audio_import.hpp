#pragma once

// Audio import: the load order's audio records -> audio/sound.osd, and the sound and music files under
// Data (archives and loose) -> generic formats in the cache (WAV as is, xWMA and FUZ audio as Ogg Vorbis).
// See docs/formats/osd.md and docs/formats/audio.md.

#include "formats/osd.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/esm.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace spud::importer {

struct AudioRecordsImport {
    osd::SoundDb db;
    // Normalised data paths of every audio file the records name, as resolved (extension fallback applied).
    std::vector<std::string> files;
    std::map<std::string, size_t> missing; // record kind -> files named by records but absent from the data
    std::map<std::string, size_t> counts;  // record kind -> imported
};

// Builds the sound database. `game_dir` supplies Skyrim_Default.ini (INI overrides of the game defaults).
AudioRecordsImport import_audio_records(const legacy::EsmDatabase& db, const legacy::DataSource& data,
                                        const std::filesystem::path& game_dir);

// Where an audio file of the data ends up in the cache: .wav stays, .xwm and .fuz become .ogg.
std::string audio_cache_path(std::string_view data_path);

struct AudioFilesSummary {
    size_t converted = 0;
    size_t copied = 0;
    size_t skipped = 0; // already in the cache
    size_t failed = 0;
    size_t lips = 0;
    uint64_t source_bytes = 0;
    uint64_t output_bytes = 0;
};

// Converts every .wav / .xwm / .fuz under sound/ and music/ in the data into `cache`.
AudioFilesSummary convert_audio_files(const legacy::DataSource& data, const std::filesystem::path& cache,
                                      unsigned threads, bool force);

} // namespace spud::importer
