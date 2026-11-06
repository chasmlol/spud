#pragma once

// .oss — spud scripts: every compiled Papyrus script of the load order as native bytecode, and what the records
// attach to forms (scripts/scripts.oss in the cache). Spec: docs/formats/oss.md
//
// A script is its variables, properties, states and functions; a function is a list of instructions over typed
// slots. Names are resolved at import: an operand says which slot or member it reads, jumps are absolute, and the
// PEX debug tables are dropped.

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace spud::oss {

inline constexpr container::FourCC k_kind = container::fourcc("SCRP");
inline constexpr uint32_t k_version = 2; // 2: quest alias scripts (Attachment::alias), fragments (FRAG)
inline constexpr uint32_t k_none = 0xFFFFFFFFu;

// The PEX opcodes, in PEX numbering.
enum class Op : uint8_t {
    nop, iadd, fadd, isub, fsub, imul, fmul, idiv, fdiv, imod, bool_not, ineg, fneg, assign, cast, cmp_eq, cmp_lt,
    cmp_le, cmp_gt, cmp_ge, jmp, jmpt, jmpf, call_method, call_parent, call_static, ret, strcat, prop_get, prop_set,
    array_create, array_length, array_get, array_set, array_find, array_rfind
};

enum class OperandKind : uint32_t {
    none,     // the None literal
    slot,     // value: index into the frame's slots (parameters, then locals)
    member,   // value: index into the object's variables
    self,     // the running object
    discard,  // ::NoneVar: writes vanish, reads give None
    state,    // ::State: the object's state name
    string,   // value: string literal
    integer,  // value: int32 bits
    real,     // value: float bits
    boolean,  // value: 0 or 1
    name      // value: a function, property, script or state name
};

struct Operand {
    OperandKind kind;
    uint32_t value;
};
static_assert(sizeof(Operand) == 8);

struct Instruction {
    Op op;
    uint8_t reserved[3];
    uint32_t first_operand; // into operands; a call's arguments follow its fixed operands
    uint32_t operand_count;
};
static_assert(sizeof(Instruction) == 12);

// A parameter or local.
struct Slot {
    uint32_t name;
    uint32_t type; // string: "Int", "Float", "String", "Bool", a script's name, or any of them with "[]"
};
static_assert(sizeof(Slot) == 8);

enum FunctionFlag : uint32_t { function_global = 1, function_native = 2 };

struct Function {
    uint32_t name;
    uint32_t return_type; // string
    uint32_t flags;
    uint32_t first_slot;
    uint32_t parameter_count;
    uint32_t local_count;
    uint32_t first_instruction;
    uint32_t instruction_count;
};
static_assert(sizeof(Function) == 32);

struct State {
    uint32_t name; // "" is the empty state
    uint32_t first_function;
    uint32_t function_count;
};
static_assert(sizeof(State) == 12);

struct Variable {
    uint32_t name;
    uint32_t type;
    uint32_t user_flags; // bit 1: conditional
    Operand initial;
};
static_assert(sizeof(Variable) == 20);

enum PropertyFlag : uint32_t { property_read = 1, property_write = 2, property_auto = 4 };

struct Property {
    uint32_t name;
    uint32_t type;
    uint32_t flags;
    uint32_t user_flags;
    uint32_t auto_variable; // auto properties: index into the script's variables, otherwise k_none
    uint32_t getter;        // index into functions, or k_none
    uint32_t setter;
};
static_assert(sizeof(Property) == 28);

struct Script {
    uint32_t name;
    uint32_t parent;     // string; k_none when the script extends nothing
    uint32_t auto_state; // string; "" is the empty state
    uint32_t user_flags; // bit 0: hidden, bit 1: conditional
    uint32_t first_variable;
    uint32_t variable_count;
    uint32_t first_property;
    uint32_t property_count;
    uint32_t first_state;
    uint32_t state_count;
};
static_assert(sizeof(Script) == 40);

// A value in a record's VMAD.
enum class ValueKind : uint32_t { none, object, string, integer, real, boolean };

struct AttachedValue {
    ValueKind kind;
    uint32_t value;  // object: form ID, resolved into the load order; string: string; else the bits
    int32_t alias;   // object: a quest alias, or -1
};
static_assert(sizeof(AttachedValue) == 12);

struct AttachedProperty {
    uint32_t name;
    uint32_t status;
    uint32_t array; // 1: the values are the elements of an array
    uint32_t first_value;
    uint32_t value_count;
};
static_assert(sizeof(AttachedProperty) == 20);

// One script a record attaches to itself or to one of a quest's aliases, in VMAD order.
struct Attachment {
    uint32_t form_id;
    int32_t alias;   // a quest's alias ID, or -1 for the form itself
    uint32_t script; // string
    uint32_t status;
    uint32_t first_property;
    uint32_t property_count;
};
static_assert(sizeof(Attachment) == 24);

enum class FragmentKind : uint32_t {
    quest_stage,  // index: stage; entry: log entry index
    info_begin, info_end,
    package_begin, package_end, package_change,
    scene_begin, scene_end,
    scene_phase,  // index: phase; entry: 0 at its start, 1 at its end
    perk_entry    // index: entry point
};

// A record's script fragment: a method of `script`, which the record also attaches.
struct Fragment {
    uint32_t form_id;
    FragmentKind kind;
    uint32_t index;
    int32_t entry;
    uint32_t script;   // string
    uint32_t function; // string
};
static_assert(sizeof(Fragment) == 24);

struct Scripts {
    std::vector<Script> scripts; // sorted by lower-case name
    std::vector<Variable> variables;
    std::vector<Property> properties;
    std::vector<State> states;
    std::vector<Function> functions;
    std::vector<Slot> slots;
    std::vector<Instruction> instructions;
    std::vector<Operand> operands;
    std::vector<Attachment> attachments; // sorted by form ID, each form's in VMAD order, then its aliases'
    std::vector<AttachedProperty> attached_properties;
    std::vector<AttachedValue> attached_values;
    std::vector<Fragment> fragments; // sorted by form ID, each form's in VMAD order
    std::vector<std::byte> strings;

    std::string_view string(uint32_t offset) const { return container::StringTable(strings).get(offset); }
};

std::vector<std::byte> write(const Scripts& scripts);
// Throws ParseError on a file whose ranges don't hold together.
Scripts read(std::span<const std::byte> file);

} // namespace spud::oss
