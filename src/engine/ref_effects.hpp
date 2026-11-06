#pragma once

// What plays on references: the process list's temp effects. The art a magic effect shows on its
// target (ModelReferenceEffect) and the effect shader over it (ShaderReferenceEffect), casting art in the hands and enchant
// effects on the weapon. Once a frame each runs its clock here; the actors and particle systems draw what it leaves.

#include "engine/actors.hpp"
#include "engine/shader_particles.hpp"
#include "engine/vfx.hpp"
#include "engine/world_fx.hpp"
#include "formats/forms_typed.hpp"
#include "game/magic_target.hpp"
#include "game/settings.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud::engine {

class Space;

// An EFSH as the timeline and the membrane read it.
struct EffectShader {
    // One ramp of the game.
    struct Ramp {
        float in = 0, full = 0, out = 0; // seconds: fade in, full, fade out
        float ratio = 0, persistent = 0; // full ratio, persistent ratio
        float pulse_amplitude = 0, pulse_frequency = 0;
        float start() const { return in == 0.0f && full == 0.0f ? ratio : 0.0f; }
    };
    Ramp fill, edge;
    Ramp birth;                 // the particles': counts, not ratios
    struct AddonModel {
        int percentage = 0;
        std::string model; // cache path
    };
    std::vector<AddonModel> addons; // DEBR, by the record's order
    float addon_scale_start = 1, addon_scale_end = 1, addon_scale_in = 0, addon_scale_out = 0;
    bool particles = false;     // flag 0x8 clear, and a birth ratio
    ParticleLook particle_look;
    std::array<glm::vec3, 3> colours{};
    std::array<float, 3> colour_scales{1.0f, 1.0f, 1.0f}; // older records stop before these [I]
    std::array<float, 3> colour_times{};
    glm::vec3 edge_colour{0.0f};
    float edge_fall_off = 1;
    glm::vec2 speed{0.0f}, texture_scale{1.0f}; // fill texture animation, scale U, V
    uint32_t flags = 0;
    MembraneLook look; // without its key
    float total_time() const; // max of fill and edge in + full + out
    float hold_time() const;  // max of fill and edge in + full
};

class RefEffects {
public:
    RefEffects(const forms::Database& db, const game::Settings& settings);

    // for a controller `id`: what a new start with that id replaces.
    struct Hit {
        uint32_t id = 0;
        uint32_t target = 0;
        uint32_t art = 0, shader = 0; // ARTO, EFSH
        uint32_t light = 0;           // LIGH, casting art's
        float scale = 1;
        bool persist = false;
        float lifetime = -1; // seconds, script's Play; -1 no limit
        std::string node;    // the attach root, lower case: "" the target's root
        bool weapon = false; // the shader goes over what the hands hold
    };
    void start(const Hit& hit);
    void finish(uint32_t id); // the controller leaves: its effects play out their end

    static std::string_view magic_node(uint8_t source); // slot,, lower case
    // Where the hand's cast stands: its art's graph follows.
    void hand_cast(uint32_t actor, uint8_t source, game::CastPhase phase);
    // Casting art on the hand's magic node, while a spell is held ready; 0: off.
    void hand(uint32_t actor, uint8_t source, uint32_t art, uint32_t light);
    // Enchant art and shader on the weapon in the hand; both 0: off.
    void weapon(uint32_t actor, uint8_t source, uint32_t art, uint32_t shader);
    void play(uint32_t target, uint32_t art); // one shot, from no effect: default-object art

    // Once a frame, after the actors have posed. No space: nothing has 3D.
    void update(float dt, Space* space, WorldFx* fx, Vfx* vfx);

private:
    struct Shader {
        uint32_t id = 0, key = 0, target = 0;
        const EffectShader* record = nullptr;
        float age = 0, lifetime = -1;
        float fill = 0, edge = 0; // the ramps' last values
        glm::vec2 uv{0.0f};
        bool finishing = false;
        bool persist = false;
        bool attached = false;
        bool weapon = false;
        bool gone = false; // for the next update to take away
        float birth = 0;   // the birth ramp's last value
        std::string node;  // a weapon's: where its blade starts
        std::shared_ptr<ShaderParticles> particles;
        struct Addon {
            uint32_t id = 0;   // Space::spawn's
            uint32_t bone = 0; // which emitter node
        };
        std::vector<Addon> addons;
        bool addons_placed = false;
        float addon_ramp = 0;
    };
    enum class HandState : uint8_t { intro, idle, charge, ready, cast, concentration, cast_end };
    struct Art {
        uint32_t id = 0, target = 0;
        std::string model, node;
        uint32_t light = 0;
        float scale = 1, age = 0, lifetime = -1;
        float finished_at = -1; // the age it began to finish
        bool persist = false, finishing = false;
        uint32_t handle = 0;    // WorldFx's
        bool shape = false;     // nothing for WorldFx: Space::spawn's
        bool running = false;
        bool hit_sequence = false;
        float hit_length = -1;  // a hit sequence that doesn't cycle
        bool gone = false;
        bool hand = false;      // casting art: its graph is followed
        HandState hand_state = HandState::intro;
        game::CastPhase hand_phase = game::CastPhase::none;
        float hand_age = 0, hand_length = -1;
    };

    const EffectShader* shader(uint32_t form);
    std::string art_model(uint32_t form) const;
    uint32_t controller(uint32_t actor, uint32_t kind);
    void discard(uint32_t id);
    void end_art(const Art& a, Space* space, WorldFx* fx, Vfx* vfx);
    void update_addons(Shader& s, float dt, Space& space);
    uint32_t m_next_addon = 0xF0000000u;
    void first_run(Art& a, fx::EffectInstance& instance);
    void hand_state(Art& a, fx::EffectInstance& instance, HandState state);
    void update_hand(Art& a, fx::EffectInstance& instance, float dt);
    bool update_art(Art& a, float dt, Space& space, WorldFx& fx, Vfx* vfx);
    void update_shader(Shader& s, float dt);
    render::GpuMaterialAnim membrane_block(const Shader& s) const;

    const forms::Database& m_db;
    float m_fill_min_rate;
    std::unordered_map<uint32_t, std::unique_ptr<EffectShader>> m_records;
    float m_art_wait;
    std::vector<BoneSegment> m_bones; // scratch
    std::vector<Shader> m_shaders;
    std::vector<Art> m_arts;
    std::unordered_map<uint64_t, uint32_t> m_controllers; // actor and hand: the id of what plays there
    uint32_t m_next_key = 1;
    uint32_t m_next_controller = 0x80000000u; // above the active effects' ids
    std::unordered_map<uint32_t, game::CastPhase> m_hand_phase; // by controller
    std::unordered_set<uint32_t> m_hand_ids, m_hand_released;
};

} // namespace spud::engine
