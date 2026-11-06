#include "importer/legacy/esm.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/path.hpp"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <execution>
#include <format>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace spud::legacy {

namespace {

constexpr RecordType k_grup = record_type("GRUP");
constexpr RecordType k_tes4 = record_type("TES4");
constexpr RecordType k_xxxx = record_type("XXXX");
constexpr RecordType k_edid = record_type("EDID");
constexpr RecordType k_hedr = record_type("HEDR");
constexpr RecordType k_mast = record_type("MAST");

constexpr size_t k_header_size = 24;
constexpr uint32_t k_max_full_plugins = 0xFE;
constexpr uint32_t k_max_light_plugins = 0x1000;

struct RawHeader {
    uint32_t type;
    uint32_t size; // records: payload size; groups: total size including this header
    uint32_t flags_or_label;
    uint32_t form_id_or_group_type;
    uint32_t stamp;
    uint32_t version;
};
static_assert(sizeof(RawHeader) == k_header_size);

bool valid_type(RecordType type) {
    for (int i = 0; i < 4; ++i) {
        auto c = static_cast<char>((type >> (8 * i)) & 0xFF);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

void inflate_record(std::span<const std::byte> in, std::span<std::byte> out) {
    uLongf out_size = static_cast<uLongf>(out.size());
    int rc = uncompress(reinterpret_cast<Bytef*>(out.data()), &out_size,
                        reinterpret_cast<const Bytef*>(in.data()), static_cast<uLong>(in.size()));
    if (rc != Z_OK || out_size != out.size()) {
        throw ParseError(std::format("zlib inflate failed ({})", rc));
    }
}

std::vector<Subrecord> split_subrecords(std::span<const std::byte> payload) {
    std::vector<Subrecord> out;
    BinaryReader r(payload);
    uint32_t big_size = 0;
    bool has_big_size = false;
    while (r.remaining() >= 6) {
        auto type = r.read<uint32_t>();
        size_t size = r.read<uint16_t>();
        // XXXX carries the real (32-bit) size of the next subrecord, whose own size field is 0.
        if (type == k_xxxx) {
            big_size = r.read<uint32_t>();
            has_big_size = true;
            continue;
        }
        if (has_big_size) {
            size = big_size;
            has_big_size = false;
        }
        out.push_back({type, r.take(size)});
    }
    return out;
}

// Header of a plugin: the TES4 record.
struct PluginHeader {
    uint32_t flags = 0;
    float version = 0.0f;
    std::vector<std::string> masters;
    size_t end = 0; // file offset just past the TES4 record
};

PluginHeader read_plugin_header(std::span<const std::byte> file) {
    BinaryReader r(file);
    auto h = r.read<RawHeader>();
    if (h.type != k_tes4) {
        throw ParseError("missing TES4 header record");
    }
    PluginHeader out;
    out.flags = h.flags_or_label;
    auto payload = r.take(h.size);
    out.end = r.tell();
    for (const auto& sub : split_subrecords(payload)) {
        if (sub.type == k_hedr) {
            out.version = sub.as<float>();
        } else if (sub.type == k_mast) {
            out.masters.emplace_back(sub.zstring());
        }
    }
    return out;
}

// Asks the OS to read a whole mapping ahead with large I/Os. Walking the group tree touches nearly every
// page, and on a cold cache demand-paging a 250 MB Skyrim.esm one fault at a time is ~10x slower.
void prefetch(std::span<const std::byte> bytes) {
#ifdef _WIN32
    WIN32_MEMORY_RANGE_ENTRY range{const_cast<std::byte*>(bytes.data()), bytes.size()};
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
#else
    (void)bytes;
#endif
}

struct ParsedPlugin {
    std::vector<RecordEntry> entries;
    uint32_t groups = 0;
};

// Walks the group tree of one plugin, recording every record's location and context.
void walk_plugin(const PluginInfo& plugin, uint16_t plugin_index, std::span<const std::byte> file,
                 size_t start, ParsedPlugin& out) {
    struct Frame {
        size_t end;
        GroupType type;
        uint32_t owner;
    };
    std::vector<Frame> stack;
    out.entries.reserve(file.size() / 200);
    size_t pos = start;
    while (pos < file.size()) {
        while (!stack.empty() && pos >= stack.back().end) {
            stack.pop_back();
        }
        size_t limit = stack.empty() ? file.size() : stack.back().end;
        if (limit - pos < k_header_size) {
            throw ParseError(std::format("truncated header at offset {:#x}", pos));
        }
        RawHeader h;
        std::memcpy(&h, file.data() + pos, sizeof h);
        if (h.type == k_grup) {
            if (h.size < k_header_size || h.size > limit - pos) {
                throw ParseError(std::format("group at {:#x} has bad size {}", pos, h.size));
            }
            auto group_type = static_cast<GroupType>(h.form_id_or_group_type);
            uint32_t owner = stack.empty() ? 0 : stack.back().owner;
            switch (group_type) {
            case group_world_children:
            case group_cell_children:
            case group_topic_children:
            case group_cell_persistent:
            case group_cell_temporary:
                owner = plugin.resolve(h.flags_or_label);
                break;
            default:
                break;
            }
            stack.push_back({pos + h.size, group_type, owner});
            pos += k_header_size;
            ++out.groups;
            continue;
        }
        if (!valid_type(h.type)) {
            throw ParseError(std::format("bad record type {:#010x} at offset {:#x}", h.type, pos));
        }
        if (h.size > limit - pos - k_header_size) {
            throw ParseError(std::format("{} record at {:#x} overruns its group", type_name(h.type), pos));
        }
        RecordEntry e{};
        e.offset = pos;
        e.form_id = plugin.resolve(h.form_id_or_group_type);
        e.type = h.type;
        e.flags = h.flags_or_label;
        e.data_size = h.size;
        e.parent = stack.empty() ? 0 : stack.back().owner;
        e.prev = k_no_entry;
        e.plugin = plugin_index;
        e.group = stack.empty() ? group_none : stack.back().type;
        out.entries.push_back(e);
        pos += k_header_size + h.size;
    }
}

} // namespace

std::string type_name(RecordType type) {
    std::string s(4, '?');
    for (int i = 0; i < 4; ++i) {
        auto c = static_cast<char>((type >> (8 * i)) & 0xFF);
        if (c >= 0x20 && c < 0x7F) {
            s[i] = c;
        }
    }
    return s;
}

RecordType parse_type(std::string_view s) {
    if (s.size() != 4) {
        throw Error(std::format("record type must be four characters: '{}'", s));
    }
    char buf[5] = {s[0], s[1], s[2], s[3], 0};
    return record_type(buf);
}

std::string_view Subrecord::zstring() const {
    std::string_view s(reinterpret_cast<const char*>(data.data()), data.size());
    if (auto nul = s.find('\0'); nul != std::string_view::npos) {
        s = s.substr(0, nul);
    }
    return s;
}

std::string PluginInfo::index_string() const {
    return light ? std::format("FE:{:03X}", light_index) : std::format("{:02X}", mod_index);
}

uint32_t PluginInfo::resolve(uint32_t local_form_id) const {
    if (local_form_id == 0) {
        return 0;
    }
    // Indices past the master list mean "this plugin", like the game does.
    size_t slot = std::min<size_t>(local_form_id >> 24, m_bases.size() - 1);
    return m_bases[slot] | (local_form_id & m_masks[slot]);
}

Record::Record(const RecordEntry& entry, const PluginInfo& plugin, std::span<const std::byte> payload)
    : m_entry(&entry), m_plugin(&plugin) {
    if (entry.flags & record_compressed) {
        BinaryReader r(payload);
        auto size = r.read<uint32_t>();
        m_storage.resize(size);
        inflate_record(r.rest(), m_storage);
        payload = m_storage;
    }
    m_subrecords = split_subrecords(payload);
}

const Subrecord* Record::find(RecordType type) const {
    for (const auto& s : m_subrecords) {
        if (s.type == type) {
            return &s;
        }
    }
    return nullptr;
}

std::string_view Record::string(RecordType type) const {
    auto s = find(type);
    return s ? s->zstring() : std::string_view();
}

std::string_view Record::edid() const {
    return string(k_edid);
}

uint32_t Record::form(RecordType type, size_t offset) const {
    auto s = find(type);
    if (!s || s->data.size() < offset + 4) {
        return 0;
    }
    uint32_t local;
    std::memcpy(&local, s->data.data() + offset, 4);
    return resolve(local);
}

EsmDatabase::EsmDatabase(const Install& install) {
    auto t0 = std::chrono::steady_clock::now();

    // Map every plugin and read its header first: masters decide which plugins can load at all.
    struct Candidate {
        const Plugin* plugin;
        MappedFile file;
        PluginHeader header;
    };
    std::vector<Candidate> candidates;
    for (const auto& p : install.load_order) {
        try {
            MappedFile file(install.data_file(p.filename));
            auto header = read_plugin_header(file.bytes());
            candidates.push_back({&p, std::move(file), std::move(header)});
        } catch (const std::exception& e) {
            log::error("skipping plugin {}: {}", p.filename, e.what());
        }
    }

    std::unordered_map<std::string, size_t> by_name; // lowercase filename -> m_plugins index
    uint32_t full_count = 0;
    uint32_t light_count = 0;
    for (auto& c : candidates) {
        bool missing = false;
        for (const auto& m : c.header.masters) {
            if (!by_name.contains(path::to_lower(m))) {
                log::error("skipping plugin {}: master {} is not loaded before it", c.plugin->filename, m);
                missing = true;
            }
        }
        if (missing) {
            continue;
        }
        PluginInfo info;
        info.filename = c.plugin->filename;
        info.master = c.plugin->master;
        info.light = c.plugin->light;
        info.localized = (c.header.flags & record_localized) != 0;
        info.version = c.header.version;
        info.masters = c.header.masters;
        if (info.light) {
            if (light_count >= k_max_light_plugins) {
                log::error("skipping plugin {}: more than {} light plugins", info.filename,
                           k_max_light_plugins);
                continue;
            }
            info.mod_index = 0xFE;
            info.light_index = static_cast<uint16_t>(light_count++);
        } else {
            if (full_count >= k_max_full_plugins) {
                log::error("skipping plugin {}: more than {} full plugins", info.filename,
                           k_max_full_plugins);
                continue;
            }
            info.mod_index = static_cast<uint8_t>(full_count++);
        }
        auto add_slot = [&info](const PluginInfo& target) {
            if (target.light) {
                // Light plugins only own a 12-bit object ID range.
                info.m_bases.push_back(0xFE000000u | (uint32_t(target.light_index) << 12));
                info.m_masks.push_back(0xFFFu);
            } else {
                info.m_bases.push_back(uint32_t(target.mod_index) << 24);
                info.m_masks.push_back(0xFFFFFFu);
            }
        };
        for (const auto& m : info.masters) {
            add_slot(m_plugins[by_name.at(path::to_lower(m))]);
        }
        add_slot(info);
        by_name[path::to_lower(info.filename)] = m_plugins.size();
        m_plugins.push_back(std::move(info));
        m_files.push_back(std::move(c.file));
        m_header_ends.push_back(c.header.end);
    }

    // Walk the plugins in parallel, biggest first so Skyrim.esm doesn't start last.
    std::vector<ParsedPlugin> parsed(m_plugins.size());
    std::vector<size_t> order(m_plugins.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return m_files[a].bytes().size() > m_files[b].bytes().size(); });
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t i; (i = next++) < order.size();) {
            auto p = order[i];
            try {
                prefetch(m_files[p].bytes());
                walk_plugin(m_plugins[p], static_cast<uint16_t>(p), m_files[p].bytes(), m_header_ends[p],
                            parsed[p]);
            } catch (const std::exception& e) {
                // Keep what was indexed before the damage; the rest of the plugin is lost.
                log::error("{}: {} ({} records indexed)", m_plugins[p].filename, e.what(),
                           parsed[p].entries.size());
            }
        }
    };
    {
        auto threads = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 16);
        std::vector<std::jthread> pool;
        for (size_t t = 0; t < std::min(threads, order.size()); ++t) {
            pool.emplace_back(worker);
        }
    }

    size_t total = 0;
    for (const auto& p : parsed) {
        total += p.entries.size();
    }
    m_entries.reserve(total);
    for (size_t i = 0; i < parsed.size(); ++i) {
        m_plugins[i].first_entry = static_cast<uint32_t>(m_entries.size());
        m_plugins[i].entry_count = static_cast<uint32_t>(parsed[i].entries.size());
        m_plugins[i].group_count = parsed[i].groups;
        m_entries.insert(m_entries.end(), parsed[i].entries.begin(), parsed[i].entries.end());
        parsed[i] = {};
    }
    auto t1 = std::chrono::steady_clock::now();

    build_indices();
    auto t2 = std::chrono::steady_clock::now();
    log::info("esm: {} plugins, {} records ({} unique forms) indexed in {} ms (+{} ms indices)",
              m_plugins.size(), m_entries.size(), m_winners.size(),
              std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
              std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count());
}

