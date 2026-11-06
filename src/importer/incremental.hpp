#pragma once

// Incremental import: which cache outputs are still valid for the current sources.
//
// Every output written from a data file (a converted mesh, an extracted texture) is recorded in
// <cache>/.import/assets.tsv with a key: the content hash of the file it came from (the loose file itself, or
// the whole archive holding it, plus its path), and the converter version. On the next import an output is
// reused only if its recorded key matches the current source, so a changed plugin, archive or loose file, or a
// different mod winning a file, re-converts exactly what it affects. File hashes are remembered by size and
// modification time (records::FileHashCache) so unchanged files aren't read again.

#include "importer/legacy/data_source.hpp"
#include "importer/records/forms_import.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace spud::importer {

// Bump when a converter's output for the same input changes, so old outputs are redone.
// Meshes: 2 .osm version 8; 3 the .ose particle effect written with the mesh (it covers the .ose too);
// 4 vertices and triangles in GPU cache order; 5 .osm version 9, simplified levels; 7 levels weigh vertex colours, leave
// cut-outs whole and hold foliage back.
inline constexpr uint32_t k_mesh_converter_version = 7;
inline constexpr uint32_t k_collision_converter_version = 2;
inline constexpr uint32_t k_texture_copy_version = 1;

class AssetCache {
public:
    AssetCache(const std::filesystem::path& cache_dir, const legacy::DataSource& data);

    // The key an output made from `source` (a normalised data path) by converter `version` would have; 0 if the
    // source doesn't exist.
    uint64_t key(std::string_view source, uint32_t version);
    // True if `output` (relative to the cache) exists and was made from the current `source`.
    bool fresh(std::string_view output, std::string_view source, uint32_t version);
    void record(std::string_view output, std::string_view source, uint32_t version);
    // Writes assets.tsv and the file hash cache.
    void save();

    size_t reused() const { return m_reused; }
    size_t converted() const { return m_converted; }

private:
    std::filesystem::path m_cache_dir;
    std::filesystem::path m_store;
    const legacy::DataSource& m_data;
    records::FileHashCache m_hashes;
    std::mutex m_mutex;
    std::unordered_map<std::string, uint64_t> m_keys; // output -> key
    size_t m_reused = 0;
    size_t m_converted = 0;
};

// Writes `bytes` to `path` unless the file already holds exactly these bytes (an unchanged world is not
// rewritten). Returns true if it wrote.
bool write_if_changed(const std::filesystem::path& path, std::span<const std::byte> bytes);

} // namespace spud::importer
