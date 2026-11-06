#pragma once

// Converts a plugin record's subrecords into the forms database's canonical field stream, driven by the record
// schema (src/importer/schema/*.txt, docs/formats/forms.md):
//   - localized strings become their text, every string becomes NUL-terminated UTF-8;
//   - FormIDs stay as written (master-relative) and are listed as fixups, resolved when units are linked;
//   - everything else is kept byte for byte.
// A field the schema doesn't describe, or whose bytes don't fit its definition, is kept raw and reported.

#include "formats/forms_schema.hpp"
#include "importer/legacy/strings.hpp"
#include "importer/records/plugin_file.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::records {

// A FormID inside a converted record's data: `offset` is relative to the record's data start.
struct Fixup {
    uint32_t offset;
    forms::TypeId type; // the schema type that holds it (for target checks); k_no_type for codec-internal ids
    // Conditional FormIDs: resolved only if the FormID at `when_offset` (itself a fixup) names a `when_type`
    // form, which only the load order can tell (see the coed codec).
    uint32_t when_offset = 0xFFFFFFFFu;
    forms::Sig when_type = 0;
};

// The condition function table (src/importer/schema/conditions.txt).
struct ConditionFunctions {
    enum class Param : uint8_t { none, integer, real, string, form };
    struct Function {
        std::string name;
        std::vector<uint32_t> param_types;
    };
    std::unordered_map<uint32_t, Function> functions;
    std::unordered_map<uint32_t, Param> param_kinds; // by exe parameter type id

    void parse(std::string_view text);
    // How CTDA parameter `slot` (0 or 1) of `function` is stored, given the CTDA flags byte.
    Param param(uint32_t function, int slot, uint8_t flags) const;
};

// Issues found while converting, aggregated per (record type, field) for `skytool records check`.
class Diagnostics {
public:
    enum class Issue : uint8_t { unknown_record, unknown_field, decode_error, leftover, truncated, missing_string };
    void add(forms::Sig record, forms::Sig field, Issue issue, std::string_view example);
    void field_seen(forms::Sig record, forms::Sig field, size_t size);

    struct Entry {
        uint64_t count = 0;
        std::vector<std::string> examples;
    };
    struct FieldStats {
        uint64_t count = 0;
        size_t min_size = SIZE_MAX;
        size_t max_size = 0;
    };
    // (record, field, issue) -> entry
    std::map<std::tuple<forms::Sig, forms::Sig, Issue>, Entry> issues() const;
    std::map<std::pair<forms::Sig, forms::Sig>, FieldStats> fields() const;

private:
    mutable std::mutex m_mutex;
    std::map<std::tuple<forms::Sig, forms::Sig, Issue>, Entry> m_issues;
    std::map<std::pair<forms::Sig, forms::Sig>, FieldStats> m_fields;
};

std::string_view issue_name(Diagnostics::Issue issue);

struct ConvertContext {
    const forms::Schema& schema;
    const ConditionFunctions& conditions;
    const legacy::Localization* strings = nullptr; // null: localized fields convert to ""
    std::string plugin_filename;
    std::string language; // for text encoding
    bool localized = false;
    Diagnostics* diagnostics = nullptr;
};

struct ConvertStats {
    uint64_t fields = 0;
    uint64_t raw_fields = 0;      // kept raw: unknown or undecodable
    uint64_t missing_strings = 0; // localized IDs not found
};

// Appends the record's canonical field stream (u32 sig, u32 size, bytes; per field) to `out`, and its
// FormID fixups (offsets relative to out.size() at entry).
void convert_record(const ConvertContext& ctx, const RawRecord& record, std::span<const RawSubrecord> fields,
                    std::vector<std::byte>& out, std::vector<Fixup>& fixups, ConvertStats& stats);

} // namespace spud::records
