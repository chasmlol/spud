#include "engine/item_menus.hpp"

#include "core/game_random.hpp"
#include "game/quests.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <format>
#include <string_view>
#include <unordered_map>

namespace spud::engine {

namespace {

using Json = ItemMenus::Json;
using game::Item;
using game::ItemKind;
using game::Stack;

Json jnum(double v) {
    Json j;
    j.kind = Json::number;
    j.n = v;
    return j;
}
Json jstr(std::string v) {
    Json j;
    j.kind = Json::string;
    j.s = std::move(v);
    return j;
}
Json jbool(bool v) {
    Json j;
    j.kind = Json::boolean;
    j.b = v;
    return j;
}
Json jobject() {
    Json j;
    j.kind = Json::object;
    return j;
}
void put(Json& o, std::string name, Json v) { o.members.emplace_back(std::move(name), std::move(v)); }

// The stations `ui open "Crafting Menu|forge"` names: a furniture of each kind.
struct NamedStation {
    std::string_view name;
    uint32_t furniture;
};
constexpr NamedStation k_named_stations[] = {
    {"forge", 0x000CAE0B},   {"smelter", 0x0009C6CE},    {"tanning", 0x000727A1}, {"cooking", 0x001010B3},
    {"grindstone", 0x0006E9C2}, {"armor", 0x000D932F}, {"enchant", 0x000BAD0D}, {"alchemy", 0x000BAD0C},
};

bool smithing_bench(int bench) { return bench == 2 || bench == 7; }

} // namespace

// ---- item menus ------------------------------------------------------------------------------------------------

void ItemMenus::open_container(uint32_t ref, std::string name, bool stealing) {
    m_container = ref;
    m_other_name = std::move(name);
    m_stealing = stealing;
    m_pickpocket = false;
    m_theft_total = 0;
}

void ItemMenus::open_pickpocket(uint32_t ref, uint32_t npc, std::string name) {
    m_container = ref ? ref : npc; // this actor's things, not what its record starts with
    m_pickpocket_npc = npc;
    m_pickpocket_ref = ref;
    m_other_name = std::move(name);
    m_stealing = true;
    m_pickpocket = true;
    m_theft_total = 0;
    m_ui.open("ContainerMenu");
}

// The chance: the target's and the player's Pickpocket, the value and the weight. Whether the target
// sees the player takes off fPickPocketDetected: detection is AI's.
int ItemMenus::pickpocket_chance(const Stack& s, int count, bool card) const {
    const game::Settings& g = m_game.settings();
    const Item* item = m_game.items().find(s.form);
    const float target = m_game.npc_skill(m_pickpocket_npc, game::pickpocket);
    const float own = std::clamp(m_game.character().skill(game::pickpocket), 0.0f, 100.0f);
    const float value = float(m_game.value(s)) * float(count);
    // The card reads the form's raw weight, -1 for what weighs nothing.
    const float each = item && item->weight > 0.0f ? item->weight : card ? -1.0f : 0.0f;
    const float weight = each * float(count);
    float chance = g.number("fPickPocketTargetSkillMult", -0.25f) * target + g.number("fPickPocketTargetSkillBase", 20) +
                   g.number("fPickPocketActorSkillMult", 1) * own + g.number("fPickPocketActorSkillBase", 20) +
                   value * g.number("fPickPocketAmountMult", -0.1f) + g.number("fPickPocketAmountBase", 0) +
                   weight * g.number("fPickPocketWeightMult", -4) + g.number("fPickPocketWeightBase", 0);
    chance = m_game.perk_value(56, chance);
    chance = std::clamp(chance, g.number("fPickPocketMinChance", 0), g.number("fPickPocketMaxChance", 90));
    return int(std::floor(chance + 0.5f));
}

void ItemMenus::open_gift(uint32_t npc, std::string name, bool giving, uint32_t filter, bool stolen,
                          std::function<void(int)> done) {
    if (m_gift_done) { // nothing while an earlier gift's callback is pending
        done(0);
        return;
    }
    m_gift_npc = npc;
    m_gift_filter = filter;
    m_gift_giving = giving;
    m_gift_stolen = stolen;
    m_gift_total = 0;
    m_gift_done = std::move(done);
    m_other_name = std::move(name);
    m_ui.open("GiftMenu");
}

bool ItemMenus::open_vendor(uint32_t npc) {
    m_vendor = m_game.vendor(npc);
    if (!m_vendor.faction) {
        return false;
    }
    m_game.refresh_vendor(m_vendor);
    m_vendor_npc = npc;
    m_container = m_vendor.chest;
    const forms::FormEntry* e = m_game.db().find(npc);
    m_other_name = std::string(forms::Form(m_game.db(), *e).name());
    m_stealing = false;
    m_buy_mult = m_game.buy_mult(npc);
    m_sell_mult = m_game.sell_mult(npc);
    m_ui.open("BarterMenu");
    return true;
}

// The stock's gold: the NPC's and the chest's ( on the merged stock).
int ItemMenus::vendor_gold() {
    return m_game.container(m_vendor_npc).gold() + m_game.container(m_vendor.chest).gold();
}

Json ItemMenus::entry_json(const Stack& s, bool other) const {
    Json e = jobject();
    put(e, "text", jstr(m_game.display_name(s)));
    put(e, "count", jnum(s.count));
    put(e, "equipState", jnum(double(s.worn)));
    put(e, "filterFlag", jnum(double(m_game.filter_flag(s) << (other ? 10 : 0))));
    put(e, "favorite", jnum(s.favorite ? 1 : 0));
    put(e, "enabled", jbool(true));
    return e;
}

Json ItemMenus::card_json(const Stack& s, bool other) const {
    const Item* item = m_game.items().find(s.form);
    Json c = jobject();
    if (!item) {
        return c;
    }
    put(c, "name", jstr(m_game.display_name(s)));
    put(c, "count", jnum(s.count));
    put(c, "value", jnum(m_game.value(s)));
    put(c, "weight", jnum(item->weight));
    put(c, "stolen", jbool(!other && s.extra.owner != 0));
    const auto round = [](float v) { return std::floor(v + 0.5f); };
    std::string effects = m_game.effects_text(s);
    int type = 3;
    switch (item->kind) {
    case ItemKind::scroll:
        type = 6;
        put(c, "potionType", jnum(m_game.potion_type(*item)));
        put(c, "spellCost", jnum(item->value));
        break;
    case ItemKind::armor: {
        type = 1;
        const float rating = round(m_game.armor_rating(s));
        put(c, "armor", jnum(rating));
        if (m_game.survival()) {
            const float warmth = round(m_game.item_warmth(*item));
            put(c, "warmth", jnum(warmth));
            put(c, "warmthChange", jnum(s.worn != game::Worn::none ? 0.0 : double(warmth) - round(m_game.slot_warmth(*item))));
        }
        put(c, "skillText", jstr(item->armor_type == 2 ? std::string()
                                 : m_game.skill_name(item->armor_type == 0 ? game::light_armor : game::heavy_armor)));
        put(c, "armorChange", jnum(s.worn != game::Worn::none ? 0.0 : double(rating) - m_game.slot_rating(*item)));
        break;
    }
    case ItemKind::book:
        if (item->book_flags & game::book_teaches_spell) {
            type = 6;
            effects = m_game.spell_text(item->book_spell);
            const int skill = m_game.spell_skill(item->book_spell);
            if (skill >= 0) {
                put(c, "skillName", jstr(m_game.skill_name(skill)));
            }
        } else {
            type = 4;
            put(c, "description", jstr(item->description));
        }
        break;
    case ItemKind::ingredient: {
        type = 8;
        const uint16_t known = m_game.player().known_effects(s.form);
        put(c, "numItemEffects", jnum(double(item->effects.size())));
        for (size_t i = 0; i < item->effects.size() && i < 4; ++i) {
            const game::MagicEffect* m = m_game.magic().effect(item->effects[i].effect);
            put(c, std::format("itemEffect{}", i), jstr((known >> i) & 1 && m ? m->name : std::string()));
        }
        break;
    }
    case ItemKind::weapon:
    case ItemKind::ammo:
        type = 2;
        put(c, "damage", jnum(round(m_game.weapon_damage(s))));
        if (item->kind == ItemKind::weapon) {
            if (const uint32_t ench = s.extra.enchantment ? s.extra.enchantment : item->enchantment) {
                const float max = float(item->enchantment_amount ? item->enchantment_amount : s.extra.max_charge);
                const float charge = s.extra.charge >= 0.0f && max > 0.0f ? s.extra.charge * 100.0f / max : 100.0f;
                const float cost = max > 0.0f ? m_game.magic().enchantment_cost(ench) * 100.0f / max : 0.0f;
                put(c, "charge", jnum(charge));
                put(c, "usedCharge", jnum(std::max(charge - cost, 0.0f)));
            }
            put(c, "poisoned", jbool(false));
            put(c, "damageChange", jnum(double(round(m_game.weapon_damage(s))) - double(m_game.total_damage())));
        } else {
            put(c, "charge", jnum(100));
            put(c, "usedCharge", jnum(100));
        }
        break;
    case ItemKind::key: type = 9; break;
    case ItemKind::potion:
        type = item->food() ? 5 : 6;
        put(c, "potionType", jnum(m_game.potion_type(*item)));
        break;
    case ItemKind::soul_gem: {
        type = 12;
        const uint8_t soul = s.extra.soul ? s.extra.soul : item->soul;
        std::string line = m_game.settings().text(soul ? "sSoulLevel" : "sEmpty");
        if (const size_t at = line.find("%s"); soul && at != std::string::npos) {
            line.replace(at, 2, m_game.soul_name(soul));
        }
        put(c, "soulLVL", jstr(std::move(line)));
        break;
    }
    default: break;
    }
    put(c, "effects", jstr(std::move(effects)));
    put(c, "type", jnum(type));
    if (m_pickpocket) {
        const int count = item->weight > 0.0f ? 1 : s.count;
        put(c, "pickpocketChance", jnum(std::clamp(pickpocket_chance(s, count, true), 0, 100)));
    }
    return c;
}

void ItemMenus::rows_of(game::Inventory& inv, bool other, std::vector<Row>& out, bool gold) {
    for (size_t i = 0; i < inv.stacks().size(); ++i) {
        const Stack& s = inv.stacks()[i];
        if (m_game.listed(s) || (gold && s.form == game::k_gold)) {
            out.push_back({&inv, i, other});
        }
    }
}

ui::ItemMenuData ItemMenus::data(const std::string& menu) {
    ui::ItemMenuData d;
    d.available = true;
    game::Player& p = m_game.player();
    d.player_name = p.name;
    d.other_name = m_other_name;
    m_rows.clear();
    const bool barter = menu == "BarterMenu";
    if (menu == "ContainerMenu") {
        d.npc_mode = m_pickpocket;
        rows_of(m_game.container(m_container), true, m_rows, true);
        if (m_pickpocket) {
            // What the target wears stays on them (EP 61 lets the perk's owner have it: seam).
            std::erase_if(m_rows, [](const Row& r) { return r.owner->stacks()[r.stack].worn != game::Worn::none; });
        }
    } else if (barter) {
        rows_of(m_game.container(m_vendor_npc), true, m_rows);
        rows_of(m_game.container(m_vendor.chest), true, m_rows);
    }
    if (menu == "GiftMenu") {
        d.giving = m_gift_giving;
        d.favor_points = false; // ShowGiftMenu's favor argument isn't passed on
        rows_of(m_gift_giving ? p.inventory : m_game.container(m_gift_npc), !m_gift_giving, m_rows);
        const forms::FormEntry* list = m_gift_filter ? m_game.db().find(m_gift_filter) : nullptr;
        const std::vector<uint32_t> allowed = list ? forms::form_list(forms::Form(m_game.db(), *list)) : std::vector<uint32_t>{};
        std::erase_if(m_rows, [&](const Row& r) {
            const Stack& s = r.owner->stacks()[r.stack];
            return (list && std::ranges::find(allowed, s.form) == allowed.end()) || (s.extra.owner && !m_gift_stolen);
        });
    } else {
        rows_of(p.inventory, false, m_rows, menu == "ContainerMenu");
    }
    if (barter) {
        // What a vendor doesn't deal in shows on neither side, and what's stolen only a fence takes.
        std::erase_if(m_rows, [this](const Row& r) {
            const Stack& s = r.owner->stacks()[r.stack];
            const Item* item = m_game.items().find(s.form);
            return !m_game.vendor_accepts(m_vendor, *item) || (s.extra.owner && !m_vendor.buys_stolen);
        });
        d.buy_mult = m_buy_mult;
        d.sell_mult = m_sell_mult;
        d.player_gold = p.inventory.gold();
        d.vendor_gold = vendor_gold();
        d.vendor_name = m_other_name;
    }
    // The fill's comparator: text by bytes, then the form.
    std::stable_sort(m_rows.begin(), m_rows.end(), [this](const Row& a, const Row& b) {
        const Stack &x = a.owner->stacks()[a.stack], &y = b.owner->stacks()[b.stack];
        const std::string p = m_game.display_name(x), q = m_game.display_name(y);
        return p != q ? p < q : x.form < y.form;
    });
    for (const Row& r : m_rows) {
        const Stack& s = r.owner->stacks()[r.stack];
        Json e = entry_json(s, r.other);
        if (r.other && m_stealing) {
            put(e, "isStealing", jbool(true));
        }
        if (const Item* gem = m_game.items().find(s.form); gem && gem->kind == ItemKind::soul_gem) {
            if (const uint8_t soul = s.extra.soul ? s.extra.soul : gem->soul) {
                put(e, "soulLVL", jstr(m_game.soul_name(soul)));
            }
        }
        d.entries.push_back(std::move(e));
        d.cards.push_back(card_json(s, r.other));
        const Item* item = m_game.items().find(s.form);
        d.models.push_back(item ? item->inventory_model : std::string());
    }
    mark_best(d);
    d.player_info = player_info();
    return d;
}

// bestInClass: in each class the item with the higher rating, then value; on a tie the player's.
void ItemMenus::mark_best(ui::ItemMenuData& d) {
    struct Best {
        int row = -1;
        double rating = 0, value = 0;
        bool other = false;
    };
    std::unordered_map<uint32_t, Best> classes;
    const auto round = [](float v) { return double(std::floor(v + 0.5f)); };
    std::vector<uint32_t> class_of(m_rows.size(), 0);
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const Row& r = m_rows[i];
        const Stack& s = r.owner->stacks()[r.stack];
        const Item* item = m_game.items().find(s.form);
        uint32_t cls = 0;
        double rating = 0;
        if (item->kind == ItemKind::weapon) {
            const int type = item->animation_type;
            cls = type >= 1 && type <= 4 ? 1 : type == 5 || type == 6 ? 2 : type == 7 || type == 9 ? 3 : 0;
            rating = round(m_game.weapon_damage(s));
        } else if (item->kind == ItemKind::ammo) {
            cls = 4;
            rating = round(m_game.weapon_damage(s));
        } else if (item->kind == ItemKind::armor && item->biped_slots) {
            cls = 0x100u + uint32_t(std::countr_zero(item->biped_slots));
            rating = round(m_game.armor_rating(s));
        }
        class_of[i] = cls;
        if (!cls) {
            continue;
        }
        const double value = m_game.value(s);
        Best& b = classes[cls];
        const bool better = b.row < 0 || rating > b.rating || (rating == b.rating && value > b.value) ||
                            (rating == b.rating && value == b.value && b.other && !r.other);
        if (better) {
            b = {int(i), rating, value, r.other};
        }
    }
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (class_of[i]) {
            put(d.entries[i], "bestInClass", jbool(classes[class_of[i]].row == int(i)));
        }
    }
}

