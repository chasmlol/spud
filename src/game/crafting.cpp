#include "game/crafting.hpp"

#include "formats/forms_typed.hpp"
#include "game/game.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace spud::game {

namespace {

using forms::Form;
using forms::Value;

constexpr uint32_t k_category_food = 0x20000, k_category_misc = 0x40000000;

// The menu's categories by the keywords that fall into them: the default objects "Keyword - ..." the constructor
// maps, by the EditorIDs those objects hold in the masters (DOBJ). Flag 1 << index.
struct KeywordCategory {
    std::string_view keyword;
    uint32_t index;
};
constexpr KeywordCategory k_keyword_categories[] = {
    {"WeapMaterialWood", 0},
    {"ArmorMaterialHide", 1},
    {"WeapMaterialDraugr", 2}, {"WeapMaterialDraugrHoned", 2},
    {"WeapMaterialIron", 3}, {"ArmorMaterialIron", 3}, {"ArmorMaterialIronBanded", 3},
    {"ArmorMaterialStudded", 4},
    {"WeapMaterialImperial", 5}, {"ArmorMaterialImperialLight", 5}, {"ArmorMaterialImperialHeavy", 5},
    {"ArmorMaterialImperialStudded", 5},
    {"WeapMaterialSteel", 6}, {"ArmorMaterialScaled", 6}, {"ArmorMaterialSteel", 6}, {"ArmorMaterialSteelPlate", 6},
    {"ArmorMaterialLeather", 7}, {"ArmorMaterialStormcloak", 7},
    {"WeapMaterialDwarven", 8}, {"ArmorMaterialDwarven", 8},
    {"WeapMaterialElven", 9}, {"ArmorMaterialElven", 9}, {"ArmorMaterialElvenGilded", 9},
    {"WeapMaterialFalmer", 10}, {"WeapMaterialFalmerHoned", 10},
    {"WeapMaterialOrcish", 11}, {"ArmorMaterialOrcish", 11},
    {"WeapMaterialEbony", 12}, {"ArmorMaterialEbony", 12},
    {"WeapMaterialGlass", 13}, {"ArmorMaterialGlass", 13},
    {"ArmorMaterialDragonplate", 14}, {"ArmorMaterialDragonscale", 14}, {"DLC1WeapMaterialDragonbone", 14},
    {"WeapMaterialDaedric", 15}, {"ArmorMaterialDaedric", 15},
    {"ArmorJewelry", 16},
    {"BYOHHouseCraftingCategoryBuilding", 18}, {"BYOHHouseCraftingCategoryContainers", 19},
    {"BYOHHouseCraftingCategoryFurniture", 20}, {"BYOHHouseCraftingCategoryWeaponRacks", 21},
    {"BYOHHouseCraftingCategoryShelf", 22}, {"BYOHHouseCraftingCategoryExterior", 23},
    {"BYOHHouseCraftingCategorySmithing", 24},
    {"DLC2ArmorMaterialBonemoldLight", 26}, {"DLC2ArmorMaterialBonemoldHeavy", 26},
    {"DLC2ArmorMaterialChitinLight", 27}, {"DLC2ArmorMaterialChitinHeavy", 27},
    {"DLC2WeaponMaterialNordic", 28}, {"DLC2ArmorMaterialNordicLight", 28}, {"DLC2ArmorMaterialNordicHeavy", 28},
    {"DLC2WeaponMaterialStalhrim", 29}, {"DLC2ArmorMaterialStalhrimLight", 29},
    {"DLC2ArmorMaterialStalhrimHeavy", 29},
};

} // namespace

Station Crafting::station(uint32_t furniture) const {
    Station s;
    s.furniture = furniture;
    const forms::Database& db = m_game.db();
    if (const forms::FormEntry* e = db.find(furniture)) {
        const Form f(db, *e);
        s.name = f.name();
        s.keywords = f.keywords();
        if (const Value* v = f.get("WBDT.bench_type")) {
            s.bench_type = int(v->integer);
        }
        if (const Value* v = f.get("WBDT.uses_skill")) {
            s.skill = int(v->integer);
        }
    }
    return s;
}

