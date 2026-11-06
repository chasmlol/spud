#pragma once

// The engine data behind the Stats and Level Up menus: what the game's character system hands them, in the form the
// executable writes it into the movies. The game implements CharacterSource and returns it
// from UiHost::ui_character; until there is a game the menus replay data captured from it.

#include <cstdint>
#include <string>
#include <vector>

namespace spud::ui {

// One skill's perk tree as the actor value holds it (ActorValueInfo).
struct PerkTree {
    struct Node {
        uint32_t index = 0;             // INAM
        uint32_t perk = 0;              // form ID, 0 for the root
        float x = 0, y = 0;             // XNAM, YNAM (grid)
        float h = 0, v = 0;             // HNAM, VNAM
        std::vector<uint32_t> children; // INAM indices
        // The game: 2 the player has the perk, 1 available (level and conditions),
        // 0 not.
        int state = 0;
    };
    uint32_t actor_value = 0;
    int max_grid_x = 0; // ActorValueInfo
    std::vector<Node> nodes;
};

// SkillStatsA: five values a skill.
struct StatsSkill {
    double level = 0;
    std::string name;
    double percent = 0;
    std::string color;
    double legendary = 0;
};

struct StatsAttribute {
    double current = 0, maximum = 0;
    std::string color;
};

// SetPlayerInfo's thirteen values.
struct StatsPlayer {
    std::string name;
    double level = 1, percent = 0;
    std::string race;
    StatsAttribute magicka, health, stamina;
};

// SetDescriptionCard's eight values.
struct StatsCard {
    bool perk = false;
    std::string name;
    double meter = 0;
    std::string description, requirements;
    double level = 0;
    std::string skill;
    double legendary = 0;
};

// The RaceSex Menu's data: SetRaceList's triples and SetOptionSliders' eights.
struct RaceSexRace {
    std::string name, description;
    bool current = false;
};
struct RaceSexSlider {
    std::string label, callback;
    int filter = 0, id = 0;
    double min = 0, max = 0, position = 0, step = 1;
};

class RaceSexSource {
public:
    virtual ~RaceSexSource() = default;
    virtual std::vector<RaceSexRace> races() = 0;
    virtual std::string player_name() = 0;
    virtual std::vector<RaceSexSlider> sliders(bool limited) = 0;
    virtual void change_race(int index) = 0;
    virtual void change_sex(bool female) = 0;
    virtual void change(const std::string& callback, double value, int id) = 0;
    virtual bool preset_would_lose_changes() = 0;
    virtual void apply_preset(int index) = 0;
    virtual void zoom(bool face) = 0;
    virtual bool ready() = 0; // the body shows the last change: the loading icon can go
    virtual void set_name(const std::string& name) = 0;
};

class CharacterSource {
public:
    virtual ~CharacterSource() = default;

    // The trees in the menu's order, or the two beast trees; states filled in.
    virtual std::vector<PerkTree> perk_trees(bool beast) = 0;
    virtual bool beast_mode() = 0;       // the player is in beast form: the menu shows the beast trees
    virtual bool werewolf() = 0;         // ... the Werewolf Race's tree first
    virtual std::vector<StatsSkill> skills() = 0;
    virtual StatsPlayer player() = 0;
    virtual StatsCard skill_card(int skill) = 0;
    virtual StatsCard perk_card(int skill, uint32_t perk, bool beast) = 0;
    // A star's name in its <font> tag.
    virtual std::string perk_label(uint32_t perk) = 0;

    virtual int perk_points(bool beast) = 0;
    virtual bool can_level_up() = 0;
    virtual void count_level() = 0;
    virtual void add_perk_point(bool beast) = 0;
    virtual void choose_attribute(int actor_value) = 0;
    virtual std::string attribute_name(int actor_value) = 0;

    virtual bool can_take_perk(uint32_t perk) = 0;
    virtual void take_perk(uint32_t perk, bool beast) = 0;

    virtual bool legendary_ready(int skill) = 0;
    virtual void reset_legendary(int skill) = 0;
};

} // namespace spud::ui
