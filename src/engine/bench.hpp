#pragma once

// Bench mode: a
// line-based scenario drives the camera on real frame time; every frame's
// CPU zones and GPU passes are recorded and written as JSON for

#include "engine/script.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace spud::engine {

struct BenchScenario {
    std::filesystem::path file;
    std::string name;
    std::string world = "tamriel"; // "none": no world, clear + present only
    uint32_t width = 2560;
    uint32_t height = 1440;
    std::optional<float> draw_distance;
    std::optional<Pose> start;     // streaming settles here before recording (not measured)
    std::optional<Pose> player;    // on foot from here (feet; z "+N" above terrain), settled with the start
    // A replayable tour: the world steps in ticks of exactly 1 / fixed_hz seconds, paced
    // by real time, so AI and combat run the same way each time and the tour lasts as long at any frame rate. 0: the
    // world steps on every frame's real time.
    float fixed_hz = 0;
    float wall_limit_s = 0;        // the whole process, however it went: past this the run stops, unfinished
    std::optional<uint32_t> seed;  // fixes every random generator
    float target_ms = 0;           // no frame over this; 0: the budgets of apply
    std::string preset;            // the locked quality level this scenario runs at
    std::vector<Command> setup;    // before `record`: run in the warm-up, not measured
    std::vector<Command> commands; // the measured part, run by ScriptExecutor
};

// A scenario is a script whose leading name/world/resolution/draw_distance/start
// lines are settings. Throws Error("<file>:<line>: ...") on any problem.
BenchScenario parse_bench(const std::filesystem::path& file);

// ---- results ---------------------------------------------------------------------

enum CpuZone : uint32_t { zone_update, zone_streaming, zone_cull, zone_record, zone_wait, k_zone_count };
inline constexpr uint32_t k_max_bench_passes = 16;
const char* cpu_zone_name(uint32_t zone);

struct BenchFrame {
    uint64_t frame_number = 0;
    double t = 0;       // seconds since recording started
    float cpu_ms = 0;   // start-to-start
    float gpu_ms = -1;  // -1 until matched
    int segment = 0;
    bool still = false; // a tour's frame between world ticks
    float tick_late_ms = 0; // a tick: how long after its time it ran
    std::array<float, k_zone_count> zones{};
    std::array<float, k_max_bench_passes> passes{}; // per GPU pass (BenchResult::pass_names), -1 until matched
};

struct BenchResult {
    std::string scenario;
    std::string gpu;
    std::string driver;
    uint32_t width = 0, height = 0;
    float draw_distance = 0;
    std::string present_mode; // the swapchain's (headless runs present to a hidden window)
    bool headless = false;
    double startup_ms = 0;
    double warmup_ms = 0;
    float fixed_hz = 0;
    uint32_t seed = 0;
    float target_ms = 0;
    float wall_limit_s = 0;
    bool finished = true;  // false: stopped at the wall limit
    double wall_s = 0;     // the process, start to the last frame
    double loading_s = 0;  // of which behind loading screens
    std::string preset;
    std::vector<std::string> segments;
    // The world's state at the end of each segment: equal across runs when the tour replayed identically.
    std::vector<uint64_t> digests;
    std::vector<std::string> pass_names;
    std::vector<BenchFrame> frames;
    uint64_t max_pending_jobs = 0;
    uint64_t max_upload_bytes_per_frame = 0;
    uint64_t cells_loaded = 0;
    uint64_t meshes_loaded = 0;
    uint64_t textures_loaded = 0;
    uint64_t peak_gpu_bytes = 0;
};

void write_bench_json(const std::filesystem::path& file, const BenchResult& result);
// Short human-readable summary (mean/p99/max, hitches) for the log.
std::string bench_summary(const BenchResult& result);

} // namespace spud::engine