void ItemMenus::flush_notices() {
    for (const game::Notice& n : m_game.take_notices()) {
        if (!n.skill.empty()) {
            m_ui.skill_notification(n.skill, n.skill_level, n.player_level, n.xp);
        } else if (!n.text.empty()) {
            m_ui.notification(n.text, n.sound);
        } else if (!n.sound.empty()) {
            m_ui.host().ui_play_sound(n.sound);
        }
    }
}

bool ItemMenus::request(const std::string& menu, const std::string& call, const std::vector<double>& args, int entry) {
    const Row* row = entry >= 0 && size_t(entry) < m_rows.size() ? &m_rows[size_t(entry)] : nullptr;
    bool changed = false;
    if (menu == "InventoryMenu" && call == "ItemSelect" && row) {
        const int hand = args.empty() ? -1 : int(args[0]);
        const uint32_t form = row->owner->stacks()[row->stack].form;
        const game::ItemUse use = m_game.select(row->stack, hand);
        changed = use == game::ItemUse::changed;
        if (use == game::ItemUse::read) {
            const Item* book = m_game.items().find(form);
            if (book->book_flags & game::book_teaches_spell) {
                if (m_game.read_book(*book)) { // a tome read from a list is used up, and opens no book
                    m_game.player().inventory.remove(form, 1);
                }
            } else {
                open_book(form, 0, false);
            }
            changed = true;
        }
    } else if (menu == "GiftMenu" && call == "ItemSelect" && row) {
        // moved from giver to receiver at once, its value added to what the script gets back.
        const Stack s = row->owner->stacks()[row->stack];
        const Item* item = m_game.items().find(s.form);
        const int moved = row->owner->remove_at(row->stack, args.empty() ? 1 : int(args[0]));
        (m_gift_giving ? m_game.container(m_gift_npc) : m_game.player().inventory).add(s.form, moved, s.extra);
        m_gift_total += moved * m_game.base_value(*item);
        m_game.sound(m_game.item_sound(*item, !m_gift_giving, false));
        changed = true;
    } else if (call == "ItemDrop" && row) {
        changed = drop(*row, args.empty() ? 1 : int(args[0]));
    } else if (menu == "ContainerMenu" && call == "ItemTransfer" && row) {
        changed = transfer(*row, args.empty() ? 1 : int(args[0]));
    } else if (menu == "ContainerMenu" && call == "TakeAllItems") {
        game::Inventory& inv = m_game.container(m_container);
        while (!inv.stacks().empty()) {
            const Row r{&inv, inv.stacks().size() - 1, true};
            if (!transfer(r, inv.stacks().back().count)) {
                break;
            }
        }
        changed = true;
        m_close = true; // ends with the menu's hide
    } else if (menu == "ContainerMenu" && call == "EquipItem" && row) {
        const int hand = args.empty() ? -1 : int(args[0]);
        if (row->other) {
            const uint32_t form = row->owner->stacks()[row->stack].form;
            transfer(*row, 1);
            auto& stacks = m_game.player().inventory.stacks();
            for (size_t i = 0; i < stacks.size(); ++i) {
                if (stacks[i].form == form) {
                    m_game.select(i, hand);
                    break;
                }
            }
        } else {
            m_game.select(row->stack, hand);
        }
        changed = true;
    } else if (menu == "BarterMenu" && call == "ItemSelect" && row && args.size() >= 2) {
        changed = trade(*row, int(args[0]), int(args[1]));
    } else if (menu == "InventoryMenu" && call == "ItemCardListCallback" && row && !args.empty()) {
        changed = recharge(*row, int(args[0]));
    } else if (call == "ToggleFavorite" && row) {
        game::Stack& s = row->owner->stacks()[row->stack];
        s.favorite = !s.favorite;
        changed = true;
    }
    flush_notices();
    return changed;
}

