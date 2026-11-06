#pragma once

// The crafting stations' rules: recipes (COBJ) and tempering., "Crafting".

#include "formats/forms_typed.hpp"
#include "game/inventory.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spud::game {

class Game;

struct Recipe {
    uint32_t form = 0;
    uint32_t created = 0;
    int32_t count = 1; // NAM1
    uint32_t bench = 0; // BNAM
    std::vector<forms::ItemCount> components;
};

// A workbench (FURN WBDT).
struct Station {
    uint32_t furniture = 0;
    int bench_type = 0; // 1 create object, 2 weapons, 3 and 4 enchanting, 5 alchemy, 7 armor
    int skill = -1;     // actor value; -1 gives no experience
    std::string name;
    std::vector<uint32_t> keywords;
};

// A carried item the smithing menu lists.
struct TemperEntry {
    size_t stack = 0;
    const Recipe* recipe = nullptr;
    float health = 1, new_health = 1;           // The game now and after improving
    float improvement = 0, new_improvement = 0; // weapon or armor points they give
    bool components = false, conditions = false;
    bool enabled() const { return new_improvement - improvement >= 1.0f && components && conditions; }
};

// What a set of ingredients makes.
struct Brew {
    std::vector<Effect> effects;
    bool poison = false;
    float cost = 0;
};

struct BrewResult {
    uint32_t potion = 0; // 0: nothing came of it
    int32_t count = 1;
    std::string sound;
    std::string message; // the box: what was learned, or the failure
};

struct EnchantChoice {
    uint32_t enchantment = 0;
    float chosen = 1, max = 0; // the effect's power on the item, and the skill's limit
};

struct EnchantNumbers {
    std::vector<Effect> effects;
    float charge = 0;   // what the soul gives
    float required = 0; // per use; 0 for armor
};

class Crafting {
public:
    explicit Crafting(Game& game) : m_game(game) {}

    Station station(uint32_t furniture) const;
    // The recipes a create-object bench lists, in the game's order: by the created object's name, then its count.
    std::vector<const Recipe*> recipes(const Station& station);
    // The category flag of a created object (the menu's 31 categories).
    uint32_t category(const Item& created) const;
    bool has_components(const Recipe& recipe) const;
    int32_t player_count(uint32_t item) const; // quest items left out
    // Takes the components and gives what the recipe makes, with the bench's experience. False without components.
    bool craft(const Recipe& recipe, const Station& station);

    // What the grindstone (2) or armor workbench (7) lists, by display name.
    std::vector<TemperEntry> temper_list(const Station& station);
    float temper_health(float skill) const; // with no perks' change
    float improvement(const Item& item, float health) const;
    // Takes the components, sets the new health on one of the stack, gives the experience.
    bool temper(const TemperEntry& entry, const Station& station);

    // alchemy.cpp. `preview`: only what the player knows of the ingredients, as the menu's result shows it.
    Brew brew(const std::vector<uint32_t>& ingredients, bool preview);
    // An item for a brew: an earlier one with the same effects, or a new one.
    uint32_t potion_of(const Brew& brew);
    // Uses one of each ingredient.
    BrewResult make_potion(const std::vector<uint32_t>& ingredients, const Station& station);

    float soul_charge(const Stack& stack) const; // -1 for an empty gem
    EnchantNumbers enchant_numbers(std::vector<EnchantChoice>& chosen, bool weapon, float soul);
    // Puts the enchantment on one of the stack. The gem is the caller's to use up.
    void enchant(size_t stack, const EnchantNumbers& numbers, bool weapon, const std::string& name);
    void disenchant(size_t stack);

private:
    void load_recipes();
    const Recipe* recipe_for(const Item& item, const std::unordered_map<uint32_t, const Recipe*>& map) const;

    Game& m_game;
    std::vector<Recipe> m_recipes;
    std::vector<std::pair<uint32_t, std::vector<Effect>>> m_created_potions;
    std::unordered_map<uint32_t, uint32_t> m_category_of_keyword;
};

} // namespace spud::game
