#pragma once

// What shows when something hits: which IPCT plays, its temp particle, decal and sounds, blood, and hazards.
//-9, 18. Works in game units as the design does; what it needs of the world comes through ProjectileWorld.

#include "game/projectiles.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::game {

class Game;

// DecalFlag, ImpactRecord and DecalRequest are in projectiles.hpp, the hook's side.

// HAZD.
struct HazardRecord {
    uint32_t form = 0;
    std::string model;
    uint32_t limit = 0, flags = 0, spell = 0, light = 0, sound = 0;
    float radius = 0, lifetime = 0, target_interval = 0;
};

enum HazardFlag : uint32_t { hazard_player_only = 0x1, hazard_align_normal = 0x4, hazard_drop_to_ground = 0x10 };

// The game's arguments.
struct HazardSpawn {
    uint32_t form = 0, source = 0;
    Units3 position{}, normal{};
    float lifetime = 0, radius = 0; // the caller's, 0 for the record's
    bool no_lifetime = false;       // the caller asks for none
    bool skip_rate_limit = false, skip_spacing = false;
};

class Impacts {
public:
    explicit Impacts(Game& game);
    ~Impacts();

    const ImpactRecord* record(uint32_t form);
    // a material with no entry plays nothing, no parent walk, no default.
    uint32_t impact_of(uint32_t set, uint32_t material) const;

    // Projectile past the noise. `velocity` is the projectile's, `normal` the contact's; `decal` false on what
    // takes none (an actor, something moving). `size` holds the decal's width once drawn.
    void projectile(uint32_t ipct, const Units3& at, const Units3& normal, const Units3& velocity, uint32_t ref, bool decal,
                    float& size);
    // `swing` is the direction of the blow.
    void melee_object(uint32_t attacker, const Item* weapon, bool bash, const ProjectileContact& c, const Units3& swing);
    // The game for the surface it found.
    void explosion(const ExplosionRecord& r, const Units3& centre, const ProjectileContact& c, bool in_water);
    void blood(const HitData& hit);
    // GetHit (19): the player took a hit or a hostile effect.
    void player_hurt();

    // the hazard's id, 0 when the rules refuse it.
    uint32_t create_hazard(const HazardSpawn& spawn);
    void update(float dt);

private:
    struct Hazard {
        uint32_t id = 0;
        const HazardRecord* record = nullptr;
        uint32_t source = 0, sound = 0;
        Units3 position{};
        float heading = 0, pitch = 0;
        float lifetime = 0, radius = 0, age = 0, next_pulse = 0;
        bool first_frame = true;
    };

    float n(std::string_view name, float fallback) const;
    float roll(float low, float high);
    ProjectileWorld& world();
    uint32_t distance_bits(const Units3& at);
    DecalAxes spun_basis(const Units3& direction);
    int roll_subtexture(const ImpactRecord& r);
    void particle(const ImpactRecord& r, const Units3& at, const Units3& direction);
    void add_decal(const ImpactRecord& r, const Units3& at, const DecalAxes& axes, float width, float height, float depth,
                   float placement, int subtexture, uint32_t ref);
    uint32_t blood_impact_set(const HitData& hit);
    uint32_t body_part_set(uint32_t race) const;
    void environment_blood(const HitData& hit, const Units3& direction, const DecalAxes& axes, int subtexture, uint32_t material);

    const HazardRecord* hazard_record(uint32_t form);
    void update_hazard(Hazard& h, float dt);
    void pulse(const Hazard& h);

    Game& m_game;
    std::unordered_map<uint32_t, std::unique_ptr<ImpactRecord>> m_records;
    std::unordered_map<uint32_t, std::unique_ptr<HazardRecord>> m_hazard_records;
    std::vector<std::unique_ptr<Hazard>> m_hazards;
    double m_time = 0, m_last_hazard = -1e9;
    uint32_t m_next_ref = 0xFFA00000; // below the projectiles' created references
    std::mt19937 m_random{0x1BC7u};   // seeded: scripted runs draw the same effects (20)
};

} // namespace spud::game
