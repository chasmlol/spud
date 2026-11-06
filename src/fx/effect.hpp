#pragma once

// spud_fx: Gamebryo/Skyrim particle effects, simulated from .ose files (docs/formats/ose.md) with the
// behaviour of the game. No rendering here: the renderer reads the
// per-particle output and the material state each frame.
//
// Typical use:
//   auto effect = fx::load_effect(read_file(cache / "meshes/interface/intmenufogparticles.ose"));
//   fx::EffectInstance fog(effect);                 // seed 1 unless given
//   fog.set_root_transform(rotation, translation_m, 1.0f);
//   fog.activate_sequence("AutoPlay");
//   each frame: fog.update(seconds_since_attached); then read fog.particles(i), fog.material(m).
//
// Determinism: an instance is a pure function of its .ose, seed and the sequence of calls made on it; the
// same calls give bit-identical output (state_hash() checks it). Nothing reads clocks or global state.

#include "formats/ose.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spud::fx {

struct Effect {
    ose::Effect data;
};

// Throws ParseError on a malformed file.
std::shared_ptr<const Effect> load_effect(std::span<const std::byte> ose_file);
std::shared_ptr<const Effect> make_effect(ose::Effect data);

// One live particle, in the world space set by set_root_transform (metres, Z up).
struct Particle {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f}; // metres per second
    float radius = 0.0f;   // the particle's radius (emitter radius x LOD size scale x system world scale), m
    float size = 1.0f;     // the size array (scale modifier); the drawn half-extent is radius * size
    glm::vec4 color{1.0f}; // the colour array, (1,1,1,1) when the system has none
    float rotation = 0.0f; // rotation angle, radians (0 without rotation angles)
    glm::vec3 rotation_axis{0.0f};             // only for systems with rotation axes
    uint32_t frame = 0;                        // sub-texture index (texture index array), 0 without one
    glm::vec4 uv_rect{0.0f, 1.0f, 0.0f, 1.0f}; // the system's sub-texture record for `frame`, as stored
    float age = 0.0f;
    float life_span = 0.0f;
};

// An effect-shader material with the effect's controllers applied.
struct MaterialState {
    const ose::EffectMaterial* source = nullptr;
    glm::vec4 base_color{1.0f};    // alpha replaced by controlled variable 5 (SetMaterialAlpha, >= 0)
    float base_color_scale = 1.0f; // variable 0
    glm::vec4 falloff{0.0f};       // variables 1-4; 1 and 2 hold cos(degrees) once a controller writes them
    glm::vec2 uv_offset{0.0f};     // variables 6 and 8
    glm::vec2 uv_scale{1.0f};      // variables 7 and 9
};

struct Bound {
    glm::vec3 center{0.0f}; // metres, world space
    float radius = 0.0f;
};

class EffectInstance {
public:
    explicit EffectInstance(std::shared_ptr<const Effect> effect, uint32_t seed = 1);
    ~EffectInstance();
    EffectInstance(EffectInstance&&) noexcept;
    EffectInstance& operator=(EffectInstance&&) noexcept;

    const ose::Effect& data() const;

    // Where the NIF's root sits in the world. It replaces the root node's own local transform, as the game
    // does for the Mist Menu fog (rotation about Z by -180 degrees, translation 0, scale left as authored:
    // pass data().nodes[data().root].scale). Particles already emitted into world space stay where they are.
    void set_root_transform(const glm::mat3& rotation, const glm::vec3& translation_m, float scale);

    // BSPSysLODModifier input: the fade node's LOD distance (BSFadeNode). 0 (the default) means no
    // LOD reduction. lod_distance() computes the game's value from a camera.
    void set_lod_distance(float value);
    // |camera - centre| / units * lod_adjust / fade_mult / 7200.
    static float lod_distance(const glm::vec3& camera_m, const glm::vec3& bound_center_m, float lod_adjust,
                              float fade_mult);

    // The global particle density scale (g_ParticleDensityScale): 1 unless the game's particle budget is
    // exceeded (iMaxDesired:Particles).
    void set_density_scale(float scale);

    // The game / Deactivate by name; false if unknown or already in that state.
    bool activate_sequence(std::string_view name, float weight = 1.0f, float ease_in = 0.0f,
                           uint8_t priority = 0);
    bool deactivate_sequence(std::string_view name, float ease_out = 0.0f);
    void set_sequence_weight(std::string_view name, float weight);
    bool sequence_active(std::string_view name) const;
    // The sequence's scaled (key) time after the last update, -1 if not active.
    float sequence_time(std::string_view name) const;
    // (stop - start) / frequency, the duration the behaviour graph uses; -1 if unknown.
    float sequence_duration(std::string_view name) const;
    std::vector<std::string> sequence_names() const;
    // the sequence stands at this key time at every update until posed
    // again or activated anew, whatever the clock does (the Lockpicking Menu's lock follows its angle).
    void pose_sequence(std::string_view name, float key_time);
    // The sequence's first and last key time; false if unknown.
    bool sequence_range(std::string_view name, float& begin, float& end) const;

    // Advances everything to `time` (seconds since the effect was attached, as the game's scene update
    // passes it). Time going backwards resets the particle systems, as in the game.
    void update(float time);
    void advance(float dt) { update(m_time + dt); }
    float time() const { return m_time; }

    size_t system_count() const;
    std::span<const Particle> particles(size_t system) const;
    size_t particle_count(size_t system) const;
    Bound bound(size_t system) const; // NiPSysBoundUpdateModifier's bound
    const MaterialState& system_material(size_t system) const;

    // Every material the effect owns, including those of .osm submeshes its controllers animate
    // (source->owner_kind == mesh_shape, source->owner = submesh index).
    size_t material_count() const;
    const MaterialState& material(size_t index) const;

    // The file's scene nodes by name (k_none if absent), and their world transforms after the last update in the
    // game's units: Skyrim units, column vectors (world = R * (s * local) + t). Node animation (transform
    // controllers driven by sequences) moves them; the loading screens' camera paths are read this way.
    uint32_t find_node(std::string_view name) const;
    glm::mat3 node_world_rotation(uint32_t node) const;
    glm::vec3 node_world_translation_units(uint32_t node) const;
    float node_world_scale(uint32_t node) const;

    // A hash of the complete simulation state (particles, controllers, RNGs), for determinism checks.
    uint64_t state_hash() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
    float m_time = 0.0f;
};

} // namespace spud::fx
