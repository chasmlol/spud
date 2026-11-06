// Alchemy and enchanting: what ingredients make, and what an enchantment does to an item.,
// "Alchemy", "Enchanting".

#include "game/crafting.hpp"

#include "formats/forms_typed.hpp"
#include "game/game.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace spud::game {

namespace {

constexpr uint32_t k_base_potion = 0x0005661F, k_base_poison = 0x0005629E; // default objects 200, 201

struct Pair {
    uint32_t ingredient;
    size_t slot;
    Effect effect;
    float cost;
    bool known;
};

float round_half_up(float v) { return std::floor(v + 0.5f); }

} // namespace

Brew Crafting::brew(const std::vector<uint32_t>& ingredients, bool preview) {
    Brew out;
    const Magic& magic = m_game.magic();
    const Player& player = m_game.player();
    std::vector<Pair> pairs;
    for (const uint32_t form : ingredients) {
        const Item* item = m_game.items().find(form);
        if (!item) {
            continue;
        }
        for (size_t i = 0; i < item->effects.size(); ++i) {
            pairs.push_back({form, i, item->effects[i], magic.effect_cost(item->effects[i]),
                             ((player.known_effects(form) >> i) & 1) != 0});
        }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.cost > b.cost; });
    std::vector<uint32_t> done;
    for (size_t i = 0; i < pairs.size(); ++i) {
        const Pair& p = pairs[i];
        if (std::find(done.begin(), done.end(), p.effect.effect) != done.end()) {
            continue;
        }
        const auto next = std::find_if(pairs.begin() + std::ptrdiff_t(i) + 1, pairs.end(), [&](const Pair& n) {
            return n.effect.effect == p.effect.effect && n.ingredient != p.ingredient && (!preview || n.known);
        });
        if (next == pairs.end() || (preview && !p.known)) {
            continue;
        }
        done.push_back(p.effect.effect);
        out.effects.push_back(p.effect);
    }
    if (out.effects.empty()) {
        return out;
    }
    const MagicEffect* first = magic.effect(out.effects[0].effect);
    out.poison = first && (first->flags & effect_hostile);
    const Settings& s = m_game.settings();
    const float factor = (m_game.character().skill(alchemy) * 0.01f * (s.number("fAlchemySkillFactor", 3.0f) - 1.0f) + 1.0f) *
                         s.number("fAlchemyIngredientInitMult", 4.0f);
    for (Effect& e : out.effects) {
        const MagicEffect* m = magic.effect(e.effect);
        if (!m) {
            continue;
        }
        if ((m->flags & effect_power_affects_magnitude) && !(m->flags & effect_no_magnitude)) {
            e.magnitude = round_half_up(m_game.perk_value(66, e.magnitude) * factor);
        }
        if ((m->flags & effect_power_affects_duration) && !(m->flags & effect_no_duration)) {
            e.duration = uint32_t(round_half_up(m_game.perk_value(66, float(e.duration)) * factor));
        }
    }
    out.cost = magic.cost(out.effects, false, 0);
    return out;
}

uint32_t Crafting::potion_of(const Brew& brew) {
    const auto same = [](const Effect& a, const Effect& b) {
        return a.effect == b.effect && a.magnitude == b.magnitude && a.area == b.area && a.duration == b.duration;
    };
    for (const auto& [form, effects] : m_created_potions) {
        if (std::equal(effects.begin(), effects.end(), brew.effects.begin(), brew.effects.end(), same) &&
            m_game.items().find(form)->poison() == brew.poison) {
            return form;
        }
    }
    const Item* base = m_game.items().find(brew.poison ? k_base_poison : k_base_potion);
    Item item = base ? *base : Item{};
    item.kind = ItemKind::potion;
    item.effects = brew.effects;
    item.effect_flags = brew.poison ? potion_poison : 0;
    item.value = 0;
    const MagicEffect* first = m_game.magic().effect(brew.effects[0].effect);
    item.name = std::format("{}{}", m_game.settings().text(brew.poison ? "sCreatedPoisonNamePrefix"
                                                                       : "sCreatedPotionNamePrefix"),
                            first ? first->name : std::string());
    const uint32_t form = m_game.items().create(std::move(item));
    m_created_potions.emplace_back(form, brew.effects);
    return form;
}

