#pragma once

// The engine data behind the item menus (InventoryMenu, ContainerMenu, BarterMenu, GiftMenu): what the game's
// inventory system hands them, in the form the game writes it into the movies. The
// game's inventory system implements ItemSource and returns it from UiHost::ui_item_source; until it exists the
// menus replay data captured from the game.

#include "ui/player/json.hpp"

#include <string>
#include <vector>

namespace spud::ui {

// One menu's data as the engine holds it when the menu opens. Objects are kept as the members the engine sets on the
// movie's objects (ItemCard, BottomBar), in their order.
struct ItemMenuData {
    bool available = false;    // false: no data (the menu opens empty)
    std::string player_name;   // the player's ALL category in ContainerMenu, BarterMenu and GiftMenu
    std::string other_name;    // the container, the vendor or the gift partner
    bool npc_mode = false;     // ContainerMenu: Menu_mc.bNPCMode (container modes 2 and 3)
    bool other_categories = false; // ContainerMenu in mode 3: the container side gets every category
    std::vector<player::json::Value> entries; // the item list in the engine's order: text, count, equipState, ...
    std::vector<player::json::Value> cards;   // RequestItemCardInfo's object for each entry (null: none)
    // The model the 3D item shows for each entry (a weapon's WNAM, a
    // book's INAM, an armour's world model for the player's sex, the form's own otherwise), as a cache .osm path;
    // "" for none.
    std::vector<std::string> models;
    player::json::Value player_info;          // UpdatePlayerInfo's object: encumbrance, gold, health, ...
    double buy_mult = 1, sell_mult = 1;       // BarterMenu: SetBarterMultipliers
    double player_gold = 0, vendor_gold = 0;  // BarterMenu: UpdatePlayerInfo's first two values
    std::string vendor_name;                  // BarterMenu: UpdatePlayerInfo's third (the vendor's base name)
    bool giving = true, favor_points = false; // GiftMenu: SetMenuInfo
};

class ItemSource {
public:
    virtual ~ItemSource() = default;
    // The data for a menu as it opens ("InventoryMenu", "ContainerMenu", "BarterMenu", "GiftMenu").
    virtual ItemMenuData data(const std::string& menu) = 0;
    // The game side of a delegate call or user event (ItemSelect, ItemDrop, ItemTransfer, EquipItem, TakeAllItems,
    // ToggleFavorite, ShowSoulGemList, ItemCardListCallback, ChargeItem, ...) on entry `entry` (-1: none selected),
    // with the movie's numeric arguments. True when the inventory changed: the menu refills its list and the player
    // info, as the engine's inventory-update message does.
    virtual bool request(const std::string& menu, const std::string& call, const std::vector<double>& args,
                         int entry) = 0;
    // ShowSoulGemList on entry `entry`: the card with the gems as its listItems; false when there are none.
    virtual bool soul_gem_list(int entry, player::json::Value& card) {
        (void)entry;
        (void)card;
        return false;
    }
    // The favourited items for the Favorites Menu: {text, index, equipState, hotkey}.
    virtual std::vector<player::json::Value> favorites() { return {}; }
    // ... one of them equipped by that index, in a hand (0 right, 1 left, -1 the default).
    virtual void equip_favorite(int index, int hand) {
        (void)index;
        (void)hand;
    }
    // The menu closes.
    virtual void closed(const std::string& menu) { (void)menu; }
    // A taking of everything ended the menu (TakeAllItems closes it), or a failed pickpocket did.
    virtual bool should_close() { return false; }
};

// The lock the Lockpicking Menu opens on.
struct LockInfo {
    bool present = false;
    double skill = 15, xp_percent = 0;
    bool next_is_blank = false;
    int level = 0, picks = 0;
    double centre = 0, size = 0, partial = 0, break_seconds = 1, health = 100;
    double start_pick = 0;     // EP 63's arc: where the pick starts
    bool unbreakable = false;  // EP 65
    bool skeleton_key = false; // DefaultObject 5 carried: its model in place of the pick
};

// The book the Book Menu opens on (the game's data).
struct BookInfo {
    bool present = false;
    std::string text;
    bool has_reference = false, steal = false;
    std::string model;
};

// The Crafting Menu's game side: what a station lists and what crafting does.
struct CraftEntry {
    std::string text;
    double count = 0, filter_flag = 0, equip_state = 0;
    bool enabled = true;
};
struct CraftIngredient {
    std::string name;
    double required = 0, have = 0;
};
struct CraftSkill {
    std::string name;
    double level = 0, percent = 0;
};

struct CraftCategory {
    std::string text;
    double flag = 0;
    bool enabled = true;
};
// What choosing an entry at the alchemy lab or the arcane enchanter asks of the menu.
struct CraftChoice {
    std::string sound;
    std::string notification, notification_sound;
    std::string confirm;                                // a question first, answered through confirmed()
    double slider_max = 0, slider_value = 0, slider_step = 0; // the magnitude slider, when max > 0
};
struct CraftOutcome {
    std::string sound;
    std::string message; // a box to show, "" for none
};

class CraftingSource {
public:
    virtual ~CraftingSource() = default;
    // Starts on the station the player activated, or the one the show data names; false: no game behind the menu.
    virtual bool begin(const std::string& show_data) = 0;
    virtual int bench() const = 0; // WBDT BenchType
    virtual std::string furniture_name() const = 0;
    virtual bool has_skill() const = 0; // the bench gives experience
    virtual CraftSkill skill() const = 0;
    virtual player::json::Value player_info() = 0;
    // The list as it stands (it changes with every craft and choice).
    virtual std::vector<CraftEntry> entries() = 0;
    virtual player::json::Value card(int entry) = 0;
    virtual std::string model(int entry) = 0;
    virtual std::vector<CraftIngredient> ingredients(int entry) = 0;
    // Why crafting the entry is refused (a GMST name), "" when it can go ahead.
    virtual std::string refusal(int entry) = 0;
    virtual void craft(int entry) = 0;
    // The sound of a finished craft.
    virtual std::string craft_sound(int entry) = 0;
    // SmithingMenu: the entry shown as improved (-1: none), which the card and the row then preview.
    virtual void preview(int entry) = 0;

    // AlchemyMenu and EnchantConstructMenu: entries are chosen, and what they make is crafted.
    virtual std::vector<CraftCategory> categories() = 0;
    virtual CraftChoice choose(int entry) = 0;
    virtual void confirmed(bool yes) = 0;
    virtual void set_category(int index) = 0;
    virtual void select(int entry) = 0; // the highlighted entry, for the buttons
    virtual bool has_result() = 0;
    virtual bool anything_chosen() = 0;
    virtual void clear_choices() = 0;
    virtual std::vector<std::string> buttons() = 0; // the texts of buttons 0 to 3
    virtual std::string craft_question() = 0;       // a GMST, "" for none
    virtual CraftOutcome finish() = 0;
    virtual bool set_slider(double value) = 0;      // false: refused, and the value stays
    virtual void slider_closed(bool accepted) = 0;
    virtual std::string rename_text() = 0;          // "": the item can't be named now
    virtual void rename(const std::string& text) = 0;
};

} // namespace spud::ui
