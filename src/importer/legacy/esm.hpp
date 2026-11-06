#pragma once

// ESM/ESP/ESL plugins: the TES4 record format as used by Skyrim SE/AE (form version 44).
// Reference: UESP "Skyrim Mod:Mod File Format", xEdit record definitions.
//
// EsmDatabase indexes every record of the active load order without decoding it: each plugin is
// memory-mapped and its group tree walked once (plugins in parallel), FormIDs are resolved to
// load-order IDs and later plugins override earlier ones. Payloads are decompressed and split into
// subrecords only when a record is read, so the index stays small and every lookup is thread-safe.

#include "core/binary.hpp"
#include "core/file.hpp"
#include "importer/legacy/install.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::legacy {

using RecordType = uint32_t;

constexpr RecordType record_type(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}

std::string type_name(RecordType type);
// "STAT" -> record_type("STAT"); throws Error unless `s` is exactly four characters.
RecordType parse_type(std::string_view s);

enum RecordFlag : uint32_t {
    record_master = 0x00000001, // TES4 only
    record_deleted = 0x00000020,
    record_localized = 0x00000080,          // TES4 only: FULL/DESC are string-table IDs
    record_light = 0x00000200,              // TES4 only
    record_persistent = 0x00000400,         // REFR/ACHR/CELL
    record_initially_disabled = 0x00000800, // REFR/ACHR
    record_compressed = 0x00040000,
    record_is_marker = 0x00800000, // base objects (STAT, FURN, ACTI, DOOR): the CK "Is Marker" flag
};

enum GroupType : uint8_t {
    group_top = 0,
    group_world_children = 1,
    group_interior_block = 2,
    group_interior_sub_block = 3,
    group_exterior_block = 4,
    group_exterior_sub_block = 5,
    group_cell_children = 6,
    group_topic_children = 7,
    group_cell_persistent = 8,
    group_cell_temporary = 9,
    group_none = 0xFF, // TES4 header record
};

struct Subrecord {
    RecordType type;
    std::span<const std::byte> data;

    BinaryReader reader() const { return BinaryReader(data); }
    // First sizeof(T) bytes; throws ParseError if the subrecord is shorter.
    template <Pod T>
    T as() const {
        return BinaryReader(data).read<T>();
    }
    // NUL-terminated string (without the terminator).
    std::string_view zstring() const;
};

struct PluginInfo {
    std::string filename;
    bool master = false;
    bool light = false;
    bool localized = false;
    uint8_t mod_index = 0;    // 0x00..0xFD for full plugins, 0xFE for light ones
    uint16_t light_index = 0; // light plugins only
    float version = 0.0f;     // HEDR
    std::vector<std::string> masters;
    uint32_t first_entry = 0; // this plugin's records are EsmDatabase::entries()[first_entry, + entry_count)
    uint32_t entry_count = 0;
    uint32_t group_count = 0;

    // "00", "1A" or "FE:003" — the prefix the in-game console shows.
    std::string index_string() const;
    // Maps a FormID as written in this plugin's data to its load-order ID.
    uint32_t resolve(uint32_t local_form_id) const;

private:
    friend class EsmDatabase;
    // resolve() tables: one slot per master, then the plugin itself.
    std::vector<uint32_t> m_bases;
    std::vector<uint32_t> m_masks;
};

// One record of one plugin, as found in the group tree. Cheap: no payload is decoded.
struct RecordEntry {
    uint64_t offset;  // of the 24-byte record header within the plugin file
    uint32_t form_id; // load-order resolved
    RecordType type;
    uint32_t flags;     // RecordFlag
    uint32_t data_size; // payload bytes as stored (compressed size for compressed records)
    uint32_t parent;    // resolved FormID owning the enclosing group: WRLD for CELLs, CELL for refs/LAND,
                        // DIAL for INFOs; 0 for top-level records and interior cells
    uint32_t prev;      // index of the entry this one overrides (earlier plugin), or k_no_entry
    uint16_t plugin;    // index into EsmDatabase::plugins()
    GroupType group;    // type of the innermost enclosing group
    uint8_t reserved = 0;

