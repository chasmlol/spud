#pragma once

#include <atomic>

// The player on foot: Skyrim's movement rules (engine/movement.hpp) driving Skyrim's character controller
// (physics::Character), on a fixed 60 Hz tick. has the source of every rule.
//
// Ticks run on the player's own simulation thread, one tick ahead of what is shown: while the camera shows
// the interpolation between ticks k-1 and k, tick k+1 is being computed. A tick (collision streaming plus the
// character step) therefore never lands on a frame. Every tick consumes one input sample, taken from the
// script queue if there is one, else from the live controls. A tick only runs when the collision around the
// player is in; until then the player waits. While a script holds the player, ticks run only on scripted
// input, so scripted runs are deterministic whatever the frame rate.

#include "engine/camera.hpp"
#include "engine/camera3.hpp"
#include "engine/movement.hpp"
#include "formats/osr.hpp"
#include "formats/osw.hpp"
#include "physics/physics.hpp"

#include <glm/glm.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace spud::engine {

// Everything the player's movement depends on, from the imported rules (see from_rules).
struct MovementTuning {
    movement::Settings settings;
    // The movement types the behaviour graph selects for the player.
    osr::MovementType walk_run, sneak, sprint, swim;
    osr::MovementType attacking, attacking_2h, power_attacking; // the graph's iState 12, 13 and 14
    movement::Body body;
    float gravity = 9.81f;        // m/s^2, the Havok world's
    float eye_height = 1.7216f;   // m above the feet, standing (the first-person camera node, 120.5 units)
    float sneak_eye_height = 1.2349f; // m above the feet, sneaking (measured: 86.433 units)
    float max_stamina = 100;
    float max_health = 100;
    float stamina_rate = 5;       // StaminaRate actor value, percent of maximum per second
    float heal_rate = 0.7f;       // HealRate actor value (measured for the new-game player)
    float stamina_rate_mult = 100;
    float fov_deg = 80;
    physics::CharacterSettings controller;
    float body_height = 1.8838f;  // m, the game: 2 x the bound's half height x scale (water levels)
    // Third person: the camera pivot above the feet standing (the Cam3 node, 121 units) and sneaking (the sneak
    // animations move the node: measured 85 units, reached over 0.2 s along a smoothstep); the movement ramp
    // (fPlayerThirdPersonDampenTime:Controls, seconds; first person has none, measured).
    float camera3_height = 1.7288f;
    float sneak_camera3_height = 1.2144f;
    float sneak_blend_time = 0.2f; // the same blend carries the first-person eye (measured: ground6 sneak)
    float third_person_dampen_time = 0.25f;
    camera3::Settings camera3;
    uint64_t camera_layers = 0; // layers the camera's sphere collides with (L_CAMERA's row); 0 = none
    // On a mount: the movement types below and the rest of this tuning are the mount's.
    bool riding = false;
    osr::MovementType fall;
    float max_upward_pitch = 0.43633f; // fHorseMaxUpwardPitch
    float max_angle_before_turn = 1.5708f; // fHorseMaxAngleBeforeTurn: how far the view leaves a moving horse

    static MovementTuning from_rules(const osr::Rules& rules);
};

// A mount under the player: what replaces the player's own movement while riding.
struct Riding {
    osr::MovementType walk_run, sprint, swim, fall;
    glm::vec3 half_extents{0};  // metres, its bound
    float max_stamina = 100, stamina = 100;
    float stamina_rate = 10;    // percent of maximum per second
    float dampen_time = 1;      // fHorseControlsDampenTime
    float max_upward_pitch = 0.43633f, max_angle_before_turn = 1.5708f;
    glm::vec3 camera_offset{0}; // units, the race's
};

