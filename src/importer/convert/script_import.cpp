#include "importer/convert/script_import.hpp"

#include "core/binary.hpp"
#include "core/path.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <unordered_map>

// PEX layout: UESP "Skyrim Mod:Compiled Script File Format", checked by reading all 14026 vanilla scripts to their
// exact ends. VMAD: UESP "Skyrim Mod:Mod File Format/VMAD Field".

namespace spud::importer {

namespace {

class BigEndian {
public:
    explicit BigEndian(std::span<const std::byte> d) : m_d(d) {}

    template <typename T>
    T read() {
        if (m_at + sizeof(T) > m_d.size()) {
            throw ParseError("pex: truncated");
        }
        uint8_t b[sizeof(T)];
        for (size_t i = 0; i < sizeof(T); ++i) {
            b[i] = static_cast<uint8_t>(m_d[m_at + sizeof(T) - 1 - i]);
        }
        m_at += sizeof(T);
        T v;
        std::memcpy(&v, b, sizeof(T));
        return v;
    }

    void skip(size_t n) { m_at += n; }

    std::string string() {
        const uint16_t n = read<uint16_t>();
        if (m_at + n > m_d.size()) {
            throw ParseError("pex: truncated string");
        }
        std::string s(reinterpret_cast<const char*>(m_d.data()) + m_at, n);
        m_at += n;
        return s;
    }

    bool at_end() const { return m_at == m_d.size(); }

private:
    std::span<const std::byte> m_d;
    size_t m_at = 0;
};

struct PexValue {
    uint8_t type = 0; // 0 none, 1 identifier, 2 string, 3 integer, 4 float, 5 bool
    uint32_t bits = 0; // string index for 1 and 2
};

// PEX fixed argument counts; a negative count is followed by a variable list.
constexpr int k_arguments[] = {0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 1, 2, 2, -3, -2, -3, 1, 3, 3, 3,
                               2, 2, 3, 3, 4, 4};
constexpr size_t k_opcode_count = std::size(k_arguments);

// Which fixed arguments name something instead of holding a value.
bool is_name(oss::Op op, size_t index) {
    switch (op) {
    case oss::Op::call_method:
    case oss::Op::call_parent:
    case oss::Op::prop_get:
    case oss::Op::prop_set: return index == 0;
    case oss::Op::call_static: return index <= 1;
    default: return false;
    }
}

} // namespace

struct ScriptCompiler::Impl {
    struct Compiled {
        oss::Script script;
        std::string key; // lower-case name
        std::vector<oss::Variable> variables;
        std::vector<oss::Property> properties;
        std::vector<oss::State> states;
        std::vector<oss::Function> functions;
        std::vector<oss::Slot> slots;
        std::vector<oss::Instruction> instructions;
        std::vector<oss::Operand> operands;
    };

    container::StringTableBuilder strings;
    std::unordered_map<std::string, Compiled> scripts;
    oss::Scripts out;

    uint32_t str(std::string_view s) { return strings.add(s); }