bool ItemMenus::transfer(const Row& row, int count) {
    game::Inventory& player = m_game.player().inventory;
    game::Inventory& other = m_game.container(m_container);
    game::Inventory& from = row.other ? other : player;
    game::Inventory& to = row.other ? player : other;
    game::Stack s = from.stacks()[row.stack];
    const Item* item = m_game.items().find(s.form);
    if (item) {
        m_game.sound(m_game.item_sound(*item, true, false)); // both directions
    }
    if (m_pickpocket) {
        // The attempt, both ways: a failed one moves nothing, is a crime and ends the menu.
        const int chance = pickpocket_chance(s, count, false);
        if (int(GameRandom::shared().next_below(100)) >= chance) {
            m_ui.notification(m_game.settings().text("sPickpocketFail"));
            m_game.crime().report_pickpocket(m_pickpocket_ref, m_pickpocket_npc);
            m_close = true;
            return false;
        }
        const float value = std::max(float(m_game.value(s)) * float(count), 1.0f);
        m_game.use_skill(game::pickpocket,
                         std::pow(value, m_game.settings().number("fPickpocketSkillUsesCurve", 0.8f)));
    }
    const int moved = from.remove_at(row.stack, count);
    if (row.other && m_stealing && s.form != game::k_gold) {
        s.extra.owner = m_pickpocket ? m_pickpocket_npc : m_game.owner_of(m_container);
    }
    if (m_stealing && !m_pickpocket) {
        m_theft_total += std::max(m_game.value(s), 1) * moved;
    }
    to.add(s.form, moved, s.extra);
    return moved > 0;
}

void ItemMenus::closed(const std::string& menu) {
    // A theft from a container is judged when the menu closes.
    if (menu == "ContainerMenu" && m_stealing && !m_pickpocket && m_theft_total > 0) {
        m_game.crime().report_theft(m_container, 0, 0, m_theft_total, m_game.owner_of(m_container), true);
        flush_notices();
    }
    m_theft_total = 0;
    if (menu == "GiftMenu" && m_gift_done) {
        std::exchange(m_gift_done, {})(m_gift_total);
    }
}

std::vector<Json> ItemMenus::favorites() {
    std::vector<Json> out;
    const auto& stacks = m_game.player().inventory.stacks();
    m_favorites.clear();
    for (size_t i = 0; i < stacks.size(); ++i) {
        if (stacks[i].favorite) {
            Json e = jobject();
            put(e, "text", jstr(m_game.display_name(stacks[i])));
            put(e, "index", jnum(double(out.size())));
            put(e, "equipState", jnum(double(stacks[i].worn)));
            put(e, "hotkey", jnum(-1));
            out.push_back(std::move(e));
            m_favorites.push_back(i);
        }
    }
    return out;
}

void ItemMenus::equip_favorite(int index, int hand) {
    if (index >= 0 && size_t(index) < m_favorites.size()) {
        m_game.select(m_favorites[size_t(index)], hand);
        flush_notices();
    }
}

bool ItemMenus::soul_gem_list(int entry, Json& card) {
    if (entry < 0 || size_t(entry) >= m_rows.size()) {
        return false;
    }
    const Stack item = m_rows[size_t(entry)].owner->stacks()[m_rows[size_t(entry)].stack];
    const Item* base = m_game.items().find(item.form);
    const float max = float(base->enchantment_amount ? base->enchantment_amount : item.extra.max_charge);
    game::Inventory& inv = m_game.player().inventory;
    m_gems.clear();
    Json list;
    list.kind = Json::array;
    for (size_t i = 0; i < inv.stacks().size(); ++i) {
        const float charge = m_game.crafting().soul_charge(inv.stacks()[i]);
        if (charge > 0.0f) {
            Json g = jobject();
            put(g, "text", jstr(m_game.display_name(inv.stacks()[i])));
            put(g, "count", jnum(inv.stacks()[i].count));
            put(g, "chargeAdded", jnum(max > 0.0f ? m_game.perk_value(49, charge) * 100.0f / max : 0.0));
            list.items.push_back(std::move(g));
            m_gems.push_back({&inv, i, false});
        }
    }
    if (m_gems.empty()) {
        m_ui.notification(m_game.settings().text("sNoChargeItems"));
        return false;
    }
    m_recharge_entry = entry;
    card = card_json(item, false);
    put(card, "listItems", std::move(list));
    put(card, "currentCharge", jnum(item.extra.charge >= 0.0f && max > 0.0f ? item.extra.charge * 100.0f / max : 100.0));
    return true;
}

