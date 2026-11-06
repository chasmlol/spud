#pragma once

// Imports the load order's records into the forms database (<cache>/forms.osf), reusing every plugin's
// compiled unit (<cache>/.import/forms/<plugin>.unit) whose inputs are unchanged.

#include "importer/legacy/data_source.hpp"
#include "importer/legacy/install.hpp"
#include "importer/records/schema_files.hpp"
#include "importer/records/units.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::records {

// Content hashes of files, remembered by (size, modification time) so unchanged files aren't re-read.
// Stored as <cache>/.import/file-hashes.tsv.
class FileHashCache {
public:
    explicit FileHashCache(std::filesystem::path store);
    uint64_t hash(const std::filesystem::path& file);
    void save() const;

private:
    struct Entry {
        uint64_t size;
        int64_t mtime;
        uint64_t hash;
    };
    std::filesystem::path m_store;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
};

struct FormsImportOptions {
    std::filesystem::path cache_dir;
    std::string language;           // sLanguage
    unsigned threads = 0;           // 0 = hardware concurrency
    bool force = false;             // recompile every unit
    bool keep_units = true;         // write units for the next incremental import
    Diagnostics* diagnostics = nullptr; // non-null: collect conversion issues (forces recompiling)
    bool validate = false;          // check every FormID's target (LinkReport::formid_problems)
    std::optional<std::filesystem::path> out; // default <cache>/forms.osf
};

struct FormsImportResult {
    LinkReport link;
    size_t compiled = 0; // units converted this run
    size_t reused = 0;   // units taken from the cache
    uint64_t fields = 0;
    uint64_t raw_fields = 0;
    uint64_t missing_strings = 0;
    double seconds = 0.0;
    std::filesystem::path path;
};

FormsImportResult import_forms(const legacy::Install& install, const legacy::DataSource& data,
                               const LoadedSchema& schema, const FormsImportOptions& options);

} // namespace spud::records
