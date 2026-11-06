// Inventory, Container, Barter and Gift menus (the shared item list and item card), implemented from
// (addresses there). The four menus share the movie classes ItemMenu, InventoryLists, ItemCard
// and BottomBar; the engine side here does what the game's menu classes do around them. The inventory data comes
// from the game's inventory system through ItemSource (ui/items.hpp); without one, from data captured in the game.

#include "ui/item3d.hpp"
#include "ui/items.hpp"
#include "ui/menus_internal.hpp"

#include "ui/ui.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <unordered_map>

namespace spud::ui {

namespace {

using Value = player::Value;
namespace json = player::json;

const json::Value* member(const json::Value& o, std::string_view name) {
    for (const auto& [k, v] : o.members) {
        if (k == name) {
            return &v;
        }
    }
    return nullptr;
}

double number_of(const json::Value& o, std::string_view name, double fallback = 0) {
    const json::Value* v = member(o, name);
    return v && v->kind == json::Value::number ? v->n : fallback;
}

bool bool_of(const json::Value& o, std::string_view name, bool fallback = false) {
    const json::Value* v = member(o, name);
    return v && v->kind == json::Value::boolean ? v->b : fallback;
}

std::string string_of(const json::Value& o, std::string_view name) {
    const json::Value* v = member(o, name);
    return v && v->kind == json::Value::string ? v->s : std::string();
}

// printf-style GMST text with integer arguments (%d, %i), as the engine's formatter fills sNotEnoughVendorGold.
std::string format_gmst(const std::string& text, std::initializer_list<long long> args) {
    std::string out;
    auto it = args.begin();
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 1 < text.size()) {
            const char c = text[i + 1];
            if ((c == 'd' || c == 'i') && it != args.end()) {
                out += std::to_string(*it++);
                ++i;
                continue;
            }
            if (c == '%') {
                out += '%';
                ++i;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

// ---- data captured from the game ----------------------------------------------------------------------------------

// The capture builds from the game's run (scenario capture-items.osc): one per menu,
// items-inventory / items-container / items-barter / items-gift, {"menu", "frame", "paths": {...}}.
class CapturedItems final : public ItemSource {
public:
    explicit CapturedItems(Ui& ui) : m_ui(ui) {}

    ItemMenuData data(const std::string& menu) override {
        ItemMenuData d;
        const json::Value* c = m_ui.capture(capture_name(menu));
        const json::Value* paths = c ? member(*c, "paths") : nullptr;
        if (!paths) {
            if (m_logged.insert("data:" + menu).second) {
                log::info("ui {}: no inventory system and no capture {}:"
                          " the menu opens empty",
                          menu, capture_name(menu));
            }
            return d;
        }
        d.available = true;
        if (const json::Value* names = member(*paths, "names")) {
            d.player_name = string_of(*names, "player");
            d.other_name = string_of(*names, "other");
        }
        if (const json::Value* e = member(*paths, "entries"); e && e->kind == json::Value::array) {
            d.entries = e->items;
        }
        if (const json::Value* cards = member(*paths, "cards"); cards && cards->kind == json::Value::array) {
            d.cards = cards->items;
        }
        d.cards.resize(d.entries.size());
        // The 3D item's model per entry.
        d.models = captured_item_models(m_ui, menu, d.entries);
        if (const json::Value* p = member(*paths, "player")) {
            d.player_info = *p;
        }
        if (const json::Value* b = member(*paths, "barter")) {
            d.buy_mult = number_of(*b, "buyMult", 1);
            d.sell_mult = number_of(*b, "sellMult", 1);
            d.player_gold = number_of(*b, "playerGold");
            d.vendor_gold = number_of(*b, "vendorGold");
            d.vendor_name = string_of(*b, "vendorName");
        }
        if (const json::Value* g = member(*paths, "gift")) {
            d.giving = bool_of(*g, "giving", true);
            d.favor_points = bool_of(*g, "favorPoints");
        }
        if (const json::Value* k = member(*paths, "container")) {
            d.npc_mode = bool_of(*k, "npcMode");
            d.other_categories = bool_of(*k, "otherCategories");
        }
        return d;
    }

    bool request(const std::string& menu, const std::string& call, const std::vector<double>&, int) override {
        // Equipping, dropping, moving and trading items is the inventory system's work (InventoryChanges,
        // ActorEquipManager, the game, the barter transaction); replayed data stays as captured.
        if (m_logged.insert(menu + ":" + call).second) {
            log::info("ui {}: {} needs the game's inventory system; the captured inventory is left as it is", menu,
                      call);
        }
        return false;
    }

private:
    static std::string capture_name(const std::string& menu) {
        if (menu == "ContainerMenu") return "items-container";
        if (menu == "BarterMenu") return "items-barter";
        if (menu == "GiftMenu") return "items-gift";
        return "items-inventory";
    }

    Ui& m_ui;
    std::set<std::string> m_logged;
};

ItemSource& item_source(Ui& ui) {
    if (ItemSource* s = ui.host().ui_item_source()) {
        return *s;
    }
    static Ui* s_ui = nullptr;
    static std::unique_ptr<CapturedItems> s_captured;
    if (!s_captured || s_ui != &ui) {
        s_captured = std::make_unique<CapturedItems>(ui);
        s_ui = &ui;
    }
    return *s_captured;
}

// InventoryMenu's saved list positions (SaveIndices into globals /): the category
// list's scroll position and each category's item index, sent back with RestoreIndices when it opens again.
struct SavedIndices {
    int scroll = 0;
    std::vector<int> items;
};
SavedIndices g_inventory_indices;

// ---- the shared engine side of the four menus ---------------------------------------------------------------------

enum class Kind { inventory, container, barter, gift };

// Control-map contexts (controlmap.txt order).
constexpr int k_menu_mode = 1, k_item_menus = 3, k_inventory = 4, k_cursor = 9;

class ItemMenu : public Menu {
public:
    ItemMenu(Ui& ui, MenuDesc d, Kind kind) : Menu(ui, std::move(d)), m_kind(kind), m_item3d(ui) {
        const UiHost& host = ui.host();
        m_item3d.set_mouse_settings(host.ui_game_setting("fMouseHeadingSensitivity:Controls", 0.0125f),
                                    host.ui_game_setting("fMouseHeadingXScale:Controls", 0.02f),
                                    host.ui_game_setting("fMouseHeadingYScale:Controls", 0.85f),
                                    host.ui_game_setting("bInvertYValues:Controls", 0.0f) != 0.0f);
    }

protected:
    // Each of the four constructors puts the HUD into "InventoryMode" (InventoryMenu::InventoryMenu,
    // ContainerMenu, BarterMenu, GiftMenu); their destructors end it, BarterMenu
    // already on hide.
    HudModeHold m_hud_mode{m_ui, "InventoryMode"};
    // So do they the blur count (,).
    BlurHold m_blur{m_ui};

public:

    bool on_user_event(const std::string& event) override {
        // ProcessMessage, message 7 ( and the others' slot 4). "Item Zoom", or Cancel while the model is
        // zoomed (direction 0, progress 1), toggles the 3D model's zoom
        // and, when it toggled, the movie's ToggleMenuFade.
        if (m_closing) {
            return false;
        }
        if (event == "Item Zoom" ||
            (event == "Cancel" && m_item3d.zoom_direction() == 0.0f && m_item3d.zoom_progress() == 1.0f)) {
            if (m_item3d.toggle_zoom()) {
                send("ToggleMenuFade");
            }
            return true;
        }
        if (m_kind == Kind::inventory || m_kind == Kind::container) {
            if (event == "LeftEquip" || event == "RightEquip") {
                send("AttemptEquip", {Value::number(event == "LeftEquip" ? 1 : 0)});
                return true;
            }
        }
        if (m_kind == Kind::inventory) {
            if (event == "XButton") {
                send("DropItem");
            } else if (event == "YButton") {
                game("ToggleFavorite", {}, "UIFavorite");
            } else if (event == "ChargeItem") {
                send("AttemptChargeItem");
            } else if (event == "Quick Inventory") {
                // The Inventory key again: a Tab key-down event to the movie, which closes as on Tab.
                player()->key(9, 0, true);
            } else {
                return false;
            }
            return true;
        }
        if (m_kind == Kind::container) {
            if (event == "XButton") {
                send("XButtonPress");
                return true;
            }
            if (event == "YButton" && !player()->vm.to_boolean(invoke("Menu_mc.isViewingContainer"))) {
                game("ToggleFavorite", {}, "UIFavorite");
                return true;
            }
        }
        return false;
    }

protected:
    void register_handlers() override {
        handle("PlaySound", [this](Call& c) {
            if (c.args.size() == 1) {
                m_ui.host().ui_play_sound(*player()->vm.to_string(c.arg(0)));
            }
        });
        // The 3D model of the selected item (the game and the others'): true and an
        // entry selected, Inventory3DManager::UpdateItem3D with it; false, Clear3D.
        handle("UpdateItem3D", [this](Call& c) {
            const int i = selected();
            if (player()->vm.to_boolean(c.arg(0))) {
                if (i >= 0 && size_t(i) < m_data.models.size()) {
                    m_item3d.show(m_data.models[size_t(i)]);
                }
            } else {
                m_item3d.clear();
            }
        });
        handle("RequestItemCardInfo", [this](Call& c) {
            const int i = selected();
            if (i >= 0 && size_t(i) < m_data.cards.size() && m_data.cards[size_t(i)].kind == json::Value::object) {
                c.respond({player::json_to_value(player()->vm, m_data.cards[size_t(i)])});
            }
        });
        handle("CloseMenu", [this](Call&) { m_ui.close(name()); });
        handle("StartMouseRotation", [this](Call&) { m_rotating = true; });
        handle("StopMouseRotation", [this](Call&) { m_rotating = false; });
        // (the mouse wheel over the model): -1 while zoomed in and at rest zooms out, +1 while not zoomed
        // zooms in; when the manager toggled, the movie's ToggleMenuFade.
        handle("ZoomItemModel", [this](Call& c) {
            const double delta = player()->vm.to_number(c.arg(0));
            const bool at_rest = m_item3d.zoom_direction() == 0.0f;
            if ((delta == -1 && at_rest && m_item3d.zoom_progress() == 1.0f) ||
                (delta == 1 && at_rest && m_item3d.zoom_progress() == 0.0f)) {
                if (m_item3d.toggle_zoom()) {
                    send("ToggleMenuFade");
                }
            }
        });
        handle("QuantitySliderOpen", [this](Call& c) {
            // (and,): the slider takes the keys. Item Menus (and for
            // InventoryMenu, Inventory) leave the control map while it is open and Cursor joins with PC controls and
            // no gamepad; closing puts them back.
            const bool opening = player()->vm.to_boolean(c.arg(0));
            const bool cursor = m_pc_controls && !m_ui.gamepad_active();
            auto drop = [this](int ctx) {
                if (auto it = std::find(m_contexts.rbegin(), m_contexts.rend(), ctx); it != m_contexts.rend()) {
                    m_contexts.erase(std::next(it).base());
                }
            };
            if (opening) {
                if (m_kind == Kind::inventory) {
                    drop(k_inventory);
                }
                drop(k_item_menus);
                if (cursor) {
                    m_contexts.push_back(k_cursor);
                }
            } else {
                if (cursor) {
                    drop(k_cursor);
                }
                m_contexts.push_back(k_item_menus);
                if (m_kind == Kind::inventory) {
                    m_contexts.push_back(k_inventory);
                }
            }
        });
        if (m_kind == Kind::inventory || m_kind == Kind::container) {
            handle("CheckForMouseEquip", [this](Call& c) {
                // the mouse button's user event decides the hand: RightEquip 0, LeftEquip 1 (and no
                // check over the list), answered to the movie's AttemptEquip.
                const std::string event = m_ui.mouse_user_event(int(player()->vm.to_number(c.arg(0))));
                if (event == "RightEquip" || event == "LeftEquip") {
                    c.respond({Value::number(event == "LeftEquip" ? 1 : 0), Value::boolean(false)});
                }
            });
        }
        // The movies also call myLog (CrossPlatformButtons) and, from InventoryMenu, DisabledItemSelect; the game
        // registers neither for these menus, so the game drops them.
        handle("myLog", [](Call&) {});
        if (m_kind == Kind::inventory || m_kind == Kind::barter) {
            handle("DisabledItemSelect", [](Call&) {});
        }
        register_menu_handlers();
    }

    virtual void register_menu_handlers() = 0;

    void advance(double dt, double engine_ms) override {
        Menu::advance(dt, engine_ms);
        // Inventory3DManager::Render (from the menu's Display): a finished load attached, the zoom.
        m_dt = static_cast<float>(dt);
        m_item3d.update(m_dt);
    }
    // While the model zooms the movie's input (message 6) is dropped.
    bool on_key(uint32_t, bool) override { return m_item3d.zoom_direction() != 0.0f; }
    // The mouse turns the model while the movie's StartMouseRotation holds (g_ItemMenuMouseRotating).
    void mouse_delta(float dx, float dy) override {
        if (!m_rotating) {
            return;
        }
        m_item3d.mouse_move(dx, dy, m_dt);
        if (m_kind == Kind::inventory) {
            send("ItemRotating"); // the update message RotateModel posts to InventoryMenu (the zoom button hint)
        }
    }
    // The item menus draw the movie, then slot 1 (InventoryMenu::Display and the others' slot 6).
    bool ui_scene(UiScene& out) override { return m_item3d.scene(out); }
    bool scene_after_movie() const override { return true; }

public:
    std::string item3d_json() const { return m_item3d.state_json(); }
    void item3d_rotate(float a, float b) { m_item3d.rotate(a, b); }

protected:

    void on_loaded() override {
        // The constructor reads Menu_mc.bPCControlsReady (true in all four movies) for the PC control scheme.
        m_pc_controls = has_path("Menu_mc.bPCControlsReady");
        m_list = invoke("GetInventoryItemList"); // kept for the menu's life, as the engine's ItemList keeps it
    }

    void load_data() { m_data = item_source(m_ui).data(name()); }

    // The item list (ItemList and the others' fill): the movie's entryList emptied and refilled with the
    // engine's entries, then InvalidateListData.
    void fill_list() {
        auto& vm = player()->vm;
        const Value entries = m_list.is_object() ? vm.get(m_list, "entryList") : Value{};
        if (!entries.is_object()) {
            return;
        }
        entries.o->elements.clear();
        for (const auto& e : m_data.entries) {
            entries.o->elements.push_back(player::json_to_value(vm, e));
        }
        send("InvalidateListData");
    }

    // The entry the list has selected (the list's selectedIndex), or -1.
    int selected() {
        if (!m_list.is_object()) {
            return -1;
        }
        const Value v = player()->vm.get(m_list, "selectedIndex");
        if (!v.is_number() || v.n < 0 || v.n >= double(m_data.entries.size())) {
            return -1;
        }
        return int(v.n);
    }

    // The categories (SetCategoriesList,,): triples of text, filter flag
    // and "don't hide when empty", from the GMSTs. An owner's set is ALL (its name, or sAllItems) and the nine kinds;
    // the other side's flags sit ten bits up, as the game shifts its items.
    void add_category(std::vector<Value>& args, const std::string& text, double flag, bool dont_hide) {
        args.push_back(Value::string(text));
        args.push_back(Value::number(flag));
        args.push_back(Value::boolean(dont_hide));
    }
    void add_owner_categories(std::vector<Value>& args, const std::string& all_text, bool shifted) {
        static constexpr std::pair<const char*, int> k_kinds[] = {
            {"sWeapons", 2},  {"sApparel", 4},      {"sPotions", 8}, {"sScrolls", 16}, {"sFood", 32},
            {"sIngredients", 64}, {"sBooks", 128}, {"sKeys", 256},  {"sMisc", 512}};
        const int shift = shifted ? 10 : 0;
        add_category(args, all_text.empty() ? m_ui.game_setting("sAllItems") : all_text, double(1023 << shift), true);
        for (const auto& [gmst, flag] : k_kinds) {
            add_category(args, m_ui.game_setting(gmst), double(flag << shift), false);
        }
    }
    void add_divider(std::vector<Value>& args) { add_category(args, "", 0, true); }

    void send_player_info() {
        if (m_data.available) {
            send("UpdatePlayerInfo", {player::json_to_value(player()->vm, m_data.player_info)});
        }
    }

    // A game-side call: the inventory system does the work; when the inventory changed, the engine's deferred
    // inventory update refills the list and the player info (ProcessMessage, message 8), after the sound.
    void game(const std::string& call, std::vector<double> args, const char* sound = nullptr) {
        if (item_source(m_ui).request(name(), call, args, selected())) {
            if (sound) {
                m_ui.host().ui_play_sound(sound);
            }
            load_data();
            fill_list();
            refresh_player_info();
        }
        if (item_source(m_ui).should_close()) {
            m_ui.close(name());
        }
    }
    virtual void refresh_player_info() { send_player_info(); }

    // A HUD notification with its sound: the text goes to the HUD Menu.
    void notify(const std::string& gmst, const char* sound) {
        m_ui.host().ui_play_sound(sound);
        log::info("ui {}: notification \"{}\"", name(), m_ui.game_setting(gmst));
    }

    std::vector<double> numbers(const Call& c) {
        std::vector<double> out;
        for (const auto& a : c.args) {
            out.push_back(player()->vm.to_number(a));
        }
        return out;
    }

    Kind m_kind;
    Item3D m_item3d;
    float m_dt = 0.0f;
    ItemMenuData m_data;
    Value m_list;
    bool m_pc_controls = false;
    bool m_rotating = false;
    bool m_closing = false;
};

// ---- InventoryMenu (movie InventoryMenu) --------------------------------------------------------------------------

class InventoryMenu final : public ItemMenu {
public:
    // Constructor: ExactFit, background alpha 0, depth 0, flags 0xa481; no context of its own, it pushes
    // Menu Mode, Item Menus and Inventory on the control map.
    explicit InventoryMenu(Ui& ui)
        : ItemMenu(ui, {"InventoryMenu", "InventoryMenu", ScaleMode::exact_fit, 0, 0xa481, 0, 19}, Kind::inventory) {
        m_contexts = {k_menu_mode, k_item_menus, k_inventory};
    }

    void on_show() override {
        // Show (message 1): the item list from _root.GetInventoryItemList, then the categories, the
        // items, the player info and the saved positions.
        m_item3d.begin(1); // Inventory3DManager::Begin(1)
        load_data();
        std::vector<Value> cats;
        add_category(cats, m_ui.game_setting("sFavorites"), 1, false);
        add_owner_categories(cats, "", false);
        send("SetCategoriesList", std::move(cats));
        fill_list();
        send_player_info();
        std::vector<Value> restore{Value::number(g_inventory_indices.scroll)};
        for (int i : g_inventory_indices.items) {
            restore.push_back(Value::number(i));
        }
        send("RestoreIndices", std::move(restore));
    }

private:
    void register_menu_handlers() override {
        handle("ShowTweenMenu", [this](Call&) {
            // an update message (0) to TweenMenu, which plays its open animation again when it is open;
            // the movie fades itself out and calls CloseMenu.
            tween_menu_update(m_ui);
        });
        handle("CloseTweenMenu", [this](Call&) {
            // the tween camera ends, TweenMenu hides, and the menu ignores user events from now on.
            m_ui.close("TweenMenu");
            m_closing = true;
        });
        handle("ItemSelect", [this](Call& c) {
            // with PC controls an argument 0 or 1 picks the right or left hand's equip slot; otherwise the
            // default slot. Equip, use or read is the inventory system's.
            if (!m_pc_controls || c.args.empty() || !c.arg(0).is_number()) {
                game("ItemSelect", {});
            } else if (c.arg(0).n == 0 || c.arg(0).n == 1) {
                game("ItemSelect", {c.arg(0).n});
            }
        });
        handle("ItemDrop", [this](Call& c) { game("ItemDrop", numbers(c)); });
        handle("ShowSoulGemList", [this](Call&) {
            if (player::json::Value card; item_source(m_ui).soul_gem_list(selected(), card)) {
                send("UpdateItemCardInfo", {player::json_to_value(player()->vm, card)});
            }
        });
        handle("ItemCardListCallback", [this](Call& c) { game("ItemCardListCallback", numbers(c), "UIEnchantRecharge"); });
        handle("SaveIndices", [this](Call& c) {
            const Value a = c.arg(0);
            if (!a.is_object()) {
                return;
            }
            auto& vm = player()->vm;
            const auto& el = a.o->elements;
            g_inventory_indices.scroll = el.empty() ? 0 : int(vm.to_number(el[0]));
            g_inventory_indices.items.clear();
            for (size_t i = 1; i < el.size(); ++i) {
                g_inventory_indices.items.push_back(int(vm.to_number(el[i])));
            }
        });
    }
};

// ---- ContainerMenu (movie ContainerMenu) --------------------------------------------------------------------------

class ContainerMenu final : public ItemMenu {
public:
    // Constructor: ExactFit, background alpha 0, depth 0, flags 0xa489, context Item Menus.
    explicit ContainerMenu(Ui& ui)
        : ItemMenu(ui, {"ContainerMenu", "ContainerMenu", ScaleMode::exact_fit, 0, 0xa489, 0, k_item_menus},
                   Kind::container) {}

    void on_show() override {
        // Show: bNPCMode, the categories, the items, the player info, then the item list opens.
        m_item3d.begin(1); // Inventory3DManager::Begin(1)
        load_data();
        if (has_path("Menu_mc")) {
            player()->set_path("Menu_mc.bNPCMode", Value::boolean(m_data.npc_mode));
        }
        std::vector<Value> cats;
        if (m_data.other_categories) {
            add_owner_categories(cats, m_data.other_name, true);
        } else {
            add_category(cats, m_data.other_name, double(1023 << 10), true);
        }
        add_divider(cats);
        add_owner_categories(cats, m_data.player_name, false);
        send("SetCategoriesList", std::move(cats));
        fill_list();
        send_player_info();
        if (m_pc_controls) {
            invoke("Menu_mc.ShowItemsList");
        }
    }

    bool on_hide() override {
        item_source(m_ui).closed(name());
        return ItemMenu::on_hide();
    }

private:
    void register_menu_handlers() override {
        handle("ItemTransfer", [this](Call& c) { game("ItemTransfer", numbers(c)); });
        handle("EquipItem", [this](Call& c) { game("EquipItem", numbers(c)); });
        handle("TakeAllItems", [this](Call&) { game("TakeAllItems", {}); });
        handle("ToggleFavorite", [this](Call&) { game("ToggleFavorite", {}, "UIFavorite"); });
        // sItemTooHeavy as a HUD notification with UIMenuCancel.
        handle("DisabledItemSelect", [this](Call&) { notify("sItemTooHeavy", "UIMenuCancel"); });
    }
};

// ---- BarterMenu (movie BarterMenu) --------------------------------------------------------------------------------

class BarterMenu final : public ItemMenu {
public:
    // Constructor: ExactFit, background alpha 0, depth 0, flags 0xa489, context Item Menus.
    explicit BarterMenu(Ui& ui)
        : ItemMenu(ui, {"BarterMenu", "BarterMenu", ScaleMode::exact_fit, 0, 0xa489, 0, k_item_menus}, Kind::barter) {}

    bool on_hide() override {
        m_hud_mode.release();
        return ItemMenu::on_hide();
    }

    void on_show() override {
        // Show: SetBarterMultipliers (the Speech and perk prices, clamped by fMinBuyMult and
        // fMaxSellMult), the categories (vendor, divider, player), the items, the player info.
        m_item3d.begin(1); // Inventory3DManager::Begin(1)
        load_data();
        send("SetBarterMultipliers", {Value::number(m_data.buy_mult), Value::number(m_data.sell_mult)});
        std::vector<Value> cats;
        add_owner_categories(cats, m_data.other_name, true);
        add_divider(cats);
        add_owner_categories(cats, m_data.player_name, false);
        send("SetCategoriesList", std::move(cats));
        fill_list();
        refresh_player_info();
    }

private:
    void refresh_player_info() override {
        // the player's gold, the vendor's gold, the vendor's name and the player info object.
        if (!m_data.available) {
            return;
        }
        send("UpdatePlayerInfo", {Value::number(m_data.player_gold), Value::number(m_data.vendor_gold),
                                  Value::string(m_data.vendor_name),
                                  player::json_to_value(player()->vm, m_data.player_info)});
    }

    void register_menu_handlers() override {
        handle("ItemSelect", [this](Call& c) { game("ItemSelect", numbers(c)); });
        handle("GetRawDealWarningString", [this](Call& c) {
            // sNotEnoughVendorGold with the deal's value and the vendor's gold.
            if (c.args.size() == 1 && c.arg(0).is_number()) {
                c.respond({Value::string(format_gmst(m_ui.game_setting("sNotEnoughVendorGold"),
                                                     {(long long)c.arg(0).n, (long long)m_data.vendor_gold}))});
            }
        });
    }
};

// ---- GiftMenu (movie GiftMenu) ------------------------------------------------------------------------------------

class GiftMenu final : public ItemMenu {
public:
    // Constructor: NoBorder, background alpha 0, the default depth 3, flags 0xa489, context Item Menus.
    explicit GiftMenu(Ui& ui)
        : ItemMenu(ui, {"GiftMenu", "GiftMenu", ScaleMode::no_border, 3, 0xa489, 0, k_item_menus}, Kind::gift) {}

    void on_show() override {
        // Show: SetMenuInfo (the player gives, favor points), the giver's categories, the items, the
        // player info (no values), then the item list opens.
        m_item3d.begin(1); // Inventory3DManager::Begin(1)
        load_data();
        send("SetMenuInfo", {Value::boolean(m_data.giving), Value::boolean(m_data.favor_points)});
        std::vector<Value> cats;
        if (m_data.giving) {
            add_owner_categories(cats, m_data.player_name, false);
        } else {
            add_owner_categories(cats, m_data.other_name, true);
        }
        send("SetCategoriesList", std::move(cats));
        fill_list();
        refresh_player_info();
        if (m_pc_controls) {
            invoke("Menu_mc.ShowItemsList");
        }
    }

private:
    void refresh_player_info() override { send("UpdatePlayerInfo"); }

    void register_menu_handlers() override {
        handle("ItemSelect", [this](Call& c) { game("ItemSelect", numbers(c)); });
        handle("DisabledItemSelect", [this](Call&) { notify("sItemTooHeavy", "UIMenuCancel"); });
    }
};

} // namespace

// The game shows the model of the entry's own form (Inventory3DManager::UpdateItem3D with the InventoryEntryData at
// the list's selectedIndex). The captured lists carry no forms, so the capture of the manager (item3d-models,
//) keys each model by what the game's entry held: "text|count|filterFlag" first; then
// the entry's place among the entries of the same text and the same owner (the player's, or the container's or
// vendor's: filterFlag << 10), "text" for the first and "text#n" for the n-th, in the engine's sorted order.
std::vector<std::string> captured_item_models(Ui& ui, const std::string& menu,
                                              const std::vector<json::Value>& entries) {
    std::vector<std::string> out(entries.size());
    const json::Value* m = ui.capture("item3d-models");
    const json::Value* by_menu = m ? member(*m, "paths") : nullptr;
    const json::Value* models = by_menu ? member(*by_menu, menu) : nullptr;
    if (!models) {
        return out;
    }
    std::unordered_map<std::string, int> seen;
    for (size_t i = 0; i < entries.size(); ++i) {
        const std::string text = string_of(entries[i], "text");
        const int count = int(number_of(entries[i], "count", 1));
        const int filter = int(number_of(entries[i], "filterFlag"));
        const bool other = (filter >> 10) != 0;
        const int n = ++seen[std::format("{}|{}", text, other)];
        out[i] = string_of(*models, std::format("{}|{}|{}", text, count, filter));
        if (out[i].empty()) {
            out[i] = string_of(*models, n > 1 ? std::format("{}#{}", text, n) : text);
        }
        if (out[i].empty() && n > 1) {
            out[i] = string_of(*models, text);
        }
    }
    return out;
}

// Automation: an item menu's 3D item manager state (ui item3d).
std::string items_item3d_json(Menu& m) {
    if (auto* im = dynamic_cast<ItemMenu*>(&m)) {
        return im->item3d_json();
    }
    return "null";
}

// Automation: the game with the angles given (ui item3d rotate)
// `game call` makes the game's.
bool items_item3d_rotate(Menu& m, float a, float b) {
    if (auto* im = dynamic_cast<ItemMenu*>(&m)) {
        im->item3d_rotate(a, b);
        return true;
    }
    return false;
}

std::unique_ptr<Menu> create_items_menu(Ui& ui, const std::string& name) {
    if (name == "InventoryMenu") return std::make_unique<InventoryMenu>(ui);
    if (name == "ContainerMenu") return std::make_unique<ContainerMenu>(ui);
    if (name == "BarterMenu") return std::make_unique<BarterMenu>(ui);
    if (name == "GiftMenu") return std::make_unique<GiftMenu>(ui);
    return nullptr;
}

} // namespace spud::ui