bool ItemMenus::recharge(const Row& row, int gem) {
    if (gem < 0 || size_t(gem) >= m_gems.size()) {
        return false;
    }
    game::Stack& item = row.owner->stacks()[row.stack];
    const Item* base = m_game.items().find(item.form);
    const float max = float(base->enchantment_amount ? base->enchantment_amount : item.extra.max_charge);
    const Row& g = m_gems[size_t(gem)];
    const float charge = m_game.perk_value(49, m_game.crafting().soul_charge(g.owner->stacks()[g.stack]));
    if (charge <= 0.0f) {
        return false;
    }
    m_game.use_skill(game::enchanting, m_game.settings().number("fSkillUsageRechargeMult", 0.0002f) * charge);
    const float now = item.extra.charge >= 0.0f ? item.extra.charge : max;
    item.extra.charge = now + charge >= max ? -1.0f : now + charge;
    const Item* gem_item = m_game.items().find(g.owner->stacks()[g.stack].form);
    if (gem_item->has_keyword(m_game.keyword("ReusableSoulGem"))) {
        g.owner->stacks()[g.stack].extra.soul = 0;
    } else {
        g.owner->remove_at(g.stack, 1);
    }
    m_game.sound("UIEnchantRecharge");
    return true;
}

// The game and what comes before it: the key, then the picks.
bool ItemMenus::open_lock(const game::Target& t) {
    game::Inventory& inv = m_game.player().inventory;
    const game::Settings& s = m_game.settings();
    if (t.key && inv.count(t.key) > 0) {
        const Item* key = m_game.items().find(t.key);
        std::string line = s.text("sOpenWithKey");
        if (const size_t at = line.find("%s"); key && at != std::string::npos) {
            line.replace(at, 2, key->name);
        }
        m_ui.notification(line, "UILockpickingUnlock");
        m_game.unlock(t.ref);
        return false;
    }
    if (m_game.lock_index(t.lock_level) >= 5) {
        m_ui.notification(s.text("sImpossibleLock"), "DRSLocked");
        return false;
    }
    if (inv.count(game::k_lockpick) == 0 && inv.count(game::k_skeleton_key) == 0) {
        m_ui.notification(s.text("sOutOfLockpicks"), "DRSLocked");
        return false;
    }
    m_lock = t;
    m_lock_reported = false;
    m_ui.open("Lockpicking Menu");
    return true;
}

ui::LockInfo ItemMenus::lock() {
    ui::LockInfo l;
    if (!m_lock.ref) {
        return l;
    }
    const game::Player& p = m_game.player();
    l.present = true;
    l.level = m_game.lock_index(m_lock.lock_level);
    l.skill = std::clamp(m_game.character().skill(game::lockpicking), 0.0f, 100.0f);
    l.xp_percent = m_game.skill_percent(game::lockpicking);
    l.next_is_blank = l.skill >= 100;
    l.picks = p.inventory.count(game::k_lockpick);
    const game::LockNumbers n = m_game.lock_numbers(l.level);
    l.size = n.size;
    l.partial = n.partial;
    l.break_seconds = n.break_seconds;
    l.health = m_game.pick_health;
    // The sweet spot's centre: uniform where it fits within the pick's range.
    const double range = 90.0 - l.size / 2.0;
    l.centre = (GameRandom::shared().next_unit() * 2.0 - 1.0) * range;
    // OnShow: EP 63's arc puts the pick near the sweet spot, EP 65 keeps it whole.
    if (const double arc = m_game.perk_value(63, 0.0f); arc > 0.0) {
        const double low = std::max(l.centre - arc, -range), high = std::min(l.centre + arc, range);
        l.start_pick = low + (high - low) * GameRandom::shared().next_unit();
    }
    l.unbreakable = m_game.perk_value(65, 0.0f) != 0.0f;
    l.skeleton_key = p.inventory.count(game::k_skeleton_key) > 0;
    return l;
}

// The game, on each break and on success: seen at an owned lock, it's a trespass, or an
// escape from jail. EP 86 starts from certainty.
void ItemMenus::check_lock_crime() {
    game::Crime& crime = m_game.crime();
    if (m_lock_reported || !m_lock.owner || !crime.witnesses) {
        return;
    }
    const auto around = crime.witnesses();
    const bool seen = std::any_of(around.begin(), around.end(), [](const game::Witness& w) { return w.detects; });
    if (!seen || GameRandom::shared().next_unit() >= m_game.perk_value(86, 1.0f)) {
        return;
    }
    m_lock_reported = true;
    if (crime.jail_days() > 0) {
        crime.set_escaping(true, false);
    } else {
        crime.report_trespass(m_lock.owner);
    }
    m_ui.close("Lockpicking Menu");
}

void ItemMenus::lock_event(std::string_view what, double pick_health) {
    static constexpr const char* k_usage[5] = {"fSkillUsageLockPickVeryEasy", "fSkillUsageLockPickEasy",
                                               "fSkillUsageLockPickAverage", "fSkillUsageLockPickHard",
                                               "fSkillUsageLockPickVeryHard"};
    static constexpr float k_defaults[5] = {1, 2, 3, 4, 5};
    m_game.pick_health = float(pick_health);
    if (what == "broken") {
        m_game.player().inventory.remove(game::k_lockpick, 1);
        m_game.use_skill(game::lockpicking, m_game.settings().number("fSkillUsageLockPickBroken", 1));
        check_lock_crime();
    } else if (what == "unlocked") {
        check_lock_crime();
        const int level = m_game.lock_index(m_lock.lock_level);
        m_game.use_skill(game::lockpicking, m_game.settings().number(k_usage[level], k_defaults[level]));
        m_game.unlock(m_lock.ref);
    }
    flush_notices();
}

bool ItemMenus::trade(const Row& row, int count, int unit_price) {
    const int total = count * unit_price;
    game::Player& p = m_game.player();
    game::Inventory& partner = m_game.container(m_vendor.chest ? m_vendor.chest : m_vendor_npc);
    game::Stack s = row.owner->stacks()[row.stack];
    const Item* item = m_game.items().find(s.form);
    const float xp = std::floor(float(m_game.base_value(*item)) * std::max(1.0f, s.extra.health) * 10.0f + 0.5f) / 10.0f;
    if (row.other) {
        if (total > p.inventory.gold()) {
            m_ui.notification(m_game.settings().text("sItemTooExpensive"), "UIMenuCancel");
            return false;
        }
        m_game.use_skill(game::speechcraft, xp);
        const int moved = row.owner->remove_at(row.stack, count);
        p.inventory.add(s.form, moved, s.extra);
        p.inventory.remove(game::k_gold, total);
        partner.add(game::k_gold, total);
        m_game.sound(m_game.item_sound(*item, true, false));
    } else {
        m_game.use_skill(game::speechcraft, xp);
        const int moved = row.owner->remove_at(row.stack, count);
        partner.add(s.form, moved, s.extra);
        const int paid = std::min(total, vendor_gold());
        // From the chest first, the rest from the vendor themself.
        const int from_chest = m_game.container(m_vendor.chest).remove(game::k_gold, paid);
        m_game.container(m_vendor_npc).remove(game::k_gold, paid - from_chest);
        p.inventory.add(game::k_gold, paid);
        m_game.sound(m_game.item_sound(*item, false, false));
    }
    return true;
}

bool ItemMenus::drop(const Row& row, int count) {
    game::Stack& s = row.owner->stacks()[row.stack];
    const Item* item = m_game.items().find(s.form);
    if (!item) {
        return false;
    }
    if (item->kind == ItemKind::key) {
        m_ui.notification(m_game.settings().text("sNoKeyDropWarning"), "UIMenuCancel");
        return false;
    }
    const game::Stack dropped{s.form, std::min(count, s.count), s.extra};
    row.owner->remove_at(row.stack, dropped.count);
    m_game.sound(m_game.item_sound(*item, false, false));
    if (on_drop) {
        on_drop(dropped);
    }
    return true;
}

