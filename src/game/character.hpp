#pragma once

// The player as a character: actor values, skills and experience, levels, perks and their entry points, spells and the
// effects running on them, race. Over the records and the Conditions evaluator; knows nothing of the renderer or the UI.
// Only the player gains perks or levels at run time (Actor::AddPerk is empty on NPCs).

#include "game/actor_values.hpp"
#include "game/conditions.hpp"
#include "game/magic.hpp"
#include "game/magic_target.hpp"
#include "game/perks.hpp"
#include "game/settings.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::game {

inline constexpr uint32_t k_player_base = 0x7;
inline constexpr uint32_t k_player_ref = 0x14;

// The game' three floats for a skill, and its legendary count.
struct SkillProgress {
    float level = 0;     // the snapshot ApplyLevelUp takes
    float xp = 0;        // towards the next level
    float threshold = 0; // 0 at 100
    uint32_t legendary = 0;
};

// What the rest of the game hears from the character.
struct CharacterEvents {
    std::function<void(const std::string& text, const std::string& sound)> notify;
    // A skill increase with the level meter: the skill's name and new level, the player's level and experience.
    std::function<void(const std::string& skill, int level, int player_level, float xp)> skill_notice;
    std::function<void(uint32_t quest, uint16_t stage)> start_quest;  // a perk's quest entry
    std::function<void(uint32_t target, uint32_t perk, int fragment)> perk_fragment;
    std::function<void(const ActiveEffect&)> effect_started, effect_finished; // scripts on the MGEF
    std::function<void(int level)> level_up;
    std::function<void(uint32_t spell)> disease_contracted, disease_cured;    // story events
    std::function<void(uint32_t race)> race_changed; // the body changes; OnRaceSwitchComplete when it's done
    std::function<float(uint32_t global)> read_global;
    std::function<void(uint32_t global, float value)> write_global;
    std::function<float()> random_percent; // 0..100
    std::function<void(float dt)> frame;   // the other actors' magic, in step with the player's
};

class Character final {
public:
    Character(Game& game, const forms::Database& db, const Settings& settings, const Magic& magic, Conditions& conditions);

    // The race's and the form's spells and perks take hold.
    void start();

    CharacterEvents events;

    const Perks& perks() const { return m_perks; }
    ActorValues& values() { return m_values; }
    const ActorValues& values() const { return m_values; }

    // ---- skills and levels ----------------------------------------------------------------------
    int level() const { return m_level; }
    float skill(int av) const { return m_values.clamped(av); }
    const SkillProgress& progress(int av) const { return m_skills[size_t(av - k_first_skill_value)]; }
    float skill_percent(int av) const;
    float experience() const { return m_xp; }
    float experience_threshold() const { return m_xp_threshold; }
    void add_experience(float xp) { m_xp += xp; }

    // `amount` through the skill's use rates, for the object and action entry point 22 sees.
    void use_skill(int av, float amount, uint32_t object = 0, int action = -1);
    // IncrementSkill: whole levels (books, trainers); the share of the level already made stays.
    void increment_skill(int av, int levels = 1);

    // `count` times, the smallest progress goes; levels never drop.
    void lose_progress(int count);

    bool can_level_up() const { return m_xp >= m_xp_threshold; }
    void count_level();                                           // the level rises here
    void choose_attribute(int attribute);                         // ConfirmLevelUpAttributeCallback::Run
    bool legendary_ready(int av) const { return m_values.base(av) >= 100.0f; }
    void reset_legendary(int av);

    // ---- perks ------------------------------------------------------------------------------------------------
    int perk_points() const { return m_perk_points; }
    void add_perk_points(int count); // Game.AddPerkPoints: saturates at 255
    int perk_rank(uint32_t perk) const;
    bool has_perk(uint32_t perk) const { return perk_rank(perk) != 0; }
    void add_perk(uint32_t perk, int rank = 0);
    void remove_perk(uint32_t perk);

    // A perk chain as the Stats Menu reads it: the first unowned rank (or the last), the one before it, how many ranks
    // are owned, how many there are.
    struct PerkWalk {
        const Perk* next = nullptr;
        const Perk* prev = nullptr;
        int owned = 0, total = 0;
    };
    PerkWalk walk_perk(uint32_t first) const;
    bool perk_conditions(const Perk& perk) const;
    int perk_state(uint32_t first) const; // 2 owned, 1 available, 0 not
    bool can_take(uint32_t first) const;  // without the menu's state
    bool take_perk(uint32_t first);       // The game; false when nothing happened

    // Arguments follow the owner: references, or base forms where the game's a bound
    // object. A float entry point leaves `value` as the player's perks do.
    float entry_point(int entry_point, float value, std::initializer_list<uint32_t> args = {}) const;
    uint32_t entry_point_spell(int entry_point, std::initializer_list<uint32_t> args) const;
    std::string entry_point_text(int entry_point, std::initializer_list<uint32_t> args) const;
    struct ActivateChoice {
        const PerkEntry* entry;
        std::string label;
    };
    // Entry point 14 on a target: the non-immediate choices; immediate ones run now. `replace` when a choice takes the
    // default activation's place.
    std::vector<ActivateChoice> activate_choices(uint32_t target, bool& replace);
    void run_choice(const PerkEntry& entry, uint32_t target);

