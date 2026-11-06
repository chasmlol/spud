#pragma once

// Skyrim's rules for the player on foot: speeds, sprint and stamina, look, jump and fall numbers. No physics.
// which gives the source of every value and formula (game settings from the
// plugins, behaviour of the game, measurements in the game). Distances are Skyrim units here; the
// physics side converts to metres.

#include "formats/osr.hpp"
#include "formats/osw.hpp"

#include <optional>
#include <span>

#include <glm/glm.hpp>

namespace spud::engine::movement {

// The game settings these rules read. Game settings (GMST) come from the plugins through the rules file; INI
// settings keep the game's defaults, which the user's INI files could override.
struct Settings {
    // GMST
    float jump_height_min = 76;   // fJumpHeightMin, units
    float jump_swimming_mult = 2; // fJumpSwimmingMult
    float jump_move_base = 0;     // fJumpMoveBase
    float jump_move_mult = 0.3f;  // fJumpMoveMult
    float jump_fall_height_min = 600;
    float jump_fall_height_mult = 0.1f;
    float jump_fall_height_exponent = 1.45f;
    float move_weight_min = 0;
    float move_weight_max = 115;
    float move_encum_effect = 0.3f;            // weapon drawn
    float move_encum_effect_no_weapon = 0.15f; // weapon sheathed
    float sprint_stamina_drain_mult = 7;
    float sprint_stamina_weight_base = 1;
    float sprint_stamina_weight_mult = 0.02f;
    float out_of_breath_stamina_regen_delay = 3;
    float damaged_stamina_regen_delay = 0.5f;
    float stamina_regen_delay_max = 5;
    float damaged_health_regen_delay = 0.5f;
    float health_regen_delay_max = 5;
    float combat_stamina_regen_rate_mult = 0.35f;
    float jog_interpolation = 0.5f; // fJogInterpolationBetweenWalkAndRun
    float actor_swim_breath_base = 10;
    float actor_swim_breath_mult = 0.2f;
    float actor_swim_breath_damage = 0.08f;
    float water_knockdown_velocity = 10;     // fWaterKnockdownVelocity, Havok m/s (Skyrim.esm: 5)
    float water_knockdown_size_normal = 0.5f; // fWaterKnockdownSizeNormal (Skyrim.esm: 0.75), medium races
    float jump_fall_velocity_min = 600;      // fJumpFallVelocityMin, units/s (Skyrim.esm: 700)
    // INI (executable defaults)
    float in_air_falling_gravity_mult = 1.35f; // fInAirFallingCharGravityMult:Havok
    float unsupported_gravity_mult = 4;         // fUnsupportedGravMult:HAVOK
    float unsupport_cast_length = 70.0f;        // fUnsupportCastLength:HAVOK, 1 Havok unit in units
    float sprint_stop_threshold = 0.5f;         // fSprintStopThreshold:Controls
    float looking_speed = 0.1f;                 // fLookingSpeed:Camera
    float mouse_heading_sensitivity = 0.0125f;  // fMouseHeadingSensitivity:Controls
    float mouse_heading_x_scale = 0.02f;        // fMouseHeadingXScale:Controls
    float mouse_heading_y_scale = 0.85f;        // fMouseHeadingYScale:Controls
    bool invert_y = false;                      // bInvertYValues:Controls
    float default_world_fov = 80;               // fDefaultWorldFOV:Display
    float max_move_ramp_down = 500;             // fMaxMoveRampDownDelta:Controls, units/s per update
    float controller_buffer_depth = 0.14f;      // fControllerBufferDepth:Controls, seconds
    float controller_sample_threshold = 0.1f;   // fControllerSampleThreshold:Controls
    bool freeze_direction_on_large_delta = true; // bFreezeDirectionOnLargeDelta:Controls
    float freeze_direction_angle = 60.0f;       // fFreezeDirectionDefaultAngleThreshold:Controls, degrees
    float freeze_direction_speed = 100.0f;      // fFreezeDirectionDefaultSpeedThreshold:Controls, units/s
    // Gamepad ([Controls] INI, executable defaults).
    float l_thumb_deadzone = 0.25f, l_thumb_deadzone_max = 0.97f; // fLThumbDeadzone, fLThumbDeadzoneMax
    float r_thumb_deadzone = 0.25f, r_thumb_deadzone_max = 0.97f;
    bool look_maxed_out_acceleration = true;   // bGamepadLookApplyMaxedOutAcceleration
    bool look_snap_to_axis = true;             // bGamepadLookApplySnapToAxis
    bool look_sensitivity_threshold = false;   // bGamepadLookApplySensitivityThreshold
    bool look_time_normalize = true;           // bGamepadLookTimeNormalizeInputs
    float look_snap_strength = 0.15f;          // fLookSnapToAxisStrength
    float look_graph[4] = {0.115141f, -0.382666f, 2.02865f, -0.754499f}; // fLookGraphCoefficient1..4
    float look_mult_exponent = 0;              // fGamepadLookMultExponent
    float gamepad_sensitivity = 0.66672f;      // fGamepadHeadingSensitivity (SkyrimPrefs.ini)
    float gamepad_sensitivity_default = 0.66672f; // fGamepadHeadingSensitivityDefault
    float look_accel_sec = 0.92f, look_accel_yaw_mult = 2.5f, look_accel_pitch_mult = 2.5f;
    float look_curve_threshold = 0.1f;         // fLookCurveSensitivityThreshold
    float gamepad_x_scale = 1.0f, gamepad_y_scale = 0.5625f; // fGamepadHeadingXScale, fGamepadHeadingYScale
    float look_floor_framerate = 20, look_target_framerate = 30; // fLookTimeNormalizing*Framerate
    // fMoveGraphX/Y1..3:Controls: the stick-magnitude response, through (0,0) and (1,1).
    glm::vec2 move_graph[3] = {{0.2f, 0.1f}, {0.7f, 0.5f}, {0.9f, 0.9f}};

