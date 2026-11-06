#pragma once

// What a character or container holds.

#include "game/items.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace spud::game {

// What makes one item of a form differ from another of it: the game's extra data on an inventory entry.
struct ItemExtra {
    float health = 1.0f;      // tempering
    uint32_t owner = 0;       // stolen from them (NPC_ or FACT), 0 for the player's own
    uint32_t enchantment = 0; // a player's enchantment (ENCH or a made one), 0 for the form's own
    int32_t max_charge = 0;   // ... its charge
    float charge = -1.0f;     // what's left, < 0 for full
    std::string name;         // the name the player gave it
    uint8_t soul = 0;         // 1 petty. 5 grand, what a gem added to it holds
    uint32_t poison = 0;      // the ALCH on a weapon
    int32_t poison_doses = 0;

    bool operator==(const ItemExtra&) const = default;
};

enum class Worn : uint8_t { none = 0, worn = 1, left = 2, right = 3, both = 4 }; // GetEquipState's values

struct Stack {
    uint32_t form = 0;
    int32_t count = 0;
    ItemExtra extra;
    Worn worn = Worn::none; // one of the stack is worn or held, as the game's lists show it
    bool favorite = false;
};

// Hears every change of an inventory, for what's to tell scripts.
class InventoryListener {
public:
    virtual ~InventoryListener() = default;
    virtual void inventory_changed(uint32_t owner, uint32_t form, int32_t delta) = 0;
};

class Inventory {
public:
    void set_listener(InventoryListener* listener, uint32_t owner) {
        m_listener = listener;
        m_owner = owner;
    }
    // Adds to the stack with the same extras, or a new one.
    void add(uint32_t form, int32_t count, const ItemExtra& extra = {});
    // Removes up to `count` from the stack `index`; returns how many.
    int32_t remove_at(size_t index, int32_t count);
    // Removes up to `count` of a form, the player's own first, then stolen ones, worn stacks last; returns how many.
    int32_t remove(uint32_t form, int32_t count);
    int32_t count(uint32_t form) const;
    float weight(const Items& items) const;
    int32_t gold() const { return count(k_gold); }

    std::vector<Stack>& stacks() { return m_stacks; }
    const std::vector<Stack>& stacks() const { return m_stacks; }
    bool empty() const { return m_stacks.empty(); }

private:
    std::vector<Stack> m_stacks;
    InventoryListener* m_listener = nullptr;
    uint32_t m_owner = 0;
};

} // namespace spud::game
