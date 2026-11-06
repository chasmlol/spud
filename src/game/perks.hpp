#pragma once

// PERK records and the skills' perk trees as the game loads them. Nothing here belongs to
// an actor: Character holds who owns what.

#include "game/conditions.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::game {

inline constexpr int k_entry_point_count = 92;

enum class EntryKind : uint8_t { value, leveled_list, activate, none, spell, graph_bool, text };

// g_EntryPointNames: a tab per argument, the owner first.
struct EntryPointInfo {
    uint8_t arguments;
    EntryKind kind;
};
const EntryPointInfo& entry_point_info(int entry_point);

// The entry points the code names; the rest go by number.
enum EntryPoint : int {
    ep_weapon_damage = 0, ep_book_skill_points = 5, ep_recovered_health = 6, ep_buy_prices = 8, ep_max_carry_weight = 10,
    ep_activate = 14, ep_ignore_broken_lock = 16, ep_skill_use = 22, ep_armor_weight = 32, ep_attack_damage = 35,
    ep_spell_magnitude = 29, ep_spell_duration = 30, ep_incoming_spell_magnitude = 41, ep_incoming_spell_duration = 42,
    ep_soul_gem_recharge = 49, ep_pickpocket_chance = 56, ep_lockpick_sweet_spot = 59, ep_sell_prices = 60,
    ep_alchemy_effectiveness = 66, ep_initial_ingredient_effects = 72, ep_filter_activation = 74,
    ep_tempering_health = 76, ep_enchantment_power = 77, ep_enchantments_allowed = 80, ep_activate_label = 81,
    ep_armor_rating = 85, ep_ingredients_harvested = 87, ep_potions_created = 89,
};

struct PerkEntry {
    enum Type : uint8_t { quest, ability, entry_point };
    Type type = entry_point;
    uint8_t rank = 0, priority = 0;
    uint32_t perk = 0;
    uint32_t form = 0;  // quest, ability's SPEL, the function data's LVLI or SPEL
    uint16_t stage = 0;
    uint8_t point = 0, function = 0, tab_count = 0;
    uint8_t data_type = 0;     // EPFT: 0 none
    float value1 = 0, value2 = 0;
    std::string text;          // EPFD string or text, EPF2 button
    uint16_t script_flags = 0; // EPF3: 1 run immediately, 2 replace default
    int32_t fragment = -1;
    std::vector<std::vector<ConditionItem>> tabs;
};

struct Perk {
    uint32_t form = 0;
    std::string name, description;
    int8_t level = 0;
    uint8_t ranks = 1;
    bool playable = true, hidden = false;
    uint32_t next = 0;                  // NNAM: the next rank's record
    std::vector<ConditionItem> conditions; // the requirement
    std::vector<PerkEntry> entries;     // rank up, priority down, type up
};

// One AVIF's perk tree.
struct PerkTreeNode {
    uint32_t index = 0;    // INAM
    uint32_t perk = 0;     // PNAM: the first rank, 0 for the root
    float x = 0, y = 0;    // XNAM, YNAM
    float h = 0, v = 0;    // HNAM, VNAM
    uint32_t skill = 0;    // SNAM: the AVIF the card names
    std::vector<uint32_t> children;
};
struct PerkTree {
    int actor_value = 0;
    int max_grid_x = 0;
    std::vector<PerkTreeNode> nodes;
};

struct SkillData {
    float use_mult = 1, use_offset = 0, improve_mult = 1, improve_offset = 20; // AVSK, or the type's defaults
    std::string name, description;
};

class Perks {
public:
    explicit Perks(const forms::Database& db) : m_db(&db) {}

    const Perk* find(uint32_t form) const;
    // The AVIF of an actor value: AVIF forms run in the value's order only for the skills, so by EditorID elsewhere.
    uint32_t avif(int actor_value) const;
    const PerkTree* tree(int actor_value) const;
    const SkillData& skill(int actor_value) const;

private:
    const forms::Database* m_db;
    mutable std::unordered_map<uint32_t, std::unique_ptr<Perk>> m_perks;
    mutable std::unordered_map<int, std::unique_ptr<PerkTree>> m_trees;
    mutable std::unordered_map<int, SkillData> m_skills;
};

} // namespace spud::game