    static Settings from_rules(const osr::Rules& rules);
};

// A movement type's speeds after the actor's multipliers: units/s; rotation rad/s.
struct Speeds {
    float left[2] = {}, right[2] = {}, forward[2] = {}, back[2] = {}; // [walk, run]
    float rotate_in_place[2] = {};
    float rotate_while_moving = 0;
};

// What the speed multipliers depend on.
struct Body {
    float scale = 1;       // reference scale x race height x NPC height
    float speed_mult = 100; // SpeedMult actor value
    float equipped_weight = 0;
    bool weapon_drawn = false;
    float inventory_weight = 0;
    float carry_weight = 300;
};

bool can_run(const Body& body);
Speeds actor_speeds(const osr::MovementType& type, const Body& body, const Settings& s);

// The speed the controls ask for: units/s along the ground, and the direction relative to facing (radians,
// clockwise, 0 = ahead). `move` is x right, y forward; `run` the run flag (always on with a gamepad).
struct Motion {
    float speed = 0;
    float direction = 0;
};
Motion requested_motion(const Speeds& speeds, glm::vec2 move, bool run, bool gamepad, const Settings& s);

// The water current at a point, Havok m/s (metres per second here): a cell's elements in the game's format
// (osw::CellCurrent), evaluated in order. `position` in metres. Nothing when no element applies.
std::optional<glm::vec3> water_current(std::span<const osw::CurrentQuad> quads, const glm::vec3& position);
// What of a current at the feet moves the actor: ignored below the knockdown
// threshold unless swimming (or falling fast); below it a swimmer's movement plus the current keeps to the run
// speed. `move` is the commanded velocity, units/s; the result is units/s.
glm::vec3 applied_current(const glm::vec3& current_ms, const glm::vec3& move, float run_speed, bool swimming,
                          bool falling_fast, const Settings& s);

// The controls' movement buffer (third person): 32 samples of the
// requested direction and speed with their update times; each update adds one and drops the oldest while the rest
// still cover more than the buffer depth, and the oldest left is what moves the player. A request below 5 units/s
// keeps the last direction. A change of direction by more than fFreezeDirectionDefaultAngleThreshold between the
// sample before a change and its last sample flags that last sample; when it leaves the buffer the controls take
// the new direction at the current speed for that update (the freeze-direction end, code 1).
class MoveBuffer {
public:
    struct Out {
        float direction = 0;     // radians, clockwise from ahead
        glm::vec2 velocity{0};   // facing space (x right, y ahead), units/s
        bool freeze_end = false; // a large direction change reached the output this update
    };
    Out push(float direction, float speed, float dt, const Settings& s);

private:
    struct Sample {
        float direction = 0, speed = 0, dt = 0;
        uint8_t flags = 0; // 1 start, 2 end of a large direction change; 4, 8 of a large speed change; 16 emitted
    };
    static constexpr unsigned k_size = 32;
    Sample m_samples[k_size]; // a ring, starting full of still samples
    unsigned m_tail = 0, m_count = k_size;
    float m_time = 0;
    int m_change_start = -1, m_change_end = -1;
    bool m_changing = false, m_check = false;
};

// A stick through the device's radial deadzone: the axes as floats
// (XInput value / 32767, clamped), then s / |s| x clamp((|s| - dz) / (max - dz), 0, 1).
glm::vec2 stick_deadzone(glm::vec2 stick, float deadzone, float deadzone_max);
// The look vector from the right stick (gamepad branch); `full_time` is the
// controls' "stick fully deflected" timer.
glm::vec2 gamepad_look(glm::vec2 stick, float dt, float& full_time, const Settings& s);

// At or above this speed the actor counts as running fast enough to start a sprint.
float jog_speed(const Speeds& speeds, const Settings& s);

float sprint_drain_per_second(float equipped_weight, const Settings& s);
// Health, magicka or stamina regained per second: the maximum times rate and rate multiplier (percent each).
float regen_per_second(float max_value, float rate, float rate_mult, bool in_combat,
                               const Settings& s);
// Health lost landing after falling `distance` units from the highest point of the fall.
float fall_damage(float distance, const Settings& s);
// Take-off speed of a jump, units/s: the speed whose rise under world gravity is fJumpHeightMin x scale.
float jump_speed(float scale, bool swimming, float gravity_units, const Settings& s);
float air_control_gain(const Settings& s);

// Mouse counts this frame to the look vector the controls work with: x a yaw-rate input (per second), y a
// pitch input. The yaw rate is x times the movement type's rotate-in-place run speed.
glm::vec2 mouse_look(glm::vec2 counts, float frame_dt, const Settings& s);
// Pitch change for a pitch input, radians, positive looking down.
float pitch_delta(float look_y, float fov_deg, const Settings& s);

} // namespace spud::engine::movement
