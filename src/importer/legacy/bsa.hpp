#pragma once

// Bethesda BSA archives, versions 104 (Skyrim LE, zlib) and 105 (Skyrim SE/AE, LZ4 frame).
// Reference: UESP "Skyrim Mod:Archive File Format".

#include "core/file.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::legacy {

class BsaArchive {
public:
    struct Entry {
        std::string path; // normalised: "meshes/foo/bar.nif"
        uint64_t offset;  // absolute offset of the file's data
        uint32_t size;    // stored size (compressed size if compressed, incl. any embedded name)
        bool compressed;
    };

    explicit BsaArchive(const std::filesystem::path& path);

    const std::filesystem::path& path() const { return m_path; }
    uint32_t version() const { return m_version; }
    const std::vector<Entry>& entries() const { return m_entries; }

    std::optional<size_t> find(std::string_view normalized_path) const;

    // Returns the file's decompressed contents. Thread-safe.
    std::vector<std::byte> read(size_t index) const;

private:
    std::filesystem::path m_path;
    MappedFile m_file;
    uint32_t m_version = 0;
    uint32_t m_flags = 0;
    std::vector<Entry> m_entries;
    std::unordered_map<std::string, size_t> m_index;
};

} // namespace spud::legacy
