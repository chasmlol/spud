#include "engine/script.hpp"

#include "core/log.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/units.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <numbers>
#include <sstream>

namespace spud::engine {

namespace {

[[noreturn]] void fail_at(const std::string& source, int line, const std::string& msg) {
    throw Error(std::format("{}:{}: {}", source, line, msg));
}

float to_float(const std::string& source, int line, std::string_view tok, const char* what) {
    float v = 0;
    const char* b = tok.data();
    const char* e = tok.data() + tok.size();
    if (b != e && *b == '+') {
        ++b;
    }
    auto [ptr, ec] = std::from_chars(b, e, v);
    if (ec != std::errc() || ptr != e || !std::isfinite(v)) {
        fail_at(source, line, std::format("bad {} '{}'", what, tok));
    }
    return v;
}

Pose to_pose(const std::string& src, int line, const std::vector<std::string>& t, size_t at) {
    Pose p;
    p.position_units.x = to_float(src, line, t[at], "x");
    p.position_units.y = to_float(src, line, t[at + 1], "y");
    p.z_above_terrain = !t[at + 2].empty() && t[at + 2][0] == '+';
    p.position_units.z = to_float(src, line, t[at + 2], "z");
    p.heading_deg = to_float(src, line, t[at + 3], "yaw");
    p.pitch_deg = to_float(src, line, t[at + 4], "pitch");
    return p;
}

float shortest_delta(float from, float to) {
    const float two_pi = 2.0f * std::numbers::pi_v<float>;
    float d = std::fmod(to - from, two_pi);
    if (d > std::numbers::pi_v<float>) {
        d -= two_pi;
    } else if (d < -std::numbers::pi_v<float>) {
        d += two_pi;
    }
    return d;
}

glm::vec3 catmull_rom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t) {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

} // namespace

std::optional<Command> parse_command(std::string_view raw, const std::string& source, int line_no) {
    std::string text(raw);
    if (auto hash = text.find('#'); hash != std::string::npos) {
        text.resize(hash);
    }
    std::istringstream ls(text);
    std::vector<std::string> t;
    for (std::string tok; ls >> tok;) {
        t.push_back(tok);
    }
    if (t.empty()) {
        return std::nullopt;
    }
    Command c;
    c.source = source;
    c.line = line_no;
    c.text = std::string(raw);
    while (!c.text.empty() && (c.text.back() == '\r' || c.text.back() == '\n')) {
        c.text.pop_back();
    }
    const std::string& cmd = t[0];
    const size_t n = t.size() - 1;
    auto need = [&](size_t lo, size_t hi) {
        if (n < lo || n > hi) {
            fail_at(source, line_no,
                    std::format("'{}' takes {} argument(s), got {}", cmd,
                                lo == hi ? std::to_string(lo) : std::format("{}-{}", lo, hi), n));
        }
    };
    // Rest of the line after the command word (file names may contain spaces).
    auto rest = [&]() {
        std::string s = text.substr(text.find(cmd) + cmd.size());
        const size_t b = s.find_first_not_of(" \t");
        const size_t e = s.find_last_not_of(" \t\r\n");
        return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    };
    using K = Command::Kind;
    if (cmd == "name") {
        need(1, 1);
        c.kind = K::name;
        c.arg = t[1];
    } else if (cmd == "world") {
        need(1, 1);
        c.kind = K::world;
        c.arg = t[1];
    } else if (cmd == "resolution") {
        need(2, 2);
        c.kind = K::resolution;
        const float w = to_float(source, line_no, t[1], "width"), h = to_float(source, line_no, t[2], "height");
        if (w < 64 || h < 64 || w > 16384 || h > 16384) {
            fail_at(source, line_no, "resolution out of range");
        }
        c.width = static_cast<uint32_t>(w);
        c.height = static_cast<uint32_t>(h);
    } else if (cmd == "draw_distance") {
        need(1, 1);
        c.kind = K::draw_distance;
        c.value = to_float(source, line_no, t[1], "draw distance");
        if (c.value < 0 || c.value > 64) {
            fail_at(source, line_no, "draw distance out of range");
        }
    } else if (cmd == "start" || cmd == "teleport") {
        need(5, 5);
        c.kind = cmd == "start" ? K::start : K::teleport;
        c.pose = to_pose(source, line_no, t, 1);
    } else if (cmd == "fly") {
        need(5, 6);
        c.kind = K::fly;
        c.pose = to_pose(source, line_no, t, 1);
        if (n == 6) {
            c.speed = to_float(source, line_no, t[6], "speed");
            if (c.speed <= 0) {
                fail_at(source, line_no, "speed must be positive");
            }
        }
    } else if (cmd == "hold") {
        need(1, 1);
        c.kind = K::hold;
        c.seconds = to_float(source, line_no, t[1], "seconds");
        if (c.seconds < 0) {
            fail_at(source, line_no, "negative duration");
        }
    } else if (cmd == "spin") {
        need(2, 2);
        c.kind = K::spin;
        c.degrees = to_float(source, line_no, t[1], "degrees");
        c.seconds = to_float(source, line_no, t[2], "seconds");
        if (c.seconds <= 0) {
            fail_at(source, line_no, "spin needs a positive duration");
        }
    } else if (cmd == "segment") {
        need(1, 1);
        c.kind = K::segment;
        c.arg = t[1];
    } else if (cmd == "seed") {
        need(1, 1);
        c.kind = K::seed;
        c.arg = t[1];
    } else if (cmd == "fixed_step" || cmd == "target_ms" || cmd == "wall_limit") {
        // Bench settings of a tour that replays: the world's tick (Hz), the frame-time limit, the
        // whole run's length in seconds.
        need(1, 1);
        c.kind = cmd == "fixed_step" ? K::fixed_step : cmd == "target_ms" ? K::target : K::wall_limit;
        c.value = to_float(source, line_no, t[1], cmd.c_str());
    } else if (cmd == "preset") {
        need(1, 1);
        c.kind = K::preset;
        c.arg = t[1];
    } else if (cmd == "record") {
        need(0, 0);
        c.kind = K::record;
    } else if (cmd == "travel") {
        need(6, 6);
        c.kind = K::travel;
        c.arg = t[1];
        c.pose = to_pose(source, line_no, t, 2);
    } else if (cmd == "attack") {
        need(2, 2);
        c.kind = K::attack;
        if (t[1] != "left" && t[1] != "right" && t[1] != "voice") {
            fail_at(source, line_no, "attack left|right|voice <seconds>");
        }
        c.ix = t[1] == "left" ? 0 : t[1] == "right" ? 1 : 2;
        c.seconds = to_float(source, line_no, t[2], "seconds");
    } else if (cmd == "papyrus") {
        need(1, 1);
        c.kind = K::papyrus;
        c.arg = t[1];
    } else if (cmd == "cell") {
        need(2, 3);
        c.kind = K::cell;
        c.ix = static_cast<int>(to_float(source, line_no, t[1], "cell x"));
        c.iy = static_cast<int>(to_float(source, line_no, t[2], "cell y"));
        if (n == 3) {
            c.z_units = to_float(source, line_no, t[3], "z");
        }
    } else if (cmd == "wait_loaded") {
        need(0, 1);
        c.kind = K::wait_loaded;
        c.seconds = n == 1 ? to_float(source, line_no, t[1], "timeout") : ScriptExecutor::k_default_wait_timeout;
    } else if (cmd == "wait_frames") {
        need(1, 1);
        c.kind = K::wait_frames;
        c.frames = static_cast<int>(to_float(source, line_no, t[1], "frame count"));
        if (c.frames < 0) {
            fail_at(source, line_no, "negative frame count");
        }
    } else if (cmd == "screenshot" || cmd == "screenshot_ui") {
        if (n < 1) {
            need(1, 1);
        }
        c.kind = K::screenshot;
        c.with_ui = cmd == "screenshot_ui";
        c.arg = rest();
    } else if (cmd == "screenshot_hdr") {
        if (n < 1) {
            need(1, 1);
        }
        c.kind = K::screenshot_hdr;
        c.arg = rest();
    } else if (cmd == "screenshot_shadow") {
        if (n < 1) {
            need(1, 1);
        }
        c.kind = K::screenshot_shadow;
        c.arg = rest();
    } else if (cmd == "state") {
        if (n < 1) {
            need(1, 1);
        }
        c.kind = K::state;
        c.arg = rest();
    } else if (cmd == "spawn" || cmd == "effect") {
        // spawn X Y Z HEADING MESH (game units, degrees, a cache path such as meshes/actors/.../eyesmale.osm); effect
        // the same for a one-shot effect's .osm (meshes/effects/watersplash.osm).
        if (n < 5) {
            need(5, 5);
        }
        c.kind = cmd == "spawn" ? K::spawn : K::effect;
        c.pose.position_units = glm::vec3(to_float(source, line_no, t[1], "x"), to_float(source, line_no, t[2], "y"),
                                          to_float(source, line_no, t[3], "z"));
        c.pose.heading_deg = to_float(source, line_no, t[4], "heading");
        for (size_t i = 5; i < t.size(); ++i) {
            c.arg += (i > 5 ? " " : "") + std::string(t[i]); // the path may contain spaces
        }
    } else if (cmd == "set") {
        need(2, 2);
        c.kind = K::set;
        c.arg = t[1];
        c.arg2 = t[2];
    } else if (cmd == "audio") {
        if (n < 1) {
            need(1, 1);
        }
        c.kind = K::audio;
        c.args.assign(t.begin() + 1, t.end());
    } else if (cmd == "door") {
        if (n < 1) {
            need(1, 2);
        }
        c.kind = K::door;
        c.args.assign(t.begin() + 1, t.end());
    } else if (cmd == "game") {
        if (n < 1) {
            need(1, 8);
        }
        c.kind = K::game;
        c.args.assign(t.begin() + 1, t.end());
    } else if (cmd == "quit") {
        need(0, 0);
        c.kind = K::quit;
    } else if (cmd == "mode") {
        need(1, 1);
        c.kind = K::mode;
        c.arg = t[1];
        if (c.arg != "walk" && c.arg != "fly") {
            fail_at(source, line_no, "mode is 'walk' or 'fly'");
        }
    } else if (cmd == "player") {
        need(5, 5);
        c.kind = K::player;
        c.pose = to_pose(source, line_no, t, 1);
    } else if (cmd == "move") {
        need(2, 4);
        c.kind = K::move;
        static const std::pair<const char*, glm::vec2> k_dirs[] = {
            {"none", {0, 0}},          {"forward", {0, 1}},        {"back", {0, -1}},
            {"left", {-1, 0}},         {"right", {1, 0}},          {"forward_left", {-1, 1}},
            {"forward_right", {1, 1}}, {"back_left", {-1, -1}},    {"back_right", {1, -1}},
        };
        auto dir = std::ranges::find_if(k_dirs, [&](const auto& d) { return t[1] == d.first; });
        if (dir == std::end(k_dirs)) {
            fail_at(source, line_no, std::format("unknown direction '{}'", t[1]));
        }
        c.move.right = dir->second.x;
        c.move.forward = dir->second.y;
        c.seconds = to_float(source, line_no, t[2], "seconds");
        if (c.seconds < 0) {
            fail_at(source, line_no, "negative duration");
        }
        for (size_t i = 3; i <= n; ++i) {
            if (t[i] == "walk") {
                c.move.gait = PlayerMove::Gait::walk;
            } else if (t[i] == "run") {
                c.move.gait = PlayerMove::Gait::run;
            } else if (t[i] == "sprint") {
                c.move.gait = PlayerMove::Gait::sprint;
            } else if (t[i] == "jump") {
                c.move.jump = true;
            } else {
                fail_at(source, line_no, std::format("'{}': expected walk, run, sprint or jump", t[i]));
            }
        }
    } else if (cmd == "jump") {
        need(0, 0);
        c.kind = K::jump;
        c.move.jump = true;
    } else if (cmd == "wait") {
        // Time passes for the player: standing, no input.
        need(1, 1);
        c.kind = K::move;
        c.seconds = to_float(source, line_no, t[1], "seconds");
    } else if (cmd == "look") {
        need(3, 3);
        c.kind = K::move;
        c.move.look = {to_float(source, line_no, t[1], "look x"), to_float(source, line_no, t[2], "look y")};
        c.seconds = to_float(source, line_no, t[3], "seconds");
    } else if (cmd == "gamepad") {
        // gamepad <lx> <ly> <rx> <ry> <seconds> [lt v] [rt v] [button...]:.
        need(5, 32);
        c.kind = K::move;
        c.move.gamepad = true;
        c.move.left_stick = {to_float(source, line_no, t[1], "lx"), to_float(source, line_no, t[2], "ly")};
        c.move.right_stick = {to_float(source, line_no, t[3], "rx"), to_float(source, line_no, t[4], "ry")};
        c.seconds = to_float(source, line_no, t[5], "seconds");
        static const std::pair<const char*, uint16_t> k_buttons[] = {
            {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}, {"start", 0x0010},
            {"back", 0x0020}, {"ls", 0x0040}, {"rs", 0x0080}, {"lb", 0x0100}, {"rb", 0x0200},
            {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000},
        };
        for (size_t i = 6; i < t.size(); ++i) {
            if ((t[i] == "lt" || t[i] == "rt") && i + 1 < t.size()) {
                ++i; // triggers drive nothing in movement
                continue;
            }
            bool known = false;
            for (const auto& [name, bit] : k_buttons) {
                if (t[i] == name) {
                    c.move.buttons |= bit;
                    known = true;
                }
            }
            if (!known) {
                fail_at(source, line_no, std::format("gamepad: unknown button '{}'", t[i]));
            }
        }
    } else if (cmd == "press") {
        need(1, 2);
        c.kind = K::move;
        c.press = true;
        c.seconds = n == 2 ? to_float(source, line_no, t[2], "seconds") : float(ScriptExecutor::k_player_tick);
        if (t[1] == "jump") {
            c.move.jump = true;
        } else if (t[1] == "sneak") {
            c.move.sneak = true;
        } else if (t[1] == "sprint") {
            c.move.gait = PlayerMove::Gait::sprint;
        } else {
            fail_at(source, line_no, std::format("'{}': spud presses jump, sneak or sprint", t[1]));
        }
    } else if (cmd == "sneak" || cmd == "run" || cmd == "restore" || cmd == "camera") {
        need(cmd == "restore" ? 0 : 1, cmd == "camera" ? 6 : 1);
        c.kind = K::player_action;
        c.arg = cmd;
        c.arg2 = n >= 1 ? t[1] : "";
        for (size_t i = 2; i < t.size(); ++i) { // camera free x y z heading pitch
            c.arg2 += " " + t[i];
        }
    } else if (cmd == "process_time" || cmd == "breath") {
        // process_time <seconds>: the session clock regeneration measures time with; breath <seconds>: the breath
        // left under water. Both.
        need(1, 1);
        c.kind = K::player_action;
        c.arg = cmd;
        c.arg2 = t[1];
    } else if (cmd == "actor") {
        // actor <reference|player> event <name> | idle <editor id> | set <variable> <value> | kill | impulse <x> <y> <z>
        need(2, 5);
        c.kind = K::player_action;
        c.arg = cmd;
        c.arg2 = rest();
    } else if (cmd == "projectile") {
        // projectile <PROJ editor id or form (hex)> <heading> <pitch> [SPEL editor id]: degrees, launched from the
        // camera, logs its end. Form 0 with a spell flies as the spell's own.
        need(3, 4);
        c.kind = K::player_action;
        c.arg = cmd;
        c.arg2 = rest();
    } else if (cmd == "watch") {
        // watch <form id (hex)>.. | off:, the references' positions beside each trace row.
        need(1, 16);
        c.kind = K::player_action;
        c.arg = cmd;
        c.arg2 = rest();
    } else if (cmd == "fps") {
        // The player ticks at a fixed 60 Hz, which is what the game pins Skyrim's frame time to.
        need(1, 1);
        if (to_float(source, line_no, t[1], "fps") != 60.0f) {
            fail_at(source, line_no, "spud's player runs at a fixed 60 Hz: only 'fps 60'");
        }
        c.kind = K::ignored;
    } else if (cmd == "ai") {
        // ai on|off: the console's ToggleAI ("All AI Processing"), which also stops the session clock that
        // regeneration counts with.
        need(1, 1);
        c.kind = K::player_action;
        c.arg = cmd;
        c.arg2 = t[1];
    } else if (cmd == "messagebox" || cmd == "wait_messagebox" || cmd == "watchwrite" || cmd == "settings" || cmd == "log") {
        c.kind = K::ignored; // Skyrim's menus and settings dumps: nothing to do here
    } else if (cmd == "cow") {
        // Skyrim's console 'cow WORLD X Y': spud loads the worldspace and goes to the cell.
        need(3, 3);
        c.kind = K::cell;
        c.arg = t[1];
        c.ix = static_cast<int>(to_float(source, line_no, t[2], "cell x"));
        c.iy = static_cast<int>(to_float(source, line_no, t[3], "cell y"));
    } else if (cmd == "trace") {
        if (n < 1) {
            need(1, 1);
        }
        c.kind = K::trace;
        c.arg = rest();
    } else if (cmd == "ui") {
        if (n < 1) {
            need(1, 1);
        }
        c.kind = K::ui;
        c.arg = t[1];
        // Everything after the sub-command, verbatim (labels and console lines contain spaces).
        const std::string r = rest();
        const size_t after = r.find(t[1]) + t[1].size();
        const size_t b = r.find_first_not_of(" 	", after);
        c.arg2 = b == std::string::npos ? std::string() : r.substr(b);
        if (c.arg2.size() >= 2 && c.arg2.front() == '"' && c.arg2.back() == '"') {
            c.arg2 = c.arg2.substr(1, c.arg2.size() - 2);
        }
    } else {
        fail_at(source, line_no, std::format("unknown command '{}'", cmd));
    }
    return c;
}

std::vector<Command> parse_script(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) {
        throw Error("cannot open script " + file.string());
    }
    std::vector<Command> out;
    std::string line;
    int n = 0;
    const std::string source = file.filename().string();
    while (std::getline(in, line)) {
        ++n;
        if (auto c = parse_command(line, source, n)) {
            out.push_back(std::move(*c));
        }
    }
    return out;
}

