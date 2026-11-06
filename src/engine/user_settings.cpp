#include "engine/user_settings.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"

#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>

namespace spud::engine {

namespace {

using Kind = SettingDesc::Kind;

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return {};
    }
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

SettingDesc toggle(std::string id, std::string tab, std::string label) {
    SettingDesc d;
    d.id = std::move(id);
    d.tab = std::move(tab);
    d.label = std::move(label);
    d.kind = Kind::toggle;
    return d;
}

SettingDesc slider(std::string id, std::string tab, std::string label, float min, float max, float step,
                       int decimals, std::string unit) {
    SettingDesc d;
    d.id = std::move(id);
    d.tab = std::move(tab);
    d.label = std::move(label);
    d.kind = Kind::slider;
    d.min = min;
    d.max = max;
    d.step = step;
    d.decimals = decimals;
    d.unit = std::move(unit);
    return d;
}

SettingDesc key(std::string id, std::string label) {
    SettingDesc d;
    d.id = std::move(id);
    d.tab = "controls";
    d.label = std::move(label);
    d.kind = Kind::key;
    return d;
}

SettingDesc note(std::string tab, std::string text) {
    SettingDesc d;
    d.tab = std::move(tab);
    d.label = std::move(text);
    d.kind = Kind::note;
    return d;
}

} // namespace

UserSettings::UserSettings() {
    SettingDesc mode;
    mode.id = "graphics.window_mode";
    mode.tab = "graphics";
    mode.label = "Window mode";
    mode.kind = Kind::choice;
    mode.choices = {"windowed", "borderless", "fullscreen"};
    mode.choice_labels = {"Windowed", "Borderless", "Fullscreen"};
    SettingDesc res;
    res.id = "graphics.resolution";
    res.tab = "graphics";
    res.label = "Resolution";
    res.kind = Kind::choice;
    res.choices = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"};

    m_desc = {
        mode,
        res,
        toggle("graphics.vsync", "graphics", "VSync"),
        slider("graphics.fov", "graphics", "Field of view", 40, 110, 1, 0, "\xC2\xB0"),
        slider("graphics.draw_distance", "graphics", "Draw distance", 4, 32, 1, 0, " cells"),
        key("controls.forward", "Forward"),
        key("controls.back", "Back"),
        key("controls.left", "Strafe left"),
        key("controls.right", "Strafe right"),
        key("controls.up", "Up"),
        key("controls.down", "Down"),
        key("controls.fast", "Fast"),
        key("controls.look", "Toggle mouse look"),
        slider("controls.mouse_sensitivity", "controls", "Mouse sensitivity", 0.1f, 3.0f, 0.05f, 2, "x"),
        toggle("controls.invert_y", "controls", "Invert Y axis"),
        // The other Audio menu levels are the sound categories the data puts on the menu, added with their
        // records' defaults when the audio system has loaded (App::apply_audio_settings).
        slider("audio.master", "audio", "Master volume", 0, 100, 5, 0, "%"),
        toggle("audio.hrtf", "audio", "Headphone spatial audio (HRTF)"),
    };
    // The settings Skyrim's Journal > System > Settings pages edit, by the game's names and ranges
    // Stored as the game stores them.
    struct Skyrim {
        const char* id;
        float min, max, def;
        int decimals;
    };
    static constexpr Skyrim k_skyrim[] = {
        {"fMouseHeadingSensitivity:Controls", 0.01f, 0.05f, 0.0125f, 5},
        {"fGamepadHeadingSensitivity:Controls", 0.4445f, 1.5556f, 1.0f, 5},
        {"bGamePadRumble:Controls", 0, 1, 1, 0},
        {"bGamepadEnable:Controls", 0, 1, 1, 0},
        {"bAlwaysRunByDefault:Controls", 0, 1, 1, 0},
        {"iDifficulty:GamePlay", 0, 5, 2, 0},
        {"bShowFloatingQuestMarkers:GamePlay", 0, 1, 1, 0},
        {"bSaveOnRest:SaveGame", 0, 1, 1, 0},
        {"bSaveOnWait:SaveGame", 0, 1, 1, 0},
        {"bSaveOnTravel:SaveGame", 0, 1, 1, 0},
        {"bDisplayMissingContentDialogue:SaveGame", 0, 1, 1, 0},
        {"bSaveOnPause:SaveGame", 0, 1, 1, 0},
        {"fAutosaveEveryXMins:SaveGame", 5, 60, 15, 0},
        {"fGamma:Display", 0.6f, 1.4f, 1.0f, 4},
        {"fHUDOpacity:Interface", 0, 1, 1.0f, 4},
        {"fLODFadeOutMultActors:LOD", 5, 15, 6, 4},
        {"fLODFadeOutMultItems:LOD", 1.5f, 10, 3, 4},
        {"fLODFadeOutMultObjects:LOD", 5, 30, 5, 4},
        {"fGrassStartFadeDistance:Grass", 400, 7000, 7000, 1},
        {"bCrosshairEnabled:Main", 0, 1, 1, 0},
        {"subtitles_24", 0, 1, 0, 0},
        {"subtitles_25", 0, 1, 0, 0},
        {"fDynamicDOFBlurMultiplier:Display", 0, 1, 0.8f, 4},
    };
    for (const auto& k : k_skyrim) {
        SettingDesc d = slider(k.id, "skyrim", k.id, k.min, k.max, 0.00001f, k.decimals, "");
        m_desc.push_back(d);
        m_order.emplace_back(k.id);
        m_defaults[k.id] = std::format("{:.{}f}", k.def, k.decimals);
        m_values[k.id] = m_defaults[k.id];
    }
    // The defaults reproduce the engine's behaviour from before there were settings.
    const std::pair<const char*, const char*> defaults[] = {
        {"graphics.window_mode", "windowed"},
        {"graphics.resolution", "1920x1080"},
        {"graphics.vsync", "on"},
        {"graphics.fov", "65"},
        {"graphics.draw_distance", "12"},
        {"controls.forward", "W"},
        {"controls.back", "S"},
        {"controls.left", "A"},
        {"controls.right", "D"},
        {"controls.up", "Space"},
        {"controls.down", "Left Ctrl"},
        {"controls.fast", "Left Shift"},
        {"controls.look", "Tab"},
        {"controls.mouse_sensitivity", "1.00"},
        {"controls.invert_y", "off"},
        {"audio.master", "100"},
        {"audio.hrtf", "off"},
        // Where New Game starts: Riverwood, Skyrim units like getpos (z "+N" = N above the terrain).
        {"new_game.world", "tamriel"},
        {"new_game.position", "18432 -47104 +250"},
        {"new_game.heading", "45"},
        {"new_game.pitch", "5"},
    };
    for (const auto& [k, v] : defaults) {
        m_order.emplace_back(k);
        m_defaults[k] = v;
        m_values[k] = v;
    }
}

