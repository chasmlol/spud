#pragma once

// The menu classes by group, each in its own file. create_menu (menus.cpp) asks each group for a menu it doesn't
// make itself; a group returns null for names it doesn't know.

#include "ui/menu.hpp"
#include "ui/player/json.hpp"
#include "ui/ui.hpp"

#include <memory>
#include <string>
#include <vector>

namespace spud::ui {

// Loading Menu and Mist Menu (the loading screens and the 3D backdrop they share with the main menu):
// menus_loading.cpp.
std::unique_ptr<Menu> create_loading_menu(Ui& ui, const std::string& name);
// Journal Menu (Quests, General Stats and System tabs, the bottom bar): menus_journal.cpp.
std::unique_ptr<Menu> create_journal_menu(Ui& ui, const std::string& name);
// Inventory, Container, Barter and Gift menus (the shared item list and item card): menus_items.cpp.
std::unique_ptr<Menu> create_items_menu(Ui& ui, const std::string& name);
// The 3D item manager's state of an item menu or the Magic Menu as JSON ("null" for another menu): ui item3d.
std::string items_item3d_json(Menu& m);
std::string magic_item3d_json(Menu& m);
// The 3D item's model for each entry of a captured item list, from the capture item3d-models (menus_items.cpp).
std::vector<std::string> captured_item_models(Ui& ui, const std::string& menu,
                                              const std::vector<player::json::Value>& entries);
// The item menu's 3D model turned by RotateModel(a, b) (ui item3d rotate); false for another menu.
bool items_item3d_rotate(Menu& m, float a, float b);
// Magic, Favorites and Tween menus: menus_magic.cpp.
std::unique_ptr<Menu> create_magic_menu(Ui& ui, const std::string& name);
// The update message (type 0, no data) a menu's ShowTweenMenu posts to "TweenMenu": an open Tween Menu plays its
// open animation again; a closed one ignores it. menus_magic.cpp.
void tween_menu_update(Ui& ui);
// HUD Menu, Sleep/Wait, Lockpicking and Training menus: menus_hud.cpp.
std::unique_ptr<Menu> create_hud_menu(Ui& ui, const std::string& name);
// A HUD mode on or off ("InventoryMode", "SleepWaitMode", "BarterMode",...: the game /
// The game), as the game's menus set them when they open and close. No-op without a HUD.
void hud_set_mode(Ui& ui, const std::string& mode, bool on);
// The crosshair's prompt, and a notification with its sound.
void hud_crosshair(Ui& ui, const CrosshairTarget& target);
void hud_notify(Ui& ui, const std::string& text, const std::string& sound = {});
void hud_stealth(Ui& ui, bool sneaking, double level);
void hud_skill_increase(Ui& ui, const std::string& skill, int level, int player_level, double xp);
// A HUD mode a menu holds while it is up, as the game's menus set theirs (the callers of the game and
// The game): "InventoryMode" for the Inventory, Container, Barter, Gift, Magic
// and Crafting menus, "BookMode", "TweenMode", "WorldMapMode", "JournalMode" and "DialogueMode". Each menu cites
// its own call sites. The HUD movie's ShowElements then hides what the mode does not list.
class HudModeHold {
public:
    HudModeHold(Ui& ui, std::string mode, bool on = true) : m_ui(ui), m_mode(std::move(mode)) { set(on); }
    ~HudModeHold() { set(false); }
    HudModeHold(const HudModeHold&) = delete;
    HudModeHold& operator=(const HudModeHold&) = delete;
    // Turns the mode off before the menu goes (BarterMenu and MapMenu on hide, DialogueMenu on CloseMenu), or off
    // and on again (DialogueMenu under a pausing menu).
    void set(bool on) {
        if (on != m_on) {
            m_on = on;
            hud_set_mode(m_ui, m_mode, on);
        }
    }
    void release() { set(false); }

private:
    Ui& m_ui;
    std::string m_mode;
    bool m_on = false;
};

// The UI blur count a menu holds while it is up (DecrementBlurCount
//): the Inventory, Container, Barter, Gift, Magic, Crafting, Journal, Book, Tutorial, Sleep/Wait,
// Lockpicking and Tween menus. Above 0 the world behind them is under DefaultObject 170.
class BlurHold {
public:
    explicit BlurHold(Ui& ui, bool on = true) : m_ui(ui) { set(on); }
    ~BlurHold() { set(false); }
    BlurHold(const BlurHold&) = delete;
    BlurHold& operator=(const BlurHold&) = delete;
    void set(bool on) {
        if (on != m_on) {
            m_on = on;
            m_ui.add_blur(on ? 1 : -1);
        }
    }

private:
    Ui& m_ui;
    bool m_on = false;
};
// Stats (skills and perks), Level Up, Book and Dialogue menus: menus_stats.cpp.
std::unique_ptr<Menu> create_stats_menu(Ui& ui, const std::string& name);
// The Stats Menu's starfield (slot 6) and the Book Menu's book (slot 3) as JSON ("null" for another menu): ui item3d.
std::string stats_starfield_json(Menu& m);
std::string book_book3d_json(Menu& m);
// Map, Crafting, Race Sex, Tutorial and the remaining menus: menus_world.cpp.
std::unique_ptr<Menu> create_world_menu(Ui& ui, const std::string& name);
// The Crafting Menu's 3D item manager state ("null" for another menu): ui item3d.
std::string crafting_item3d_json(Menu& m);

} // namespace spud::ui