// One tick's worth of controls, as Skyrim's the game hold them.
struct PlayerInput {
    glm::vec2 move{0};  // x right, y forward: keys give -1, 0 or 1 per axis
    bool run = true;    // the run flag (always run unless toggled)
    bool gamepad = false;
    glm::vec2 look{0};  // the game's look vector: x yaw-rate input (per second), y pitch input
    glm::vec2 right_stick{0}; // gamepad: the look stick after its deadzone, turned into `look` per tick
    bool jump = false;  // pressed this tick
    bool lift_off = false; // riding: the mount's jump clip says it leaves the ground now (Player::lift_off)
    bool sprint_press = false; // the sprint button went down this tick (toggles the sprint request)
    bool sneak = false; // pressed this tick (toggles sneaking)
    float yaw = 0;      // facing for the tick, radians clockwise from north (set by the player)
    float pitch = 0;    // view pitch, radians, positive up
    bool third_person = false; // the view for the tick (set by the player)
    bool weapon_drawn = false; // the camera's combat offset
    // What the behaviour graph holds of the movement: driven with
    // bAllowRotation or bAnimationDriven, the keys don't move the player, the clips' motion does (units/s, facing space).
    bool graph_driven = false;
    glm::vec2 graph_velocity{0.0f};
    int graph_state = 0; // iState: an attack moves and turns with its own movement type
    float zoom = 0;            // third person: wheel notches this tick, + zooms out
};

enum class View { first_person, third_person };

// The gameplay side of the player: what Skyrim keeps as actor state and actor values.
struct PlayerStatus {
    bool sprinting = false;      // the sprint movement state (actor flag)
    bool graph_sprint = false;   // what the behaviour graph is told: the flag without its tail after a stop
    glm::vec2 asked{0.0f};       // the commanded velocity, units/s, x right and y ahead: the graph's Speed and Direction
    bool sprint_request = false;
    bool sneaking = false;
    bool swimming = false;
    uint32_t jumps_asked = 0;    // riding: jump presses so far, for the mount's graph to act on
    bool riding = false;
    float mount_heading = 0;     // while riding; the player's own yaw is then the view's
    float stamina = 100;         // the mount's while riding, as the HUD shows it
    float health = 100;
    float stamina_regen_delay = 0;
    float health_regen_delay = 0;
    float speed = 0;             // units/s requested this tick
    float last_fall_damage = 0;
    uint32_t falls_hurt = 0; // landings that have hurt: the app answers each one
    float breath = 20;           // seconds of air left under water
    float submersion = 0;        // water depth at the feet over the body height
    float process_time = 0;      // the session clock the regeneration pass read this tick, seconds
};

class Player {
public:
    static constexpr double k_tick = 1.0 / 60.0;