    bool deleted() const { return (flags & record_deleted) != 0; }
};
static_assert(sizeof(RecordEntry) == 40);

inline constexpr uint32_t k_no_entry = 0xFFFFFFFFu;

// A decoded record: its payload (decompressed if needed) split into subrecords. Move-only,
// because subrecord spans may point into the owned decompression buffer.
class Record {
public:
    Record(const RecordEntry& entry, const PluginInfo& plugin, std::span<const std::byte> payload);
    Record(Record&&) noexcept = default;
    Record& operator=(Record&&) noexcept = default;
    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;

    const RecordEntry& entry() const { return *m_entry; }
    const PluginInfo& plugin() const { return *m_plugin; }
    RecordType type() const { return m_entry->type; }
    uint32_t form_id() const { return m_entry->form_id; }
    uint32_t flags() const { return m_entry->flags; }

    std::span<const Subrecord> subrecords() const { return m_subrecords; }
    const Subrecord* find(RecordType type) const;
    bool has(RecordType type) const { return find(type) != nullptr; }
    // The subrecord's NUL-terminated string, or "" if absent.
    std::string_view string(RecordType type) const;
    std::string_view edid() const;
    // A FormID stored at `offset` in the subrecord, resolved through this record's plugin; 0 if absent.
    uint32_t form(RecordType type, size_t offset = 0) const;
    uint32_t resolve(uint32_t local_form_id) const { return m_plugin->resolve(local_form_id); }

private:
    const RecordEntry* m_entry;
    const PluginInfo* m_plugin;
    std::vector<std::byte> m_storage; // decompressed payload
    std::vector<Subrecord> m_subrecords;
};

class EsmDatabase {
public:
    // Loads the whole load order. Plugins with a missing master are skipped with an error, as the
    // game refuses to start with them.
    explicit EsmDatabase(const Install& install);
    ~EsmDatabase();
    EsmDatabase(const EsmDatabase&) = delete;
    EsmDatabase& operator=(const EsmDatabase&) = delete;

    std::span<const PluginInfo> plugins() const { return m_plugins; }
    std::optional<size_t> find_plugin(std::string_view filename) const; // case-insensitive

    // Every record of every plugin: plugins in load order, each in file order.
    std::span<const RecordEntry> entries() const { return m_entries; }
    std::span<const RecordEntry> plugin_entries(size_t plugin) const;

    // The winning (last-loaded) entry for a FormID, deletions included; nullptr if no plugin has it.
    const RecordEntry* find(uint32_t form_id) const;
    // The winning record, or nullopt if absent or deleted.
    std::optional<Record> get(uint32_t form_id) const;
    // Decodes any entry of entries() (not necessarily a winner; the Record keeps a pointer to it).
    // Throws ParseError on corrupt data.
    Record read(const RecordEntry& entry) const;
    // The record header's form version (the low half of its last field).
    uint16_t form_version(const RecordEntry& entry) const;
    // Every plugin's version of a FormID, earliest first (the last one wins).
    std::vector<const RecordEntry*> versions(uint32_t form_id) const;

    // Winning, non-deleted records of one type, by FormID.
    std::span<const RecordEntry* const> of_type(RecordType type) const;
    // Winning, non-deleted records whose winning version sits in a group owned by `parent` (see
    // RecordEntry::parent): a WRLD's CELLs, a CELL's references and LAND. Sorted by FormID.
    std::span<const RecordEntry* const> children(uint32_t parent) const;

    size_t winner_count() const { return m_winners.size(); }

private:
    void build_indices();

    std::vector<PluginInfo> m_plugins;
    std::vector<MappedFile> m_files;   // parallel to m_plugins
    std::vector<size_t> m_header_ends; // file offset past each plugin's TES4 record
    std::vector<RecordEntry> m_entries;
    std::vector<std::pair<uint32_t, uint32_t>> m_winners; // (form id, entry index), sorted
    std::unordered_map<RecordType, std::vector<const RecordEntry*>> m_by_type;
    std::vector<uint32_t> m_child_parents; // sorted, parallel to m_children
    std::vector<const RecordEntry*> m_children;
};

} // namespace spud::legacy
