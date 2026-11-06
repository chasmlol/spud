#pragma once

// What the item systems need of magic: effects, and what a magic item costs.

#include "game/conditions.hpp"
#include "game/items.hpp"
#include "game/settings.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::game {

enum MagicEffectFlag : uint32_t {
    effect_hostile = 0x1,
    effect_recover = 0x2,
    effect_detrimental = 0x4,
    effect_snap_to_navmesh = 0x8,
    effect_no_hit_event = 0x10,
    effect_dispel_keywords = 0x100,
    effect_no_duration = 0x200,
    effect_no_magnitude = 0x400,
    effect_no_area = 0x800,
    effect_fx_persist = 0x1000,
    effect_gory_visuals = 0x4000,
    effect_power_affects_magnitude = 0x200000,
    effect_power_affects_duration = 0x400000,
    effect_no_cost = 0x01000000, // left out of a magic item's cost
    effect_no_expiry = 0x02000000,
    effect_painless = 0x04000000,
    effect_no_death_dispel = 0x10000000,
};

// Table.
enum MagicArchetype : uint32_t {
    archetype_value_modifier = 0, archetype_script = 1, archetype_dispel = 2, archetype_cure_disease = 3,
    archetype_absorb = 4, archetype_dual_value_modifier = 5, archetype_calm = 6, archetype_demoralize = 7,
    archetype_frenzy = 8, archetype_disarm = 9, archetype_command_summoned = 10, archetype_invisibility = 11,
    archetype_light = 12, archetype_darkness = 13, archetype_night_eye = 14, archetype_lock = 15, archetype_open = 16,
    archetype_bound_weapon = 17, archetype_summon_creature = 18, archetype_detect_life = 19, archetype_telekinesis = 20,
    archetype_paralysis = 21, archetype_reanimate = 22, archetype_soul_trap = 23, archetype_turn_undead = 24,
    archetype_guide = 25, archetype_werewolf_feed = 26, archetype_cure_paralysis = 27, archetype_cure_addiction = 28,
    archetype_cure_poison = 29, archetype_concussion = 30, archetype_value_and_parts = 31,
    archetype_accumulate_magnitude = 32, archetype_stagger = 33, archetype_peak_value_modifier = 34, archetype_cloak = 35,
    archetype_werewolf = 36, archetype_slow_time = 37, archetype_rally = 38, archetype_enhance_weapon = 39,
    archetype_spawn_hazard = 40, archetype_etherealize = 41, archetype_banish = 42, archetype_spawn_scripted_ref = 43,
    archetype_disguise = 44, archetype_grab_actor = 45, archetype_vampire_lord = 46,
};
inline constexpr uint32_t k_archetype_count = 47;

// The bits of an archetype's flags that have a reader.
enum ArchetypeFlag : uint16_t {
    archetype_stacks = 0x10,       // a new effect leaves what the same source left
    archetype_self_alone = 0x20,   // self-delivered, the cast fails while another caster is busy
    archetype_custom_skill = 0x80, // skill comes from the running effect
    archetype_miss_skill = 0x100,  // a projectile that hit nothing still pays skill
    archetype_caster_list = 0x200, // lives on the caster's list
};

struct ArchetypeInfo {
    uint16_t flags;
    int16_t actor_value; // forced, -1 for the MGEF's own
};
const ArchetypeInfo& archetype_info(uint32_t archetype);

enum SpellType : uint32_t {
    spell_plain = 0, spell_disease = 1, spell_power = 2, spell_lesser_power = 3, spell_ability = 4, spell_poison = 5,
    spell_enchantment = 6, spell_potion = 7, spell_ingredient = 8, spell_leveled = 9, spell_addiction = 10, spell_voice = 11,
    spell_staff_enchantment = 12, spell_scroll = 13,
};

// SPIT's. An enchantment's flags stay 0: they answer false to all of these.
enum SpellFlag : uint32_t {
    spell_area_ignores_los = 0x80000,
    spell_ignore_resistance = 0x100000,
    spell_no_absorb = 0x200000,
    spell_no_dual_modification = 0x800000,
};

enum CastingType : uint32_t { cast_constant = 0, cast_fire_and_forget = 1, cast_concentration = 2 };
enum Delivery : uint32_t {
    delivery_self = 0, delivery_touch = 1, delivery_aimed = 2, delivery_target_actor = 3, delivery_target_location = 4,
};

struct MagicEffect {
    uint32_t form = 0;
    std::string name, description; // FULL, DNAM
    uint32_t flags = 0;
    float base_cost = 0;
    uint32_t casting_type = 0; // 0 constant, 1 fire and forget, 2 concentration
    uint32_t delivery = 0;
    float cast_time = 0;       // the charge an auto-calculated item takes from it
    int32_t magic_skill = -1;
    int32_t resist_value = -1;
    int32_t actor_value = -1;
    std::vector<uint32_t> keywords;
    uint32_t archetype = 0;
    int32_t second_actor_value = -1;
    float second_weight = 0;
    float taper_weight = 0, taper_curve = 0, taper_duration = 0;
    uint32_t associated = 0;
    uint32_t perk = 0;                     // perk to apply
    std::vector<ConditionItem> conditions; // gate the effect when it's added
    bool has_scripts = false;
    float skill_mult = 0;
    float minimum_skill = 0;
    float script_ai_score = 0, script_ai_delay = 0;
    uint32_t projectile = 0, explosion = 0;
    uint32_t dual_projectile = 0;          // the DUAL record's, 0 for none
    float dual_scale = 1;
    uint32_t image_space = 0;
    uint32_t sound_level = 0;              // casting
    std::array<uint32_t, 6> sounds{};      // SNDD by type: 0 draw, 1 charge, 2 ready, 3 release, 4 concentration loop, 5 on hit
    uint32_t casting_art = 0, casting_light = 0;
    uint32_t hit_art = 0, hit_shader = 0;          // or the hit visuals' RFCT
    uint32_t enchant_art = 0, enchant_shader = 0;  // or the enchant visuals' RFCT
    uint32_t dual_art = 0, dual_shader = 0;        // the DUAL's
    bool dual_art_scales = true;                   // ... inherit scale flag 0x1

