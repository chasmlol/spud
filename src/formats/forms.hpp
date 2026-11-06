#pragma once

// .osf — spud forms database. Spec: docs/formats/forms.md
//
// Every form of a load order, after overrides: one entry per FormID (the winning version, deleted ones
// included and flagged), its fields in canonical form (FormIDs load-order resolved, text as UTF-8), the list
// of plugins that define it, and the record schema that describes every field's layout.

#include "formats/container.hpp"
#include "formats/forms_schema.hpp"

#include <unordered_map>
#include <memory>
#include <mutex>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spud::forms {

inline constexpr container::FourCC k_kind = container::fourcc("FORM");
inline constexpr uint32_t k_version = 3;

// Chunks
inline constexpr container::FourCC k_chunk_schema = container::fourcc("SCHM");     // schema text (UTF-8)
inline constexpr container::FourCC k_chunk_conditions = container::fourcc("COND"); // condition function table
inline constexpr container::FourCC k_chunk_header = container::fourcc("HEAD");     // Header
inline constexpr container::FourCC k_chunk_plugins = container::fourcc("PLUG");    // PluginEntry[]
inline constexpr container::FourCC k_chunk_masters = container::fourcc("MAST");    // u16[] plugin indices
inline constexpr container::FourCC k_chunk_strings = container::fourcc("STRS");    // container string table
inline constexpr container::FourCC k_chunk_forms = container::fourcc("FRMS");      // FormEntry[], by form_id
inline constexpr container::FourCC k_chunk_sources = container::fourcc("SRCS");    // u16[] plugin indices
inline constexpr container::FourCC k_chunk_source_groups = container::fourcc("SGRP"); // u8[]: group of each SRCS record
inline constexpr container::FourCC k_chunk_data = container::fourcc("DATA");       // field streams
inline constexpr container::FourCC k_chunk_fixups = container::fourcc("FIXS");     // u32[] FormID offsets in DATA

struct Header {
    uint32_t plugin_count;
    uint32_t form_count;
    uint64_t data_size;
    uint32_t language; // string offset: sLanguage the text was resolved with
    uint32_t reserved[3];
};
static_assert(sizeof(Header) == 32);

enum PluginFlag : uint32_t {
    plugin_master = 1u << 0,    // ESM flag or .esm/.esl extension
    plugin_light = 1u << 1,     // ESL flag or .esl extension
    plugin_localized = 1u << 2, // TES4 0x80: its text came from string tables
};

struct PluginEntry {
    uint32_t name;          // string offset: file name as on disk
    uint32_t flags;         // PluginFlag
    uint32_t header_flags;  // TES4 record flags as stored
    float version;          // HEDR
    uint8_t mod_index;      // 0x00..0xFD, 0xFE for light plugins
    uint8_t reserved0;
    uint16_t light_index;   // light plugins only
    uint32_t first_master;  // masters are MAST[first_master, + master_count) (plugin indices)
    uint32_t master_count;
    uint32_t record_count;  // records in the file (TES4 excluded)
    uint32_t author;        // string offset (CNAM)
    uint32_t description;   // string offset (SNAM)
    uint64_t content_hash;  // XXH64 of the plugin file
    uint32_t form_version;  // TES4 header form version
    uint32_t header_size;   // the TES4 record's canonical field stream: DATA[header_offset, + header_size)
    uint64_t header_offset;
};
static_assert(sizeof(PluginEntry) == 64);

enum FormFlag : uint32_t {
    // Record header flags are stored as they are (Skyrim's meanings, docs/formats/forms.md); this one is ours.
    form_deleted = 0x00000020,
};

struct FormEntry {
    uint32_t form_id;      // load-order resolved
    Sig type;              // record signature
    uint32_t flags;        // record header flags of the winning version
    uint16_t form_version; // record header
    uint16_t plugin;       // winning plugin
    uint32_t parent;       // resolved owner of the record's group: WRLD of a CELL, CELL of a ref/LAND/NAVM, DIAL of an INFO
    uint8_t group;         // innermost group type in the winning plugin (8 persistent / 9 temporary cell children, ...)
    uint8_t layers;        // records whose fields DATA holds, in load order: 1, or more when the winner is deleted
                           // (the game loads a deleted record on top of the earlier version: see docs)
    uint16_t source_count; // plugins defining this form, load order: SRCS[first_source, + source_count)
    uint32_t first_source;
    uint32_t file_order;   // index of the winning record among its plugin's records (file order)
    uint64_t data_offset;  // field stream in DATA
    uint32_t data_size;
    uint32_t vc_stamp;     // record header version-control stamp
    uint32_t first_fixup;  // FIXS[first_fixup, + fixup_count): FormID positions (relative to data_offset)
    uint32_t fixup_count;
};
static_assert(sizeof(FormEntry) == 56);

