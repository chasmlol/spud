#pragma once

// Forms import in two steps, so that an incremental import only redoes what changed:
//
//   compile  one plugin -> a PluginUnit: every record converted to canonical fields (text resolved from the
//            plugin's string tables), FormIDs left as the plugin wrote them and listed as fixups. A unit
//            depends only on the plugin, its string tables, the language and the schema: never on the load
//            order, so it is cached by those inputs' hashes.
//   link     the load order's units -> the forms database (.osf): FormIDs resolved through each plugin's
//            masters, overrides decided (the last plugin wins), parents and source lists filled in.

#include "formats/forms.hpp"
#include "importer/records/convert.hpp"
#include "importer/records/plugin_file.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <tuple>
#include <span>
#include <string>
#include <vector>

namespace spud::records {

struct UnitRecord {
    uint32_t form_id; // as written in the plugin
    Sig type;
    uint32_t flags;
    uint32_t vc_stamp;
    uint16_t form_version;
    uint16_t vc_unknown;
    uint32_t parent; // as written
    uint8_t group;
    uint8_t reserved[3];
    uint32_t first_fixup;
    uint32_t fixup_count;
    uint32_t data_size;
    uint64_t data_offset;
};
static_assert(sizeof(UnitRecord) == 48);

// A fixup resolved only if another FormID of the record names a form of `when_type` (Fixup::when_offset).
struct FixupCondition {
    uint32_t fixup; // index into PluginUnit::fixups
    uint32_t when_offset;
    Sig when_type;
};

struct PluginUnit {
    static constexpr uint32_t k_version = 1;

    std::string filename;
    uint64_t key = 0;          // hash of every input; a cached unit is valid while it matches
    uint64_t content_hash = 0; // XXH64 of the plugin file
    uint32_t header_flags = 0;
    uint32_t header_form_version = 0;
    float version = 0.0f;
    std::string author;
    std::string description;
    std::vector<std::string> masters;
    std::vector<uint32_t> overrides; // ONAM, as written
    UnitRecord header{};             // the TES4 record (form_id 0), fields converted like any record
    std::vector<UnitRecord> records; // file order
    std::vector<std::byte> data;
    std::vector<uint32_t> fixups;      // offsets relative to their record's data start
    std::vector<uint32_t> fixup_types; // parallel: schema TypeId holding the FormID (k_no_type in codecs)
    std::vector<FixupCondition> fixup_conditions; // by fixup index
    ConvertStats stats;

    void write(const std::filesystem::path& path) const;
    // nullopt if missing, unreadable, or of another version.
    static std::optional<PluginUnit> read(const std::filesystem::path& path);
};

struct CompileInputs {
    const forms::Schema& schema;
    const ConditionFunctions& conditions;
    const legacy::Localization* strings = nullptr;
    std::string language;
    Diagnostics* diagnostics = nullptr;
    unsigned threads = 1;
};

PluginUnit compile_plugin(const PluginFile& file, const std::string& filename, const CompileInputs& in);

struct LinkPlugin {
    const PluginUnit* unit;
    bool master = false; // load-order rules: ESM flag or .esm/.esl extension
    bool light = false;  // ESL flag or .esl extension
};

struct LinkReport {
    struct Problem {
        uint64_t count = 0;
        std::vector<std::string> examples;
    };
    // FormID checks (link_units with a schema): (record, field, "missing"/"wrong type <SIG>") -> problem
    std::map<std::tuple<Sig, Sig, std::string>, Problem> formid_problems;
    uint64_t formids_checked = 0;
    size_t plugins = 0;
    size_t skipped_plugins = 0;
    size_t records = 0;
    size_t forms = 0;
    size_t deleted = 0;
    size_t overridden = 0; // forms defined by more than one plugin
    size_t layered = 0;    // deleted winners loaded on top of an earlier version (FormEntry::layers > 1)
    std::vector<std::string> messages;
};

// Writes the forms database for a load order (masters already sorted first, as the game loads them); with an
// empty `out_path` only links and checks.
// Plugins whose masters are not loaded before them are skipped, as the game refuses to start with them.
LinkReport link_units(std::span<const LinkPlugin> load_order, std::string_view schema_text,
                      std::string_view conditions_text, std::string_view language,
                      const std::filesystem::path& out_path, const forms::Schema* validate = nullptr);

} // namespace spud::records