    // `water` gives the cells' water heights (may be null: no water).
    Player(physics::World& world, std::shared_ptr<const osw::World> water, const MovementTuning& tuning,
           const glm::vec3& feet);
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Main thread, once per frame: advances the shown time by `dt`, collecting finished ticks and starting
    // the next with `live` (unless scripted input is queued).
    void frame(double dt, const PlayerInput& live);
    // Mouse movement this frame (counts), turned into heading and pitch as the game does.
    void look_counts(glm::vec2 counts);
    // The Cam3 height of a form the player has taken, metres; 0 for their own.
    void set_form_camera_height(float metres) { m_form_camera_height.store(metres, std::memory_order_relaxed); }
    // Instant move (waits for a running tick). The player waits there until its collision is in.
    void teleport(const glm::vec3& feet);
    // Into the saddle: the character becomes the mount's, at its feet and heading, and off it again at `feet`.
    void mount(const Riding& riding, const glm::vec3& feet, float heading);
    void dismount(const glm::vec3& feet);
    bool riding() const { return m_tuning.riding; }
    // While paused: the character is carried, by a dragon. No collision streams around it.
    void carried_to(const glm::vec3& feet);
    float mount_heading() const { return m_status.mount_heading; }
    // Through a load door into another space: the character moves into `world`,
    // its position mapped by `point`, its velocity and headings turned by `angle` (counter-clockwise); what it was
    // doing (scripted input, gait, status) carries on.
    void carry(physics::World& world, std::shared_ptr<const osw::World> water,
               const std::function<glm::vec3(const glm::vec3&)>& point, float angle);
    void set_facing(float yaw, float pitch);
    // Health and stamina back to full (scripts: between tests).
    void restore();
    // Scripts: set the session clock the regeneration pass measures time with.
    void set_process_time(float seconds);
    // Scripts: the breath left under water, so a drowning test needn't wait it out.
    void set_breath(float seconds);
    // Scripts: the console's ToggleAI. With AI processing off the process lists stop, and with them the session
    // clock, so regeneration delays stop counting down.
    void set_ai_processing(bool on);
    // First or third person (the game's Toggle POV). Third person starts at the closest zoom.
    void set_view(View view);
    void set_weapon_drawn(bool drawn) { m_weapon_drawn = drawn; }
    // Mouse wheel notches (up zooms in): the third-person zoom; zooming in past the closest switches to first
    // person, zooming out in first person to third (FirstPersonState).
    void wheel(float notches);
    View view() const { return m_view; }
    // Riding: the mount's jump clip reached its lift-off, so the next tick jumps.
    void lift_off() { m_lift_off.store(true, std::memory_order_relaxed); }
    // The view the frame shows: a switch to third person shows as one once its camera has a place, so the body and the
    // camera change on the same frame and the eyes never look out of the third-person head.
    View shown_view() const { return m_view == View::third_person && !m_third_camera_ready ? View::first_person : m_view; }
    // Stop ticking (noclip): the player stays where it is.
    void set_paused(bool paused) { m_paused = paused; }
    bool paused() const { return m_paused; }
    // Scripts: between scripted moves, time stands still for the player (collision keeps streaming).
    void set_hold(bool hold) { m_hold = hold; }
    // Dead or down: the body doesn't regenerate (the game's update stops at IsDead).
    void set_down(bool down) { m_down = down; }
    // A run that replays: the 60 Hz step never slips a frame, whatever the job took.
    void set_lockstep(bool on, float shown) {
        m_lockstep = on;
        m_lockstep_shown = shown;
    }

    // How far the frame being drawn is between the player's (and physics') last two steps.
    float shown_fraction() const { return m_shown_fraction; }
    // Main thread, each world tick: what the behaviour graph says of the movement (PlayerInput).
    void set_graph(bool driven, glm::vec2 velocity, int state);
    // Interpolated for the frame being drawn.
    glm::vec3 feet() const { return m_shown_feet; }
    glm::vec3 eye() const { return m_shown_feet + glm::vec3(0, 0, eye_height()); }
    glm::vec3 first_person_lag() const { return m_shown_lag; } // metres, the eye behind the body
    // The view position: the eye in first person, the third-person camera otherwise (interpolated too).
    glm::vec3 camera() const;
    float camera_yaw() const;
    float camera_pitch() const; // positive up
    float eye_height() const { return eye_height(m_shown_sneak); }
    float yaw() const { return m_yaw; }
    float pitch() const { return m_pitch; }
    const physics::CharacterState& state() const { return m_curr; } // latest shown tick
    const PlayerStatus& status() const { return m_status; }
    // The character's health and stamina, as hits, effects and level-ups leave them. The next tick starts from them: the
    // one running now can't be told, so what it hands back is read as a change, never as a value.
    void set_body(float health, float stamina) {
        m_body_hurt = m_body_hurt || health < m_status.health; // any loss holds regeneration
        m_status.health = m_body.x = health;
        m_status.stamina = m_body.y = stamina;
        m_body_set = true;
    }
    // Health and stamina the ticks shown since the last call added or took away.
    // A shove, metres a second: it fades to nothing over `seconds`.
    void knock(const glm::vec3& velocity, float seconds) {
        m_knock_next = velocity;
        m_knock_next_seconds = seconds;
    }
    glm::vec2 take_body_change() { return std::exchange(m_body_change, glm::vec2(0.0f)); }
    // Waiting for collision around the player.
    bool waiting() const { return m_waiting; }
    uint64_t ticks() const { return m_ticks; }