const SettingDesc* UserSettings::find(const std::string& id) const {
    for (const auto& d : m_desc) {
        if (d.id == id) {
            return &d;
        }
    }
    return nullptr;
}

void UserSettings::set_choices(const std::string& id, std::vector<std::string> values,
                               std::vector<std::string> labels) {
    for (auto& d : m_desc) {
        if (d.id == id) {
            d.choices = std::move(values);
            d.choice_labels = std::move(labels);
        }
    }
}

std::string UserSettings::get(const std::string& id) const {
    auto it = m_values.find(id);
    if (it == m_values.end()) {
        throw Error(std::format("unknown setting '{}'", id));
    }
    return it->second;
}

float UserSettings::number(const std::string& id) const {
    const std::string v = get(id);
    float out = 0;
    std::from_chars(v.data(), v.data() + v.size(), out);
    return out;
}

std::string UserSettings::normalise(const std::string& id, const std::string& raw) const {
    const std::string value = trim(raw);
    const SettingDesc* d = find(id);
    if (!d) {
        if (!m_defaults.contains(id)) {
            throw Error(std::format("unknown setting '{}'", id));
        }
        return value; // hidden keys (new_game.*) are checked where they are used
    }
    switch (d->kind) {
    case Kind::toggle: {
        const std::string s = path::to_lower(value);
        if (s == "on" || s == "1" || s == "true" || s == "yes") {
            return "on";
        }
        if (s == "off" || s == "0" || s == "false" || s == "no") {
            return "off";
        }
        throw Error(std::format("{} needs on/off, got '{}'", id, value));
    }
    case Kind::choice: {
        const std::string s = path::to_lower(value);
        for (const auto& c : d->choices) {
            if (path::to_lower(c) == s) {
                return c;
            }
        }
        // Any size is a valid resolution, not only the ones the display lists.
        unsigned w = 0, h = 0;
        if (id == "graphics.resolution" && std::sscanf(s.c_str(), "%ux%u", &w, &h) == 2 && w >= 320 && h >= 200 &&
            w <= 16384 && h <= 16384) {
            return std::format("{}x{}", w, h);
        }
        throw Error(std::format("{} must be one of the listed choices, got '{}'", id, value));
    }
    case Kind::slider: {
        float v = 0;
        auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
        if (ec != std::errc() || p != value.data() + value.size() || !std::isfinite(v)) {
            throw Error(std::format("{} needs a number, got '{}'", id, value));
        }
        v = std::clamp(v, d->min, d->max);
        const int decimals = std::max(d->decimals, d->step < 1.0f ? 2 : 0);
        return std::format("{:.{}f}", v, decimals);
    }
    case Kind::key: {
        const SDL_Scancode sc = SDL_GetScancodeFromName(value.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) {
            throw Error(std::format("{}: unknown key '{}'", id, value));
        }
        return SDL_GetScancodeName(sc);
    }
    case Kind::note:
        break;
    }
    return value;
}

