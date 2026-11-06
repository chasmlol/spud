#pragma once

// The one command language: bench
// scenario commands plus cell / wait_loaded / wait_frames / screenshot /
// screenshot_ui / state / set / ui / quit, and the player's mode / player / move / jump. Used by --run (batch),
// --bench (batch + timing),
// --screenshot (a one-line script) and --control (live session).
//
// Coordinates are Skyrim units, angles degrees like getangle (heading
// clockwise from north, pitch positive down); z "+N" = N units above terrain.

#include "engine/camera.hpp"

#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spud::engine {

struct Pose {
    glm::vec3 position_units{0};
    bool z_above_terrain = false;
    float heading_deg = 0;
    float pitch_deg = 0;
};

// Scripted player controls, held for a number of 60 Hz ticks.
struct PlayerMove {
    float forward = 0; // -1 .. 1
    float right = 0;
    enum class Gait { walk, run, sprint } gait = Gait::run; // sprint: the sprint button held
    bool jump = false;  // pressed on the first tick
    bool sneak = false; // pressed on the first tick
    glm::vec2 look{0};  // the game's look vector (x yaw-rate input, y pitch input), every tick
    // A gamepad: stick deflections -1..1 (y up = forward) as XInput reports them, and XInput button bits held.
    bool gamepad = false;
    glm::vec2 left_stick{0}, right_stick{0};
    uint16_t buttons = 0;
};

struct Command {
    enum class Kind {
        name, world, resolution, draw_distance, start, fly, teleport, hold, spin, segment, papyrus, // scenario
        cell, wait_loaded, wait_frames, screenshot, screenshot_hdr, screenshot_shadow, state, set, quit, spawn, effect, // automation
        mode, player, move, jump, trace, player_action,                                   // player
        ignored,                                                                          // the game-only
        ui,                                                                               // game UI (docs/UI.md)
        audio,                                                                            // audio <sub> ...
        door, game,                                                                            // door <sub> ...
        fixed_step, seed, target, wall_limit, preset, record, travel, attack,             // bench: a replayable tour
    };
    Kind kind = Kind::quit;
    std::string source; // file name or "control"
    int line = 0;
    std::string text;   // the original line, for messages
    Pose pose;
    float speed = 0;    // fly, m/s (0 = default)
    float seconds = 0;  // hold, spin, wait_loaded timeout
    float degrees = 0;  // spin
    float value = 0;    // draw_distance
    int ix = 0, iy = 0; // cell
    std::optional<float> z_units; // cell "+z"
    uint32_t width = 0, height = 0;
    int frames = 0;
    std::string arg;    // name / world / segment label / file / setting name / mode / ui sub-command
    std::string arg2;   // setting value / ui arguments
    PlayerMove move;    // move, jump
    bool press = false; // press: held for `seconds`, then released for a tick
    bool with_ui = false; // screenshot_ui
    std::vector<std::string> args; // audio: the words after the command

    std::string where() const { return source + ":" + std::to_string(line); }
};

// Throws Error("<source>:<line>: ...") for malformed lines and unknown commands.
// Returns nullopt for blank lines and comments.
std::optional<Command> parse_command(std::string_view line, const std::string& source, int line_no);
std::vector<Command> parse_script(const std::filesystem::path& file);