void Crafting::load_recipes() {
    if (!m_recipes.empty()) {
        return;
    }
    const forms::Database& db = m_game.db();
    for (const forms::FormEntry* e : db.of_type(forms::parse_sig("COBJ").value())) {
        const Form f(db, *e);
        Recipe r;
        r.form = e->form_id;
        const Value* created = f.get("CNAM");
        const Value* bench = f.get("BNAM");
        r.created = created && created->kind == Value::Kind::form ? created->form : 0;
        r.bench = bench && bench->kind == Value::Kind::form ? bench->form : 0;
        if (const Value* n = f.get("NAM1")) {
            r.count = int32_t(n->integer);
        }
        r.components = forms::container_items(f);
        if (r.created) {
            m_recipes.push_back(std::move(r));
        }
    }
    for (const forms::FormEntry* e : db.of_type(forms::parse_sig("KYWD").value())) {
        const std::string_view id = db.editor_id(*e);
        for (const KeywordCategory& k : k_keyword_categories) {
            if (k.keyword == id) {
                m_category_of_keyword[e->form_id] = 1u << k.index;
            }
        }
    }
}

std::vector<const Recipe*> Crafting::recipes(const Station& station) {
    load_recipes();
    const forms::Database& db = m_game.db();
    std::vector<const Recipe*> out;
    for (const Recipe& r : m_recipes) {
        if (std::find(station.keywords.begin(), station.keywords.end(), r.bench) == station.keywords.end()) {
            continue;
        }
        if (m_game.recipe_passes(r.form, 0, false)) {
            out.push_back(&r);
        }
    }
    const auto name = [this](const Recipe* r) -> std::string_view {
        const Item* i = m_game.items().find(r->created);
        return i ? std::string_view(i->name) : std::string_view();
    };
    std::stable_sort(out.begin(), out.end(), [&](const Recipe* a, const Recipe* b) {
        const std::string_view x = name(a), y = name(b);
        return x != y ? x < y : a->count < b->count;
    });
    return out;
}

uint32_t Crafting::category(const Item& created) const {
    if (created.food()) {
        return k_category_food;
    }
    for (const uint32_t k : created.keywords) {
        if (const auto it = m_category_of_keyword.find(k); it != m_category_of_keyword.end()) {
            return it->second;
        }
    }
    return k_category_misc;
}

int32_t Crafting::player_count(uint32_t item) const { return m_game.player().inventory.count(item); }

bool Crafting::has_components(const Recipe& recipe) const {
    return std::all_of(recipe.components.begin(), recipe.components.end(), [this](const forms::ItemCount& c) {
        return player_count(c.item) >= std::abs(c.count);
    });
}

bool Crafting::craft(const Recipe& recipe, const Station& station) {
    if (!has_components(recipe)) {
        return false;
    }
    Inventory& inv = m_game.player().inventory;
    for (const forms::ItemCount& c : recipe.components) {
        inv.remove(c.item, int32_t(std::abs(c.count)));
    }
    inv.add(recipe.created, recipe.count);
    if (station.skill >= k_first_skill && station.skill < k_first_skill + k_skill_count) {
        // Update.esm's 3, 0.65 and 25 over the game's 1, 0.5 and 0.
        const Settings& s = m_game.settings();
        const Item* item = m_game.items().find(recipe.created);
        const float value = item ? float(m_game.base_value(*item)) : 0.0f;
        const float total = value <= 0.0f ? 1.0f : value * float(recipe.count);
        m_game.use_skill(station.skill, s.number("fConstructibleSkillUseMult", 1.0f) *
                                                std::pow(total, s.number("fConstructibleSkilluseExp", 0.5f)) +
                                            s.number("fConstructibleSkillUseConst", 0.0f));
    }
    return true;
}

float Crafting::temper_health(float skill) const {
    const Settings& s = m_game.settings();
    const float v1 = s.number("fHealthDataValue1", 1.1f), v6 = s.number("fHealthDataValue6", 1.6f);
    return std::max(((skill - 15.0f) * s.number("fSmithingConditionFactor", 0.5825f) + 1.0f) * 0.01f * (v6 - v1) + v1,
                    1.0f);
}

float Crafting::improvement(const Item& item, float health) const {
    const Settings& s = m_game.settings();
    const float v1 = s.number("fHealthDataValue1", 1.1f), v6 = s.number("fHealthDataValue6", 1.6f);
    const float divisor = v6 == v1 ? 1.0f : v6 - v1;
    if (item.kind == ItemKind::weapon) {
        return std::max((health - v1) / divisor * (s.number("fSmithingWeaponMax", 10.0f) - 1.0f) + 1.0f, 0.0f);
    }
    float value = (health - v1) / divisor * (s.number("fSmithingArmorMax", 10.0f) - 1.0f) + 1.0f;
    if (item.has_keyword(m_game.keyword("ArmorCuirass"))) {
        value *= 2.0f;
    }
    return std::max(value, 0.0f);
}