// A field of a form: its signature and canonical bytes (layout: the schema's definition for it).
struct Field {
    Sig sig;
    std::span<const std::byte> data;

    // Text of a string field (without the terminator).
    std::string_view text() const;
    template <Pod T>
    T as() const {
        return BinaryReader(data).read<T>();
    }
};

class FieldIterator {
public:
    FieldIterator() = default;
    explicit FieldIterator(std::span<const std::byte> rest) : m_rest(rest) { load(); }
    const Field& operator*() const { return m_field; }
    const Field* operator->() const { return &m_field; }
    FieldIterator& operator++() {
        load();
        return *this;
    }
    bool operator==(const FieldIterator& o) const { return m_done == o.m_done && (m_done || m_rest.data() == o.m_rest.data()); }

private:
    void load();
    std::span<const std::byte> m_rest;
    Field m_field{};
    bool m_done = true;
};

struct FieldRange {
    std::span<const std::byte> data;
    FieldIterator begin() const { return FieldIterator(data); }
    FieldIterator end() const { return FieldIterator(); }
};

class Database {
public:
    // Memory-maps a database. Throws ParseError on a malformed file.
    explicit Database(const std::filesystem::path& path);
    // A database in memory (tests, the importer's own checks).
    explicit Database(std::vector<std::byte> bytes);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    const Schema& schema() const { return m_schema; }
    std::string_view schema_text() const { return m_schema_text; }
    std::string_view language() const;
    std::span<const PluginEntry> plugins() const { return m_plugins; }
    std::string_view plugin_name(size_t plugin) const;
    std::span<const uint16_t> masters(const PluginEntry& p) const;
    FieldRange header_fields(const PluginEntry& p) const;
    std::span<const FormEntry> forms() const { return m_forms; }
    std::span<const uint16_t> sources(const FormEntry& f) const;
    // The group type each of sources(f)'s records sat in (8 persistent, 9 temporary cell children, ...).
    std::span<const uint8_t> source_groups(const FormEntry& f) const;
    std::span<const uint32_t> fixups(const FormEntry& f) const;
    std::string_view string(uint32_t offset) const { return m_strings.get(offset); }

    const FormEntry* find(uint32_t form_id) const;
    FieldRange fields(const FormEntry& f) const;
    std::optional<Field> field(const FormEntry& f, Sig sig) const;
    std::string_view editor_id(const FormEntry& f) const;
    // Every form of one record type, deleted ones excluded, by FormID.
    const std::vector<const FormEntry*>& of_type(Sig type) const;
    // A place for forms_typed.cpp to keep decoded records: the database never changes, so a record is decoded once.
    template <class T>
    T& cache() const {
        std::call_once(m_cache_once, [this] { m_cache = std::make_shared<T>(); });
        return *static_cast<T*>(m_cache.get());
    }

private:
    void open(std::span<const std::byte> bytes);
    mutable std::once_flag m_types_once;
    mutable std::unordered_map<uint32_t, std::vector<const FormEntry*>> m_types; // by record type, built at first use
    mutable std::once_flag m_cache_once;
    mutable std::shared_ptr<void> m_cache;

    struct Mapping;
    std::unique_ptr<Mapping> m_mapping;
    std::vector<std::byte> m_owned;
    Schema m_schema;
    std::string_view m_schema_text;
    Header m_header{};
    std::span<const PluginEntry> m_plugins;
    std::span<const uint16_t> m_masters;
    container::StringTable m_strings;
    std::span<const FormEntry> m_forms;
    void build_index();
    std::vector<uint64_t> m_index; // see find()
    int m_index_shift = 54;
    std::span<const uint16_t> m_sources;
    std::span<const uint8_t> m_source_groups;
    std::span<const std::byte> m_data;
    std::span<const uint32_t> m_fixups;
};

} // namespace spud::forms
