#pragma once

// The game's items: every record the player can carry, decoded once when first asked for.

#include "formats/forms_typed.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::game {

enum class ItemKind : uint8_t {
    none,
    armor,      // ARMO
    weapon,     // WEAP
    ammo,       // AMMO
    potion,     // ALCH: potion, poison or food by its flags
    scroll,     // SCRL
    ingredient, // INGR
    book,       // BOOK
    key,        // KEYM
    misc,       // MISC
    soul_gem,   // SLGM
    apparatus,  // APPA
    light,      // LIGH with the carried flag
};

inline constexpr uint32_t k_gold = 0x0000000F;     // Gold001
inline constexpr uint32_t k_lockpick = 0x0000000A; // Lockpick
inline constexpr uint32_t k_skeleton_key = 0x0003A070; // DefaultObject 5, the Skeleton Key

enum PotionFlag : uint32_t {
    potion_manual_calc = 0x1,
    potion_food = 0x2,
    potion_medicine = 0x10000,
    potion_poison = 0x20000,
};

enum BookFlag : uint8_t {
    book_teaches_skill = 0x01,
    book_cant_take = 0x02,
    book_teaches_spell = 0x04,
};

struct Effect {
    uint32_t effect = 0; // MGEF
    float magnitude = 0;
    uint32_t area = 0;
    uint32_t duration = 0;
};

struct Item {
    uint32_t form = 0;
    float survival_weight = 0; // ammo and lockpicks only weigh this with Survival Mode on
    ItemKind kind = ItemKind::none;
    std::string editor_id, name;
    std::string description; // DESC or CNAM: what the card shows
    int32_t value = 0;
    float weight = 0;
    uint32_t swing_sound = 0, draw_sound = 0, sheathe_sound = 0; // weapons: TNAM, NAM9, NAM8
    std::string model;           // world model in the cache ("meshes/.../x.osm"), "" for none
    std::string inventory_model; // what the 3D item shows
    std::vector<uint32_t> keywords;
    uint32_t pickup_sound = 0, putdown_sound = 0; // YNAM, ZNAM
    uint32_t equip_slot = 0;                      // ETYP
    uint32_t enchantment = 0;                     // EITM
    uint16_t enchantment_amount = 0;              // EAMT
    uint32_t template_form = 0;                   // CNAM of a WEAP, TNAM of an ARMO
    std::vector<Effect> effects;                  // ALCH, INGR, SCRL
    uint32_t effect_flags = 0;                    // ENIT flags of ALCH, INGR
    uint32_t consume_sound = 0;                   // ENIT of an ALCH
    // Armor.
    uint32_t biped_slots = 0;
    uint32_t armor_type = 0;  // 0 light, 1 heavy, 2 clothing
    uint32_t armor_flags = 0; // BODT general flags (0x4 non-playable)
    int32_t armor_rating = 0; // DNAM: the rating x 100
    // Weapon and ammo.
    int32_t damage = 0;
    uint8_t animation_type = 0;
    uint32_t weapon_flags = 0; // WEAP DNAM flags, AMMO DATA flags
    int32_t skill = -1; // the actor value the weapon uses
    int32_t resist = -1; // ... the victim resists it with
    float stagger = 0;
    int32_t crit_damage = 0;
    float crit_mult = 1;
    uint32_t crit_effect = 0;
    bool crit_on_death = false;
    float speed = 1, reach = 1;
    uint32_t projectile = 0;                    // PROJ an AMMO flies as
    uint32_t projectile_count = 1;              // DNAM: how many a shot fires
    uint32_t attack_sound = 0, attack_sound_2d = 0; // SNAM, XNAM
    uint32_t impact_set = 0;                    // INAM
    uint32_t bash_impact_set = 0;               // BIDS
    int32_t sound_level = 0;                    // VNAM
    // Book.
    uint8_t book_flags = 0, book_type = 0;
    int32_t book_skill = -1; // what it teaches
    uint32_t book_spell = 0;
    std::string book_text;
    // Soul gem.
    uint8_t soul = 0, soul_capacity = 0;
    uint32_t soul_gem_filled = 0; // NAM0: the gem the soul trap fills this one to

    bool has_keyword(uint32_t keyword) const;
    bool food() const { return kind == ItemKind::potion && (effect_flags & potion_food); }
    bool poison() const { return kind == ItemKind::potion && (effect_flags & potion_poison); }
};

class Items {
public:
    explicit Items(const forms::Database& db) : m_db(&db) {}

    // The item of that form; null for anything that isn't one (LVLI included), or a deleted form.
    const Item* find(uint32_t form) const;
    // A new item of the running game (a brewed potion): its form.
    uint32_t create(Item item) const;
    const forms::Database& db() const { return *m_db; }

private:
    const forms::Database* m_db;
    mutable std::unordered_map<uint32_t, std::unique_ptr<Item>> m_items;
    mutable uint32_t m_next_created = 0xFF100000; // the game's created forms sit in the FF range
};

// The cache's path of a MODL: "weapons\iron\sword.nif" -> "meshes/weapons/iron/sword.osm".
std::string model_path(std::string_view modl);

} // namespace spud::game
