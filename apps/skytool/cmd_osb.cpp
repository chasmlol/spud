#include "commands.hpp"

#include "core/file.hpp"
#include "formats/osb.hpp"

#include <cstdio>
#include <format>
#include <string>

namespace skytool {

namespace {

std::string text_of(const spud::osb::Value& v, int depth) {
    using spud::osb::Kind;
    switch (v.kind) {
    case Kind::none: return "null";
    case Kind::integer: return std::to_string(v.integer);
    case Kind::real: return std::format("{}", v.real);
    case Kind::text: return '"' + v.text + '"';
    case Kind::object: return std::format("@{}", v.object);
    case Kind::ints: return std::format("ints[{}]", v.ints.size());
    case Kind::bytes: return std::format("bytes[{}]", v.bytes.size());
    case Kind::floats: {
        std::string s = "(";
        for (size_t i = 0; i < v.floats.size() && i < 16; ++i) {
            s += std::format("{}{}", i ? " " : "", v.floats[i]);
        }
        return s + (v.floats.size() > 16 ? " ...)" : ")");
    }
    case Kind::list: {
        std::string s = "[";
        for (size_t i = 0; i < v.list.size() && i < 12 && depth < 3; ++i) {
            s += (i ? ", " : "") + text_of(v.list[i], depth + 1);
        }
        return s + (v.list.size() > 12 ? std::format(", ... {}]", v.list.size()) : "]");
    }
    }
    return {};
}

} // namespace

int cmd_osb(Args args) {
    if (args.empty() || has_flag(args, "--help")) {
        std::puts("osb <file.osb> [class]\n  Prints a behaviour file's objects, of one class if given.");
        return args.empty() ? 1 : 0;
    }
    const auto g = spud::osb::read(spud::read_file(args[0]));
    const std::string want = args.size() > 1 ? args[1] : std::string();
    for (size_t i = 0; i < g.objects.size(); ++i) {
        const auto& o = g.objects[i];
        if (!want.empty() && o.cls != want) {
            continue;
        }
        std::printf("@%zu %s\n", i, o.cls.c_str());
        for (const auto& [name, value] : o.members) {
            std::printf("  %s = %s\n", name.c_str(), text_of(value, 0).c_str());
        }
    }
    return 0;
}

} // namespace skytool
