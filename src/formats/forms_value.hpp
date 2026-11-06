#pragma once

// Generic access to a forms database's fields: decodes canonical field bytes into a value tree using the
// database's own schema, so any field of any record type can be read (or printed) by name.

#include "formats/forms.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spud::forms {

struct Value {
    enum class Kind : uint8_t { none, integer, real, form, text, bytes, object, list };
    Kind kind = Kind::none;
    int64_t integer = 0;
    double real = 0.0;
    uint32_t form = 0;
    std::string text; // text, or hex for bytes
    std::vector<std::pair<std::string, Value>> members;
    std::vector<Value> items;

    const Value* member(std::string_view name) const;
    // "a.b.c": nested members.
    const Value* path(std::string_view dotted) const;
    std::string json() const;
};

struct DecodedField {
    Sig sig;
    const FieldDef* def; // null: not described by the schema (bytes are raw)
    Value value;
};

// Decodes every field of a form in order (context variables are tracked like the importer does).
std::vector<DecodedField> decode_form(const Database& db, const FormEntry& form);
std::vector<DecodedField> decode_fields(const Schema& schema, Sig record, uint16_t form_version, FieldRange fields);

// One JSON object for a form: id, type, edid, flags, plugin, sources, fields (repeated fields become lists).
std::string form_json(const Database& db, const FormEntry& form);

} // namespace spud::forms
