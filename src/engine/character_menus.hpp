#pragma once

// The game side of the Stats and Level Up menus: the character's
// state in the form the game writes it into the movies.

#include "game/face.hpp"
#include "game/game.hpp"
#include "ui/character.hpp"

namespace spud::engine {

class CharacterMenus final : public ui::CharacterSource, public ui::RaceSexSource {
public:
    using Sound = std::function<void(const char*)>;

    CharacterMenus(game::Game& game, Sound sound)
        : m_game(game), m_sound(std::move(sound)), m_editor(game.db(), game.settings(), game.character()) {}

    game::RaceSexEditor& editor() { return m_editor; }
    void set_face_host(game::FaceHost* host) {
        m_host = host;
        m_editor.set_host(host);
    }

    std::vector<ui::PerkTree> perk_trees(bool beast) override;
    bool beast_mode() override { return m_game.character().beast_form(); }
    bool werewolf() override;
    std::vector<ui::StatsSkill> skills() override;
    ui::StatsPlayer player() override;
    ui::StatsCard skill_card(int skill) override;
    ui::StatsCard perk_card(int skill, uint32_t perk, bool beast) override;
    std::string perk_label(uint32_t perk) override;
    int perk_points(bool beast) override;
    // A level waits for a sleep with Survival Mode on (StatsMenu::ProcessMessage).
    bool can_level_up() override {
        return !m_level_open && m_game.character().can_level_up() && (m_sleep_level || !m_game.survival());
    }
    bool sleep_level() const { return m_sleep_level; }
    void set_sleep_level(bool on) { m_sleep_level = on; }
    void count_level() override;
    void add_perk_point(bool beast) override;
    void choose_attribute(int actor_value) override;
    std::string attribute_name(int actor_value) override;
    bool can_take_perk(uint32_t perk) override { return m_game.character().can_take(perk); }
    void take_perk(uint32_t perk, bool beast) override;
    bool legendary_ready(int skill) override;
    void reset_legendary(int skill) override;

    // ui::RaceSexSource
    std::vector<ui::RaceSexRace> races() override;
    std::string player_name() override { return m_game.character().name; }
    std::vector<ui::RaceSexSlider> sliders(bool limited) override;
    void change_race(int index) override { m_editor.change_race(index); }
    void change_sex(bool female) override { m_editor.change_sex(female); }
    void change(const std::string& callback, double value, int id) override { m_editor.change(callback, float(value), id); }
    bool preset_would_lose_changes() override { return m_editor.preset_would_lose_changes(); }
    void apply_preset(int index) override { m_editor.apply_preset(index); }
    void zoom(bool face) override {
        if (m_host) {
            m_host->zoom(face);
        }
    }
    bool ready() override { return !m_host || m_host->face_ready(); }
    void set_name(const std::string& name) override { m_game.character().name = name; }

private:
    uint32_t beast_points_global() const;

    game::Game& m_game;
    Sound m_sound;
    bool m_level_open = false; // counted, the attribute not chosen yet
    bool m_sleep_level = false;
    game::RaceSexEditor m_editor;
    game::FaceHost* m_host = nullptr;
};

} // namespace spud::engine
