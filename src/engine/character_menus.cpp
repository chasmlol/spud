#include "engine/character_menus.hpp"

#include "formats/forms_typed.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace spud::engine {

namespace {

using game::av_health;
using game::av_magicka;
using game::av_stamina;
using game::Modifier;

// The ring's skills (the order of the capture stats-perktrees).
constexpr int k_ring[18] = {23, 10, 11, 9, 7, 6, 8, 12, 15, 14, 13, 17, 16, 21, 19, 20, 22, 18};
constexpr int k_beast[2] = {game::av_werewolf_perks, game::av_vampire_perks};
constexpr uint16_t k_get_base_actor_value = 277;

// the modifiers other than damage.
std::string meter_color(const game::ActorValues& v, int av) {
    const float change = v.current(av) - (v.base(av) + v.modifier(av, Modifier::damage));
    return change < -0.001f ? "#FF0000" : change > 0.001f ? "#189515" : "#FFFFFF";
}

std::string race_name(const game::Game& g) {
    const forms::FormEntry* e = g.db().find(g.character().race());
    return e ? std::string(forms::Form(g.db(), *e).name()) : std::string();
}

ui::StatsAttribute attribute(const game::ActorValues& v, int av) {
    const float current = v.current(av);
    return {std::floor(std::max(current, 0.0f)), std::floor(current - v.modifier(av, Modifier::damage)), meter_color(v, av)};
}

} // namespace