void ItemMenus::open_book(uint32_t form, uint32_t ref, bool steal) {
    m_book = form;
    m_book_ref = ref;
    m_book_steal = steal;
    m_ui.open("Book Menu");
}

ui::BookInfo ItemMenus::book() {
    ui::BookInfo b;
    const Item* item = m_game.items().find(m_book);
    if (!item) {
        return b;
    }
    b.present = true;
    // The game: carriage returns become spaces, double quotes in a tag single ones.
    b.text = item->book_text;
    // a book in a quest's alias has that quest's text.
    if (game::QuestSystem* quests = m_game.quests(); quests && b.text.find("<Alias") != std::string::npos) {
        for (const auto& q : quests->all_quests()) {
            const bool holds = q->is_running() && std::ranges::any_of(q->filled_refs, [this](const auto& alias) {
                return m_book_ref ? alias.second == m_book_ref : m_game.base_of(alias.second) == m_book;
            });
            if (holds) {
                b.text = quests->quest_text(*q, q->instance, std::move(b.text));
                break;
            }
        }
    }
    bool in_tag = false;
    for (char& c : b.text) {
        in_tag = c == '<' || (in_tag && c != '>');
        c = c == '\r' ? ' ' : in_tag && c == '"' ? '\'' : c;
    }
    b.has_reference = m_book_ref != 0;
    b.steal = m_book_steal;
    b.model = item->inventory_model;
    if (m_game.read_book(*item, m_book_ref)) {
        m_game.player().inventory.remove(m_book, 1);
    }
    flush_notices();
    return b;
}

Json ItemMenus::player_info() {
    const game::Player& p = m_game.player();
    Json o = jobject();
    put(o, "encumbrance", jnum(std::floor(m_game.encumbrance() + 0.5f)));
    put(o, "maxEncumbrance", jnum(std::floor(m_game.carry_weight() + 0.5f)));
    put(o, "gold", jnum(p.inventory.gold()));
    put(o, "damage", jnum(m_game.total_damage()));
    put(o, "armor", jnum(m_game.total_armor()));
    if (m_game.survival()) { // BottomBar::UpdatePlayerInfo
        put(o, "warmth", jnum(std::floor(m_game.warmth() + 0.5f)));
    }
    const game::ActorValues& av = m_game.character().values();
    put(o, "health", jnum(av.current(game::av_health)));
    put(o, "maxHealth", jnum(av.maximum(game::av_health)));
    put(o, "healthColor", jstr("#FFFFFF"));
    put(o, "magicka", jnum(av.current(game::av_magicka)));
    put(o, "maxMagicka", jnum(av.maximum(game::av_magicka)));
    put(o, "magickaColor", jstr("#FFFFFF"));
    put(o, "stamina", jnum(av.current(game::av_stamina)));
    put(o, "maxStamina", jnum(av.maximum(game::av_stamina)));
    put(o, "staminaColor", jstr("#FFFFFF"));
    put(o, "shoutPoints", jnum(100));
    put(o, "maxShoutPoints", jnum(100));
    return o;
}

// ---- crafting --------------------------------------------------------------------------------------------------

bool ItemMenus::begin(const std::string& show_data) {
    for (const NamedStation& s : k_named_stations) {
        if (show_data == s.name) {
            m_furniture = s.furniture;
        }
    }
    if (!m_furniture) {
        return false;
    }
    m_station = m_game.crafting().station(m_furniture);
    m_furniture = 0;
    m_preview = -1;
    m_selected = 0;
    m_disenchanting = true;
    clear_choices();
    return m_station.bench_type != 0;
}

bool ItemMenus::has_skill() const { return m_station.skill >= game::k_first_skill; }

ui::CraftSkill ItemMenus::skill() const {
    const game::Player& p = m_game.player();
    ui::CraftSkill s;
    s.name = m_game.skill_name(m_station.skill);
    s.level = m_game.character().skill(m_station.skill);
    s.percent = m_game.skill_percent(m_station.skill);
    return s;
}

std::vector<ui::CraftEntry> ItemMenus::entries() {
    std::vector<ui::CraftEntry> out;
    game::Crafting& c = m_game.crafting();
    if (alchemy()) {
        m_ingredients.clear();
        for (const Stack& s : m_game.player().inventory.stacks()) {
            const Item* item = m_game.items().find(s.form);
            if (!item || item->kind != ItemKind::ingredient) {
                continue;
            }
            const auto it = std::find_if(m_ingredients.begin(), m_ingredients.end(),
                                         [&](const Ingredient& i) { return i.form == s.form; });
            if (it != m_ingredients.end()) {
                it->count += s.count;
            } else {
                m_ingredients.push_back({s.form, s.count});
            }
        }
        const auto name = [this](const Ingredient& i) -> const std::string& { return m_game.items().find(i.form)->name; };
        std::stable_sort(m_ingredients.begin(), m_ingredients.end(),
                         [&](const Ingredient& a, const Ingredient& b) { return name(a) < name(b); });
        categories(); // the filter flags go by them
        const game::Player& p = m_game.player();
        for (const Ingredient& i : m_ingredients) {
            const Item* item = m_game.items().find(i.form);
            uint32_t filter = 0;
            for (size_t k = 0; k < item->effects.size() && k < 4; ++k) {
                if ((p.known_effects(i.form) >> k) & 1) {
                    const auto at = std::find(m_alchemy_effects.begin(), m_alchemy_effects.end(), item->effects[k].effect);
                    const auto index = uint32_t(at - m_alchemy_effects.begin());
                    if (index < 255) {
                        filter |= (index + 1) << (8 * k);
                    }
                }
            }
            bool enabled = m_alchemy_chosen.empty();
            for (const uint32_t other : m_alchemy_chosen) {
                enabled = enabled || other == i.form || !p.incompatible.contains(game::pair_key(i.form, other));
            }
            const bool chosen =
                std::find(m_alchemy_chosen.begin(), m_alchemy_chosen.end(), i.form) != m_alchemy_chosen.end();
            out.push_back({item->name, double(i.count), double(filter), chosen ? 1.0 : 0.0, enabled});
        }
        return out;
    }
    if (enchanter()) {
        build_enchant_rows();
        for (const EnchantRow& r : m_enchant_rows) {
            const bool chosen =
                r.stack >= 0 ? r.stack == m_chosen_item || r.stack == m_chosen_gem
                             : std::any_of(m_chosen_enchants.begin(), m_chosen_enchants.end(), [&](const auto& c) {
                                   return c.enchantment == r.enchantment;
                               });
            out.push_back({r.text, double(r.count), double(r.flag), chosen ? 1.0 : 0.0, r.enabled});
        }
        return out;
    }
    if (smithing_bench(m_station.bench_type)) {
        m_temper = c.temper_list(m_station);
        const auto& stacks = m_game.player().inventory.stacks();
        for (size_t i = 0; i < m_temper.size(); ++i) {
            const game::TemperEntry& t = m_temper[i];
            Stack s = stacks[t.stack];
            if (int(i) == m_preview) {
                s.extra.health = t.new_health;
            }
            out.push_back({m_game.display_name(s), double(s.count), 1, int(i) == m_preview ? 1.0 : 0.0, t.enabled()});
        }
        return out;
    }
    m_recipes = c.recipes(m_station);
    for (const game::Recipe* r : m_recipes) {
        const Item* item = m_game.items().find(r->created);
        if (item) {
            out.push_back({item->name, double(r->count), double(c.category(*item)), 0, c.has_components(*r)});
        }
    }
    return out;
}

