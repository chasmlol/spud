#pragma once

// The game side of the item and crafting menus (ui::ItemSource, ui::CraftingSource over game::Game): what the movies
// are told about the player's and containers' items, and what their calls do (
//).

#include "game/game.hpp"
#include "ui/items.hpp"
#include "ui/ui.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spud::engine {

class ItemMenus final : public ui::ItemSource, public ui::CraftingSource {
public:
    using Json = ui::player::json::Value;

    ItemMenus(game::Game& game, ui::Ui& ui) : m_game(game), m_ui(ui) {}

    // What opens next: the container or vendor chest behind ContainerMenu and BarterMenu, a station's furniture.
    void open_container(uint32_t ref, std::string name, bool stealing);
    // the vendor by their NPC_ (Actors and Dialogue call this for a real one); false for no vendor.
    bool open_vendor(uint32_t npc);
    // the player gives to `npc`, or takes from them; `done` gets the items' value when it closes.
    void open_gift(uint32_t npc, std::string name, bool giving, uint32_t filter, bool stolen, std::function<void(int)> done);
    void open_station(uint32_t furniture) { m_furniture = furniture; }

    // Pickpocketing a live actor: the container is their NPC_, the mode 2 of the game.
    void open_pickpocket(uint32_t ref, uint32_t npc, std::string name);
    // The Lockpicking Menu on a reference: false when it can't open (a message said why).
    bool open_lock(const game::Target& target);
    ui::LockInfo lock();
    void lock_event(std::string_view what, double pick_health);
    uint32_t unlocked_ref() const { return m_lock.ref; }

    // ui::ItemSource
    ui::ItemMenuData data(const std::string& menu) override;
    bool request(const std::string& menu, const std::string& call, const std::vector<double>& args,
                 int entry) override;
    bool soul_gem_list(int entry, Json& card) override;
    std::vector<Json> favorites() override;
    void equip_favorite(int index, int hand) override;
    bool should_close() override { return std::exchange(m_close, false); }
    void closed(const std::string& menu) override;

    // ui::CraftingSource
    bool begin(const std::string& show_data) override;
    int bench() const override { return m_station.bench_type; }
    std::string furniture_name() const override { return m_station.name; }
    bool has_skill() const override;
    ui::CraftSkill skill() const override;
    Json player_info() override;
    std::vector<ui::CraftEntry> entries() override;
    Json card(int entry) override;
    std::string model(int entry) override;
    std::vector<ui::CraftIngredient> ingredients(int entry) override;
    std::string refusal(int entry) override;
    void craft(int entry) override;
    std::string craft_sound(int entry) override;
    void preview(int entry) override { m_preview = entry; }
    std::vector<ui::CraftCategory> categories() override;
    ui::CraftChoice choose(int entry) override;
    void confirmed(bool yes) override;
    void set_category(int index) override { m_disenchanting = index == 0; }
    void select(int entry) override { m_selected = entry; }
    bool has_result() override;
    bool anything_chosen() override;
    void clear_choices() override;
    std::vector<std::string> buttons() override;
    std::string craft_question() override;
    ui::CraftOutcome finish() override;
    bool set_slider(double value) override;
    void slider_closed(bool accepted) override;
    std::string rename_text() override;
    void rename(const std::string& text) override { m_custom_name = text; }

    ui::CrosshairTarget crosshair(const game::Target& target) const;
    // The Book Menu on a book of the player's (ref 0) or a reference in the world.
    void open_book(uint32_t form, uint32_t ref, bool steal);
    ui::BookInfo book();
    uint32_t book_ref() const { return m_book_ref; }
    // HUD notifications and sounds the game left.
    void flush_notices();
    // Something left the player's inventory for the world.
    std::function<void(const game::Stack&)> on_drop;
    // Automation: the card of one of a form, as JSON.
    std::string card_text(uint32_t form) const;

private:
    // One entry of a menu's list: whose stack it is.
    struct Row {
        game::Inventory* owner = nullptr;
        size_t stack = 0;
        bool other = false; // the container's or vendor's side
    };

    Json entry_json(const game::Stack& s, bool other) const;
    Json card_json(const game::Stack& s, bool other) const;
    void rows_of(game::Inventory& inv, bool other, std::vector<Row>& out, bool gold = false);
    int pickpocket_chance(const game::Stack& s, int count, bool card) const;
    bool recharge(const Row& row, int gem);
    bool drop(const Row& row, int count);
    void mark_best(ui::ItemMenuData& d);

    game::Game& m_game;
    ui::Ui& m_ui;
    std::vector<Row> m_rows; // the open menu's list, in its order
    uint32_t m_container = 0;
    std::string m_other_name;
    bool m_stealing = false;
    double m_buy_mult = 1, m_sell_mult = 1;
    bool m_close = false;
    int m_theft_total = 0;
    bool m_pickpocket = false;
    uint32_t m_pickpocket_npc = 0, m_pickpocket_ref = 0;
    bool m_lock_reported = false;
    void check_lock_crime();
    game::Target m_lock;
    std::vector<Row> m_gems; // the soul gems of the recharge list
    std::vector<size_t> m_favorites; // the Favorites Menu's entries, as stacks
    int m_recharge_entry = -1;
    uint32_t m_vendor_npc = 0;
    uint32_t m_gift_npc = 0, m_gift_filter = 0;
    bool m_gift_giving = true, m_gift_stolen = false;
    int m_gift_total = 0;
    std::function<void(int)> m_gift_done;
    game::Game::Vendor m_vendor;
    bool transfer(const Row& row, int count);
    bool trade(const Row& row, int count, int unit_price);
    int vendor_gold();
    uint32_t m_furniture = 0;
    uint32_t m_book = 0, m_book_ref = 0;
    bool m_book_steal = false;
    game::Station m_station;
    std::vector<const game::Recipe*> m_recipes;
    std::vector<game::TemperEntry> m_temper;
    int m_preview = -1;

    // The alchemy lab.
    struct Ingredient {
        uint32_t form = 0;
        int32_t count = 0;
    };
    bool alchemy() const { return m_station.bench_type == 5; }
    bool enchanter() const { return m_station.bench_type == 3 || m_station.bench_type == 4; }
    uint32_t alchemy_result(); // the potion the chosen ingredients show, 0 for none
    std::vector<Ingredient> m_ingredients;
    std::vector<uint32_t> m_alchemy_chosen;
    std::vector<uint32_t> m_alchemy_effects; // the categories, in order

    // The arcane enchanter: a row of the list.
    struct EnchantRow {
        uint32_t flag = 0;
        int stack = -1;          // of the player's inventory
        uint32_t enchantment = 0; // flags 16, 32
        std::string text;
        int32_t count = 1;
        float charge = 0;
        bool enabled = true;
    };
    void build_enchant_rows();
    void update_enchant();
    bool enchant_complete() const { return m_chosen_item >= 0 && m_chosen_gem >= 0 && !m_chosen_enchants.empty(); }
    bool enchant_weapon() const;
    float enchant_soul() const;
    std::vector<EnchantRow> m_enchant_rows;
    int m_chosen_item = -1, m_chosen_gem = -1, m_disenchant = -1; // stack indices
    std::vector<game::EnchantChoice> m_chosen_enchants;
    std::vector<uint32_t> m_chosen_flags;
    game::EnchantNumbers m_numbers;
    std::string m_custom_name;
    bool m_disenchanting = true;
    int m_selected = 0;
    float m_slider_kept = 0;
};

} // namespace spud::engine
