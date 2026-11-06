#include "importer/records/units.hpp"

#include "core/binary.hpp"
#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <thread>
#include <unordered_map>

namespace spud::records {

namespace {

constexpr container::FourCC k_unit_kind = container::fourcc("FUNT");

// Writes an SPUD container straight to a file, chunk by chunk, so multi-hundred-megabyte payloads are never
// copied into one buffer. Same bytes as container::Writer. Written to a temporary file and renamed.
class StreamWriter {
public:
    StreamWriter(const std::filesystem::path& path, container::FourCC kind, uint32_t version, uint32_t chunks)
        : m_path(path), m_tmp(path) {
        m_tmp += ".tmp";
        std::filesystem::create_directories(path.parent_path());
        m_out.open(m_tmp, std::ios::binary | std::ios::trunc);
        if (!m_out) {
            throw Error("cannot write " + m_tmp.string());
        }
        container::FileHeader h{container::k_magic, kind, version, chunks};
        put(&h, sizeof h);
    }

    void chunk(container::FourCC id, std::span<const std::byte> payload) {
        container::ChunkHeader h{id, 0, payload.size()};
        put(&h, sizeof h);
        put(payload.data(), payload.size());
        static const std::byte zeros[16]{};
        put(zeros, (16 - payload.size() % 16) % 16);
        ++m_written;
    }
    template <typename T>
    void chunk_of(container::FourCC id, std::span<const T> items) {
        chunk(id, std::as_bytes(items));
    }
    template <typename T>
    void chunk_of(container::FourCC id, const std::vector<T>& items) {
        chunk(id, std::as_bytes(std::span<const T>(items)));
    }
    void text(container::FourCC id, std::string_view s) { chunk(id, std::as_bytes(std::span(s.data(), s.size()))); }

    void finish() {
        m_out.close();
        if (!m_out) {
            throw Error("write failed: " + m_tmp.string());
        }
        std::filesystem::rename(m_tmp, m_path);
    }

private:
    void put(const void* p, size_t n) { m_out.write(static_cast<const char*>(p), static_cast<std::streamsize>(n)); }

