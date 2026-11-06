#pragma once

// The record schema language: every record type and the layout of each of its fields. One text, parsed here,
// drives both sides: the importer converts plugin records with it, and the same text is stored in every forms
// database (.osf, docs/formats/forms.md) so readers decode fields without knowing where they came from.
// Language reference: docs/formats/forms.md, "Schema".

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::forms {

using Sig = uint32_t;

constexpr Sig sig(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}
std::string sig_name(Sig s);
// The integer a string gives a schema variable or union case: FNV-1a of its lowercased bytes.
uint32_t string_key(std::string_view s);
// Four characters -> Sig; nullopt otherwise.
std::optional<Sig> parse_sig(std::string_view s);

enum class Kind : uint8_t {
    i8, i16, i32, i64, u8, u16, u32, u64, f32, f64,
    formid,  // u32, a load-order-resolved FormID in the database
    zstring, // NUL-terminated text; at the end of a field it may also end without NUL
    lstring, // localized text in plugins; NUL-terminated UTF-8 in the database
    wstring, // u16 byte length + text
    wstring32, // u32 byte length + text
    bytes,   // raw bytes: a fixed count, or the rest of the field
    empty,   // a zero-length marker field
    structure,
    array,
    union_,
    codec,   // a layout too irregular for the language, decoded by named code (ctda, vmad, ...)
};

enum class CountMode : uint8_t { to_end, fixed, prefix_u8, prefix_u16, prefix_u32, member };
enum class SelectorMode : uint8_t { size, member, var, formver };

using TypeId = uint32_t;
inline constexpr TypeId k_no_type = 0xFFFFFFFFu;

struct Member {
    std::string name; // "_" for padding and unused bytes
    TypeId type;
};

struct Case {
    int64_t value;
    TypeId type;
};

struct Type {
    Kind kind = Kind::u8;
    std::string name;            // set for named types (`type NAME = ...`)
    std::vector<Sig> targets;    // formid: allowed target record types (empty: any)
    uint8_t table = 0;           // lstring: 0 .STRINGS, 1 .DLSTRINGS, 2 .ILSTRINGS
    uint32_t count = 0;          // bytes(N) / array fixed count
    bool sized = false;          // bytes: count is fixed
    std::vector<Member> members; // structure
    TypeId element = k_no_type;  // array
    CountMode count_mode = CountMode::to_end;
    std::string count_member;    // array: CountMode::member
    SelectorMode selector = SelectorMode::size; // union
    std::string selector_name;   // union: member or variable name
    std::vector<Case> cases;     // union
    TypeId fallback = k_no_type; // union: `*` case
    std::string codec;           // codec name

    // Fixed byte size in plugin data, or nullopt when it depends on the data.
    std::optional<uint32_t> fixed_size;
};

struct Condition {
    std::string var;
    std::vector<int64_t> values;
    bool negate = false;
};

struct FieldDef {
    Sig sig = 0;
    std::string name;
    TypeId type = k_no_type;
    std::vector<Condition> when; // all must hold for this definition to apply
    std::string sets_var;        // record context variable set after decoding this field
    std::string sets_from;       // member providing the value ("" = first scalar)
    std::optional<int64_t> sets_value; // constant instead of a member
    bool sets_first_char = false;      // `=char0`: the first character of the field's first string
};

struct RecordDef {
    Sig sig = 0;
    std::string description;
    std::vector<FieldDef> fields;
};

class Schema {
public:
    Schema() = default;
    // Parses schema text; throws ParseError with "file:line: message" on errors. Several texts may be
    // parsed into one schema (types are shared across them).
    void parse(std::string_view text, std::string_view file_name = "schema");
    // Resolves forward references; call once after the last parse(). Throws ParseError.
    void finish();

    std::span<const Type> types() const { return m_types; }
    const Type& type(TypeId id) const { return m_types[id]; }
    const RecordDef* record(Sig sig) const;
    std::span<const RecordDef> records() const { return m_records; }
    std::optional<TypeId> named(std::string_view name) const;

    // A type as schema text, for messages.
    std::string type_text(TypeId id) const;

private:
    friend class SchemaParser;
    TypeId add(Type t);
    void compute_size(TypeId id, std::vector<uint8_t>& state);

    std::vector<Type> m_types;
    std::unordered_map<std::string, TypeId> m_named;
    std::vector<RecordDef> m_records;
    std::unordered_map<Sig, size_t> m_record_index;
    std::vector<std::pair<TypeId, std::string>> m_unresolved; // placeholder type -> referenced name
    std::unordered_map<std::string, std::vector<FieldDef>> m_fieldsets; // `fields NAME ... end`
};

} // namespace spud::forms