std::vector<TemperEntry> Crafting::temper_list(const Station& station) {
    load_recipes();
    const ItemKind kind = station.bench_type == 2 ? ItemKind::weapon : ItemKind::armor;
    std::unordered_map<uint32_t, const Recipe*> map;
    for (const Recipe& r : m_recipes) {
        if (std::find(station.keywords.begin(), station.keywords.end(), r.bench) != station.keywords.end()) {
            map[r.created] = &r;
        }
    }
    const forms::Database& db = m_game.db();
    Player& player = m_game.player();
    const Settings& s = m_game.settings();
    const float v1 = s.number("fHealthDataValue1", 1.1f);
    std::vector<TemperEntry> out;
    const auto& stacks = player.inventory.stacks();
    for (size_t i = 0; i < stacks.size(); ++i) {
        const Stack& st = stacks[i];
        const Item* item = m_game.items().find(st.form);
        if (!item || item->kind != kind) {
            continue;
        }
        const Recipe* recipe = recipe_for(*item, map);
        if (!recipe) {
            continue;
        }
        TemperEntry t;
        t.stack = i;
        t.recipe = recipe;
        t.health = std::max(st.extra.health, 1.0f);
        const float base = temper_health(m_game.character().skill(smithing));
        float excess = base - v1;
        t.new_health = base;
        if (excess > 0.0f) {
            excess = m_game.perk_value(76, excess, {st.form}); // Modify Tempering Health
            t.new_health = std::floor((std::max(excess, 0.0f) + v1) * 10.0f + 0.5f) / 10.0f;
        }
        t.improvement = improvement(*item, t.health);
        t.new_improvement = improvement(*item, t.new_health);
        t.components = has_components(*recipe);
        const bool enchanted = item->enchantment != 0 || st.extra.enchantment != 0;
        t.conditions = m_game.recipe_passes(recipe->form, st.form, enchanted);
        out.push_back(t);
    }
    std::stable_sort(out.begin(), out.end(), [&](const TemperEntry& a, const TemperEntry& b) {
        return m_game.display_name(stacks[a.stack]) < m_game.display_name(stacks[b.stack]);
    });
    return out;
}

const Recipe* Crafting::recipe_for(const Item& item, const std::unordered_map<uint32_t, const Recipe*>& map) const {
    if (const auto it = map.find(item.form); it != map.end()) {
        return it->second;
    }
    if (const auto it = map.find(item.template_form); item.template_form && it != map.end()) {
        return it->second;
    }
    return nullptr;
}

bool Crafting::temper(const TemperEntry& entry, const Station& station) {
    if (!entry.enabled()) {
        return false;
    }
    Inventory& inv = m_game.player().inventory;
    Stack stack = inv.stacks()[entry.stack];
    const Item* item = m_game.items().find(stack.form);
    for (const forms::ItemCount& c : entry.recipe->components) {
        inv.remove(c.item, int32_t(std::abs(c.count)));
    }
    // One of a stack goes to its own entry, keeping its other extras.
    for (size_t i = 0; i < inv.stacks().size(); ++i) {
        Stack& s = inv.stacks()[i];
        if (s.form == stack.form && s.extra == stack.extra) {
            const Worn worn = s.worn;
            inv.remove_at(i, 1);
            ItemExtra extra = stack.extra;
            extra.health = entry.new_health;
            inv.add(stack.form, 1, extra);
            if (worn != Worn::none) {
                for (Stack& n : inv.stacks()) {
                    if (n.form == stack.form && n.extra == extra) {
                        n.worn = worn;
                    }
                }
            }
            break;
        }
    }
    if (station.skill >= k_first_skill && station.skill < k_first_skill + k_skill_count && item) {
        // Update.esm's 8, 0.6, 25 and 1.
        const Settings& s = m_game.settings();
        const float value = std::max(float(m_game.base_value(*item)), 1.0f);
        m_game.use_skill(smithing, (s.number("fTemperingSkillUseItemValMult", 1.0f) *
                                        std::pow(value, s.number("fTemperingSkillUseItemValExp", 1.0f)) +
                                    s.number("fTemperingSkillUseItemValConst", 0.0f)) *
                                       ((entry.new_health - entry.health) * s.number("fTemperingSkillUseMult", 1.0f)));
    }
    return true;
}

} // namespace spud::game