    std::filesystem::path m_path;
    std::filesystem::path m_tmp;
    std::ofstream m_out;
    uint32_t m_written = 0;
};

void put_string(BinaryWriter& w, std::string_view s) {
    w.write(static_cast<uint32_t>(s.size()));
    w.write_string(s);
}

std::string get_string(BinaryReader& r) {
    auto n = r.read<uint32_t>();
    return r.read_string(n);
}

struct Chunk {
    std::vector<std::byte> data;
    std::vector<uint32_t> fixups;
    std::vector<uint32_t> fixup_types;
    std::vector<FixupCondition> conditions;
    std::vector<UnitRecord> records;
    ConvertStats stats;
};

// Converts records [begin, end) of `file` into a chunk.
void convert_range(const PluginFile& file, size_t begin, size_t end, const ConvertContext& ctx, Chunk& out) {
    std::vector<std::byte> storage;
    std::vector<Fixup> fixups;
    for (size_t i = begin; i < end; ++i) {
        const auto& raw = file.records()[i];
        UnitRecord u{};
        u.form_id = raw.form_id;
        u.type = raw.type;
        u.flags = raw.flags;
        u.vc_stamp = raw.vc_stamp;
        u.form_version = raw.form_version;
        u.vc_unknown = raw.vc_unknown;
        u.parent = raw.parent;
        u.group = raw.group;
        u.data_offset = out.data.size();
        u.first_fixup = static_cast<uint32_t>(out.fixups.size());
        fixups.clear();
        try {
            auto subs = PluginFile::subrecords(raw, storage);
            convert_record(ctx, raw, subs, out.data, fixups, out.stats);
        } catch (const ParseError& e) {
            // A record that doesn't even split into subrecords: keep its payload as one raw pseudo-field.
            log::error("{}: {} {:08X}: {}", ctx.plugin_filename, forms::sig_name(raw.type), raw.form_id, e.what());
            out.data.resize(u.data_offset);
            uint32_t header[2] = {forms::sig("RAW_"), static_cast<uint32_t>(raw.payload.size())};
            out.data.insert(out.data.end(), reinterpret_cast<std::byte*>(header), reinterpret_cast<std::byte*>(header) + 8);
            out.data.insert(out.data.end(), raw.payload.begin(), raw.payload.end());
            fixups.clear();
        }
        u.data_size = static_cast<uint32_t>(out.data.size() - u.data_offset);
        for (const auto& f : fixups) {
            if (f.when_type != 0) {
                out.conditions.push_back({static_cast<uint32_t>(out.fixups.size()), f.when_offset, f.when_type});
            }
            out.fixups.push_back(f.offset);
            out.fixup_types.push_back(f.type);
        }
        u.fixup_count = static_cast<uint32_t>(fixups.size());
        out.records.push_back(u);
    }
}

} // namespace

PluginUnit compile_plugin(const PluginFile& file, const std::string& filename, const CompileInputs& in) {
    PluginUnit unit;
    unit.filename = filename;
    const auto& h = file.header();
    unit.header_flags = h.flags;
    unit.header_form_version = h.form_version;
    unit.version = h.version;
    unit.author = h.author;
    unit.description = h.description;
    unit.masters = h.masters;
    unit.overrides = h.overrides;

    ConvertContext ctx{in.schema, in.conditions, in.strings, filename, in.language, (h.flags & 0x80) != 0,
                       in.diagnostics};

    // The TES4 header record, converted like any other.
    {
        Chunk c;
        RawRecord tes4{};
        tes4.type = forms::sig("TES4");
        tes4.flags = h.flags;
        tes4.form_version = h.form_version;
        tes4.group = 0xFF;
        tes4.payload = h.raw;
        std::vector<std::byte> storage;
        std::vector<Fixup> fixups;
        auto subs = PluginFile::subrecords(tes4, storage);
        convert_record(ctx, tes4, subs, c.data, fixups, c.stats);
        unit.header.type = tes4.type;
        unit.header.flags = h.flags;
        unit.header.form_version = h.form_version;
        unit.header.group = 0xFF;
        unit.header.data_offset = 0;
        unit.header.data_size = static_cast<uint32_t>(c.data.size());
        unit.header.first_fixup = 0;
        unit.header.fixup_count = static_cast<uint32_t>(fixups.size());
        unit.data = std::move(c.data);
        for (const auto& f : fixups) {
            if (f.when_type != 0) {
                unit.fixup_conditions.push_back({static_cast<uint32_t>(unit.fixups.size()), f.when_offset, f.when_type});
            }
            unit.fixups.push_back(f.offset);
            unit.fixup_types.push_back(f.type);
        }
    }

    // Records in parallel chunks, concatenated in file order.
    const size_t n = file.records().size();
    const size_t chunk_size = 4096;
    const size_t chunk_count = (n + chunk_size - 1) / chunk_size;
    std::vector<Chunk> chunks(chunk_count);
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t c; (c = next++) < chunk_count;) {
            convert_range(file, c * chunk_size, std::min(n, (c + 1) * chunk_size), ctx, chunks[c]);
        }
    };
    {
        std::vector<std::jthread> pool;
        auto threads = std::max<unsigned>(1, std::min<unsigned>(in.threads, static_cast<unsigned>(chunk_count)));
        for (unsigned t = 1; t < threads; ++t) {
            pool.emplace_back(worker);
        }
        worker();
    }
    unit.records.reserve(n);
    for (auto& c : chunks) {
        const uint64_t data_base = unit.data.size();
        const auto fixup_base = static_cast<uint32_t>(unit.fixups.size());
        for (auto r : c.records) {
            r.data_offset += data_base;
            r.first_fixup += fixup_base;
            unit.records.push_back(r);
        }
        for (auto cond : c.conditions) {
            cond.fixup += fixup_base;
            unit.fixup_conditions.push_back(cond);
        }
        unit.data.insert(unit.data.end(), c.data.begin(), c.data.end());
        unit.fixups.insert(unit.fixups.end(), c.fixups.begin(), c.fixups.end());
        unit.fixup_types.insert(unit.fixup_types.end(), c.fixup_types.begin(), c.fixup_types.end());
        unit.stats.fields += c.stats.fields;
        unit.stats.raw_fields += c.stats.raw_fields;
        unit.stats.missing_strings += c.stats.missing_strings;
        c = {};
    }
    return unit;
}

