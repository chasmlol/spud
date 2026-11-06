#pragma once

// The player as the item systems need them: inventory and what they have learned. Skills, perks and spells are the
// Character's (game/character.hpp).

#include "game/inventory.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace spud::game {

// Actor values of the skills, in the game's order (NPC_ DNAM's skill arrays hold them from one_handed on).
enum Skill : int {
    one_handed = 6, two_handed, marksman, block, smithing, heavy_armor, light_armor, pickpocket, lockpicking, sneak,
    alchemy, speechcraft, alteration, conjuration, destruction, illusion, restoration, enchanting,
};
inline constexpr int k_first_skill = one_handed;
inline constexpr int k_skill_count = 18;

// Two ingredients as the key of the known-incompatible set.
inline uint64_t pair_key(uint32_t a, uint32_t b) {
    return a < b ? uint64_t(a) << 32 | b : uint64_t(b) << 32 | a;
}

struct Player {
    std::string name;
    Inventory inventory;
    std::unordered_set<uint32_t> books_read;
    std::unordered_map<uint32_t, uint16_t> ingredient_effects; // INGR: the bits of the effects found out
    std::unordered_set<uint64_t> incompatible;                 // pair_key: brewed together, nothing in common
    std::unordered_set<uint32_t> known_enchantments;           // ENCH, disenchanted

    uint16_t known_effects(uint32_t ingredient) const {
        const auto it = ingredient_effects.find(ingredient);
        return it == ingredient_effects.end() ? 0 : it->second;
    }
};

} // namespace spud::game