    // ---- spells and effects -----------------------------------------------------------------------------------
    MagicTarget& magic_target() { return m_target; }
    const MagicTarget& magic_target() const { return m_target; }
    bool has_spell(uint32_t spell) const { return m_target.has_spell(spell); } // the added spells, the base form's, the race's
    bool add_spell(uint32_t spell) { return m_target.add_spell(spell); }
    bool remove_spell(uint32_t spell) { return m_target.remove_spell(spell); }
    bool dispel_spell(uint32_t spell) { return m_target.dispel_spell(spell); } // the effects only
    void dispel_effect(uint32_t id) { m_target.dispel_effect(id); }
    // the effects of a spell, enchantment, potion or ingredient take hold on the player.
    bool apply(uint32_t spell, uint32_t caster = k_player_ref, uint32_t effect_mask = 0xFFFFFFFF, uint32_t object = 0) {
        return m_target.apply(spell, caster, effect_mask, object);
    }
    // A hit's attack spell: a disease rolls DiseaseResist first.
    bool combat_spell_apply(uint32_t spell, uint32_t caster) { return m_target.combat_spell_apply(spell, caster); }
    const std::vector<ActiveEffect>& effects() const { return m_target.effects(); }
    bool has_magic_effect(uint32_t effect) const { return m_target.has_magic_effect(effect); }
    bool has_magic_effect_keyword(uint32_t keyword) const { return m_target.has_magic_effect_keyword(keyword); }
    bool has_disease() const { return m_target.has_disease(); }
    const std::vector<uint32_t>& added_spells() const { return m_target.added_spells(); }
    // Effects with a perk to apply hold it while they run.
    void add_temporary_perk(uint32_t perk);
    void remove_temporary_perk(uint32_t perk);
    bool hold_magicka = false; // a caster holds an item: no regeneration

    // ---- race -------------------------------------------------------------------------------------------------
    uint32_t race() const { return m_race; }
    uint32_t original_race() const { return m_original_race ? m_original_race : m_race; }
    bool female() const { return m_female; }
    void set_female(bool female) { m_female = female; }
    void switch_race(uint32_t race, bool keep_stats);
    bool beast_form() const { return m_beast; }
    void set_beast_form(bool beast) { m_beast = beast; }
    std::string name;
    // A save's change layer (game_save.cpp). Active effects are not in it yet.
    void save(BinaryWriter& w) const;
    void load(BinaryReader& r);

    // ---- the frame ----------------------------------------------------------------------------------------------
    // Regeneration and effects. `body`: health and stamina regenerate in the movement
    // simulation, which reports what it did through body_changed.
    void update(float dt, bool in_combat, bool body);
    void body_changed(float health, float stamina);
    float regen_rate(int attribute, bool in_combat) const; // per second

    // What the record and effect conditions ask of the player. False: the function isn't one of the character's.
    bool condition_value(uint16_t function, uint32_t subject, uint32_t target, uint32_t param1, uint32_t param2, double& out);

private:
    void reset_stats();
    void refresh_thresholds();
    float skill_threshold(int av, float level) const;
    void advance_skill(int av, float xp, bool quiet);
    void set_perk_rank(uint32_t perk, int rank);
    void apply_entry(const PerkEntry& e);
    void remove_entry(const PerkEntry& e);
    bool entry_passes(const PerkEntry& e, int point, std::span<const uint32_t> args) const;
    void run_function(const PerkEntry& e, float& value) const;
    const std::vector<const PerkEntry*>& entries(int point) const { return m_entries[size_t(point)]; }

    void race_spells(uint32_t race, bool add);
    void form_spells();
    void on_value_change(int av, float old_current, float delta);

    const forms::Database* m_db;
    const Settings* m_settings;
    const Magic* m_magic;
    Conditions* m_conditions;
    Perks m_perks;
    ActorValues m_values;
    MagicTarget m_target;

    int m_level = 1;
    float m_xp = 0, m_xp_threshold = 0;
    std::array<SkillProgress, k_skill_value_count> m_skills{};
    int m_perk_points = 0;
    std::unordered_map<uint32_t, int> m_ranks;           // the player's perks, rank from 1
    std::unordered_map<uint32_t, int> m_temporary_perks; // reference counts, from effects
    std::array<std::vector<const PerkEntry*>, k_entry_point_count> m_entries;

    // Entry point 22's conditions read these (player,).
    int m_advance_skill = -1, m_advance_action = -1;
    uint32_t m_advance_object = 0;

    uint32_t m_race = 0, m_original_race = 0;
    bool m_female = false, m_beast = false;

    float m_magicka_delay = 0;
};

} // namespace spud::game