    uint32_t art_on_hit(bool dual) const { return dual && dual_art ? dual_art : hit_art; }
    uint32_t shader_on_hit(bool dual) const { return dual && dual_shader ? dual_shader : hit_shader; }
    float hit_scale(bool dual) const { return dual && (!dual_art || dual_art_scales) ? dual_scale : 1.0f; }
};

struct SpellEffect {
    uint32_t effect = 0; // MGEF
    float magnitude = 0;
    uint32_t area = 0, duration = 0;
    std::vector<ConditionItem> conditions; // rechecked as the effect runs
};

// A SPEL, ENCH, ALCH, INGR or SCRL as magic: what MagicItem holds.
struct Spell {
    uint32_t form = 0;
    std::string name;
    uint32_t type = spell_plain;
    uint32_t flags = 0;
    uint32_t cast_type = 0, delivery = 0;
    std::vector<SpellEffect> effects;
    bool manual_cost = false;      // SPIT or ENIT says not to calculate
    float base_cost = 0;           // ... the record's own
    float charge_time = 0, cast_duration = 0, range = 0;
    uint32_t equip_slot = 0;       // ETYP: BothHands for master spells
    std::vector<uint32_t> keywords; // KWDA
    uint32_t half_cost_perk = 0;
    int32_t charge_amount = 0;     // ENIT, the charge a weapon starts with
    uint32_t hostile_count = 0;
    bool extend_on_recast = false; // ENIT flag 4
    bool food = false;             // a food ingredient
    bool permanent() const { return type == spell_disease || type == spell_ability || type == spell_addiction; }
    bool constant_enchantment() const {
        return (type == spell_enchantment || type == spell_staff_enchantment) && cast_type == cast_constant;
    }
    bool ignores_resistance() const { return (flags & spell_ignore_resistance) != 0; }
    bool no_absorb() const { return (flags & spell_no_absorb) != 0; }
};

// A SHOU: three words, each with its spell and seconds of recovery.
struct ShoutWord {
    uint32_t word = 0, spell = 0;
    float recovery = 0;
};
struct Shout {
    uint32_t form = 0;
    std::array<ShoutWord, 3> words{};
    bool spells_as_powers = false; // record flag 0x80
};

class Magic {
public:
    Magic(const forms::Database& db, const Settings& settings) : m_db(&db), m_settings(&settings) {}

    const MagicEffect* effect(uint32_t form) const;
    const Spell* spell(uint32_t form) const;
    const Shout* shout(uint32_t form) const;
    // A player's enchantment: its form.
    uint32_t create_enchantment(std::vector<Effect> effects, uint32_t cast_type, int32_t cost) const;
    // What a known enchantment counts as: ENIT's base enchantment, else itself.
    uint32_t base_enchantment(uint32_t ench) const;
    // Whether the player made it.
    bool created(uint32_t ench) const { return m_created.contains(ench); }
    // The effects of an ENCH or SPEL.
    std::vector<Effect> effects_of(uint32_t form) const;
    // The stored cost of one effect.
    float effect_cost(const Effect& e) const;
    float effect_cost(const SpellEffect& e) const { return effect_cost(Effect{e.effect, e.magnitude, e.area, e.duration}); }
    // The costliest effect of an item, of those with a projectile or a skill when asked; null for none.
    const SpellEffect* costliest(const Spell& spell, bool with_projectile = false, bool with_skill = false) const;
    // EffectItem equality: same MGEF, and magnitude, area, duration where those apply, same conditions.
    bool same_effect(const SpellEffect& a, const SpellEffect& b) const;
    // The seconds a concentration projectile relaunches after.
    float relaunch_interval(uint32_t projectile) const;
    // of what the cast costs.
    float dual_effectiveness(float cost) const;
    // The game with no actor: the record's own when it says not to calculate, else its effects'.
    float cost(std::span<const Effect> effects, bool manual, int32_t manual_cost) const;
    // ... of an ENCH.
    float enchantment_cost(uint32_t ench) const;
    // Its casting type: 0 constant effect.
    uint32_t enchantment_cast_type(uint32_t ench) const;

private:
    const forms::Database* m_db;
    const Settings* m_settings;
    mutable std::unordered_map<uint32_t, std::unique_ptr<MagicEffect>> m_effects;
    mutable std::unordered_map<uint32_t, std::unique_ptr<Spell>> m_spells;
    mutable std::unordered_map<uint32_t, std::unique_ptr<Shout>> m_shouts;
    struct Created {
        std::vector<Effect> effects;
        uint32_t cast_type = 0;
        int32_t cost = 0;
    };
    mutable std::unordered_map<uint32_t, Created> m_created;
    mutable uint32_t m_next_created = 0xFF200000;
};

} // namespace spud::game