// ---- executor ---------------------------------------------------------------------

void ScriptExecutor::push(Command command, Done done) { m_queue.push_back({std::move(command), std::move(done)}); }

void ScriptExecutor::push_front(Command command, Done done) {
    m_queue.push_front({std::move(command), std::move(done)});
}

int ScriptExecutor::current_segment() {
    if (m_segment < 0) {
        m_segments.push_back("default");
        m_segment = 0;
    }
    return m_segment;
}

glm::vec3 ScriptExecutor::resolve(const Pose& pose) {
    glm::vec3 p = pose.position_units * units::k_skyrim_unit_m;
    if (pose.z_above_terrain) {
        p.z = m_host.terrain_height(p.x, p.y).value_or(0.0f) + pose.position_units.z * units::k_skyrim_unit_m;
    }
    return p;
}

void ScriptExecutor::apply_pose(const Pose& pose) {
    FreeCamera& cam = m_host.camera();
    cam.position = resolve(pose);
    cam.set_skyrim_angles(pose.heading_deg, pose.pitch_deg);
}

void ScriptExecutor::finish(bool ok, const std::string& result) {
    m_active = false;
    if (m_current.done) {
        m_current.done(ok, result);
    }
    for (auto& e : m_path_entries) {
        if (e.done) {
            e.done(ok, result);
        }
    }
    m_path_entries.clear();
}