    // Scripts: inputs for the next ticks, in order. scripted() counts those not yet shown.
    void queue_input(const PlayerInput& input, int ticks);
    size_t scripted();
    void clear_script() { m_script.clear(); }

    float land_height(glm::vec2 p) const; // metres, the drawn LAND surface; NaN where there is none
    MovementTuning& tuning() { return m_tuning; }
    physics::World& world() { return *m_world; }

    // Every tick's result from now on, for traces.
    // References whose collision body positions every trace row also records (pushed clutter).
    void watch(std::vector<uint32_t> form_ids);
    struct WatchedRef {
        uint32_t form_id;
        glm::vec3 position; // metres
    };
    struct TraceRow {
        uint64_t tick;
        std::vector<WatchedRef> refs;
        physics::CharacterState state;
        PlayerStatus status;
        float yaw, pitch;
        float eye_height;
        glm::vec3 camera; // the view position (metres)
        bool third_person;
    };
    void start_trace() {
        m_trace.clear();
        m_tracing = true;
    }
    std::vector<TraceRow> stop_trace() {
        m_tracing = false;
        return std::move(m_trace);
    }

private:
    std::atomic<float> m_form_camera_height{0.0f};
    void replace_character(const glm::vec3& feet);
    struct TickResult {
        bool ran = false;   // the character moved one tick
        bool ready = false; // collision around the player is in
        physics::CharacterState state;
        PlayerStatus status;
        glm::vec2 body_change{0.0f}; // health and stamina, by this tick
        float yaw = 0, pitch = 0; // facing after the tick (third person turns the player in the tick)
        glm::vec3 camera{0};      // third-person camera, metres
        float camera_yaw = 0, camera_pitch = 0;
        bool camera_free = false;
        float camera_zoom = 0;
        float sneak = 0;          // 0 standing .. 1 sneaking: the eye's and the camera pivot's blend, eased
        std::vector<WatchedRef> refs;
    };
    // Simulation-thread state carried from tick to tick (the game's one-frame pipelines).
    struct Sim {
        PlayerStatus status;
        // The game's float clock of world time since the session started (g_ProcessListsRunningTime), advanced
        // once per stepped tick after the player's update, and its value at the last regeneration pass.
        float process_time = 0;
        float regen_last = 0;
        bool ai_processing = true;
        bool graph_sprint = false; // the sprint state the behaviour graph moves with this tick
        int sprint_tail = 0;       // ticks the sprint state outlasts the graph's sprint (its stop transition)
        bool was_moving = false;
        float last_speed = 0;      // units/s moved last tick
        // Third person.
        camera3::ThirdPerson camera;
        bool camera_started = false;
        float sneak_blend = 0;        // 0 standing .. 1 sneaking, before easing
        glm::vec2 move{0};            // commanded velocity in facing space (x right, y ahead), units/s
        glm::vec3 world_move{0};      // ... in the world, last tick
        movement::MoveBuffer buffer;  // third person: the controls' 32-sample buffer
    };

    float eye_height(float sneak) const {
        return m_tuning.eye_height + (m_tuning.sneak_eye_height - m_tuning.eye_height) * sneak;
    }
    void launch();
    bool job_done();
    void wait();
    void tick(const PlayerInput& in, bool step); // simulation thread
    void sim_main();
    float turn_speed() const; // rad/s per unit of yaw input, for the movement type in use

    float water_height(glm::vec2 p) const; // metres, NaN where there is no water
    std::optional<glm::vec3> current_at(const glm::vec3& p) const; // the cell water's current, m/s

    physics::World* m_world;
    std::shared_ptr<const osw::World> m_water;
    MovementTuning m_tuning;
    MovementTuning m_on_foot;   // while riding
    float m_foot_stamina = 0;
    std::unique_ptr<physics::Character> m_character;
    Sim m_sim;

