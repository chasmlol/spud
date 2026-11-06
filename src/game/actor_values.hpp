#pragma once

// Actor values: the game's 164 values and an actor's base, permanent, temporary and damage
// parts, as ActorValueOwner holds them. Bases that aren't stored yet come from the records the way
// Actor::GetBaseActorValue finds them, so an NPC_ or the player's form both work.

#include "formats/forms_typed.hpp"
#include "game/settings.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>

namespace spud::game {

inline constexpr int k_actor_value_count = 164;

// The values the code names; the rest go by index (actor_value_info).
enum ActorValue : int {
    av_aggression = 0, av_confidence, av_energy, av_morality, av_mood, av_assistance,
    av_one_handed = 6, av_two_handed, av_marksman, av_block, av_smithing, av_heavy_armor, av_light_armor, av_pickpocket,
    av_lockpicking, av_sneak, av_alchemy, av_speechcraft, av_alteration, av_conjuration, av_destruction, av_illusion,
    av_restoration, av_enchanting,
    av_health = 24, av_magicka, av_stamina, av_heal_rate, av_magicka_rate, av_stamina_rate, av_speed_mult,
    av_inventory_weight, av_carry_weight, av_crit_chance, av_melee_damage, av_unarmed_damage, av_mass, av_voice_points,
    av_voice_rate, av_damage_resist,
    av_disease_resist = 45,
    av_bow_speed_bonus = 78,
    av_skill_advance_first = 113, // OneHandedSkillAdvance; the skills in order
    av_dragon_souls = 133,
    av_combat_health_regen_mult = 134,
    av_heal_rate_mult = 155, av_magicka_rate_mult, av_stamina_rate_mult,
    av_werewolf_perks = 158, av_vampire_perks,
};
inline constexpr int k_first_skill_value = av_one_handed;
inline constexpr int k_skill_value_count = 18;

enum ActorValueFlag : uint32_t {
    av_flag_clamped = 0x10,      // the clamped read holds it to 0..100
    av_flag_floor_zero = 0x20,   // damage stops at 0
    av_flag_enumerated = 0x100,  // SetAV wants a whole value
    av_flag_damage_positive = 0x200,
    av_flag_derived = 0x800,     // derives for every actor; a race change clears the stored base
    av_flag_no_script = 0x4000,  // scripts and the console may not change it
    av_flag_default_0 = 0x8000,
    av_flag_default_1 = 0x10000,
    av_flag_default_100 = 0x20000,
};

struct ActorValueInfo {
    const char* name;
    uint8_t type; // 0 attribute, 1 skill, 2 AI, 3 resist, 4 condition, 5 status, 6 other
    uint32_t flags;
};
const ActorValueInfo& actor_value_info(int av);
// By the name scripts and the console use, ignoring case; -1 for none.
int actor_value_by_name(std::string_view name);

enum class Modifier : uint8_t { permanent, temporary, damage };

// A form's answers about the values it starts with, auto-calculated stats worked out.
struct ActorRecord {
    uint32_t npc = 0;    // the NPC_ giving the stats
    uint32_t race = 0;
    uint32_t klass = 0;
    uint32_t acbs_flags = 0;
    int level = 1;
    std::array<float, k_skill_value_count> skills{};
    float health = 0, magicka = 0, stamina = 0;
    float speed_mult = 100;
    std::array<float, 6> ai{}; // AIDT, actor values 0..5
    float voice_points = 50;
    int bleedout_override = -1;   // ACBS, health points, under flag 0x40000
    float bleedout_default = 0;   // CLAS, a share of health
    bool essential_or_protected() const { return (acbs_flags & 0x802) != 0; }
};
// `level` overrides the record's (the player's level as it stands); `race` the record's when non-zero.
ActorRecord actor_record(const forms::Database& db, const Settings& settings, uint32_t npc, int level, uint32_t race);

class ActorValues {
public:
    ActorValues(const forms::Database& db, const Settings& settings, bool player) : m_db(&db), m_settings(&settings), m_player(player) {}

    void set_record(const ActorRecord& record) { m_record = record; }
    void save(BinaryWriter& w) const; // game_save.cpp
    void load(BinaryReader& r);
    const ActorRecord& record() const { return m_record; }

    float base(int av) const;                                                     // Actor::GetBaseActorValue
    float permanent(int av) const { return base(av) + part(av, Modifier::permanent); } // GetPermanentActorValue
    float current(int av) const;                                                  // GetActorValue
    float maximum(int av) const { return permanent(av) + part(av, Modifier::temporary); } // GetActorValueMax
    float percent(int av) const;                                                  // GetActorValuePercent: a fraction
    float clamped(int av) const;                                                  // GetClampedActorValue
    float modifier(int av, Modifier m) const { return part(av, m); }

    void set_base(int av, float value);
    void mod_base(int av, float delta) { set_base(av, base(av) + delta); }
    void mod(Modifier m, int av, float delta);
    void restore(int av, float amount);        // Actor::RestoreActorValue

    // A race change clears these: flag 0x800, and skills and attributes when the stats aren't kept.
    void clear_derived_bases();
    void clear_skill_and_attribute_bases();
    void clear_base(int av) { m_base.erase(av); }

    // This hit may kill an essential or protected actor (a protected one, by the player).
    bool killable_now = false;
    // Every change, with the old current value and the difference (the game's table).
    std::function<void(int av, float old_current, float delta)> on_change;
    // InventoryWeight and DamageResist come from the inventory.
    std::function<float(int av)> derive;

private:
    float part(int av, Modifier m) const {
        const auto it = m_mods.find(av);
        return it == m_mods.end() ? 0.0f : it->second[size_t(m)];
    }
    float record_value(int av) const;
    float race_value(std::string_view field) const;

    const forms::Database* m_db;
    const Settings* m_settings;
    bool m_player;
    ActorRecord m_record;
    mutable std::unordered_map<int, float> m_base;
    std::unordered_map<int, std::array<float, 3>> m_mods;
};

} // namespace spud::game