// What the executor drives. Implemented by the application.
class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    virtual FreeCamera& camera() = 0;
    virtual bool papyrus_call(std::string_view target) = 0; // "Script.Function"
    virtual std::optional<float> terrain_height(float x, float y) = 0; // metres
    virtual float cell_size() = 0;
    virtual bool streaming_busy() = 0;
    virtual void load_world(const std::string& edid) = 0;
    virtual void set_resolution(uint32_t width, uint32_t height) = 0;
    virtual void set_setting(const std::string& name, const std::string& value) = 0; // throws on unknown
    virtual void request_screenshot() = 0;                         // capture the next rendered frame
    virtual void request_ui_screenshot() = 0;                      // ... including the game UI
    virtual void save_screenshot(const std::filesystem::path& file) = 0; // after that frame
    // The HDR scene before tonemapping, as a float16.npy (H x W x 4): checks against the game.
    virtual void request_hdr_screenshot() = 0;
    virtual void save_hdr_screenshot(const std::filesystem::path& file) = 0;
    // The sun's shadow cascades of the next frame, as uint16 layers x size x size.
    virtual void request_shadow_screenshot() = 0;
    virtual void save_shadow_screenshot(const std::filesystem::path& file) = 0;
    virtual std::string state_json() = 0;
    // The player. These throw when there is no world to walk in.
    virtual void set_walk_mode(bool walk) = 0;
    virtual void place_player(const glm::vec3& feet, float heading_deg, float pitch_deg) = 0; // walk mode
    virtual void queue_player(const PlayerMove& move, int ticks) = 0;
    virtual size_t player_scripted() = 0; // queued ticks not yet run
    // Starts recording every player tick, or with "off" stops and writes the CSV started last.
    virtual void trace_player(const std::string& file) = 0;
    // sneak on|off, run on|off, restore, camera first: returns the ticks it queued (0: done at once).
    virtual int player_action(const std::string& what, const std::string& arg) = 0;
    // Draws a mesh of the cache that no reference places (render-path tests: actor heads, eyes, hair), at a
    // position in game units with a heading in degrees.
    virtual void spawn(const std::string& mesh, const glm::vec3& position_units, float heading_deg) = 0;
    virtual void play_effect(const std::string& model, const glm::vec3& position_units, float heading_deg) = 0;
    // "ui <sub> <args>": open/close/activate menus, keys, the console (docs/UI.md). Returns the inline
    // result ("ui state -"), throws on errors.
    virtual std::string ui_command(const std::string& sub, const std::string& args) = 0;
    // `audio <sub> ...` (docs/AUDIO.md). Returns true when done; otherwise the executor waits until
    // audio_busy() turns false (captures). Throws on bad arguments.
    virtual bool audio_command(const std::vector<std::string>& args, std::string& result) = 0;
    virtual bool audio_busy() = 0;
    // `door list | open|go <formid|nearest> | through | close <formid|nearest|all>` (docs/AUTOMATION.md). Returns
    // the inline result; throws on bad arguments.
    virtual std::string door_command(const std::vector<std::string>& args) = 0;
    // `game start | additem <form> [n] | ...` (docs/AUTOMATION.md): the player's items. Throws on bad arguments.
    virtual std::string game_command(const std::vector<std::string>& args) = 0;
    // `travel <space|-> <x> <y> <z> <heading> <pitch>`: the player moved there behind a real loading screen. The
    // command ends when loading() does.
    virtual void travel(const std::string& space, const Pose& pose) = 0;
    virtual bool loading() = 0;
    // `attack left|right <seconds>`: the Attack/Block button of that hand held, as the mouse button would be.
    virtual void attack(int hand, float seconds) = 0;
};

class ScriptExecutor {
public:
    // ok, and the inline result (state -) or the error message.
    using Done = std::function<void(bool ok, const std::string& result)>;

    explicit ScriptExecutor(ScriptHost& host) : m_host(host) {}

    // Without `done`, errors propagate as exceptions (batch mode).
    void push(Command command, Done done = {});
    // Runs before anything already queued (a console line typed while a script waits).
    void push_front(Command command, Done done = {});
    // Drops every queued command (not the one running), without calling their callbacks.
    void clear() { m_queue.clear(); }
    // Advances the script by `dt` seconds before a frame. Returns false when nothing is queued.
    bool before_frame(double dt);
    // After the frame has been rendered.
    void after_frame();

    bool idle() const { return !m_active && m_queue.empty(); }
    bool quit_requested() const { return m_quit; }
    // The camera went through a door into another space: the rest of a fly path follows it there (`point` maps
    // positions, `angle` turns headings counter-clockwise).
    void carry(const std::function<glm::vec3(const glm::vec3&)>& point, float angle);
    // Bench: index into segments() for this frame ("default" if no segment started yet).
    int current_segment();
    const std::vector<std::string>& segments() const { return m_segments; }

    static constexpr float k_default_fly_speed = 30.0f;        // m/s
    static constexpr float k_default_wait_timeout = 180.0f;    // s
    static constexpr float k_default_cell_height_units = 2000.0f;
    static constexpr double k_player_tick = 1.0 / 60.0;

private:
    struct Entry {
        Command command;
        Done done;
    };
    struct PathPoint {
        glm::vec3 pos;
        float yaw;
        float pitch;
        float speed;
    };

    glm::vec3 resolve(const Pose& pose);
    void apply_pose(const Pose& pose);
    // Runs an instant command or sets up a timed one; returns true if it already finished.
    bool start(Entry& e);
    // Advances a timed command; returns true when finished. Consumes from dt.
    bool advance(Entry& e, double& dt);
    void finish(bool ok, const std::string& result);
    void fail(const std::string& message);

    ScriptHost& m_host;
    std::deque<Entry> m_queue;
    bool m_active = false;
    Entry m_current;
    double m_elapsed = 0;
    int m_frames_waited = 0;
    bool m_capture_pending = false;
    bool m_quit = false;
    std::string m_result;
    // fly
    std::vector<PathPoint> m_path;
    size_t m_path_seg = 0;
    std::vector<Entry> m_path_entries; // later fly commands folded into this path
    float m_spin_yaw0 = 0;
    std::vector<std::string> m_segments;
    int m_segment = -1;
};

} // namespace spud::engine