Json ItemMenus::card(int entry) {
    if (alchemy()) {
        Stack s;
        s.form = alchemy_result();
        if (!s.form && entry >= 0 && size_t(entry) < m_ingredients.size()) {
            s.form = m_ingredients[size_t(entry)].form;
            s.count = m_ingredients[size_t(entry)].count;
        }
        return card_json(s, false);
    }
    if (enchanter()) {
        const auto& stacks = m_game.player().inventory.stacks();
        if (!m_disenchanting && m_chosen_item >= 0 && !m_chosen_enchants.empty()) {
            Stack s = stacks[size_t(m_chosen_item)];
            s.count = 1;
            s.extra.enchantment = m_game.magic().create_enchantment(m_numbers.effects, enchant_weapon() ? 1 : 0,
                                                                    int32_t(m_numbers.required));
            s.extra.max_charge = int32_t(m_numbers.charge);
            if (!m_custom_name.empty()) {
                s.extra.name = m_custom_name;
            }
            Json c = card_json(s, false);
            const bool charged = m_numbers.required > 0 && m_numbers.charge > 0;
            for (auto& [name, v] : c.members) {
                if (name == "type") {
                    v = jnum(15);
                }
            }
            put(c, "totalCharges", jnum(charged ? std::floor(m_numbers.charge / m_numbers.required) : 0));
            put(c, "usedCharge",
                jnum(charged ? (m_numbers.charge - m_numbers.required) * 100.0 / m_numbers.charge : 0));
            put(c, "sliderShown", jbool(false));
            return c;
        }
        if (entry >= 0 && size_t(entry) < m_enchant_rows.size() && m_enchant_rows[size_t(entry)].stack >= 0) {
            return card_json(stacks[size_t(m_enchant_rows[size_t(entry)].stack)], false);
        }
        return jobject();
    }
    if (smithing_bench(m_station.bench_type)) {
        if (entry < 0 || size_t(entry) >= m_temper.size()) {
            return jobject();
        }
        Stack s = m_game.player().inventory.stacks()[m_temper[size_t(entry)].stack];
        if (entry == m_preview) {
            s.extra.health = m_temper[size_t(entry)].new_health;
        }
        return card_json(s, false);
    }
    if (entry < 0 || size_t(entry) >= m_recipes.size()) {
        return jobject();
    }
    Stack s;
    s.form = m_recipes[size_t(entry)]->created;
    return card_json(s, false);
}

std::string ItemMenus::model(int entry) {
    const Item* item = nullptr;
    if (alchemy()) {
        const uint32_t result = alchemy_result();
        item = m_game.items().find(result ? result
                                   : entry >= 0 && size_t(entry) < m_ingredients.size()
                                       ? m_ingredients[size_t(entry)].form
                                       : 0);
    } else if (enchanter()) {
        const int stack = m_chosen_item >= 0 ? m_chosen_item
                          : entry >= 0 && size_t(entry) < m_enchant_rows.size() ? m_enchant_rows[size_t(entry)].stack
                                                                                : -1;
        if (stack >= 0) {
            item = m_game.items().find(m_game.player().inventory.stacks()[size_t(stack)].form);
        }
    } else if (smithing_bench(m_station.bench_type)) {
        if (entry >= 0 && size_t(entry) < m_temper.size()) {
            item = m_game.items().find(m_game.player().inventory.stacks()[m_temper[size_t(entry)].stack].form);
        }
    } else if (entry >= 0 && size_t(entry) < m_recipes.size()) {
        item = m_game.items().find(m_recipes[size_t(entry)]->created);
    }
    return item ? item->inventory_model : std::string();
}

std::vector<ui::CraftIngredient> ItemMenus::ingredients(int entry) {
    std::vector<ui::CraftIngredient> out;
    const game::Recipe* r = nullptr;
    const auto text = [this](const char* gmst) { return m_game.settings().text(gmst); };
    if (alchemy()) {
        // The three slots: what's chosen, else sIngredient, sIngredient, sOptional.
        static constexpr const char* k_slots[3] = {"sIngredient", "sIngredient", "sOptional"};
        for (size_t i = 0; i < 3; ++i) {
            const Item* item = i < m_alchemy_chosen.size() ? m_game.items().find(m_alchemy_chosen[i]) : nullptr;
            out.push_back({item ? item->name : text(k_slots[i]), 1, item ? 1.0 : 0.0});
        }
        return out;
    }
    if (enchanter()) {
        // Item, each allowed enchantment, soul gem.
        const auto& stacks = m_game.player().inventory.stacks();
        out.push_back({m_chosen_item >= 0 ? m_game.display_name(stacks[size_t(m_chosen_item)]) : text("sItem"), 1,
                       m_chosen_item >= 0 ? 1.0 : 0.0});
        const size_t allowed = size_t(std::max(m_game.perk_value(80, 1.0f) + 0.5f, 1.0f));
        for (size_t i = 0; i < allowed; ++i) {
            std::string name = text("sEnchantment");
            if (i < m_chosen_enchants.size()) {
                const auto row = std::find_if(m_enchant_rows.begin(), m_enchant_rows.end(), [&](const EnchantRow& r2) {
                    return r2.enchantment == m_chosen_enchants[i].enchantment;
                });
                name = row != m_enchant_rows.end() ? row->text : name;
            }
            out.push_back({name, 1, i < m_chosen_enchants.size() ? 1.0 : 0.0});
        }
        out.push_back({m_chosen_gem >= 0 ? m_game.display_name(stacks[size_t(m_chosen_gem)]) : text("sSoulGem"), 1,
                       m_chosen_gem >= 0 ? 1.0 : 0.0});
        return out;
    }
    if (smithing_bench(m_station.bench_type)) {
        // The requirements are empty while a preview is shown.
        if (m_preview < 0 && entry >= 0 && size_t(entry) < m_temper.size()) {
            r = m_temper[size_t(entry)].recipe;
        }
    } else if (entry >= 0 && size_t(entry) < m_recipes.size()) {
        r = m_recipes[size_t(entry)];
    }
    if (r) {
        for (const forms::ItemCount& c : r->components) {
            const Item* item = m_game.items().find(c.item);
            out.push_back({item ? item->name : std::string(), double(std::max<int64_t>(c.count, 0)),
                           double(std::max(m_game.crafting().player_count(c.item), 0))});
        }
    }
    return out;
}

std::string ItemMenus::refusal(int entry) {
    if (alchemy()) {
        return {};
    }
    if (enchanter()) {
        return enchant_complete() ? std::string() : "sEnchantMustChooseItems";
    }
    if (smithing_bench(m_station.bench_type)) {
        const game::TemperEntry& t = m_temper.at(size_t(entry));
        if (t.conditions) {
            if (t.new_improvement - t.improvement < 1.0f) {
                return "sLackRequiredSkillToImprove";
            }
            return t.components ? std::string() : "sLackRequiredToImprove";
        }
        const Stack& s = m_game.player().inventory.stacks()[t.stack];
        const Item* item = m_game.items().find(s.form);
        return item && (item->enchantment || s.extra.enchantment) ? "sLackRequiredPerkToImproveMagical"
                                                                  : "sLackRequiredPerksToImprove";
    }
    return m_game.crafting().has_components(*m_recipes.at(size_t(entry))) ? std::string() : "sLackRequiredToCreate";
}

void ItemMenus::craft(int entry) {
    if (smithing_bench(m_station.bench_type)) {
        m_game.crafting().temper(m_temper.at(size_t(entry)), m_station);
    } else {
        const game::Recipe* r = m_recipes.at(size_t(entry));
        if (m_game.crafting().craft(*r, m_station)) {
            const Item* item = m_game.items().find(r->created);
            const std::string name = item ? item->name : std::string();
            const std::string& added = m_game.settings().text("sAddItemtoInventory");
            m_ui.notification(r->count > 1 ? std::format("{} ({}) {}", name, r->count, added)
                                           : std::format("{} {}", name, added));
        }
    }
}

std::string ItemMenus::craft_sound(int entry) {
    if (smithing_bench(m_station.bench_type)) {
        return m_station.bench_type == 2 ? "UISmithingImproveWeapon" : "UISmithingImproveArmor";
    }
    const Item* item = m_game.items().find(m_recipes.at(size_t(entry))->created);
    return item && item->kind == ItemKind::weapon  ? "UISmithingImproveWeapon"
           : item && item->kind == ItemKind::armor ? "UISmithingImproveArmor"
                                                   : "UISmithingCreateGeneric";
}