std::vector<ui::PerkTree> CharacterMenus::perk_trees(bool beast) {
    const game::Character& c = m_game.character();
    std::vector<ui::PerkTree> out;
    const std::span<const int> list = beast ? std::span<const int>(k_beast) : std::span<const int>(k_ring);
    for (const int av : list) {
        ui::PerkTree t;
        t.actor_value = uint32_t(av);
        if (const game::PerkTree* tree = c.perks().tree(av)) {
            t.max_grid_x = tree->max_grid_x;
            for (const game::PerkTreeNode& n : tree->nodes) {
                ui::PerkTree::Node node;
                node.index = n.index;
                node.perk = n.perk;
                node.x = n.x;
                node.y = n.y;
                node.h = n.h;
                node.v = n.v;
                node.children = n.children;
                node.state = n.perk ? c.perk_state(n.perk) : 0;
                t.nodes.push_back(std::move(node));
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

bool CharacterMenus::werewolf() {
    const forms::FormEntry* e = m_game.db().find(m_game.character().race());
    return e && m_game.db().editor_id(*e) == "WerewolfBeastRace"; // default object 345
}

std::vector<ui::StatsSkill> CharacterMenus::skills() {
    const game::Character& c = m_game.character();
    std::vector<ui::StatsSkill> out;
    for (const int av : k_ring) {
        out.push_back({std::floor(c.values().current(av)), c.perks().skill(av).name, c.skill_percent(av),
                       meter_color(c.values(), av), double(c.progress(av).legendary)});
    }
    return out;
}

ui::StatsPlayer CharacterMenus::player() {
    const game::Character& c = m_game.character();
    ui::StatsPlayer p;
    p.name = c.name;
    p.level = c.level();
    p.percent = c.experience() / c.experience_threshold() * 100.0f;
    p.race = race_name(m_game);
    p.magicka = attribute(c.values(), av_magicka);
    p.health = attribute(c.values(), av_health);
    p.stamina = attribute(c.values(), av_stamina);
    return p;
}

ui::StatsCard CharacterMenus::skill_card(int skill) {
    const game::Character& c = m_game.character();
    const int av = k_ring[skill];
    ui::StatsCard card;
    card.description = c.perks().skill(av).description;
    card.level = c.values().current(av);
    card.legendary = c.progress(av).legendary;
    return card;
}

ui::StatsCard CharacterMenus::perk_card(int skill, uint32_t perk, bool beast) {
    // the perk branch.
    const game::Character& c = m_game.character();
    const game::Settings& s = m_game.settings();
    const int av = beast ? k_beast[skill] : k_ring[skill];
    const game::Character::PerkWalk w = c.walk_perk(perk);
    ui::StatsCard card;
    card.perk = true;
    if (!w.next) {
        return card;
    }
    card.name = w.next->name;
    if (w.owned > 0 && w.owned < w.total && c.perk_points() > 0) {
        card.description = std::format("{} {}", s.text("sStatsNextRank"), w.next->description);
    } else if (w.total > 1 && w.owned < w.total && w.prev) {
        card.description = w.prev->description;
    } else {
        card.description = w.next->description;
    }
    if (!beast) {
        // the skill's name isn't in it.
        for (const game::ConditionItem& item : w.next->conditions) {
            if (item.function != k_get_base_actor_value || item.param1 >= uint32_t(game::k_actor_value_count)) {
                continue;
            }
            float value = item.value;
            if (item.flags & game::condition_global) {
                const forms::FormEntry* e = m_game.db().find(item.value_global);
                value = e ? forms::global_value(forms::Form(m_game.db(), *e)).value_or(0.0f) : 0.0f;
            }
            card.requirements += card.requirements.empty() ? s.text("sRequirementsText") : std::string(",");
            card.requirements += std::format(" <font face='$EverywhereMediumFont' size='24' color='#FFFFFF'>{}</font>", int(value));
        }
        card.meter = c.progress(av).xp;
        card.level = c.progress(av).level;
        card.skill = std::format("{} <font face='$EverywhereBoldFont' size='24' color='#FFFFFF'>{:.0f}</font>",
                                 c.perks().skill(av).name, c.values().base(av));
        if (!card.requirements.empty()) {
            card.skill += "  ";
        }
    } else {
        card.skill = c.perks().skill(av).name;
    }
    return card;
}

std::string CharacterMenus::perk_label(uint32_t perk) {
    const game::Character& c = m_game.character();
    const game::Perk* first = c.perks().find(perk);
    if (!first) {
        return {};
    }
    const game::Character::PerkWalk w = c.walk_perk(perk);
    const char* color = w.next && c.perk_conditions(*w.next) ? "#FFFFFF" : "#999999";
    std::string name = first->name;
    if (first->next) {
        name += std::format(" ({}/{})", w.owned, w.total);
    }
    return std::format("<font color='{}'>{}</font>", color, name);
}

uint32_t CharacterMenus::beast_points_global() const {
    // default object 326 for the Werewolf Race's player, else 327.
    const forms::FormEntry* race = m_game.db().find(m_game.character().race());
    const bool wolf = race && m_game.db().editor_id(*race) == "WerewolfBeastRace";
    for (const forms::FormEntry* e : m_game.db().of_type(forms::sig("GLOB"))) {
        if (m_game.db().editor_id(*e) == (wolf ? "DLC1WerewolfPerkPoints" : "DLC1VampirePerkPoints")) {
            return e->form_id;
        }
    }
    return 0;
}

int CharacterMenus::perk_points(bool beast) {
    const game::Character& c = m_game.character();
    if (!beast) {
        return c.perk_points();
    }
    // The points, up to what's left to take in the two trees.
    int takeable = 0;
    for (const int av : k_beast) {
        if (const game::PerkTree* tree = c.perks().tree(av)) {
            for (const game::PerkTreeNode& n : tree->nodes) {
                const game::Character::PerkWalk w = c.walk_perk(n.perk);
                takeable += w.total - w.owned;
            }
        }
    }
    const uint32_t global = beast_points_global();
    const float points = global && c.events.read_global ? c.events.read_global(global) : 0.0f;
    return takeable ? std::min(int(points), takeable) : 0;
}

void CharacterMenus::count_level() {
    m_level_open = true;
    m_sound("UILevelUp");
    m_game.character().count_level();
}

void CharacterMenus::add_perk_point(bool beast) {
    game::Character& c = m_game.character();
    if (const uint32_t global = beast ? beast_points_global() : 0; global && c.events.write_global) {
        c.events.write_global(global, c.events.read_global(global) + 1.0f);
    } else if (!beast) {
        c.add_perk_points(1);
    }
}

void CharacterMenus::choose_attribute(int actor_value) {
    m_game.character().choose_attribute(actor_value);
    m_level_open = false;
}

std::string CharacterMenus::attribute_name(int actor_value) {
    const uint32_t avif = 0x3E8 + uint32_t(actor_value - av_health); // AVHealth 000003E8, in the attributes' order
    const forms::FormEntry* e = m_game.db().find(avif);
    return e ? std::string(forms::Form(m_game.db(), *e).name()) : std::string();
}

void CharacterMenus::take_perk(uint32_t perk, bool beast) {
    game::Character& c = m_game.character();
    c.take_perk(perk);
    if (const uint32_t global = beast ? beast_points_global() : 0; global && c.events.write_global) {
        c.events.write_global(global, c.events.read_global(global) - 1.0f);
    }
}

std::vector<ui::RaceSexRace> CharacterMenus::races() {
    std::vector<ui::RaceSexRace> out;
    const int current = m_editor.current_race();
    for (const game::RaceSexEditor::RaceChoice& r : m_editor.races()) {
        out.push_back({r.name, r.description, int(out.size()) == current});
    }
    return out;
}

std::vector<ui::RaceSexSlider> CharacterMenus::sliders(bool limited) {
    m_editor.build_sliders();
    std::vector<ui::RaceSexSlider> out;
    for (const game::RaceSexEditor::Slider& s : m_editor.sliders()) {
        if (s.id == -1 && limited) {
            continue;
        }
        out.push_back({s.label, s.callback, s.filter, s.id, s.min, s.max, s.position, s.step});
    }
    return out;
}

bool CharacterMenus::legendary_ready(int skill) { return m_game.character().legendary_ready(k_ring[skill]); }

void CharacterMenus::reset_legendary(int skill) { m_game.character().reset_legendary(k_ring[skill]); }

} // namespace spud::engine
