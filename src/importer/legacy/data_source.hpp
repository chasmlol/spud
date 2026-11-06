#pragma once

// The game's merged view of its data: loose files in Data/ override archives,
// and later archives override earlier ones — the same resolution the original
// engine uses.

#include "importer/legacy/bsa.hpp"
#include "importer/legacy/install.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::legacy {

class DataSource {
public:
    // Opens every archive in `install.archives` and indexes loose files under
    // the asset folders of Data/ (meshes, textures, ...).
    explicit DataSource(const Install& install, bool loose_files = true);

    bool exists(std::string_view normalized_path) const;

    // Decompressed file contents, or nullopt if no source has it. Thread-safe.
    std::optional<std::vector<std::byte>> read(std::string_view normalized_path) const;

    // Where a file resolves from: an archive filename or "loose".
    std::string origin(std::string_view normalized_path) const;
    // The file on disk a data path is read from: the loose file, or the archive holding it; empty if none.
    std::filesystem::path source_file(std::string_view normalized_path) const;

    // Every resolvable path starting with `prefix` and ending with `extension` (both normalised).
    std::vector<std::string> list(std::string_view prefix = {}, std::string_view extension = {}) const;

    const std::vector<std::unique_ptr<BsaArchive>>& archives() const { return m_archives; }

private:
    static constexpr uint32_t k_loose = 0xFFFFFFFFu;
    struct Location {
        uint32_t archive; // index into m_archives, or k_loose
        uint32_t entry;
    };

    std::filesystem::path m_data_dir;
    std::shared_ptr<const VirtualData> m_vfs; // a mod manager's merged Data folder, or null
    std::vector<std::unique_ptr<BsaArchive>> m_archives;
    std::unordered_map<std::string, Location> m_files;
};

} // namespace spud::legacy
