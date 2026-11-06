#pragma once

// Levelled item lists: what a list gives for a level.

#include "formats/forms_typed.hpp"
#include "game/inventory.hpp"
#include "game/settings.hpp"

#include <cstdint>
#include <functional>
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace spud::game {

struct Rolled {
    uint32_t form = 0;
    int32_t count = 0;
    ItemExtra extra;
};

class Leveled {
public:
    Leveled(const forms::Database& db, const Settings& settings) : m_db(&db), m_settings(&settings) {}

    // A global's value as the game holds it now; the plugins' value by default.
    std::function<float(uint32_t)> global_value;
    int32_t player_level = 1;

    // What `count` of the list `form` gives at `level`, into `out`. Anything that isn't a list is itself.
    void roll(uint32_t form, int32_t count, uint16_t level, std::vector<Rolled>& out) const;
    // LeveledItem.AddForm, Revert: entries scripts add, on top of the record's.
    void add_form(uint32_t list, uint32_t form, uint16_t level, uint16_t count) { m_added[list].push_back({form, level, count}); }
    void revert(uint32_t list) { m_added.erase(list); }
    void clear_added() { m_added.clear(); }
    // What scripts added, list by list in order, for the save.
    std::vector<std::pair<uint32_t, forms::LeveledEntry>> added() const {
        std::vector<std::pair<uint32_t, forms::LeveledEntry>> out;
        for (const auto& [list, entries] : m_added) {
            for (const forms::LeveledEntry& e : entries) {
                out.emplace_back(list, e);
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return out;
    }

private:
    bool nothing(const forms::LeveledList& list) const;
    const forms::LeveledEntry* pick(const forms::LeveledList& list, uint16_t level) const;
    void add(const forms::LeveledEntry& entry, int32_t times, uint16_t level, std::vector<Rolled>& out) const;
    bool is_list(const forms::FormEntry& e) const;
    float setting(const char* name, float fallback) const { return m_settings->number(name, fallback); }

    const forms::Database* m_db;
    const Settings* m_settings;
    std::unordered_map<uint32_t, std::vector<forms::LeveledEntry>> m_added;
};

} // namespace spud::game