void UserSettings::add_hidden(const std::string& id, const std::string& default_value) {
    if (m_defaults.emplace(id, default_value).second) {
        m_order.push_back(id);
        m_values.emplace(id, default_value);
    }
}

void UserSettings::set(const std::string& id, const std::string& value) {
    const std::string v = normalise(id, value);
    auto& slot = m_values[id];
    if (slot != v) {
        slot = v;
        m_dirty = true;
    }
}

std::vector<std::string> UserSettings::reset(const std::string& tab) {
    std::vector<std::string> changed;
    for (const auto& [k, v] : m_defaults) {
        if (k.starts_with(tab + ".") && m_values[k] != v) {
            m_values[k] = v;
            changed.push_back(k);
            m_dirty = true;
        }
    }
    return changed;
}

void UserSettings::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) {
        return;
    }
    std::string section, line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            log::warn("settings: {}:{}: expected 'key = value'", file.string(), n);
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (section.empty() && key == "version") {
            if (value != std::to_string(k_version)) {
                log::warn("settings: {} is version {}, this build writes {}; reading what it can", file.string(),
                          value, k_version);
            }
            continue;
        }
        const std::string id = section.empty() ? key : section + "." + key;
        if (!m_defaults.contains(id)) {
            m_values[id] = value; // a newer build's key: keep it
            continue;
        }
        try {
            m_values[id] = normalise(id, value);
        } catch (const std::exception& e) {
            log::warn("settings: {}:{}: {} (keeping {})", file.string(), n, e.what(), m_values[id]);
        }
    }
    m_dirty = false;
}

void UserSettings::save(const std::filesystem::path& file) const {
    std::string out = "# spud settings (docs/UI.md). Written by the engine when Settings closes and on exit;\n"
                      "# edit it while spud is not running.\n";
    out += std::format("version = {}\n", k_version);
    std::string section;
    auto write_key = [&](const std::string& id, const std::string& value) {
        const size_t dot = id.find('.');
        const std::string s = dot == std::string::npos ? std::string() : id.substr(0, dot);
        if (s != section) {
            section = s;
            out += std::format("\n[{}]\n", section);
        }
        out += std::format("{} = {}\n", dot == std::string::npos ? id : id.substr(dot + 1), value);
    };
    for (const auto& id : m_order) {
        write_key(id, m_values.at(id));
    }
    for (const auto& [id, value] : m_values) {
        if (!m_defaults.contains(id)) {
            write_key(id, value);
        }
    }
    write_file(file, std::as_bytes(std::span(out.data(), out.size())));
    m_dirty = false;
}

} // namespace spud::engine