std::string ItemMenus::card_text(uint32_t form) const {
    Stack s;
    s.form = form;
    s.count = 1;
    std::string out = "{";
    for (const auto& [name, v] : card_json(s, false).members) {
        out += (out.size() > 1 ? ", " : "") + ui::player::json::quote(name) + ": ";
        out += v.kind == Json::string    ? ui::player::json::quote(v.s)
               : v.kind == Json::boolean ? std::string(v.b ? "true" : "false")
                                         : ui::player::json::number(v.n);
    }
    return out + "}";
}

// ---- the alchemy lab -------------------------------------------------------------------------------------------

std::vector<ui::CraftCategory> ItemMenus::categories() {
    std::vector<ui::CraftCategory> out;
    m_alchemy_effects.clear();
    if (!alchemy()) {
        return out;
    }
    const game::Player& p = m_game.player();
    std::vector<std::pair<uint32_t, int>> known;
    for (const Ingredient& i : m_ingredients) {
        const Item* item = m_game.items().find(i.form);
        for (size_t k = 0; k < item->effects.size(); ++k) {
            if ((p.known_effects(i.form) >> k) & 1) {
                const auto it = std::find_if(known.begin(), known.end(),
                                             [&](const auto& e) { return e.first == item->effects[k].effect; });
                if (it != known.end()) {
                    it->second += 1;
                } else {
                    known.emplace_back(item->effects[k].effect, 1);
                }
            }
        }
    }
    const auto name = [this](uint32_t form) {
        const game::MagicEffect* m = m_game.magic().effect(form);
        return m ? m->name : std::string();
    };
    std::stable_sort(known.begin(), known.end(), [&](const auto& a, const auto& b) { return name(a.first) < name(b.first); });
    for (size_t i = 0; i < known.size(); ++i) {
        m_alchemy_effects.push_back(known[i].first);
        out.push_back({name(known[i].first), double(i + 1), known[i].second > 1});
    }
    return out;
}

uint32_t ItemMenus::alchemy_result() {
    if (m_alchemy_chosen.size() < 2) {
        return 0;
    }
    const game::Brew b = m_game.crafting().brew(m_alchemy_chosen, true);
    return b.effects.empty() ? 0x0005661F /* "Unknown Potion" */ : m_game.crafting().potion_of(b);
}

bool ItemMenus::has_result() {
    return alchemy() ? m_alchemy_chosen.size() >= 2 : enchant_complete();
}

bool ItemMenus::anything_chosen() {
    return !m_alchemy_chosen.empty() || m_chosen_item >= 0 || m_chosen_gem >= 0 || !m_chosen_enchants.empty();
}

void ItemMenus::clear_choices() {
    m_alchemy_chosen.clear();
    m_chosen_item = m_chosen_gem = m_disenchant = -1;
    m_chosen_enchants.clear();
    m_chosen_flags.clear();
    m_custom_name.clear();
    m_numbers = {};
}

std::vector<std::string> ItemMenus::buttons() {
    const auto text = [this](const char* gmst) { return m_game.settings().text(gmst); };
    if (alchemy()) {
        const bool chosen = m_selected >= 0 && size_t(m_selected) < m_ingredients.size() &&
                            std::find(m_alchemy_chosen.begin(), m_alchemy_chosen.end(),
                                      m_ingredients[size_t(m_selected)].form) != m_alchemy_chosen.end();
        return {text(chosen ? "sRemove" : "sAdd"), text("sExit"), m_alchemy_chosen.empty() ? "" : text("sClearSelections"),
                text("sCraft")};
    }
    const bool rename = !m_disenchanting && enchant_complete() && m_custom_name.empty();
    return {text(m_disenchanting ? "sLearn" : "sSelect"), text("sExit"), rename ? text("sRenameItem") : "",
            m_disenchanting ? "" : text("sCraft")};
}

std::string ItemMenus::craft_question() { return enchanter() ? "sEnchantItem" : ""; }

ui::CraftOutcome ItemMenus::finish() {
    ui::CraftOutcome o;
    if (alchemy()) {
        const game::BrewResult r = m_game.crafting().make_potion(m_alchemy_chosen, m_station);
        o.sound = r.sound;
        o.message = r.message;
        if (r.potion) {
            const std::string& added = m_game.settings().text("sAddItemtoInventory");
            const Item* item = m_game.items().find(r.potion);
            m_ui.notification(r.count > 1 ? std::format("{} ({}) {}", item->name, r.count, added)
                                          : std::format("{} {}", item->name, added));
        }
        // After a success the same ingredients stay chosen while there are more of them.
        std::erase_if(m_alchemy_chosen, [this, &r](uint32_t form) {
            return !r.potion || m_game.player().inventory.count(form) == 0;
        });
        if (r.potion && m_alchemy_chosen.size() < 2) {
            m_alchemy_chosen.clear();
        }
        return o;
    }
    game::Inventory& inv = m_game.player().inventory;
    const uint32_t gem = inv.stacks()[size_t(m_chosen_gem)].form;
    const Stack gem_stack = inv.stacks()[size_t(m_chosen_gem)];
    const uint8_t soul = m_game.items().find(gem)->soul;
    m_game.crafting().enchant(size_t(m_chosen_item), m_numbers, enchant_weapon(), m_custom_name);
    // The reusable gem (Azura's Star) loses its soul; any other is used up.
    const Item* gem_item = m_game.items().find(gem);
    if (gem_item->has_keyword(m_game.keyword("ReusableSoulGem"))) {
        for (Stack& s : inv.stacks()) {
            if (s.form == gem && s.extra == gem_stack.extra) {
                s.extra.soul = 0;
            }
        }
    } else {
        inv.remove(gem, 1);
    }
    (void)soul;
    clear_choices();
    o.sound = "UIEnchantingItemCreate";
    return o;
}

// ---- the arcane enchanter --------------------------------------------------------------------------------------

void ItemMenus::build_enchant_rows() {
    m_enchant_rows.clear();
    const game::Player& p = m_game.player();
    const game::Crafting& c = m_game.crafting();
    const uint32_t disallow = m_game.keyword("MagicDisallowEnchanting");
    const auto& stacks = p.inventory.stacks();
    for (size_t i = 0; i < stacks.size(); ++i) {
        const Stack& s = stacks[i];
        const Item* item = m_game.items().find(s.form);
        if (!item || !m_game.listed(s)) {
            continue;
        }
        const uint32_t ench = s.extra.enchantment ? s.extra.enchantment : item->enchantment;
        EnchantRow r;
        r.stack = int(i);
        r.text = m_game.display_name(s);
        r.count = s.count;
        if (item->kind == ItemKind::armor && !item->has_keyword(disallow)) {
            r.flag = ench ? 8 : 4;
        } else if (item->kind == ItemKind::weapon && item->animation_type != 8 && !item->has_keyword(disallow)) {
            r.flag = ench ? 2 : 1;
        } else if (item->kind == ItemKind::soul_gem && c.soul_charge(s) > 0) {
            r.flag = 64;
            r.charge = c.soul_charge(s);
        } else {
            continue;
        }
        if (ench) {
            r.enabled = !m_game.magic().created(ench) &&
                        !p.known_enchantments.contains(m_game.magic().base_enchantment(ench));
        }
        m_enchant_rows.push_back(std::move(r));
    }
    for (const uint32_t ench : p.known_enchantments) {
        EnchantRow r;
        r.enchantment = ench;
        r.count = 0;
        const uint32_t type = m_game.magic().enchantment_cast_type(ench);
        r.flag = type == 0 ? 32 : type == 1 ? 16 : 0;
        const forms::FormEntry* e = m_game.db().find(ench);
        r.text = e ? std::string(forms::Form(m_game.db(), *e).name()) : std::string();
        if (r.flag) {
            m_enchant_rows.push_back(std::move(r));
        }
    }
    std::stable_sort(m_enchant_rows.begin(), m_enchant_rows.end(), [](const EnchantRow& a, const EnchantRow& b) {
        if (a.flag != b.flag) {
            return a.flag < b.flag;
        }
        return a.flag == 64 ? a.charge > b.charge : a.text < b.text;
    });
}