    static PexValue value(BigEndian& r) {
        PexValue v;
        v.type = r.read<uint8_t>();
        switch (v.type) {
        case 0: break;
        case 1:
        case 2: v.bits = r.read<uint16_t>(); break;
        case 3:
        case 4: v.bits = r.read<uint32_t>(); break;
        case 5: v.bits = r.read<uint8_t>(); break;
        default: throw ParseError(std::format("pex: value type {}", v.type));
        }
        return v;
    }
};

ScriptCompiler::ScriptCompiler() : m(std::make_unique<Impl>()) {}
ScriptCompiler::~ScriptCompiler() = default;

void ScriptCompiler::add(std::span<const std::byte> pex) {
    BigEndian r(pex);
    if (r.read<uint32_t>() != 0xFA57C0DE) {
        throw ParseError("pex: bad magic");
    }
    r.skip(2 + 2 + 8); // version, game ID, compile time
    r.string();        // source file name
    r.string();        // user
    r.string();        // machine
    std::vector<std::string> table(r.read<uint16_t>());
    for (auto& s : table) {
        s = r.string();
    }
    auto text = [&](uint32_t i) -> const std::string& {
        if (i >= table.size()) {
            throw ParseError(std::format("pex: string {} of {}", i, table.size()));
        }
        return table[i];
    };
    if (r.read<uint8_t>() != 0) {
        r.skip(8); // modification time
        const uint16_t functions = r.read<uint16_t>();
        for (uint16_t i = 0; i < functions; ++i) {
            r.skip(2 + 2 + 2 + 1);
            r.skip(size_t{r.read<uint16_t>()} * 2); // line numbers
        }
    }
    const uint16_t flag_names = r.read<uint16_t>();
    r.skip(size_t{flag_names} * 3);

    const uint16_t objects = r.read<uint16_t>();
    for (uint16_t o = 0; o < objects; ++o) {
        Impl::Compiled c;
        const std::string& name = text(r.read<uint16_t>());
        r.skip(4); // size
        const std::string& parent = text(r.read<uint16_t>());
        r.read<uint16_t>(); // doc string
        c.script.user_flags = r.read<uint32_t>();
        c.script.auto_state = m->str(text(r.read<uint16_t>()));
        c.script.name = m->str(name);
        c.script.parent = parent.empty() ? oss::k_none : m->str(parent);
        c.key = path::to_lower(name);

        std::unordered_map<std::string, uint32_t> members;
        auto operand = [&](const PexValue& v, const std::unordered_map<std::string, uint32_t>* slots) {
            switch (v.type) {
            case 0: return oss::Operand{oss::OperandKind::none, 0};
            case 2: return oss::Operand{oss::OperandKind::string, m->str(text(v.bits))};
            case 3: return oss::Operand{oss::OperandKind::integer, v.bits};
            case 4: return oss::Operand{oss::OperandKind::real, v.bits};
            case 5: return oss::Operand{oss::OperandKind::boolean, v.bits != 0};
            default: break;
            }
            const std::string key = path::to_lower(text(v.bits));
            if (key == "self") {
                return oss::Operand{oss::OperandKind::self, 0};
            }
            if (key == "::nonevar") {
                return oss::Operand{oss::OperandKind::discard, 0};
            }
            if (key == "::state") {
                return oss::Operand{oss::OperandKind::state, 0};
            }
            if (slots) {
                if (auto it = slots->find(key); it != slots->end()) {
                    return oss::Operand{oss::OperandKind::slot, it->second};
                }
            }
            if (auto it = members.find(key); it != members.end()) {
                return oss::Operand{oss::OperandKind::member, it->second};
            }
            // An identifier the compiler left unresolved, such as a script used as a call's object.
            return oss::Operand{oss::OperandKind::name, m->str(text(v.bits))};
        };

        auto function = [&](std::string_view function_name) {
            oss::Function f{};
            f.name = m->str(function_name);
            f.return_type = m->str(text(r.read<uint16_t>()));
            r.read<uint16_t>(); // doc string
            r.read<uint32_t>(); // user flags
            f.flags = r.read<uint8_t>();
            f.first_slot = static_cast<uint32_t>(c.slots.size());
            std::unordered_map<std::string, uint32_t> slots;
            auto slot = [&]() {
                const std::string& slot_name = text(r.read<uint16_t>());
                const std::string& type = text(r.read<uint16_t>());
                slots.emplace(path::to_lower(slot_name), static_cast<uint32_t>(c.slots.size()) - f.first_slot);
                c.slots.push_back({m->str(slot_name), m->str(type)});
            };
            f.parameter_count = r.read<uint16_t>();
            for (uint32_t i = 0; i < f.parameter_count; ++i) {
                slot();
            }
            f.local_count = r.read<uint16_t>();
            for (uint32_t i = 0; i < f.local_count; ++i) {
                slot();
            }
            f.first_instruction = static_cast<uint32_t>(c.instructions.size());
            f.instruction_count = r.read<uint16_t>();
            for (uint32_t i = 0; i < f.instruction_count; ++i) {
                const uint8_t code = r.read<uint8_t>();
                if (code >= k_opcode_count) {
                    throw ParseError(std::format("pex: opcode {}", code));
                }
                oss::Instruction ins{};
                ins.op = static_cast<oss::Op>(code);
                ins.first_operand = static_cast<uint32_t>(c.operands.size());
                const int arguments = k_arguments[code];
                const size_t fixed = static_cast<size_t>(std::abs(arguments));
                for (size_t a = 0; a < fixed; ++a) {
                    const PexValue v = Impl::value(r);
                    const bool jump_target = (ins.op == oss::Op::jmp && a == 0) ||
                                             ((ins.op == oss::Op::jmpt || ins.op == oss::Op::jmpf) && a == 1);
                    if (jump_target) {
                        // Relative in PEX; absolute within the function here.
                        c.operands.push_back({oss::OperandKind::integer,
                                              static_cast<uint32_t>(static_cast<int32_t>(i) + static_cast<int32_t>(v.bits))});
                    } else if (is_name(ins.op, a)) {
                        c.operands.push_back({oss::OperandKind::name, m->str(text(v.bits))});
                    } else {
                        c.operands.push_back(operand(v, &slots));
                    }
                }
                if (arguments < 0) {
                    const PexValue count = Impl::value(r);
                    for (uint32_t a = 0; a < count.bits; ++a) {
                        c.operands.push_back(operand(Impl::value(r), &slots));
                    }
                }
                ins.operand_count = static_cast<uint32_t>(c.operands.size()) - ins.first_operand;
                c.instructions.push_back(ins);
            }
            c.functions.push_back(f);
            return static_cast<uint32_t>(c.functions.size() - 1);
        };

        const uint16_t variables = r.read<uint16_t>();
        for (uint16_t i = 0; i < variables; ++i) {
            const std::string& variable = text(r.read<uint16_t>());
            const std::string& type = text(r.read<uint16_t>());
            const uint32_t flags = r.read<uint32_t>();
            members.emplace(path::to_lower(variable), i);
            c.variables.push_back({m->str(variable), m->str(type), flags, operand(Impl::value(r), nullptr)});
        }
        const uint16_t properties = r.read<uint16_t>();
        for (uint16_t i = 0; i < properties; ++i) {
            const std::string& property = text(r.read<uint16_t>());
            oss::Property p{};
            p.name = m->str(property);
            p.type = m->str(text(r.read<uint16_t>()));
            r.read<uint16_t>(); // doc string
            p.user_flags = r.read<uint32_t>();
            p.flags = r.read<uint8_t>();
            p.auto_variable = p.getter = p.setter = oss::k_none;
            if (p.flags & oss::property_auto) {
                const std::string key = path::to_lower(text(r.read<uint16_t>()));
                if (auto it = members.find(key); it != members.end()) {
                    p.auto_variable = it->second;
                } else {
                    warnings.push_back(std::format("{}: auto property {} has no variable", name, property));
                }
            } else {
                if (p.flags & oss::property_read) {
                    p.getter = function("get_" + property);
                }
                if (p.flags & oss::property_write) {
                    p.setter = function("set_" + property);
                }
            }
            c.properties.push_back(p);
        }
        const uint16_t states = r.read<uint16_t>();
        for (uint16_t i = 0; i < states; ++i) {
            oss::State s{};
            s.name = m->str(text(r.read<uint16_t>()));
            s.first_function = static_cast<uint32_t>(c.functions.size());
            s.function_count = r.read<uint16_t>();
            for (uint32_t f = 0; f < s.function_count; ++f) {
                function(text(r.read<uint16_t>()));
            }
            c.states.push_back(s);
        }
        m->scripts.insert_or_assign(c.key, std::move(c));
    }
    if (!r.at_end()) {
        throw ParseError("pex: bytes after the last object");
    }
}

void ScriptCompiler::add_attachments(uint32_t form_id, legacy::RecordType type, std::span<const std::byte> vmad,
                                     const legacy::PluginInfo& plugin) {
    BinaryReader r(vmad);
    auto wstring = [&r]() {
        const uint16_t n = r.read<uint16_t>();
        std::string s(n, '\0');
        for (uint16_t i = 0; i < n; ++i) {
            s[i] = static_cast<char>(r.read<uint8_t>());
        }
        return s;
    };
    auto object = [&r, &plugin](int16_t format, int16_t& alias) {
        uint32_t local = 0;
        if (format == 1) {
            local = r.read<uint32_t>();
            alias = r.read<int16_t>();
            r.skip(2);
        } else {
            r.skip(2);
            alias = r.read<int16_t>();
            local = r.read<uint32_t>();
        }
        return local ? plugin.resolve(local) : 0;
    };
    auto value = [&](uint8_t kind, int16_t format) {
        oss::AttachedValue v{oss::ValueKind::none, 0, -1};
        switch (kind) {
        case 1: {
            int16_t alias = -1;
            const uint32_t form = object(format, alias);
            v = {oss::ValueKind::object, form, alias};
            break;
        }
        case 2: v = {oss::ValueKind::string, m->str(wstring()), -1}; break;
        case 3: v = {oss::ValueKind::integer, r.read<uint32_t>(), -1}; break;
        case 4: v = {oss::ValueKind::real, r.read<uint32_t>(), -1}; break;
        case 5: v = {oss::ValueKind::boolean, r.read<uint8_t>() != 0, -1}; break;
        default: throw ParseError(std::format("VMAD property type {}", kind));
        }
        return v;
    };
    auto scripts = [&](uint32_t target, int32_t alias, int16_t version, int16_t format) {
        const uint16_t count = r.read<uint16_t>();
        for (uint16_t s = 0; s < count; ++s) {
            oss::Attachment a{};
            a.form_id = target;
            a.alias = alias;
            a.script = m->str(wstring());
            if (version >= 4) {
                a.status = r.read<uint8_t>();
            }
            a.first_property = static_cast<uint32_t>(m->out.attached_properties.size());
            a.property_count = r.read<uint16_t>();
            for (uint32_t p = 0; p < a.property_count; ++p) {
                oss::AttachedProperty property{};
                property.name = m->str(wstring());
                const uint8_t kind = r.read<uint8_t>();
                if (version >= 4) {
                    property.status = r.read<uint8_t>();
                }
                std::vector<oss::AttachedValue> values;
                if (kind >= 11 && kind <= 15) {
                    property.array = 1;
                    property.value_count = r.read<uint32_t>();
                    for (uint32_t i = 0; i < property.value_count; ++i) {
                        values.push_back(value(static_cast<uint8_t>(kind - 10), format));
                    }
                } else {
                    property.value_count = 1;
                    values.push_back(value(kind, format));
                }
                property.first_value = static_cast<uint32_t>(m->out.attached_values.size());
                m->out.attached_values.insert(m->out.attached_values.end(), values.begin(), values.end());
                m->out.attached_properties.push_back(property);
            }
            m->out.attachments.push_back(a);
        }
    };
    auto fragment = [&](oss::FragmentKind kind, uint32_t index, int32_t entry) {
        r.skip(1);
        oss::Fragment f{form_id, kind, index, entry, 0, 0};
        f.script = m->str(wstring());
        f.function = m->str(wstring());
        m->out.fragments.push_back(f);
    };

    const int16_t version = r.read<int16_t>();
    const int16_t format = r.read<int16_t>();
    scripts(form_id, -1, version, format);
    if (r.remaining() == 0) {
        return;
    }
    // Layouts as the record codec reads them (importer/records/codecs.cpp), which every vanilla VMAD passes.
    using legacy::record_type;
    if (type == record_type("INFO") || type == record_type("PACK") || type == record_type("SCEN")) {
        r.skip(1);
        const uint8_t flags = r.read<uint8_t>();
        wstring();
        const bool pack = type == record_type("PACK");
        const oss::FragmentKind first = type == record_type("INFO") ? oss::FragmentKind::info_begin
                                        : pack                      ? oss::FragmentKind::package_begin
                                                                    : oss::FragmentKind::scene_begin;
        for (uint32_t bit = 0; bit < (pack ? 3u : 2u); ++bit) {
            if (flags & (1u << bit)) {
                fragment(static_cast<oss::FragmentKind>(static_cast<uint32_t>(first) + bit), 0, 0);
            }
        }
        if (type == record_type("SCEN")) {
            const uint16_t phases = r.read<uint16_t>();
            for (uint16_t i = 0; i < phases; ++i) {
                const uint8_t when = r.read<uint8_t>();
                const uint32_t phase = r.read<uint32_t>();
                fragment(oss::FragmentKind::scene_phase, phase, when);
            }
        }
    } else if (type == record_type("PERK")) {
        r.skip(1);
        wstring();
        const uint16_t n = r.read<uint16_t>();
        for (uint16_t i = 0; i < n; ++i) {
            const uint32_t entry = r.read<uint32_t>();
            fragment(oss::FragmentKind::perk_entry, entry, 0);
        }
    } else if (type == record_type("QUST")) {
        r.skip(1);
        const uint16_t n = r.read<uint16_t>();
        wstring();
        for (uint16_t i = 0; i < n; ++i) {
            const uint32_t stage = r.read<uint32_t>() & 0xFFFF;
            const int32_t log = r.read<int32_t>();
            fragment(oss::FragmentKind::quest_stage, stage, log);
        }
        const uint16_t aliases = r.read<uint16_t>();
        for (uint16_t i = 0; i < aliases; ++i) {
            int16_t alias = -1;
            const uint32_t quest = object(format, alias);
            const int16_t alias_version = r.read<int16_t>();
            const int16_t alias_format = r.read<int16_t>();
            scripts(quest ? quest : form_id, alias, alias_version, alias_format);
        }
    }
}

oss::Scripts ScriptCompiler::finish() {
    std::vector<Impl::Compiled*> sorted;
    sorted.reserve(m->scripts.size());
    for (auto& [key, c] : m->scripts) {
        sorted.push_back(&c);
    }
    std::ranges::sort(sorted, {}, &Impl::Compiled::key);
    auto& out = m->out;
    for (Impl::Compiled* c : sorted) {
        const uint32_t variable_base = static_cast<uint32_t>(out.variables.size());
        const uint32_t property_base = static_cast<uint32_t>(out.properties.size());
        const uint32_t state_base = static_cast<uint32_t>(out.states.size());
        const uint32_t function_base = static_cast<uint32_t>(out.functions.size());
        const uint32_t slot_base = static_cast<uint32_t>(out.slots.size());
        const uint32_t instruction_base = static_cast<uint32_t>(out.instructions.size());
        const uint32_t operand_base = static_cast<uint32_t>(out.operands.size());
        c->script.first_variable = variable_base;
        c->script.variable_count = static_cast<uint32_t>(c->variables.size());
        c->script.first_property = property_base;
        c->script.property_count = static_cast<uint32_t>(c->properties.size());
        c->script.first_state = state_base;
        c->script.state_count = static_cast<uint32_t>(c->states.size());
        out.scripts.push_back(c->script);
        out.variables.insert(out.variables.end(), c->variables.begin(), c->variables.end());
        for (auto p : c->properties) {
            p.getter = p.getter == oss::k_none ? oss::k_none : p.getter + function_base;
            p.setter = p.setter == oss::k_none ? oss::k_none : p.setter + function_base;
            out.properties.push_back(p);
        }
        for (auto s : c->states) {
            s.first_function += function_base;
            out.states.push_back(s);
        }
        for (auto f : c->functions) {
            f.first_slot += slot_base;
            f.first_instruction += instruction_base;
            out.functions.push_back(f);
        }
        out.slots.insert(out.slots.end(), c->slots.begin(), c->slots.end());
        for (auto i : c->instructions) {
            i.first_operand += operand_base;
            out.instructions.push_back(i);
        }
        out.operands.insert(out.operands.end(), c->operands.begin(), c->operands.end());
    }
    std::ranges::stable_sort(out.attachments, {}, &oss::Attachment::form_id);
    std::ranges::stable_sort(out.fragments, {}, &oss::Fragment::form_id);
    const auto bytes = m->strings.bytes();
    out.strings.assign(bytes.begin(), bytes.end());
    return std::move(out);
}

ScriptImport import_scripts(const legacy::EsmDatabase& db, const legacy::DataSource& data, std::span<const std::byte> extra) {
    ScriptImport result;
    ScriptCompiler compiler;
    if (!extra.empty()) {
        compiler.add(extra);
    }
    for (const std::string& path : data.list("scripts/", ".pex")) {
        const auto bytes = data.read(path);
        if (!bytes) {
            continue;
        }
        try {
            compiler.add(*bytes);
        } catch (const std::exception& e) {
            result.warnings.push_back(std::format("{}: {}", path, e.what()));
        }
    }
    const auto vmad = legacy::record_type("VMAD");
    for (const legacy::RecordEntry& entry : db.entries()) {
        if (entry.deleted() || db.find(entry.form_id) != &entry) {
            continue;
        }
        try {
            const legacy::Record record = db.read(entry);
            if (const auto* sub = record.find(vmad)) {
                compiler.add_attachments(entry.form_id, entry.type, sub->data, record.plugin());
            }
        } catch (const std::exception& e) {
            result.warnings.push_back(std::format("{:08X} VMAD: {}", entry.form_id, e.what()));
        }
    }
    result.scripts = compiler.finish();
    result.warnings.insert(result.warnings.end(), compiler.warnings.begin(), compiler.warnings.end());
    return result;
}

} // namespace spud::importer