    // Shown state: ticks k-1 and k, and time into that interval.
    physics::CharacterState m_prev, m_curr;
    PlayerStatus m_status;
    double m_into = 0;
    glm::vec3 m_shown_feet{0};
    float m_yaw = 0, m_pitch = 0;
    View m_view = View::first_person;
    glm::vec3 m_prev_camera{0}, m_curr_camera{0}, m_shown_camera{0};
    std::atomic<bool> m_lift_off{false};
    bool m_third_camera_ready = false; // a third-person tick has placed the camera since the view changed
    // The game steps this once a frame at 60 fps, and its spring depends on the step: it stays on the tick.
    FirstPersonLag m_lag;
    bool m_lag_on = false;
    glm::vec3 m_prev_lag{0}, m_curr_lag{0}, m_shown_lag{0};
    float m_prev_sneak = 0, m_curr_sneak = 0, m_shown_sneak = 0;
    float m_camera_yaw = 0, m_camera_pitch = 0, m_prev_camera_yaw = 0, m_prev_camera_pitch = 0;
    bool m_camera_free = false; // free rotation: the look input circles the camera, not the player
    // The mouse runs at the frame rate and the third-person camera on ticks. In free rotation the look input is the
    // camera's turn: what the ticks haven't reported yet, waiting or on its way through one, joins the last tick's
    // angles. Otherwise the camera chases the heading at its own speed, so the turn can't be told ahead (it overshoots
    // and comes back every tick): the frames between show the last two ticks' angles blended, as they do its position.
    glm::vec2 look_ahead() const;
    glm::vec2 m_look_counts{0}; // third person: mouse counts since the last live tick
    uint64_t m_ticks = 0;
    bool m_waiting = true;
    bool m_paused = false;
    bool m_lockstep = false;
    float m_lockstep_shown = 0.0f; // how far between its last two ticks the frame shows the player
    float m_shown_fraction = 1.0f;
    PlayerInput m_graph; // its graph_ fields only
    bool m_hold = false;
    bool m_down = false;

    PlayerInput m_live;
    bool m_jump_latch = false;  // a jump pressed since the last live tick
    bool m_sneak_latch = false;
    bool m_sprint_latch = false;
    float m_pad_full_time = 0; // the gamepad look's full-deflection timer
    float m_wheel = 0;         // wheel notches for the next live tick
    PlayerInput m_pad_polled;  // scripts: the pad state the device reads on the next tick
    std::optional<glm::vec3> m_water_feet; // after a teleport: where the water update last saw the feet
    std::vector<uint32_t> m_watch;         // see watch(); read by the tick
    std::vector<uint32_t> m_job_watch;
    glm::vec2 m_body{0.0f}, m_job_body{0.0f}; // health, stamina
    bool m_body_set = false, m_job_body_set = false;
    bool m_body_hurt = false, m_job_body_hurt = false;
    glm::vec3 m_knock_next{0.0f}, m_job_knock_next{0.0f}, m_knock{0.0f};
    float m_knock_next_seconds = 0, m_job_knock_next_seconds = 0, m_knock_seconds = 0, m_knock_left = 0;
    glm::vec2 m_body_change{0.0f};
    float m_camera_zoom = 0;   // the third-person camera's zoom, last tick
    bool m_weapon_drawn = false;
    std::deque<PlayerInput> m_script;
    std::vector<TraceRow> m_trace;
    bool m_tracing = false;
    PlayerInput m_launched;       // input of the tick in flight
    bool m_launched_step = false; // ... false: it only streams collision (held)
    bool m_launched_scripted = false;

    // Simulation thread: one job at a time. The job's input and tuning are copies, so the main thread can
    // change its own while a tick runs.
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_job = false;        // a tick is queued or running
    bool m_job_queued = false; // ... and not yet picked up
    bool m_stop = false;
    PlayerInput m_job_input;
    bool m_job_step = true;
    MovementTuning m_job_tuning;
    TickResult m_result;
};

} // namespace spud::engine