BrewResult Crafting::make_potion(const std::vector<uint32_t>& ingredients, const Station& station) {
    BrewResult r;
    const Brew made = brew(ingredients, false);
    Player& player = m_game.player();
    const Settings& s = m_game.settings();
    std::vector<std::string> learned;
    std::vector<bool> contributed(ingredients.size(), false);
    if (!made.effects.empty()) {
        const uint32_t potion = potion_of(made);
        r.potion = potion;
        r.count = int32_t(std::max(std::floor(m_game.perk_value(89, 1.0f) + 0.5f), 0.0f));
        r.sound = made.poison ? "UIAlchemyCreatePoison" : "UIAlchemyCreatePotion";
        player.inventory.add(potion, r.count);
        if (station.skill >= k_first_skill) {
            m_game.use_skill(alchemy, made.cost);
        }
        for (const Effect& e : made.effects) {
            for (size_t i = 0; i < ingredients.size(); ++i) {
                const Item* item = m_game.items().find(ingredients[i]);
                for (size_t k = 0; item && k < item->effects.size(); ++k) {
                    if (item->effects[k].effect != e.effect) {
                        continue;
                    }
                    contributed[i] = true;
                    uint16_t& known = player.ingredient_effects[ingredients[i]];
                    if (!(known & (1u << k))) {
                        known = uint16_t(known | (1u << k));
                        const MagicEffect* m = m_game.magic().effect(e.effect);
                        learned.push_back(std::format("<font color=\"#ffffff\">{}</font> "
                                                      "(<font color=\"#999999\">{}</font>)\n",
                                                      m ? m->name : std::string(), item->name));
                    }
                }
            }
        }
    } else {
        r.sound = "UIAlchemyFail";
        if (station.skill >= k_first_skill) {
            m_game.use_skill(alchemy, s.number("fWortFailSkillUseMagnitude", 1.0f));
        }
    }
    for (const uint32_t form : ingredients) {
        player.inventory.remove(form, 1);
    }
    // An ingredient that gave nothing is known not to mix with the others.
    for (size_t i = 0; i < ingredients.size() && ingredients.size() > 1; ++i) {
        for (size_t j = 0; !contributed[i] && j < ingredients.size(); ++j) {
            if (i != j) {
                player.incompatible.insert(pair_key(ingredients[i], ingredients[j]));
            }
        }
    }
    if (r.potion == 0) {
        r.message = s.text("sPotionCreationFailed");
    } else if (!learned.empty()) {
        const Item* potion = m_game.items().find(r.potion);
        r.message = std::format("{} {}\n\n{}:\n", potion->name, s.text("sCreated"), s.text("sDiscoveredEffects"));
        for (const std::string& line : learned) {
            r.message += line;
        }
    }
    return r;
}

float Crafting::soul_charge(const Stack& stack) const {
    const Item* item = m_game.items().find(stack.form);
    const uint8_t soul = stack.extra.soul ? stack.extra.soul : item ? item->soul : uint8_t(0);
    static constexpr const char* k_names[] = {"iSoulLevelValuePetty", "iSoulLevelValueLesser", "iSoulLevelValueCommon",
                                              "iSoulLevelValueGreater", "iSoulLevelValueGrand"};
    static constexpr float k_values[] = {250, 500, 1000, 2000, 3000};
    return soul >= 1 && soul <= 5 ? m_game.settings().number(k_names[soul - 1], k_values[soul - 1]) : -1.0f;
}