void PluginUnit::write(const std::filesystem::path& path) const {
    BinaryWriter meta;
    put_string(meta, filename);
    meta.write(key);
    meta.write(content_hash);
    meta.write(header_flags);
    meta.write(header_form_version);
    meta.write(version);
    put_string(meta, author);
    put_string(meta, description);
    meta.write(static_cast<uint32_t>(masters.size()));
    for (const auto& m : masters) {
        put_string(meta, m);
    }
    meta.write(static_cast<uint32_t>(overrides.size()));
    meta.write_span(std::span<const uint32_t>(overrides));
    meta.write(header);
    meta.write(stats.fields);
    meta.write(stats.raw_fields);
    meta.write(stats.missing_strings);

    StreamWriter w(path, k_unit_kind, k_version, 6);
    w.chunk(container::fourcc("META"), meta.buffer());
    w.chunk_of(container::fourcc("RECS"), records);
    w.chunk(container::fourcc("DATA"), data);
    w.chunk_of(container::fourcc("FIXS"), fixups);
    w.chunk_of(container::fourcc("FIXT"), fixup_types);
    w.chunk_of(container::fourcc("FIXC"), fixup_conditions);
    w.finish();
}

std::optional<PluginUnit> PluginUnit::read(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }
    try {
        MappedFile file(path);
        container::Reader r(file.bytes(), k_unit_kind);
        if (r.version() != k_version) {
            return std::nullopt;
        }
        PluginUnit u;
        BinaryReader meta(r.require(container::fourcc("META")));
        u.filename = get_string(meta);
        u.key = meta.read<uint64_t>();
        u.content_hash = meta.read<uint64_t>();
        u.header_flags = meta.read<uint32_t>();
        u.header_form_version = meta.read<uint32_t>();
        u.version = meta.read<float>();
        u.author = get_string(meta);
        u.description = get_string(meta);
        auto masters = meta.read<uint32_t>();
        for (uint32_t i = 0; i < masters; ++i) {
            u.masters.push_back(get_string(meta));
        }
        u.overrides = meta.read_array<uint32_t>(meta.read<uint32_t>());
        u.header = meta.read<UnitRecord>();
        u.stats.fields = meta.read<uint64_t>();
        u.stats.raw_fields = meta.read<uint64_t>();
        u.stats.missing_strings = meta.read<uint64_t>();
        u.records = r.read_array<UnitRecord>(container::fourcc("RECS"));
        auto data = r.require(container::fourcc("DATA"));
        u.data.assign(data.begin(), data.end());
        u.fixups = r.read_array<uint32_t>(container::fourcc("FIXS"));
        u.fixup_types = r.read_array<uint32_t>(container::fourcc("FIXT"));
        u.fixup_conditions = r.read_array<FixupCondition>(container::fourcc("FIXC"));
        return u;
    } catch (const std::exception& e) {
        log::warn("ignoring cached unit {}: {}", path.string(), e.what());
        return std::nullopt;
    }
}

