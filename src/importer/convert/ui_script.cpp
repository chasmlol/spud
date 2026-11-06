#include "importer/convert/ui_script.hpp"

#include "core/error.hpp"

#include <cstring>
#include <format>
#include <map>

namespace spud::importer {

namespace {

constexpr uint8_t op_constant_pool = 0x88;
constexpr uint8_t op_push = 0x96;
constexpr uint8_t op_jump = 0x99;
constexpr uint8_t op_if = 0x9D;
constexpr uint8_t op_define_function = 0x9B;
constexpr uint8_t op_define_function2 = 0x8E;
constexpr uint8_t op_store_register = 0x87;
constexpr uint8_t op_goto_frame = 0x81;
constexpr uint8_t op_goto_label = 0x8C;
constexpr uint8_t op_goto_frame2 = 0x9F;
constexpr uint8_t op_get_url = 0x83;
constexpr uint8_t op_get_url2 = 0x9A;
constexpr uint8_t op_set_target = 0x8B;
constexpr uint8_t op_wait_for_frame = 0x8A;
constexpr uint8_t op_wait_for_frame2 = 0x8D;
constexpr uint8_t op_with = 0x94;
constexpr uint8_t op_try = 0x8F;

struct Cursor {
    std::span<const std::byte> b;
    size_t p = 0;
    uint8_t u8() { return p < b.size() ? uint8_t(b[p++]) : throw ParseError("avm1: truncated"); }
    uint16_t u16() {
        const uint16_t lo = u8();
        return static_cast<uint16_t>(lo | (uint16_t(u8()) << 8));
    }
    uint32_t u32() {
        const uint32_t lo = u16();
        return lo | (uint32_t(u16()) << 16);
    }
    std::string cstr() {
        std::string s;
        for (;;) {
            const char c = static_cast<char>(u8());
            if (!c) {
                return s;
            }
            s += c;
        }
    }
};

} // namespace

uint32_t ScriptTranslator::block(std::span<const std::byte> code) {
    return translate(code, osu::FunctionKind::block, {}, {}, 0, 0, {});
}

uint32_t ScriptTranslator::translate(std::span<const std::byte> code, osu::FunctionKind kind, std::string_view name,
                                     const std::vector<std::pair<std::string, uint32_t>>& params, uint8_t registers,
                                     uint16_t flags, std::vector<std::string> pool) {
    const uint32_t index = static_cast<uint32_t>(m_movie.functions.size());
    m_movie.functions.emplace_back();

    struct Pending {
        osu::Instruction ins;
        size_t target_byte = SIZE_MAX; // branches
    };
    std::vector<Pending> out;
    std::map<size_t, uint32_t> at_byte; // byte offset of every decoded action -> instruction index
    Cursor c{code};
    while (c.p < code.size()) {
        const size_t start = c.p;
        at_byte[start] = static_cast<uint32_t>(out.size());
        const uint8_t op = c.u8();
        if (op == 0) {
            break;
        }
        const size_t length = op >= 0x80 ? c.u16() : 0;
        const size_t body = c.p;
        if (body + length > code.size()) {
            throw ParseError(std::format("avm1: action 0x{:02x} at {} overruns its block", op, start));
        }
        Pending p;
        p.ins.op = op;
        switch (op) {
        case op_constant_pool: {
            const unsigned n = c.u16();
            pool.clear();
            for (unsigned i = 0; i < n; ++i) {
                pool.push_back(c.cstr());
            }
            c.p = body + length;
            continue; // resolved into the push values; no instruction
        }
        case op_push: {
            p.ins.arg = static_cast<uint32_t>(m_movie.values.size());
            uint16_t count = 0;
            while (c.p < body + length) {
                osu::PushValue v{};
                const uint8_t type = c.u8();
                switch (type) {
                case 0:
                    v.kind = osu::ValueKind::string;
                    v.u = m_strings.add(c.cstr());
                    break;
                case 1: {
                    const uint32_t bits = c.u32();
                    float f;
                    std::memcpy(&f, &bits, 4);
                    v.kind = osu::ValueKind::number;
                    v.number = f;
                    break;
                }
                case 2: v.kind = osu::ValueKind::null; break;
                case 3: v.kind = osu::ValueKind::undefined; break;
                case 4:
                    v.kind = osu::ValueKind::reg;
                    v.u = c.u8();
                    break;
                case 5:
                    v.kind = osu::ValueKind::boolean;
                    v.u = c.u8() != 0;
                    break;
                case 6: { // AVM1 doubles store the two 32-bit halves swapped
                    const uint64_t hi = c.u32(), lo = c.u32();
                    const uint64_t bits = (hi << 32) | lo;
                    double d;
                    std::memcpy(&d, &bits, 8);
                    v.kind = osu::ValueKind::number;
                    v.number = d;
                    break;
                }
                case 7:
                    v.kind = osu::ValueKind::number;
                    v.number = double(static_cast<int32_t>(c.u32()));
                    break;
                case 8:
                case 9: {
                    const unsigned i = type == 8 ? c.u8() : c.u16();
                    v.kind = osu::ValueKind::string;
                    if (i < pool.size()) {
                        v.u = m_strings.add(pool[i]);
                    } else {
                        m_warnings.push_back(std::format("avm1: constant {} outside the pool", i));
                        v.kind = osu::ValueKind::undefined;
                    }
                    break;
                }
                default:
                    throw ParseError(std::format("avm1: push type {}", type));
                }
                m_movie.values.push_back(v);
                ++count;
            }
            p.ins.count = count;
            break;
        }
        case op_jump:
        case op_if: {
            const auto off = static_cast<int16_t>(c.u16());
            p.target_byte = size_t(int64_t(body + length) + off);
            break;
        }
        case op_store_register: p.ins.arg = c.u8(); break;
        case op_goto_frame: p.ins.arg = c.u16(); break;
        case op_goto_label: p.ins.arg = m_strings.add(c.cstr()); break;
        case op_goto_frame2: {
            const uint8_t f = c.u8();
            p.ins.count = f & 1;
            p.ins.arg = (f & 2) ? c.u16() : 0;
            break;
        }
        case op_get_url2: p.ins.count = c.u8(); break;
        case op_get_url: {
            // Translated to GetURL2 semantics: push url and target, then the call.
            const std::string url = c.cstr(), target = c.cstr();
            Pending push;
            push.ins.op = op_push;
            push.ins.arg = static_cast<uint32_t>(m_movie.values.size());
            push.ins.count = 2;
            for (const auto& s : {url, target}) {
                osu::PushValue v{};
                v.kind = osu::ValueKind::string;
                v.u = m_strings.add(s);
                m_movie.values.push_back(v);
            }
            out.push_back(push);
            p.ins.op = op_get_url2;
            p.ins.count = 0;
            break;
        }
        case op_set_target: p.ins.arg = m_strings.add(c.cstr()); break;
        case op_define_function:
        case op_define_function2: {
            const std::string fname = c.cstr();
            const unsigned n = c.u16();
            std::vector<std::pair<std::string, uint32_t>> ps;
            uint8_t regs = 0;
            uint16_t fflags = 0;
            if (op == op_define_function2) {
                regs = c.u8();
                fflags = c.u16();
                for (unsigned i = 0; i < n; ++i) {
                    const uint8_t reg = c.u8();
                    ps.emplace_back(c.cstr(), reg);
                }
            } else {
                for (unsigned i = 0; i < n; ++i) {
                    ps.emplace_back(c.cstr(), 0);
                }
            }
            const unsigned size = c.u16();
            if (c.p + size > code.size()) {
                throw ParseError("avm1: function body overruns its block");
            }
            const auto child = translate(code.subspan(c.p, size),
                                         op == op_define_function2 ? osu::FunctionKind::function2
                                                                   : osu::FunctionKind::function,
                                         fname, ps, regs, fflags, pool);
            p.ins.arg = child;
            out.push_back(p);
            c.p += size; // the body follows the header and belongs to the child
            continue;
        }
        case op_with:
        case op_try:
        case op_wait_for_frame:
        case op_wait_for_frame2:
            m_warnings.push_back(std::format("avm1: action 0x{:02x} is not supported by the player", op));
            break;
        default:
            if (op >= 0x80) {
                m_warnings.push_back(std::format("avm1: unknown action 0x{:02x}", op));
            }
            break;
        }
        c.p = body + length;
        out.push_back(p);
    }
    at_byte[c.p] = static_cast<uint32_t>(out.size());
    at_byte[code.size()] = static_cast<uint32_t>(out.size());

    osu::Function& f = m_movie.functions[index];
    f.name = name.empty() ? osu::k_none : m_strings.add(name);
    f.kind = kind;
    f.registers = registers;
    f.flags = flags;
    f.first_param = static_cast<uint32_t>(m_movie.params.size());
    f.param_count = static_cast<uint16_t>(params.size());
    for (const auto& [pname, reg] : params) {
        m_movie.params.push_back({m_strings.add(pname), reg});
    }
    f.first_instruction = static_cast<uint32_t>(m_movie.instructions.size());
    f.instruction_count = static_cast<uint32_t>(out.size());
    for (auto& p : out) {
        if (p.target_byte != SIZE_MAX) {
            auto it = at_byte.lower_bound(p.target_byte);
            if (it == at_byte.end() || it->first != p.target_byte) {
                m_warnings.push_back(std::format("avm1: branch to byte {} is not an action boundary", p.target_byte));
                it = it == at_byte.end() ? std::prev(it) : it;
            }
            p.ins.arg = it->second;
        }
        m_movie.instructions.push_back(p.ins);
    }
    return index;
}

} // namespace spud::importer