EnchantNumbers Crafting::enchant_numbers(std::vector<EnchantChoice>& chosen, bool weapon, float soul) {
    EnchantNumbers n;
    const Settings& s = m_game.settings();
    const float skill = m_game.character().skill(enchanting);
    const float ratio = soul / s.number("iSoulLevelValueGrand", 3000.0f);
    n.charge = std::max(m_game.perk_value(79, soul), 0.0f);
    float total = 0;
    for (EnchantChoice& c : chosen) {
        std::vector<Effect> effects = m_game.magic().effects_of(c.enchantment);
        Effect* top = nullptr;
        float top_cost = -1;
        for (Effect& e : effects) {
            if (const float cost = m_game.magic().effect_cost(e); cost > top_cost) {
                top_cost = cost;
                top = &e;
            }
        }
        if (top) {
            const MagicEffect* m = m_game.magic().effect(top->effect);
            const bool magnitude = m && (m->flags & effect_power_affects_magnitude);
            const bool duration = m && (m->flags & effect_power_affects_duration);
            const float base = magnitude ? top->magnitude : duration ? float(top->duration) : 1.0f;
            const float grown = base * (1.0f + skill / 100.0f * (s.number("fEnchantingSkillFactor", 3.0f) - 1.0f) *
                                                   std::max((skill - 14.0f) * 0.011764706f, 0.01f));
            const float max = std::floor(m_game.perk_value(77, grown));
            if (weapon) {
                c.chosen = max != c.max ? max : std::max(std::min(c.chosen, max), 1.0f);
            } else {
                c.chosen = std::max(max * ratio, 1.0f);
            }
            c.max = max;
            if (magnitude) {
                top->magnitude = c.chosen;
            } else if (duration) {
                top->duration = uint32_t(c.chosen + 0.5f);
            }
        }
        const float cost = m_game.magic().enchantment_cost(c.enchantment);
        total += c.max > 0.0f ? cost * c.chosen / c.max : cost;
        n.effects.insert(n.effects.end(), effects.begin(), effects.end());
    }
    if (weapon) {
        n.required = (1.0f - std::pow(skill * s.number("fEnchantingSkillCostBase", 0.005f),
                                      s.number("fEnchantingSkillCostScale", 0.5f))) *
                     (std::pow(total, s.number("fEnchantingCostExponent", 1.1f)) *
                      s.number("fEnchantingSkillCostMult", 3.0f));
    }
    return n;
}

void Crafting::enchant(size_t stack, const EnchantNumbers& numbers, bool weapon, const std::string& name) {
    Inventory& inv = m_game.player().inventory;
    Stack item = inv.stacks().at(stack);
    const uint32_t made = m_game.magic().create_enchantment(numbers.effects, weapon ? 1 : 0, int32_t(numbers.required));
    inv.remove_at(stack, 1);
    ItemExtra extra = item.extra;
    extra.enchantment = made;
    extra.max_charge = int32_t(numbers.charge);
    extra.charge = -1.0f;
    extra.name = name;
    inv.add(item.form, 1, extra);
    if (item.worn != Worn::none) {
        for (Stack& s : inv.stacks()) {
            if (s.form == item.form && s.extra == extra) {
                s.worn = item.worn;
            }
        }
    }
    m_game.use_skill(enchanting, 1.0f);
}

void Crafting::disenchant(size_t stack) {
    Inventory& inv = m_game.player().inventory;
    const Stack& s = inv.stacks().at(stack);
    const Item* item = m_game.items().find(s.form);
    const uint32_t ench = s.extra.enchantment ? s.extra.enchantment : item->enchantment;
    const float cost = m_game.magic().enchantment_cost(ench);
    m_game.player().known_enchantments.insert(m_game.magic().base_enchantment(ench));
    m_game.use_skill(enchanting, m_game.settings().number("fDisenchantSkillUse", 0.0025f) * cost);
    inv.remove_at(stack, 1);
}

} // namespace spud::game