LinkReport link_units(std::span<const LinkPlugin> load_order, std::string_view schema_text,
                      std::string_view conditions_text, std::string_view language,
                      const std::filesystem::path& out_path, const forms::Schema* validate) {
    LinkReport report;

    // Plugins that can load, with their index assignment and FormID resolution tables.
    struct Loaded {
        const LinkPlugin* plugin;
        forms::PluginEntry entry{};
        std::vector<uint16_t> masters; // plugin indices
        std::vector<uint32_t> bases;   // per master slot, then the plugin itself
        std::vector<uint32_t> masks;
    };
    std::vector<Loaded> loaded;
    std::unordered_map<std::string, size_t> by_name;
    uint32_t full_count = 0;
    uint32_t light_count = 0;
    for (const auto& p : load_order) {
        const auto& u = *p.unit;
        Loaded l{&p};
        bool ok = true;
        for (const auto& m : u.masters) {
            auto it = by_name.find(path::to_lower(m));
            if (it == by_name.end()) {
                report.messages.push_back(std::format("skipped {}: master {} is not loaded before it", u.filename, m));
                ok = false;
                break;
            }
            l.masters.push_back(static_cast<uint16_t>(it->second));
        }
        if (!ok) {
            ++report.skipped_plugins;
            continue;
        }
        if (p.light ? light_count >= 0x1000 : full_count >= 0xFE) {
            report.messages.push_back(std::format("skipped {}: too many {} plugins", u.filename, p.light ? "light" : "full"));
            ++report.skipped_plugins;
            continue;
        }
        auto& e = l.entry;
        e.flags = (p.master ? forms::plugin_master : 0) | (p.light ? forms::plugin_light : 0) |
                  ((u.header_flags & 0x80) ? forms::plugin_localized : 0);
        e.header_flags = u.header_flags;
        e.version = u.version;
        e.form_version = u.header_form_version;
        if (p.light) {
            e.mod_index = 0xFE;
            e.light_index = static_cast<uint16_t>(light_count++);
        } else {
            e.mod_index = static_cast<uint8_t>(full_count++);
        }
        e.record_count = static_cast<uint32_t>(u.records.size());
        e.content_hash = u.content_hash;
        auto add_slot = [&l](const forms::PluginEntry& target, bool light) {
            if (light) {
                // Light plugins only own a 12-bit object ID range.
                l.bases.push_back(0xFE000000u | (uint32_t(target.light_index) << 12));
                l.masks.push_back(0xFFFu);
            } else {
                l.bases.push_back(uint32_t(target.mod_index) << 24);
                l.masks.push_back(0xFFFFFFu);
            }
        };
        for (auto m : l.masters) {
            add_slot(loaded[m].entry, loaded[m].plugin->light);
        }
        add_slot(e, p.light);
        by_name[path::to_lower(u.filename)] = loaded.size();
        loaded.push_back(std::move(l));
    }
    report.plugins = loaded.size();

    auto resolve = [&](const Loaded& l, uint32_t local) -> uint32_t {
        if (local == 0) {
            return 0;
        }
        // Indices past the master list mean "this plugin".
        size_t slot = std::min<size_t>(local >> 24, l.bases.size() - 1);
        return l.bases[slot] | (local & l.masks[slot]);
    };

    // Every record of every plugin by resolved FormID; within one FormID, load order then file order.
    struct Ref {
        uint32_t form_id;
        uint16_t plugin;
        uint32_t record;
    };
    std::vector<Ref> refs;
    for (size_t p = 0; p < loaded.size(); ++p) {
        const auto& u = *loaded[p].plugin->unit;
        for (uint32_t i = 0; i < u.records.size(); ++i) {
            refs.push_back({resolve(loaded[p], u.records[i].form_id), static_cast<uint16_t>(p), i});
        }
        report.records += u.records.size();
    }
    std::stable_sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) { return a.form_id < b.form_id; });

    // The winning record type of every FormID, for conditional fixups.
    std::vector<std::pair<uint32_t, Sig>> winner_types;
    for (size_t i = 0; i < refs.size(); ++i) {
        if (i + 1 == refs.size() || refs[i + 1].form_id != refs[i].form_id) {
            winner_types.emplace_back(refs[i].form_id, loaded[refs[i].plugin].plugin->unit->records[refs[i].record].type);
        }
    }
    auto type_of = [&](uint32_t id) -> Sig {
        auto it = std::lower_bound(winner_types.begin(), winner_types.end(), id,
                                   [](const auto& w, uint32_t v) { return w.first < v; });
        return it != winner_types.end() && it->first == id ? it->second : 0;
    };

    container::StringTableBuilder strings;
    for (auto& l : loaded) {
        l.entry.name = strings.add(l.plugin->unit->filename);
        l.entry.author = strings.add(l.plugin->unit->author);
        l.entry.description = strings.add(l.plugin->unit->description);
    }
    std::vector<uint16_t> master_list;
    for (auto& l : loaded) {
        l.entry.first_master = static_cast<uint32_t>(master_list.size());
        l.entry.master_count = static_cast<uint32_t>(l.masters.size());
        master_list.insert(master_list.end(), l.masters.begin(), l.masters.end());
    }

    std::vector<forms::FormEntry> forms_out;
    std::vector<uint16_t> sources;
    std::vector<uint8_t> source_groups; // parallel to sources
    std::vector<uint32_t> fixups_out;
    std::vector<uint32_t> fixup_types_out; // parallel, for validation
    std::vector<std::byte> data;
    // Appends one record's fields; its FormID positions are recorded relative to `form_start` (the start of the
    // form's data, which spans several records for a layered form).
    auto append_record = [&](const Loaded& l, const UnitRecord& r, uint64_t& offset, uint32_t& first_fixup,
                             uint32_t& fixup_count, std::optional<uint64_t> form_start = std::nullopt) {
        const auto& u = *l.plugin->unit;
        offset = data.size();
        const uint32_t shift = static_cast<uint32_t>(offset - form_start.value_or(offset));
        auto src = std::span<const std::byte>(u.data).subspan(r.data_offset, r.data_size);
        data.insert(data.end(), src.begin(), src.end());
        first_fixup = static_cast<uint32_t>(fixups_out.size());
        auto by_fixup = [](const FixupCondition& a, const FixupCondition& b) { return a.fixup < b.fixup; };
        auto cond_begin = std::lower_bound(u.fixup_conditions.begin(), u.fixup_conditions.end(),
                                           FixupCondition{r.first_fixup, 0, 0}, by_fixup);
        auto cond_end = std::lower_bound(cond_begin, u.fixup_conditions.end(),
                                         FixupCondition{r.first_fixup + r.fixup_count, 0, 0}, by_fixup);
        auto resolve_at = [&](uint32_t at, uint32_t type) {
            uint32_t local;
            std::memcpy(&local, data.data() + offset + at, 4);
            uint32_t resolved = resolve(l, local);
            std::memcpy(data.data() + offset + at, &resolved, 4);
            fixups_out.push_back(at + shift);
            fixup_types_out.push_back(type);
        };
        for (uint32_t k = 0; k < r.fixup_count; ++k) {
            uint32_t g = r.first_fixup + k;
            bool conditional = std::any_of(cond_begin, cond_end, [&](const auto& c) { return c.fixup == g; });
            if (!conditional) {
                resolve_at(u.fixups[g], u.fixup_types[g]);
            }
        }
        // Conditional ones once the FormIDs they depend on are resolved.
        for (auto c = cond_begin; c != cond_end; ++c) {
            uint32_t owner;
            std::memcpy(&owner, data.data() + offset + c->when_offset, 4);
            if (type_of(owner) == c->when_type) {
                resolve_at(u.fixups[c->fixup], u.fixup_types[c->fixup]);
            }
        }
        fixup_count = static_cast<uint32_t>(fixups_out.size() - first_fixup);
    };

    for (auto& l : loaded) {
        uint32_t ff, fc;
        append_record(l, l.plugin->unit->header, l.entry.header_offset, ff, fc);
        l.entry.header_size = l.plugin->unit->header.data_size;
    }

    for (size_t i = 0; i < refs.size();) {
        size_t j = i;
        while (j < refs.size() && refs[j].form_id == refs[i].form_id) {
            ++j;
        }
        // The last version in load order wins; a plugin listing the same FormID twice keeps its last copy.
        const auto& win = refs[j - 1];
        const auto& l = loaded[win.plugin];
        const auto& r = l.plugin->unit->records[win.record];
        forms::FormEntry e{};
        e.form_id = win.form_id;
        e.type = r.type;
        e.flags = r.flags;
        e.form_version = r.form_version;
        e.plugin = win.plugin;
        e.parent = resolve(l, r.parent);
        e.group = r.group;
        e.first_source = static_cast<uint32_t>(sources.size());
        for (size_t k = i; k < j; ++k) {
            uint8_t group = loaded[refs[k].plugin].plugin->unit->records[refs[k].record].group;
            if (sources.size() == e.first_source || sources.back() != refs[k].plugin) {
                sources.push_back(refs[k].plugin);
                source_groups.push_back(group);
            } else {
                source_groups.back() = group; // the plugin's last copy counts
            }
        }
        e.source_count = static_cast<uint16_t>(sources.size() - e.first_source);
        e.file_order = win.record;
        e.vc_stamp = r.vc_stamp;
        // A deleted record is loaded on top of the form's earlier data (the game skips ClearData and
        // InitializeData for it, "Overrides and deletions"), so the form holds the last
        // non-deleted version followed by every later one. Any other winner replaces the form. A master's
        // temporary cell child is read only when its cell loads, and then from the winning plugin alone, so only
        // versions the game reads at startup are layered under a deleted winner.
        auto record_of = [&](size_t k) -> const auto& {
            return loaded[refs[k].plugin].plugin->unit->records[refs[k].record];
        };
        auto at_startup = [&](size_t k) {
            return record_of(k).group != 9 || !(loaded[refs[k].plugin].entry.flags & forms::plugin_master);
        };
        std::vector<size_t> parts{j - 1};
        if (r.flags & forms::form_deleted) {
            for (size_t k = j - 1; k-- > i;) {
                if (!at_startup(k)) {
                    continue;
                }
                parts.insert(parts.begin(), k);
                if (!(record_of(k).flags & forms::form_deleted)) {
                    break;
                }
            }
        }
        e.data_offset = data.size();
        e.first_fixup = static_cast<uint32_t>(fixups_out.size());
        for (size_t k : parts) {
            uint64_t part_offset;
            uint32_t ff, fc;
            append_record(loaded[refs[k].plugin], record_of(k), part_offset, ff, fc, e.data_offset);
        }
        e.data_size = static_cast<uint32_t>(data.size() - e.data_offset);
        e.fixup_count = static_cast<uint32_t>(fixups_out.size() - e.first_fixup);
        e.layers = static_cast<uint8_t>(std::min<size_t>(parts.size(), 255));
        report.layered += e.layers > 1;
        report.deleted += (e.flags & forms::form_deleted) != 0;
        report.overridden += e.source_count > 1;
        forms_out.push_back(e);
        i = j;
    }
    report.forms = forms_out.size();

    if (validate) {
        // Every FormID must name a form of a type its field allows (0 is "none").
        auto lookup = [&](uint32_t id) -> const forms::FormEntry* {
            auto it = std::lower_bound(forms_out.begin(), forms_out.end(), id,
                                       [](const forms::FormEntry& e, uint32_t v) { return e.form_id < v; });
            return it != forms_out.end() && it->form_id == id ? &*it : nullptr;
        };
        for (const auto& e : forms_out) {
            if (e.fixup_count == 0) {
                continue;
            }
            auto record_data = std::span<const std::byte>(data).subspan(e.data_offset, e.data_size);
            for (uint32_t k = 0; k < e.fixup_count; ++k) {
                auto at = fixups_out[e.first_fixup + k];
                auto type_id = fixup_types_out[e.first_fixup + k];
                uint32_t id;
                std::memcpy(&id, record_data.data() + at, 4);
                ++report.formids_checked;
                // 0x14 is the player reference, which the engine creates at startup (no plugin defines it; the
                // game's form table has it with no source files).
                if (id == 0x14) {
                    continue;
                }
                const auto* target = lookup(id);
                std::string problem;
                if (!target) {
                    problem = "missing";
                } else if (type_id != forms::k_no_type) {
                    const auto& targets = validate->type(type_id).targets;
                    if (!targets.empty() && std::find(targets.begin(), targets.end(), target->type) == targets.end()) {
                        problem = "wrong type " + forms::sig_name(target->type);
                    }
                }
                if (problem.empty()) {
                    continue;
                }
                // The field holding it: walk the field stream to the offset.
                Sig field = 0;
                for (size_t pos = 0; pos + 8 <= record_data.size();) {
                    uint32_t h[2];
                    std::memcpy(h, record_data.data() + pos, 8);
                    if (at >= pos + 8 && at < pos + 8 + h[1]) {
                        field = h[0];
                        break;
                    }
                    pos += 8 + h[1];
                }
                auto& p = report.formid_problems[{e.type, field, problem}];
                ++p.count;
                if (p.examples.size() < 4) {
                    p.examples.push_back(std::format("{:08X} -> {:08X} ({})", e.form_id, id,
                                                     loaded[e.plugin].plugin->unit->filename));
                }
            }
        }
    }

    std::vector<forms::PluginEntry> plugin_entries;
    for (const auto& l : loaded) {
        plugin_entries.push_back(l.entry);
    }
    forms::Header header{};
    header.plugin_count = static_cast<uint32_t>(plugin_entries.size());
    header.form_count = static_cast<uint32_t>(forms_out.size());
    header.data_size = data.size();
    header.language = strings.add(language);

    if (out_path.empty()) {
        return report; // a check: nothing to write
    }
    StreamWriter w(out_path, forms::k_kind, forms::k_version, 11);
    w.text(forms::k_chunk_schema, schema_text);
    w.text(forms::k_chunk_conditions, conditions_text);
    w.chunk(forms::k_chunk_header, std::as_bytes(std::span(&header, 1)));
    w.chunk_of(forms::k_chunk_plugins, plugin_entries);
    w.chunk_of(forms::k_chunk_masters, master_list);
    w.chunk(forms::k_chunk_strings, strings.bytes());
    w.chunk_of(forms::k_chunk_forms, forms_out);
    w.chunk_of(forms::k_chunk_sources, sources);
    w.chunk_of(forms::k_chunk_source_groups, source_groups);
    w.chunk(forms::k_chunk_data, data);
    w.chunk_of(forms::k_chunk_fixups, fixups_out);
    w.finish();
    return report;
}

} // namespace spud::records
