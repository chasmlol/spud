#pragma once

// What scripts change on a placed reference or an actor; its record says the rest. Saved.

#include <array>
#include <cstdint>
#include <map>
#include <optional>

namespace spud::game {

// GetFactionRank with no row at all. A row of -1 is a faction the actor left, ranks from 0 up a member's.
constexpr int32_t k_not_in_faction = -2;
constexpr int32_t k_left_faction = -1;

enum RefFlag : uint32_t {
    ref_ghost = 1u << 0,
    ref_restrained = 1u << 1,
    ref_ai_off = 1u << 2,
    ref_alert = 1u << 3,
    ref_teammate = 1u << 4,
    ref_doing_favor = 1u << 5,
    ref_no_bleedout_recovery = 1u << 6,
    ref_bribed = 1u << 26,
    ref_intimidated = 1u << 27,
    ref_off_stealth_meter = 1u << 7,
    ref_no_favor = 1u << 8,
    ref_ignore_friendly_hits = 1u << 9,
    ref_no_dialogue = 1u << 10,
    ref_can_do_favor = 1u << 11,
    ref_ghost_set = 1u << 12, // ref_ghost stands over the base's
    ref_bleedout_dialogue = 1u << 13,
    ref_public = 1u << 14, // on a cell: Cell.SetPublic, over its record
    ref_not_public = 1u << 15,
    ref_talked_to_pc = 1u << 16, // form flag 0x40, GetTalkedToPC
};

struct RefChanges {
    std::map<uint32_t, int32_t> factions; // by FACT, over the record's
    bool left_all_factions = false;       // RemoveFromAllFactions: the record's rows are gone or -1
    std::optional<uint32_t> crime_faction;
    std::optional<std::array<float, 3>> angle; // radians
    std::optional<float> scale;
    std::optional<uint8_t> lock_level;
    std::optional<bool> locked;
    std::optional<uint32_t> owner; // NPC_ or FACT, 0 for nobody
    std::optional<uint32_t> outfit;
    uint32_t flags = 0;
    float alpha = 1.0f;
    int32_t open_state = 0; // 0 the record's: GetOpenState's numbers otherwise
};

} // namespace spud::game