EsmDatabase::~EsmDatabase() = default;

void EsmDatabase::build_indices() {
    // Sort (form id, entry index): each FormID's versions end up together in load order.
    std::vector<uint64_t> keys(m_entries.size());
    for (size_t i = 0; i < m_entries.size(); ++i) {
        keys[i] = (uint64_t(m_entries[i].form_id) << 32) | i;
    }
    std::sort(std::execution::par_unseq, keys.begin(), keys.end());

    m_winners.clear();
    std::vector<std::pair<uint32_t, const RecordEntry*>> children;
    for (size_t i = 0; i < keys.size();) {
        auto form_id = static_cast<uint32_t>(keys[i] >> 32);
        auto prev = k_no_entry;
        size_t j = i;
        for (; j < keys.size() && static_cast<uint32_t>(keys[j] >> 32) == form_id; ++j) {
            auto index = static_cast<uint32_t>(keys[j]);
            m_entries[index].prev = prev;
            prev = index;
        }
        i = j;
        m_winners.emplace_back(form_id, prev);
        const auto& winner = m_entries[prev];
        if (winner.deleted()) {
            continue;
        }
        m_by_type[winner.type].push_back(&winner);
        if (winner.parent != 0) {
            children.emplace_back(winner.parent, &winner);
        }
    }
    // Stable: children of one parent stay in FormID order.
    std::stable_sort(children.begin(), children.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    m_child_parents.resize(children.size());
    m_children.resize(children.size());
    for (size_t i = 0; i < children.size(); ++i) {
        m_child_parents[i] = children[i].first;
        m_children[i] = children[i].second;
    }
}

std::optional<size_t> EsmDatabase::find_plugin(std::string_view filename) const {
    auto lower = path::to_lower(filename);
    for (size_t i = 0; i < m_plugins.size(); ++i) {
        if (path::to_lower(m_plugins[i].filename) == lower) {
            return i;
        }
    }
    return std::nullopt;
}

std::span<const RecordEntry> EsmDatabase::plugin_entries(size_t plugin) const {
    const auto& p = m_plugins.at(plugin);
    return std::span<const RecordEntry>(m_entries).subspan(p.first_entry, p.entry_count);
}

const RecordEntry* EsmDatabase::find(uint32_t form_id) const {
    auto it = std::lower_bound(m_winners.begin(), m_winners.end(), form_id,
                               [](const auto& w, uint32_t id) { return w.first < id; });
    if (it == m_winners.end() || it->first != form_id) {
        return nullptr;
    }
    return &m_entries[it->second];
}

std::optional<Record> EsmDatabase::get(uint32_t form_id) const {
    auto e = find(form_id);
    if (!e || e->deleted()) {
        return std::nullopt;
    }
    return read(*e);
}

Record EsmDatabase::read(const RecordEntry& entry) const {
    auto payload = m_files[entry.plugin].bytes().subspan(entry.offset + k_header_size, entry.data_size);
    try {
        return Record(entry, m_plugins[entry.plugin], payload);
    } catch (const ParseError& e) {
        throw ParseError(std::format("{}: {} {:08X}: {}", m_plugins[entry.plugin].filename,
                                     type_name(entry.type), entry.form_id, e.what()));
    }
}

uint16_t EsmDatabase::form_version(const RecordEntry& entry) const {
    RawHeader h;
    std::memcpy(&h, m_files[entry.plugin].bytes().data() + entry.offset, sizeof h);
    return static_cast<uint16_t>(h.version & 0xFFFF);
}

std::vector<const RecordEntry*> EsmDatabase::versions(uint32_t form_id) const {
    std::vector<const RecordEntry*> out;
    for (auto e = find(form_id); e; e = e->prev == k_no_entry ? nullptr : &m_entries[e->prev]) {
        out.push_back(e);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::span<const RecordEntry* const> EsmDatabase::of_type(RecordType type) const {
    auto it = m_by_type.find(type);
    if (it == m_by_type.end()) {
        return {};
    }
    return it->second;
}

std::span<const RecordEntry* const> EsmDatabase::children(uint32_t parent) const {
    auto [lo, hi] = std::equal_range(m_child_parents.begin(), m_child_parents.end(), parent);
    auto first = static_cast<size_t>(lo - m_child_parents.begin());
    return std::span<const RecordEntry* const>(m_children).subspan(first, static_cast<size_t>(hi - lo));
}

} // namespace spud::legacy
