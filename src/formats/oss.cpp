#include "formats/oss.hpp"

#include <format>

namespace spud::oss {

namespace {

constexpr auto k_scrp = container::fourcc("SCRP");
constexpr auto k_vars = container::fourcc("VARS");
constexpr auto k_prop = container::fourcc("PROP");
constexpr auto k_stat = container::fourcc("STAT");
constexpr auto k_func = container::fourcc("FUNC");
constexpr auto k_slot = container::fourcc("SLOT");
constexpr auto k_inst = container::fourcc("INST");
constexpr auto k_oper = container::fourcc("OPER");
constexpr auto k_atch = container::fourcc("ATCH");
constexpr auto k_aprp = container::fourcc("APRP");
constexpr auto k_aval = container::fourcc("AVAL");
constexpr auto k_frag = container::fourcc("FRAG");
constexpr auto k_strs = container::fourcc("STRS");

void check_range(uint32_t first, uint32_t count, size_t size, const char* what) {
    if (uint64_t{first} + count > size) {
        throw ParseError(std::format(".oss {} range {}+{} out of range ({})", what, first, count, size));
    }
}

void validate(const Scripts& s) {
    for (const auto& c : s.scripts) {
        check_range(c.first_variable, c.variable_count, s.variables.size(), "script variables");
        check_range(c.first_property, c.property_count, s.properties.size(), "script properties");
        check_range(c.first_state, c.state_count, s.states.size(), "script states");
    }
    for (const auto& st : s.states) {
        check_range(st.first_function, st.function_count, s.functions.size(), "state functions");
    }
    for (const auto& f : s.functions) {
        check_range(f.first_slot, f.parameter_count + f.local_count, s.slots.size(), "function slots");
        check_range(f.first_instruction, f.instruction_count, s.instructions.size(), "function code");
    }
    for (const auto& i : s.instructions) {
        check_range(i.first_operand, i.operand_count, s.operands.size(), "instruction operands");
    }
    for (const auto& a : s.attachments) {
        check_range(a.first_property, a.property_count, s.attached_properties.size(), "attachment properties");
    }
    for (const auto& p : s.attached_properties) {
        check_range(p.first_value, p.value_count, s.attached_values.size(), "attached values");
    }
}

} // namespace

std::vector<std::byte> write(const Scripts& s) {
    validate(s);
    container::Writer w(k_kind, k_version);
    w.add_array(k_scrp, s.scripts);
    w.add_array(k_vars, s.variables);
    w.add_array(k_prop, s.properties);
    w.add_array(k_stat, s.states);
    w.add_array(k_func, s.functions);
    w.add_array(k_slot, s.slots);
    w.add_array(k_inst, s.instructions);
    w.add_array(k_oper, s.operands);
    w.add_array(k_atch, s.attachments);
    w.add_array(k_aprp, s.attached_properties);
    w.add_array(k_aval, s.attached_values);
    w.add_array(k_frag, s.fragments);
    w.add(k_strs, s.strings);
    return w.finish();
}

Scripts read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() != k_version) {
        throw ParseError(std::format(".oss version {} (want {})", r.version(), k_version));
    }
    Scripts s;
    s.scripts = r.read_array<Script>(k_scrp);
    s.variables = r.read_array<Variable>(k_vars);
    s.properties = r.read_array<Property>(k_prop);
    s.states = r.read_array<State>(k_stat);
    s.functions = r.read_array<Function>(k_func);
    s.slots = r.read_array<Slot>(k_slot);
    s.instructions = r.read_array<Instruction>(k_inst);
    s.operands = r.read_array<Operand>(k_oper);
    s.attachments = r.read_array<Attachment>(k_atch);
    s.attached_properties = r.read_array<AttachedProperty>(k_aprp);
    s.attached_values = r.read_array<AttachedValue>(k_aval);
    s.fragments = r.read_array<Fragment>(k_frag);
    const auto strings = r.require(k_strs);
    s.strings.assign(strings.begin(), strings.end());
    validate(s);
    return s;
}

} // namespace spud::oss
