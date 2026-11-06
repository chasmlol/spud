#include "game/leveled.hpp"

#include "core/game_random.hpp"
#include "formats/forms_typed.hpp"

#include <algorithm>
#include <cmath>

namespace spud::game {

namespace {

enum ListFlag : uint8_t { list_all_levels = 1, list_each_item = 2, list_use_all = 4, list_special_loot = 8 };

} // namespace

// The chance-none roll: the global's value when the list has one, else its own.
bool Leveled::nothing(const forms::LeveledList& list) const {
    float chance = float(list.chance_none);
    if (list.chance_none_global) {
        if (global_value) {
            chance = global_value(list.chance_none_global);
        } else if (const forms::FormEntry* e = m_db->find(list.chance_none_global)) {
            chance = forms::global_value(forms::Form(*m_db, *e)).value_or(0.0f);
        }
    }
    return GameRandom::shared().next_below(100) < uint32_t(std::max(int32_t(chance), 0));
}

// Pick one: the entry a roll gives, null for nothing.
const forms::LeveledEntry* Leveled::pick(const forms::LeveledList& list, uint16_t level) const {
    if (nothing(list)) {
        return nullptr;
    }
    int32_t top = std::min<int32_t>(player_level, level); // containers clamp to the player
    if (list.flags & list_special_loot) {
        const float p = float(player_level), z = float(level);
        const auto bound = [&](const char* pc_base, const char* pc_mult, const char* zone_base, const char* zone_mult,
                               float pc_default, float zone_default) {
            return setting(pc_mult, pc_default) * p + setting(pc_base, 0) + setting(zone_base, 0) +
                   setting(zone_mult, zone_default) * z;
        };
        const float low = bound("fSpecialLootMinPCLevelBase", "fSpecialLootMinPCLevelMult",
                                "fSpecialLootMinZoneLevelBase", "fSpecialLootMinZoneLevelMult", 0.6f, 0.4f);
        const float high = bound("fSpecialLootMaxPCLevelBase", "fSpecialLootMaxPCLevelMult",
                                 "fSpecialLootMaxZoneLevelBase", "fSpecialLootMaxZoneLevelMult", 1.0f, 1.0f);
        const auto r = float(GameRandom::shared().next_unit());
        top = int32_t(std::pow(r, setting("fSpecialLootWeighting", 4.0f)) * (high - low) + low);
    }
    // Entries run in ascending level order: all up to the level, or only the highest level reached.
    std::vector<const forms::LeveledEntry*> candidates;
    for (const forms::LeveledEntry& e : list.entries) {
        if (e.level > top) {
            continue;
        }
        if (!(list.flags & list_all_levels) && !candidates.empty() && candidates.back()->level < e.level) {
            candidates.clear();
        }
        candidates.push_back(&e);
    }
    return candidates.empty() ? nullptr : candidates[GameRandom::shared().next_below(uint32_t(candidates.size()))];
}

void Leveled::add(const forms::LeveledEntry& entry, int32_t times, uint16_t level, std::vector<Rolled>& out) const {
    const int32_t count = int32_t(entry.count) * times;
    const forms::FormEntry* e = m_db->find(entry.form);
    if (!e || count <= 0) {
        return;
    }
    if (is_list(*e)) {
        roll(entry.form, count, level, out);
        return;
    }
    for (Rolled& r : out) {
        if (r.form == entry.form) {
            r.count += count;
            return;
        }
    }
    out.push_back({entry.form, count, {}});
}

bool Leveled::is_list(const forms::FormEntry& e) const {
    static const forms::Sig lvli = forms::parse_sig("LVLI").value();
    static const forms::Sig lvsp = forms::parse_sig("LVSP").value();
    static const forms::Sig lvln = forms::parse_sig("LVLN").value();
    return e.type == lvli || e.type == lvsp || e.type == lvln;
}

void Leveled::roll(uint32_t form, int32_t count, uint16_t level, std::vector<Rolled>& out) const {
    const forms::FormEntry* e = m_db->find(form);
    if (!e || count == 0) {
        return;
    }
    if (!is_list(*e)) {
        out.push_back({form, count, {}});
        return;
    }
    const forms::Form f(*m_db, *e);
    auto list = forms::leveled_list(f);
    if (list) {
        if (const auto added = m_added.find(form); added != m_added.end()) {
            list->entries.insert(list->entries.end(), added->second.begin(), added->second.end());
        }
    }
    if (!list || list->entries.empty()) {
        return;
    }
    level = std::max<uint16_t>(level, 1);
    if (list->flags & list_all_levels) {
        uint16_t highest = 0;
        for (const forms::LeveledEntry& entry : list->entries) {
            highest = std::max(highest, entry.level);
        }
        level = std::min(level, highest);
    }
    if (list->flags & list_each_item) {
        for (int32_t i = 0; i < count; ++i) {
            if (const forms::LeveledEntry* entry = pick(*list, level)) {
                add(*entry, 1, level, out);
            }
        }
    } else if (list->flags & list_use_all) {
        if (!nothing(*list)) {
            for (const forms::LeveledEntry& entry : list->entries) {
                add(entry, count, level, out);
            }
        }
    } else if (const forms::LeveledEntry* entry = pick(*list, level)) {
        add(*entry, count, level, out);
    }
}

} // namespace spud::game
