#include "importer/legacy/data_source.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"

#include <algorithm>
#include <array>

namespace spud::legacy {

namespace {

constexpr std::array k_asset_folders = {"meshes", "textures", "sound", "music", "interface", "scripts", "seq",
                                        "strings", "shadersfx", "lodsettings", "grass", "terrain"};

} // namespace

DataSource::DataSource(const Install& install, bool loose_files) : m_data_dir(install.data_dir), m_vfs(install.vfs) {
    for (const auto& archive_path : install.archives) {
        try {
            auto archive = std::make_unique<BsaArchive>(archive_path);
            auto index = static_cast<uint32_t>(m_archives.size());
            for (uint32_t i = 0; i < archive->entries().size(); ++i) {
                m_files[archive->entries()[i].path] = {index, i};
            }
            m_archives.push_back(std::move(archive));
        } catch (const std::exception& e) {
            log::warn("skipping archive {}: {}", archive_path.filename().string(), e.what());
        }
    }

    size_t loose_count = 0;
    if (loose_files && m_vfs) {
        for (const auto& [rel, real] : *m_vfs) {
            auto slash = rel.find('/');
            auto top = std::string_view(rel).substr(0, slash);
            if (slash != std::string::npos &&
                std::find(k_asset_folders.begin(), k_asset_folders.end(), top) != k_asset_folders.end()) {
                m_files[path::normalize(rel)] = {k_loose, 0};
                ++loose_count;
            }
        }
    } else if (loose_files) {
        for (auto folder : k_asset_folders) {
            auto root = m_data_dir / folder;
            if (!std::filesystem::is_directory(root)) {
                continue;
            }
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                auto rel = std::filesystem::relative(entry.path(), m_data_dir).generic_string();
                m_files[path::normalize(rel)] = {k_loose, 0};
                ++loose_count;
            }
        }
    }
    log::info("data source: {} archives, {} files ({} loose)", m_archives.size(), m_files.size(), loose_count);
}

bool DataSource::exists(std::string_view normalized_path) const {
    return m_files.contains(std::string(normalized_path));
}

std::optional<std::vector<std::byte>> DataSource::read(std::string_view normalized_path) const {
    auto it = m_files.find(std::string(normalized_path));
    if (it == m_files.end()) {
        return std::nullopt;
    }
    if (it->second.archive == k_loose) {
        if (m_vfs) {
            return read_file(m_vfs->at(it->first));
        }
        return read_file(m_data_dir / it->first);
    }
    return m_archives[it->second.archive]->read(it->second.entry);
}

std::string DataSource::origin(std::string_view normalized_path) const {
    auto it = m_files.find(std::string(normalized_path));
    if (it == m_files.end()) {
        return {};
    }
    if (it->second.archive == k_loose) {
        if (m_vfs) {
            return "loose: " + m_vfs->at(it->first).string();
        }
        return "loose";
    }
    return m_archives[it->second.archive]->path().filename().string();
}

std::filesystem::path DataSource::source_file(std::string_view normalized_path) const {
    auto it = m_files.find(std::string(normalized_path));
    if (it == m_files.end()) {
        return {};
    }
    if (it->second.archive == k_loose) {
        return m_vfs ? m_vfs->at(it->first) : m_data_dir / it->first;
    }
    return m_archives[it->second.archive]->path();
}

std::vector<std::string> DataSource::list(std::string_view prefix, std::string_view extension) const {
    std::vector<std::string> out;
    for (const auto& [p, loc] : m_files) {
        if (p.starts_with(prefix) && p.ends_with(extension)) {
            out.push_back(p);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace spud::legacy