bool ItemMenus::enchant_weapon() const {
    if (m_chosen_item >= 0) {
        return m_game.items().find(m_game.player().inventory.stacks()[size_t(m_chosen_item)].form)->kind ==
               ItemKind::weapon;
    }
    return !m_chosen_flags.empty() && m_chosen_flags[0] == 16;
}

float ItemMenus::enchant_soul() const {
    const auto& stacks = m_game.player().inventory.stacks();
    if (m_chosen_gem >= 0) {
        return m_game.crafting().soul_charge(stacks[size_t(m_chosen_gem)]);
    }
    float best = 0;
    for (const Stack& s : stacks) {
        best = std::max(best, m_game.crafting().soul_charge(s));
    }
    return best;
}

void ItemMenus::update_enchant() {
    m_numbers = {};
    if (!m_chosen_enchants.empty()) {
        m_numbers = m_game.crafting().enchant_numbers(m_chosen_enchants, enchant_weapon(), enchant_soul());
    }
}

ui::CraftChoice ItemMenus::choose(int entry) {
    ui::CraftChoice c;
    if (alchemy()) {
        if (entry < 0 || size_t(entry) >= m_ingredients.size()) {
            return c;
        }
        const uint32_t form = m_ingredients[size_t(entry)].form;
        const auto it = std::find(m_alchemy_chosen.begin(), m_alchemy_chosen.end(), form);
        if (it != m_alchemy_chosen.end()) {
            m_alchemy_chosen.erase(it);
            c.sound = "UISelectOff";
        } else if (m_alchemy_chosen.size() < 3) {
            m_alchemy_chosen.push_back(form);
            c.sound = "UISelectOn";
        }
        return c;
    }
    if (entry < 0 || size_t(entry) >= m_enchant_rows.size()) {
        return c;
    }
    const EnchantRow& r = m_enchant_rows[size_t(entry)];
    if (r.flag == 2 || r.flag == 8) {
        if (!r.enabled) {
            c.notification = "sEnchantmentKnown";
            c.notification_sound = "UIActivateFail";
            return c;
        }
        clear_choices();
        m_disenchant = r.stack;
        c.sound = "UISelectOn";
        c.confirm = "sConfirmDisenchant";
        return c;
    }
    const bool weapon_side = r.flag == 1 || r.flag == 16;
    const bool armor_side = r.flag == 4 || r.flag == 32;
    // A weapon takes a weapon's enchantment and armor an armor's.
    if ((weapon_side && m_chosen_item >= 0 && !enchant_weapon()) ||
        (armor_side && m_chosen_item >= 0 && enchant_weapon()) ||
        (weapon_side && !m_chosen_flags.empty() && m_chosen_flags[0] == 32) ||
        (armor_side && !m_chosen_flags.empty() && m_chosen_flags[0] == 16)) {
        if (r.flag == 1 || r.flag == 4 || m_chosen_item >= 0) {
            c.notification = "sEnchantArmorIncompatible";
            c.notification_sound = "UIActivateFail";
            return c;
        }
    }
    c.sound = "UISelectOn";
    if (r.flag == 1 || r.flag == 4) {
        const bool same = m_chosen_item == r.stack;
        m_chosen_item = same ? -1 : r.stack;
        c.sound = same ? "UISelectOff" : c.sound;
    } else if (r.flag == 64) {
        const bool same = m_chosen_gem == r.stack;
        m_chosen_gem = same ? -1 : r.stack;
        c.sound = same ? "UISelectOff" : c.sound;
    } else {
        const auto it = std::find_if(m_chosen_enchants.begin(), m_chosen_enchants.end(),
                                     [&](const auto& e) { return e.enchantment == r.enchantment; });
        if (it != m_chosen_enchants.end()) {
            m_chosen_flags.erase(m_chosen_flags.begin() + (it - m_chosen_enchants.begin()));
            m_chosen_enchants.erase(it);
            c.sound = "UISelectOff";
        } else {
            const size_t limit = size_t(std::max(m_game.perk_value(80, 1.0f) + 0.5f, 1.0f));
            if (m_chosen_enchants.size() >= limit) {
                m_chosen_enchants.pop_back();
                m_chosen_flags.pop_back();
            }
            m_chosen_enchants.push_back({r.enchantment});
            m_chosen_flags.push_back(r.flag);
        }
    }
    update_enchant();
    if ((r.flag == 16 || r.flag == 64) && m_numbers.required > m_numbers.charge && !m_chosen_enchants.empty()) {
        for (game::EnchantChoice& e : m_chosen_enchants) {
            e.chosen = 1.0f;
        }
        m_numbers = m_game.crafting().enchant_numbers(m_chosen_enchants, enchant_weapon(), enchant_soul());
    }
    if (r.flag == 16 && !m_chosen_enchants.empty() && m_chosen_enchants.back().max > 1.0f) {
        const game::Settings& s = m_game.settings();
        const float max = m_chosen_enchants.back().max;
        m_slider_kept = m_chosen_enchants.back().chosen;
        c.slider_max = max;
        c.slider_value = m_slider_kept;
        c.slider_step = max > s.number("fEnchantingRuleOfTen", 120.0f)    ? 10
                        : max > s.number("fEnchantingRuleOfFive", 60.0f)  ? 5
                        : max > s.number("fEnchantingRuleOfTwo", 30.0f)   ? 2
                                                                          : 1;
    }
    return c;
}

void ItemMenus::confirmed(bool yes) {
    if (yes && m_disenchant >= 0) {
        const Stack& s = m_game.player().inventory.stacks()[size_t(m_disenchant)];
        const Item* item = m_game.items().find(s.form);
        const uint32_t ench = s.extra.enchantment ? s.extra.enchantment : item->enchantment;
        const forms::FormEntry* e = m_game.db().find(m_game.magic().base_enchantment(ench));
        std::string line = m_game.settings().text("sEnchantmentsLearned");
        if (const size_t at = line.find("%s"); at != std::string::npos && e) {
            line.replace(at, 2, forms::Form(m_game.db(), *e).name());
        }
        m_game.crafting().disenchant(size_t(m_disenchant));
        m_ui.host().ui_play_sound("UIEnchantingItemDestroy");
        m_ui.host().ui_play_sound("UIEnchantingLearnEffect");
        m_ui.message(line, {m_game.settings().text("sOk")}, nullptr);
    }
    m_disenchant = -1;
}

bool ItemMenus::set_slider(double value) {
    if (m_chosen_enchants.empty()) {
        return true;
    }
    const float old = m_chosen_enchants.back().chosen;
    m_chosen_enchants.back().chosen = float(value);
    update_enchant();
    if (m_numbers.charge < m_numbers.required) {
        m_chosen_enchants.back().chosen = old;
        update_enchant();
        return false;
    }
    return true;
}

void ItemMenus::slider_closed(bool accepted) {
    if (!accepted && !m_chosen_enchants.empty()) {
        m_chosen_enchants.back().chosen = m_slider_kept;
        update_enchant();
    }
}

std::string ItemMenus::rename_text() {
    if (!enchant_complete() || m_disenchanting || !m_custom_name.empty()) {
        return {};
    }
    return m_game.display_name(m_game.player().inventory.stacks()[size_t(m_chosen_item)]);
}

ui::CrosshairTarget ItemMenus::crosshair(const game::Target& t) const {
    ui::CrosshairTarget c;
    if (t.kind == game::TargetKind::none) {
        return c;
    }
    c.text = t.count > 1 ? std::format("{}\n{} ({})", t.verb, t.name, t.count) : std::format("{}\n{}", t.verb, t.name);
    if (!t.detail.empty()) {
        c.text += "\n" + t.detail;
    }
    c.hostile = t.hostile;
    if (t.item) {
        c.weight = t.item->weight;
        Stack s;
        s.form = t.base;
        c.value = m_game.value(s);
    }
    return c;
}

} // namespace spud::engine
