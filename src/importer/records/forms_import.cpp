#include "importer/records/forms_import.hpp"

#include "core/error.hpp"
#include "core/hash.hpp"
#include "core/log.hpp"
#include "core/path.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

namespace spud::records {

namespace {

using Clock = std::chrono::steady_clock;

int64_t mtime_of(const std::filesystem::path& p) {
    return std::filesystem::last_write_time(p).time_since_epoch().count();
}

} // namespace

FileHashCache::FileHashCache(std::filesystem::path store) : m_store(std::move(store)) {
    std::ifstream in(m_store);
    for (std::string line; std::getline(in, line);) {
        std::istringstream ls(line);
        std::string path;
        Entry e{};
        if (std::getline(ls, path, '\t') && (ls >> e.size >> e.mtime >> std::hex >> e.hash)) {
            m_entries[path] = e;
        }
    }
}

uint64_t FileHashCache::hash(const std::filesystem::path& file) {
    auto key = path::to_lower(std::filesystem::absolute(file).generic_string());
    auto size = std::filesystem::file_size(file);
    auto mtime = mtime_of(file);
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_entries.find(key); it != m_entries.end() && it->second.size == size && it->second.mtime == mtime) {
            return it->second.hash;
        }
    }
    auto h = hash_file(file);
    std::lock_guard lock(m_mutex);
    m_entries[key] = {size, mtime, h};
    return h;
}

void FileHashCache::save() const {
    std::lock_guard lock(m_mutex);
    std::filesystem::create_directories(m_store.parent_path());
    std::vector<std::string> keys;
    for (const auto& [k, e] : m_entries) {
        keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    std::ofstream out(m_store, std::ios::trunc);
    for (const auto& k : keys) {
        const auto& e = m_entries.at(k);
        out << k << '\t' << e.size << ' ' << e.mtime << ' ' << std::hex << e.hash << std::dec << '\n';
    }
}

FormsImportResult import_forms(const legacy::Install& install, const legacy::DataSource& data,
                               const LoadedSchema& schema, const FormsImportOptions& options) {
    auto start = Clock::now();
    FormsImportResult result;
    const auto threads = options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency());
    const auto import_dir = options.cache_dir / ".import";
    const auto unit_dir = import_dir / "forms";
    FileHashCache hashes(import_dir / "file-hashes.tsv");
    legacy::Localization strings(data, options.language);
    const bool force = options.force || options.diagnostics != nullptr;

    struct Job {
        const legacy::Plugin* plugin;
        std::filesystem::path path;
        uint64_t size = 0;
        std::optional<PluginUnit> unit;
        std::string error;
    };
    std::vector<Job> jobs;
    for (const auto& p : install.load_order) {
        Job j{&p, install.data_file(p.filename)};
        j.size = std::filesystem::exists(j.path) ? std::filesystem::file_size(j.path) : 0;
        jobs.push_back(std::move(j));
    }

    // Unit key: everything a unit is made from.
    auto unit_key = [&](Job& j, uint64_t content) {
        Hasher h;
        h.add(uint64_t(PluginUnit::k_version));
        h.add(content);
        h.add(schema.hash);
        h.add(path::to_lower(options.language));
        for (int k = 0; k < 3; ++k) {
            auto p = strings.table_path(j.plugin->filename, static_cast<legacy::StringTableKind>(k));
            auto bytes = data.read(p);
            h.add(bytes ? xxh64(*bytes) : 0);
        }
        return h.value();
    };

    auto run = [&](Job& j, unsigned inner_threads) {
        try {
            auto content = hashes.hash(j.path);
            auto key = unit_key(j, content);
            auto unit_path = unit_dir / (path::to_lower(j.plugin->filename) + ".unit");
            if (!force) {
                if (auto cached = PluginUnit::read(unit_path); cached && cached->key == key) {
                    j.unit = std::move(cached);
                    return;
                }
            }
            PluginFile file(j.path);
            CompileInputs in{schema.schema, schema.conditions, &strings, options.language, options.diagnostics,
                             inner_threads};
            auto unit = compile_plugin(file, j.plugin->filename, in);
            unit.key = key;
            unit.content_hash = content;
            if (options.keep_units) {
                unit.write(unit_path);
            }
            j.unit = std::move(unit);
            j.error = "compiled";
        } catch (const std::exception& e) {
            j.error = e.what();
        }
    };

    // Big plugins one at a time with every thread; the many small ones side by side, one thread each.
    std::vector<size_t> order(jobs.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return jobs[a].size > jobs[b].size; });
    std::vector<size_t> small;
    for (auto i : order) {
        if (jobs[i].size > (8u << 20)) {
            run(jobs[i], threads);
        } else {
            small.push_back(i);
        }
    }
    {
        std::atomic<size_t> next{0};
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&] {
                for (size_t k; (k = next++) < small.size();) {
                    run(jobs[small[k]], 1);
                }
            });
        }
    }

    std::vector<LinkPlugin> link;
    for (auto& j : jobs) {
        if (!j.unit) {
            log::error("forms: skipping plugin {}: {}", j.plugin->filename, j.error);
            continue;
        }
        if (j.error == "compiled") {
            ++result.compiled;
        } else {
            ++result.reused;
        }
        result.fields += j.unit->stats.fields;
        result.raw_fields += j.unit->stats.raw_fields;
        result.missing_strings += j.unit->stats.missing_strings;
        link.push_back({&*j.unit, j.plugin->master, j.plugin->light});
    }
    hashes.save();

    result.path = options.out.value_or(options.cache_dir / "forms.osf");
    result.link = link_units(link, schema.text, schema.conditions_text, options.language, result.path,
                             options.validate ? &schema.schema : nullptr);
    for (const auto& m : result.link.messages) {
        log::error("forms: {}", m);
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    log::info("forms: {} plugins ({} compiled, {} reused), {} records -> {} forms ({} overridden, {} deleted, {} "
              "layered), {} raw fields, {} missing strings, {:.1f}s -> {}",
              result.link.plugins, result.compiled, result.reused, result.link.records, result.link.forms,
              result.link.overridden, result.link.deleted, result.link.layered, result.raw_fields,
              result.missing_strings, result.seconds, result.path.string());
    return result;
}

} // namespace spud::records
