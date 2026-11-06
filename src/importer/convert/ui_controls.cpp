// interface/controls/pc: keyboard_<language>.txt, mouse.txt and gamepad.txt (key name, hardware value, Flash key
// code) and controlmap.txt (user events per input context) -> interface/controls.osu (docs/formats/osu.md).
//
// controlmap.txt, as its own header describes it: one user event per line, fields separated by tabs (event name,
// keyboard key, mouse button, gamepad button, three remappable flags, optional group flags); blank lines start a
// new input context, named by the comment above it; 0xff means unmapped; "a,b" lists keys, "a+b" is a chord (the
// last key triggers it) and "!0,Name" means the keys of event Name in context 0.

#include "importer/convert/ui_controls.hpp"

#include "core/log.hpp"
#include "formats/container.hpp"
#include "importer/legacy/data_source.hpp"

#include <charconv>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>

namespace spud::importer {

namespace {

std::string text_of(const std::vector<std::byte>& b) { return std::string(reinterpret_cast<const char*>(b.data()), b.size()); }

std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> out;
    std::string line;
    std::istringstream in(s);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(line);
    }
    return out;
}

// Splits on runs of tabs.
std::vector<std::string> fields_of(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == '\t') {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        const size_t end = line.find('\t', i);
        out.push_back(line.substr(i, end == std::string::npos ? std::string::npos : end - i));
        i = end == std::string::npos ? line.size() : end;
    }
    while (!out.empty() && out.back().find_first_not_of(' ') == std::string::npos) {
        out.pop_back();
    }
    return out;
}

bool blank(const std::string& line) { return line.find_first_not_of(" \t") == std::string::npos; }

std::optional<uint32_t> number(std::string_view s) {
    while (!s.empty() && s.front() == ' ') {
        s.remove_prefix(1);
    }
    while (!s.empty() && s.back() == ' ') {
        s.remove_suffix(1);
    }
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    }
    uint32_t v = 0;
    if (s.empty() || std::from_chars(s.data(), s.data() + s.size(), v, base).ec != std::errc()) {
        return std::nullopt;
    }
    return v;
}

} // namespace

osu::Controls import_controls(const legacy::DataSource& data, std::string_view language) {
    container::StringTableBuilder strings;
    osu::Controls out;
    auto key_file = [&](const std::string& file, osu::InputDevice device) {
        auto bytes = data.read(file);
        if (!bytes) {
            log::warn("ui controls: {} is missing", file);
            return;
        }
        for (const auto& line : lines_of(text_of(*bytes))) {
            if (line.starts_with("//") || blank(line)) {
                continue;
            }
            const auto f = fields_of(line);
            if (f.size() < 2) {
                continue;
            }
            const auto code = number(f[1]);
            if (!code) {
                continue;
            }
            const auto flash = f.size() > 2 ? number(f[2]) : std::nullopt;
            out.keys.push_back({strings.add(f[0]), device, 0, flash ? uint16_t(*flash) : osu::k_no_flash, *code});
        }
    };
    key_file("interface/controls/pc/keyboard_" + std::string(language) + ".txt", osu::InputDevice::keyboard);
    key_file("interface/controls/pc/mouse.txt", osu::InputDevice::mouse);
    key_file("interface/controls/pc/gamepad.txt", osu::InputDevice::gamepad);

    auto bytes = data.read("interface/controls/pc/controlmap.txt");
    if (!bytes) {
        log::warn("ui controls: interface/controls/pc/controlmap.txt is missing");
    } else {
        struct Event {
            std::string name;
            std::vector<uint32_t> codes[3];
            bool remap[3] = {};
            uint32_t flags = 0;
        };
        std::vector<std::pair<std::string, std::vector<Event>>> contexts;
        std::string comment;
        bool in_context = false;
        // Header comments come first; then every run of event lines is one context.
        bool header = true;
        for (const auto& line : lines_of(text_of(*bytes))) {
            if (line.starts_with("//")) {
                std::string c = line.substr(2);
                while (!c.empty() && (c.front() == ' ' || c.front() == '\t')) {
                    c.erase(0, 1);
                }
                while (!c.empty() && (c.back() == ' ' || c.back() == '\t')) {
                    c.pop_back();
                }
                if (!header && !c.empty()) {
                    comment = c;
                }
                continue;
            }
            if (blank(line)) {
                in_context = false;
                continue;
            }
            header = false;
            const auto f = fields_of(line);
            if (f.empty()) {
                continue;
            }
            if (!in_context) {
                contexts.emplace_back(comment, std::vector<Event>{});
                comment.clear();
                in_context = true;
            }
            Event e;
            e.name = f[0];
            for (int d = 0; d < 3; ++d) {
                if (size_t(d + 1) >= f.size()) {
                    break;
                }
                const std::string& field = f[size_t(d + 1)];
                // "!0,Name" references, or comma-separated keys with chords.
                std::vector<std::string> parts;
                std::stringstream ss(field);
                for (std::string p; std::getline(ss, p, ',');) {
                    parts.push_back(p);
                }
                for (size_t i = 0; i < parts.size(); ++i) {
                    if (parts[i] == "!0" && i + 1 < parts.size()) {
                        const std::string& ref = parts[++i];
                        if (!contexts.empty()) {
                            for (const auto& other : contexts.front().second) {
                                if (other.name == ref) {
                                    e.codes[d].insert(e.codes[d].end(), other.codes[d].begin(), other.codes[d].end());
                                    break;
                                }
                            }
                        }
                        continue;
                    }
                    const std::string last = parts[i].substr(parts[i].rfind('+') + 1);
                    if (auto v = number(last); v && *v != 0xFF) {
                        e.codes[d].push_back(*v);
                    }
                }
            }
            for (int d = 0; d < 3; ++d) {
                e.remap[d] = f.size() > size_t(4 + d) && f[size_t(4 + d)] == "1";
            }
            if (f.size() > 7) {
                e.flags = number(f[7]).value_or(0);
            }
            contexts.back().second.push_back(std::move(e));
        }
        for (const auto& [name, events] : contexts) {
            osu::ControlContext c{strings.add(name), uint32_t(out.bindings.size()), 0};
            for (const auto& e : events) {
                for (int d = 0; d < 3; ++d) {
                    for (uint32_t code : e.codes[d]) {
                        out.bindings.push_back({strings.add(e.name), osu::InputDevice(d), uint8_t(e.remap[d]), 0, code,
                                                e.flags});
                    }
                }
            }
            c.binding_count = uint32_t(out.bindings.size()) - c.first_binding;
            out.contexts.push_back(c);
        }
    }
    const auto b = strings.bytes();
    out.strings.resize(b.size());
    std::memcpy(out.strings.data(), b.data(), b.size());
    log::info("ui controls: {} keys, {} input contexts, {} bindings", out.keys.size(), out.contexts.size(),
              out.bindings.size());
    return out;
}

} // namespace spud::importer
