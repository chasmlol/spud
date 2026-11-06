#pragma once

// The player's settings: what Settings in the menus edits, persisted in a small
// text file (%LOCALAPPDATA%\spud\settings.ini, format in docs/UI.md). Only
// interactive sessions (or --settings <file>) read and write it: scripts,
// benches and control sessions run on the defaults so they stay reproducible.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace spud::engine {

// One stored setting: its kind and range, for validation.
struct SettingDesc {
    enum class Kind { toggle, choice, slider, key, note };
    std::string id;  // stable key, e.g. "graphics.vsync" or a Skyrim setting "fGamma:Display"
    std::string tab; // "graphics", "controls", "audio", "skyrim"
    std::string label;
    Kind kind = Kind::toggle;
    std::vector<std::string> choices;
    std::vector<std::string> choice_labels;
    float min = 0, max = 1, step = 1;
    int decimals = 0;
    std::string unit;
};

class UserSettings {
public:
    static constexpr int k_version = 1;

    UserSettings(); // defaults

    // A missing file leaves the defaults. Unknown keys are kept and written back; bad values are
    // skipped with a warning.
    void load(const std::filesystem::path& file);
    void save(const std::filesystem::path& file) const;

    // What the options screen shows (hidden keys such as new_game.* have no descriptor).
    const std::vector<SettingDesc>& descriptors() const { return m_desc; }
    void set_choices(const std::string& id, std::vector<std::string> values, std::vector<std::string> labels);

    std::string get(const std::string& id) const;
    float number(const std::string& id) const;
    bool flag(const std::string& id) const { return get(id) == "on"; }
    // Normalises and validates against the descriptor; throws Error on a bad value or unknown id.
    void set(const std::string& id, const std::string& value);
    // A hidden key (no descriptor, like new_game.*) known from the data, such as a sound category's volume; a value
    // the file already gave it stays.
    void add_hidden(const std::string& id, const std::string& default_value);
    // Back to the defaults; returns the ids that changed.
    std::vector<std::string> reset(const std::string& tab);

    bool dirty() const { return m_dirty; }
    void clear_dirty() const { m_dirty = false; }

private:
    const SettingDesc* find(const std::string& id) const;
    std::string normalise(const std::string& id, const std::string& value) const;

    std::vector<SettingDesc> m_desc;
    std::vector<std::string> m_order;              // file order of known keys
    std::map<std::string, std::string> m_defaults; // every known key
    std::map<std::string, std::string> m_values;   // known and unknown keys
    mutable bool m_dirty = false;
};

} // namespace spud::engine
