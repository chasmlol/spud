#include "importer/incremental.hpp"

#include "core/file.hpp"
#include "core/hash.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace spud::importer {

AssetCache::AssetCache(const std::filesystem::path& cache_dir, const legacy::DataSource& data)
    : m_cache_dir(cache_dir), m_store(cache_dir / ".import" / "assets.tsv"), m_data(data),
      m_hashes(cache_dir / ".import" / "asset-file-hashes.tsv") {
    std::ifstream in(m_store);
    for (std::string line; std::getline(in, line);) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        m_keys[line.substr(0, tab)] = std::stoull(line.substr(tab + 1), nullptr, 16);
    }
}

uint64_t AssetCache::key(std::string_view source, uint32_t version) {
    auto file = m_data.source_file(source);
    if (file.empty()) {
        return 0;
    }
    Hasher h;
    h.add(uint64_t(version));
    h.add(source);
    h.add(m_hashes.hash(file));
    return h.value();
}

bool AssetCache::fresh(std::string_view output, std::string_view source, uint32_t version) {
    if (!std::filesystem::exists(m_cache_dir / output)) {
        return false;
    }
    auto k = key(source, version);
    std::lock_guard lock(m_mutex);
    auto it = m_keys.find(std::string(output));
    bool ok = k != 0 && it != m_keys.end() && it->second == k;
    ++(ok ? m_reused : m_converted);
    return ok;
}

void AssetCache::record(std::string_view output, std::string_view source, uint32_t version) {
    auto k = key(source, version);
    std::lock_guard lock(m_mutex);
    m_keys[std::string(output)] = k;
}

void AssetCache::save() {
    std::vector<std::pair<std::string, uint64_t>> rows;
    {
        std::lock_guard lock(m_mutex);
        rows.assign(m_keys.begin(), m_keys.end());
    }
    std::sort(rows.begin(), rows.end());
    std::ostringstream out;
    for (const auto& [o, k] : rows) {
        out << o << '\t' << std::hex << k << std::dec << '\n';
    }
    auto text = out.str();
    write_file(m_store, std::as_bytes(std::span(text.data(), text.size())));
    m_hashes.save();
}

bool write_if_changed(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    if (std::filesystem::exists(path) && std::filesystem::file_size(path) == bytes.size()) {
        auto old = read_file(path);
        if (std::memcmp(old.data(), bytes.data(), bytes.size()) == 0) {
            return false;
        }
    }
    write_file(path, bytes);
    return true;
}

} // namespace spud::importer
