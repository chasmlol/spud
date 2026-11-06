#include "importer/records/codecs.hpp"

#include "core/error.hpp"

#include <bit>
#include <cstring>
#include <format>

namespace spud::records {

namespace {

class Writer {
public:
    explicit Writer(CodecContext& c) : m_c(c) {}

    void bytes(std::span<const std::byte> b) { m_c.out.insert(m_c.out.end(), b.begin(), b.end()); }
    template <typename T>
    void put(const T& v) {
        auto p = reinterpret_cast<const std::byte*>(&v);
        m_c.out.insert(m_c.out.end(), p, p + sizeof(T));
    }
    template <typename T>
    T copy(BinaryReader& in) {
        auto v = in.read<T>();
        put(v);
        return v;
    }
    void form(BinaryReader& in) {
        auto v = in.read<uint32_t>();
        if (v != 0) {
            m_c.fixups.push_back({static_cast<uint32_t>(m_c.out.size() - m_c.base), forms::k_no_type});
        }
        put(v);
    }
    // u16-length text, re-encoded as UTF-8. Script and property names use the plugin's code page; string
    // property values are UTF-8 when valid (as the Papyrus compiler writes them).
    void wstring(BinaryReader& in, bool value) {
        auto n = in.read<uint16_t>();
        auto b = in.take(n);
        std::string_view s(reinterpret_cast<const char*>(b.data()), b.size());
        std::string text = value ? legacy::to_utf8(s, "utf-8") : legacy::to_utf8(s, m_c.ctx.language);
        if (text.size() > 0xFFFF) {
            throw ParseError("VMAD string too long after conversion");
        }
        put(static_cast<uint16_t>(text.size()));
        bytes(std::as_bytes(std::span(text.data(), text.size())));
    }

private:
    CodecContext& m_c;
};

// ---- CTDA ----------------------------------------------------------------------------------------------------

void ctda(CodecContext& c, BinaryReader& in) {
    // Layout (32 bytes in Skyrim SE; the game reads each member only if the field still has bytes for it):
    //   0 u8 operator/flags, 1 u8[3], 4 f32 value or GLOB (flag 0x04), 8 u16 function, 10 u16,
    //   12 param 1, 16 param 2, 20 u32 run on, 24 reference (run on 2), 28 i32 param 3
    auto data = in.take(in.remaining());
    const size_t start = c.out.size();
    c.out.insert(c.out.end(), data.begin(), data.end());
    auto u32_at = [&](size_t o) {
        uint32_t v;
        std::memcpy(&v, data.data() + o, 4);
        return v;
    };
    auto fix = [&](size_t o) {
        if (o + 4 <= data.size() && u32_at(o) != 0) {
            c.fixups.push_back({static_cast<uint32_t>(start + o - c.base), forms::k_no_type});
        }
    };
    if (data.empty()) {
        return;
    }
    auto flags = static_cast<uint8_t>(data[0]);
    if (flags & 0x04) {
        fix(4);
    }
    if (data.size() < 10) {
        return;
    }
    uint16_t function;
    std::memcpy(&function, data.data() + 8, 2);
    if (!c.ctx.conditions.functions.contains(function)) {
        throw ParseError(std::format("unknown condition function {}", function));
    }
    for (int slot = 0; slot < 2; ++slot) {
        if (c.ctx.conditions.param(function, slot, flags) == ConditionFunctions::Param::form) {
            fix(12 + 4 * size_t(slot));
        }
    }
    if (data.size() >= 24 && u32_at(20) == 2) {
        fix(24);
    }
}

// ---- VMAD ----------------------------------------------------------------------------------------------------

void vmad_object(Writer& w, BinaryReader& in, int16_t format) {
    if (format == 1) {
        w.form(in);
        w.copy<int16_t>(in);
        w.copy<uint16_t>(in);
    } else {
        w.copy<uint16_t>(in);
        w.copy<int16_t>(in);
        w.form(in);
    }
}

void vmad_scripts(Writer& w, BinaryReader& in, int16_t version, int16_t format) {
    auto count = w.copy<uint16_t>(in);
    for (uint16_t s = 0; s < count; ++s) {
        w.wstring(in, false);
        if (version >= 4) {
            w.copy<uint8_t>(in);
        }
        auto props = w.copy<uint16_t>(in);
        for (uint16_t p = 0; p < props; ++p) {
            w.wstring(in, false);
            auto type = w.copy<uint8_t>(in);
            if (version >= 4) {
                w.copy<uint8_t>(in);
            }
            switch (type) {
            case 0:
                break;
            case 1:
                vmad_object(w, in, format);
                break;
            case 2:
                w.wstring(in, true);
                break;
            case 3:
                w.copy<int32_t>(in);
                break;
            case 4:
                w.copy<float>(in);
                break;
            case 5:
                w.copy<uint8_t>(in);
                break;
            case 11:
            case 12:
            case 13:
            case 14:
            case 15: {
                auto n = w.copy<uint32_t>(in);
                for (uint32_t i = 0; i < n; ++i) {
                    if (type == 11) {
                        vmad_object(w, in, format);
                    } else if (type == 12) {
                        w.wstring(in, true);
                    } else if (type == 15) {
                        w.copy<uint8_t>(in);
                    } else {
                        w.copy<uint32_t>(in);
                    }
                }
                break;
            }
            default:
                throw ParseError(std::format("unknown VMAD property type {}", type));
            }
        }
    }
}

void vmad_fragment(Writer& w, BinaryReader& in) {
    w.copy<uint8_t>(in);
    w.wstring(in, false);
    w.wstring(in, false);
}

void vmad(CodecContext& c, BinaryReader& in) {
    Writer w(c);
    auto version = w.copy<int16_t>(in);
    auto format = w.copy<int16_t>(in);
    if (format != 1 && format != 2) {
        throw ParseError(std::format("unknown VMAD object format {}", format));
    }
    vmad_scripts(w, in, version, format);
    if (in.remaining() == 0) {
        return;
    }
    // Script fragments: only quests, topic infos, packages, perks and scenes have them.
    switch (c.record.type) {
    case forms::sig("INFO"):
    case forms::sig("PACK"):
    case forms::sig("SCEN"): {
        w.copy<int8_t>(in);
        auto flags = w.copy<uint8_t>(in);
        w.wstring(in, false);
        int n = std::popcount(static_cast<unsigned>(c.record.type == forms::sig("PACK") ? flags & 7 : flags & 3));
        for (int i = 0; i < n; ++i) {
            vmad_fragment(w, in);
        }
        if (c.record.type == forms::sig("SCEN")) {
            auto phases = w.copy<uint16_t>(in);
            for (uint16_t i = 0; i < phases; ++i) {
                w.copy<uint8_t>(in);
                w.copy<uint32_t>(in);
                vmad_fragment(w, in);
            }
        }
        break;
    }
    case forms::sig("PERK"): {
        w.copy<int8_t>(in);
        w.wstring(in, false);
        auto n = w.copy<uint16_t>(in);
        for (uint16_t i = 0; i < n; ++i) {
            w.copy<uint32_t>(in);
            vmad_fragment(w, in);
        }
        break;
    }
    case forms::sig("QUST"): {
        w.copy<int8_t>(in);
        auto n = w.copy<uint16_t>(in);
        w.wstring(in, false);
        for (uint16_t i = 0; i < n; ++i) {
            w.copy<uint32_t>(in);
            w.copy<uint32_t>(in);
            vmad_fragment(w, in);
        }
        auto aliases = w.copy<uint16_t>(in);
        for (uint16_t a = 0; a < aliases; ++a) {
            vmad_object(w, in, format);
            auto alias_version = w.copy<int16_t>(in);
            auto alias_format = w.copy<int16_t>(in);
            vmad_scripts(w, in, alias_version, alias_format);
        }
        break;
    }
    default:
        throw ParseError(std::format("{} bytes after the scripts of a {} VMAD", in.remaining(),
                                     forms::sig_name(c.record.type)));
    }
}

// ---- COED ----------------------------------------------------------------------------------------------------

void coed(CodecContext& c, BinaryReader& in) {
    // owner (NPC_/FACT), then a GLOB if the owner is an NPC or a required rank if it is a faction, then the
    // item condition. Only the load order knows the owner's type, so the second FormID is conditional.
    Writer w(c);
    const auto owner_at = static_cast<uint32_t>(c.out.size() - c.base);
    w.form(in);
    const auto value_at = static_cast<uint32_t>(c.out.size() - c.base);
    auto value = w.copy<uint32_t>(in);
    if (value != 0) {
        c.fixups.push_back({value_at, forms::k_no_type, owner_at, forms::sig("NPC_")});
    }
    w.copy<float>(in);
}

} // namespace

void run_codec(std::string_view name, CodecContext& c, BinaryReader& in) {
    if (name == "ctda") {
        ctda(c, in);
    } else if (name == "vmad") {
        vmad(c, in);
    } else if (name == "coed") {
        coed(c, in);
    } else {
        throw ParseError(std::format("unknown codec '{}'", name));
    }
}

} // namespace spud::records
