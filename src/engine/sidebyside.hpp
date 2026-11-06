#pragma once

// Mirror mode's end in the engine: --mirror <port> listens for the game,
// which streams the game's own input events and, every frame, where the player is. They come out here as queued
// input and the latest pose; the App applies them at its usual points of the frame. Telemetry goes to the session
// folder on the shared timeline: t, seconds of the machine's performance counter since the session's start.

#include <SDL3/SDL_scancode.h>

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace spud::engine {

struct MirrorInput {
    enum class Kind { key, mouse_button, mouse_move, wheel, text, release_all, mark, resync };
    Kind kind = Kind::key;
    uint32_t code = 0; // key: DirectInput scan code; mouse_button: 0 left, 1 right, 2 middle...; text: code point
    bool down = false;
    float x = 0, y = 0; // mouse_move: counts; wheel: x notches
    double t = 0;       // when the game saw it
};

// Where the game has the player, as of its last frame.
struct MirrorPose {
    bool valid = false;
    double t = 0;
    glm::vec3 position{0}; // feet, units
    glm::vec3 velocity{0}; // units/s
    float heading = 0, pitch = 0; // degrees
    float hour = 0;
    uint32_t weather = 0, cell = 0;
    bool interior = false;
    bool settled = false; // in the world: no load under way, the player's controller there
    int view = 0;         // the game's camera state id
    std::string world;    // the worldspace's or interior cell's EditorID, lowercase
};

class SideBySide {
public:
    SideBySide(uint16_t port, std::filesystem::path dir, int64_t qpc0);
    ~SideBySide();
    SideBySide(const SideBySide&) = delete;
    SideBySide& operator=(const SideBySide&) = delete;

    bool connected() const { return m_connected.load(); }
    const std::filesystem::path& dir() const { return m_dir; }
    double now() const; // the shared timeline

    // Main thread.
    std::vector<MirrorInput> take_input();
    MirrorPose pose();

    // What the game holds down, for the engine's polled input (App::key_state).
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    uint32_t mouse_buttons = 0; // SDL_BUTTON_MASK bits

    // Telemetry, one JSON object per line. `fields` is the object's body without its braces.
    void frame(std::string_view fields);
    void world(std::string_view fields);
    void event(std::string_view type, std::string_view fields = {});
    void log_line(int level, std::string_view message); // any thread
    void flush();

private:
    void thread_main();
    void parse(const std::string& line);
    void write(FILE* f, double t, std::string_view fields);

    std::filesystem::path m_dir;
    int64_t m_qpc0 = 0;
    double m_tick = 1.0;
    uintptr_t m_listen = ~uintptr_t(0);
    std::atomic<uintptr_t> m_client{~uintptr_t(0)};
    std::atomic<bool> m_stop{false}, m_connected{false};
    std::thread m_thread;
    std::mutex m_mutex;
    std::deque<MirrorInput> m_input;
    MirrorPose m_pose;
    FILE* m_frames = nullptr;
    FILE* m_world = nullptr;
    FILE* m_events = nullptr;
    FILE* m_log = nullptr;
    std::mutex m_log_mutex;
};

} // namespace spud::engine