void ScriptExecutor::fail(const std::string& message) {
    const std::string msg = message.starts_with(m_current.command.source + ":")
                                ? message
                                : std::format("{}: {} ('{}')", m_current.command.where(), message,
                                              m_current.command.text);
    if (!m_current.done) {
        m_active = false;
        m_path_entries.clear();
        throw Error(msg);
    }
    finish(false, msg);
}

bool ScriptExecutor::start(Entry& e) {
    using K = Command::Kind;
    const Command& c = e.command;
    m_result.clear();
    switch (c.kind) {
    case K::name:
    case K::fixed_step:
    case K::seed:
    case K::target:
    case K::wall_limit:
    case K::preset:
    case K::record:
        return true;
    case K::travel:
        m_host.travel(c.arg, c.pose);
        return false; // ends with the loading screen (after_frame)
    case K::attack:
        m_host.attack(c.ix, c.seconds);
        return true;
    case K::papyrus:
        if (!m_host.papyrus_call(c.arg)) {
            throw std::runtime_error("papyrus: no such script: " + c.arg);
        }
        return true;
    case K::segment:
        m_segments.push_back(c.arg);
        m_segment = static_cast<int>(m_segments.size() - 1);
        return true;
    case K::world:
        m_host.load_world(c.arg);
        return true;
    case K::resolution:
        m_host.set_resolution(c.width, c.height);
        return true;
    case K::draw_distance:
        m_host.set_setting("draw_distance", std::format("{}", c.value));
        return true;
    case K::teleport:
        apply_pose(c.pose);
        return true;
    case K::start:
        apply_pose(c.pose);
        return false; // then waits for streaming like wait_loaded
    case K::cell: {
        if (!c.arg.empty()) {
            m_host.load_world(c.arg); // cow: that worldspace (nothing to do if it is loaded)
        }
        const float cs = m_host.cell_size();
        const glm::vec2 xy = glm::vec2(c.ix + 0.5f, c.iy + 0.5f) * cs;
        const float above = c.z_units.value_or(k_default_cell_height_units) * units::k_skyrim_unit_m;
        m_host.camera().position = glm::vec3(xy, m_host.terrain_height(xy.x, xy.y).value_or(0.0f) + above);
        return true;
    }
    case K::set:
        m_host.set_setting(c.arg, c.arg2);
        return true;
    case K::state: {
        const std::string json = m_host.state_json();
        if (c.arg == "-") {
            m_result = json;
        } else {
            std::filesystem::path p(c.arg);
            if (p.has_parent_path()) {
                std::filesystem::create_directories(p.parent_path());
            }
            write_file(p, std::as_bytes(std::span(json.data(), json.size())));
        }
        return true;
    }
    case K::screenshot:
        if (c.arg == "-") {
            throw Error("screenshot needs a file name");
        }
        if (c.with_ui) {
            m_host.request_ui_screenshot();
        } else {
            m_host.request_screenshot();
        }
        m_capture_pending = true;
        return false;
    case K::screenshot_hdr:
        if (c.arg == "-") {
            throw Error("screenshot_hdr needs a file name");
        }
        m_host.request_hdr_screenshot();
        m_capture_pending = true;
        return false;
    case K::screenshot_shadow:
        m_host.request_shadow_screenshot();
        m_capture_pending = true;
        return false;
    case K::quit:
        m_quit = true;
        return true;
    case K::mode:
        m_host.set_walk_mode(c.arg == "walk");
        return true;
    case K::ignored:
        return true;
    case K::player_action:
        return m_host.player_action(c.arg, c.arg2) == 0;
    case K::player:
        // As long: its console move lands after that frame's movement,
        // then one more frame runs before the next command.
        m_host.place_player(resolve(c.pose), c.pose.heading_deg, c.pose.pitch_deg);
        m_host.queue_player({}, 1);
        return false;
    case K::trace:
        m_host.trace_player(c.arg);
        return true;
    case K::move:
    case K::jump: {
        int ticks = static_cast<int>(std::lround(c.seconds / k_player_tick));
        if (ticks == 0 && c.kind == K::move && !c.press) {
            return true; // a wait of no frames ends in the frame it starts
        }
        ticks = std::max(ticks, 1);
        if (c.kind == K::jump || c.press) {
            // A press lasts as long: the frames the button is held, then the frame it is released.
            ticks = (c.kind == K::jump ? 1 : ticks) + 1;
            PlayerMove held = c.move;
            m_host.queue_player(held, ticks - 1);
            held.jump = held.sneak = false;
            held.gait = PlayerMove::Gait::run;
            m_host.queue_player(held, 1);
            return false;
        }
        m_host.queue_player(c.move, ticks);
        return false; // done once the player has run them (after_frame)
    }
    case K::spawn:
        m_host.spawn(c.arg, c.pose.position_units, c.pose.heading_deg);
        return true;
    case K::effect:
        m_host.play_effect(c.arg, c.pose.position_units, c.pose.heading_deg);
        return true;
    case K::ui:
        m_result = m_host.ui_command(c.arg, c.arg2);
        return true;
    case K::audio:
        return m_host.audio_command(c.args, m_result);
    case K::door:
        m_result = m_host.door_command(c.args);
        return true;
    case K::game:
        m_result = m_host.game_command(c.args);
        log::info("game {}: {}", c.args[0], m_result);
        return true;
    case K::fly: {
        // One smooth path through this and the directly following fly commands.
        m_path.clear();
        const FreeCamera& cam = m_host.camera();
        m_path.push_back({cam.position, cam.yaw, cam.pitch, 0});
        auto add = [&](const Command& f) {
            FreeCamera tmp;
            tmp.set_skyrim_angles(f.pose.heading_deg, f.pose.pitch_deg);
            m_path.push_back({resolve(f.pose), tmp.yaw, tmp.pitch, f.speed > 0 ? f.speed : k_default_fly_speed});
        };
        add(c);
        while (!m_queue.empty() && m_queue.front().command.kind == K::fly) {
            add(m_queue.front().command);
            m_path_entries.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        m_path_seg = 0;
        return false;
    }
    case K::spin:
        m_spin_yaw0 = m_host.camera().yaw;
        return false;
    case K::hold:
    case K::wait_loaded:
    case K::wait_frames:
        return c.kind == K::wait_frames && c.frames == 0;
    }
    return true;
}

bool ScriptExecutor::advance(Entry& e, double& dt) {
    using K = Command::Kind;
    const Command& c = e.command;
    FreeCamera& cam = m_host.camera();
    switch (c.kind) {
    case K::hold:
    case K::spin: {
        const double step = std::min(dt, c.seconds - m_elapsed);
        m_elapsed += step;
        dt -= step;
        if (c.kind == K::spin) {
            cam.yaw = m_spin_yaw0 + glm::radians(c.degrees) * static_cast<float>(m_elapsed / c.seconds);
        }
        return m_elapsed >= c.seconds - 1e-9;
    }
    case K::fly:
        while (m_path_seg + 1 < m_path.size()) {
            const PathPoint& a = m_path[m_path_seg];
            const PathPoint& b = m_path[m_path_seg + 1];
            const double duration = std::max(1e-3, double(glm::length(b.pos - a.pos)) / b.speed);
            const double step = std::min(dt, duration - m_elapsed);
            m_elapsed += step;
            dt -= step;
            const float u = static_cast<float>(std::clamp(m_elapsed / duration, 0.0, 1.0));
            const glm::vec3 p0 = m_path_seg > 0 ? m_path[m_path_seg - 1].pos : a.pos - (b.pos - a.pos);
            const glm::vec3 p3 = m_path_seg + 2 < m_path.size() ? m_path[m_path_seg + 2].pos : b.pos + (b.pos - a.pos);
            cam.position = catmull_rom(p0, a.pos, b.pos, p3, u);
            cam.yaw = a.yaw + shortest_delta(a.yaw, b.yaw) * u;
            cam.set_pitch(a.pitch + (b.pitch - a.pitch) * u);
            if (m_elapsed < duration - 1e-9) {
                return false;
            }
            // Land exactly on the waypoint: the spline's float error at u = 1 would leave the camera a
            // hair off, and the same pose must render the same however the camera got there.
            cam.position = b.pos;
            cam.yaw = b.yaw;
            cam.set_pitch(b.pitch);
            ++m_path_seg;
            m_elapsed = 0;
        }
        return true;
    case K::start:
    case K::wait_loaded:
    case K::travel:
    case K::move:
    case K::jump:
    case K::player:
    case K::player_action:
        m_elapsed += dt;
        dt = 0;
        return false; // completes in after_frame
    case K::audio:
        dt = 0;
        return !m_host.audio_busy();
    default:
        dt = 0;
        return false;
    }
}

void ScriptExecutor::carry(const std::function<glm::vec3(const glm::vec3&)>& point, float angle) {
    for (auto& p : m_path) {
        p.pos = point(p.pos);
        p.yaw -= angle;
    }
}

bool ScriptExecutor::before_frame(double dt) {
    for (;;) {
        if (!m_active) {
            if (m_queue.empty()) {
                return false;
            }
            m_current = std::move(m_queue.front());
            m_queue.pop_front();
            m_active = true;
            m_elapsed = 0;
            m_frames_waited = 0;
            try {
                if (start(m_current)) {
                    finish(true, m_result);
                    continue;
                }
            } catch (const std::exception& ex) {
                fail(ex.what());
                continue;
            }
        }
        bool done = false;
        try {
            done = advance(m_current, dt);
        } catch (const std::exception& ex) {
            fail(ex.what());
            continue;
        }
        if (done) {
            finish(true, m_result);
            continue;
        }
        return true;
    }
}

void ScriptExecutor::after_frame() {
    if (!m_active) {
        return;
    }
    using K = Command::Kind;
    const Command& c = m_current.command;
    try {
        switch (c.kind) {
        case K::wait_frames:
            if (++m_frames_waited >= c.frames) {
                finish(true, {});
            }
            break;
        case K::start:
        case K::wait_loaded: {
            ++m_frames_waited;
            if (m_frames_waited >= 2 && !m_host.streaming_busy()) {
                finish(true, {});
            } else if (m_elapsed > (c.kind == K::start ? k_default_wait_timeout : c.seconds)) {
                fail(std::format("streaming did not finish within {:.0f} s", m_elapsed));
            }
            break;
        }
        case K::travel:
            if (!m_host.loading()) {
                finish(true, {});
            }
            break;
        case K::move:
        case K::jump:
        case K::player:
        case K::player_action:
            if (m_host.player_scripted() == 0) {
                finish(true, {});
            } else if (m_elapsed > c.seconds + k_default_wait_timeout) {
                fail("the player did not get to move (collision never loaded?)");
            }
            break;
        case K::screenshot:
            if (m_capture_pending) {
                m_capture_pending = false;
                m_host.save_screenshot(c.arg);
                finish(true, {});
            }
            break;
        case K::screenshot_hdr:
            if (m_capture_pending) {
                m_capture_pending = false;
                m_host.save_hdr_screenshot(c.arg);
                finish(true, {});
            }
            break;
        case K::screenshot_shadow:
            if (m_capture_pending) {
                m_capture_pending = false;
                m_host.save_shadow_screenshot(c.arg);
                finish(true, {});
            }
            break;
        default:
            break;
        }
    } catch (const std::exception& ex) {
        m_capture_pending = false;
        fail(ex.what());
    }
}

} // namespace spud::engine
