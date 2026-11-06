#include "engine/bench.hpp"

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
#include <thread>

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#include <psapi.h>
#endif

namespace spud::engine {

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += std::format("\\u{:04x}", int(c));
        } else {
            out += c;
        }
    }
    return out;
}

std::string cpu_brand() {
#ifdef _WIN32
    int regs[4];
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004) {
        return "unknown";
    }
    char brand[49] = {};
    for (int i = 0; i < 3; ++i) {
        __cpuid(regs, 0x80000002 + i);
        std::memcpy(brand + i * 16, regs, 16);
    }
    std::string s(brand);
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    return s.substr(s.find_first_not_of(' ') == std::string::npos ? 0 : s.find_first_not_of(' '));
#else
    return "unknown";
#endif
}

double ram_gb() {
#ifdef _WIN32
    MEMORYSTATUSEX m{sizeof(m)};
    GlobalMemoryStatusEx(&m);
    return double(m.ullTotalPhys) / double(1ull << 30);
#else
    return 0;
#endif
}

double peak_rss_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return double(pmc.PeakWorkingSetSize) / double(1 << 20);
    }
#endif
    return 0;
}

std::string compiler() {
#if defined(_MSC_FULL_VER)
    return std::format("MSVC {}.{}.{}", _MSC_VER / 100, _MSC_VER % 100, _MSC_FULL_VER % 100000);
#else
    return "unknown";
#endif
}

double percentile(std::vector<float> v, double p) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    const double idx = p * (v.size() - 1);
    const size_t i = static_cast<size_t>(idx);
    const double f = idx - i;
    return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
}

} // namespace

const char* cpu_zone_name(uint32_t zone) {
    switch (zone) {
    case zone_update: return "update";
    case zone_streaming: return "streaming";
    case zone_cull: return "cull";
    case zone_record: return "record";
    case zone_wait: return "wait";
    default: return "?";
    }
}

BenchScenario parse_bench(const std::filesystem::path& file) {
    BenchScenario s;
    s.file = file;
    s.name = file.stem().string();
    bool measured = false;
    std::vector<Command> commands = parse_script(file);
    using K = Command::Kind;
    const bool has_setup = std::ranges::any_of(commands, [](const Command& c) { return c.kind == K::record; });
    bool in_setup = has_setup;
    for (auto& c : commands) {
        switch (c.kind) {
        case K::fixed_step: s.fixed_hz = c.value; continue;
        case K::seed: s.seed = static_cast<uint32_t>(std::stoul(c.arg)); continue;
        case K::target: s.target_ms = c.value; continue;
        case K::wall_limit: s.wall_limit_s = c.value; continue;
        case K::preset: s.preset = c.arg; continue;
        case K::record: in_setup = false; measured = true; continue;
        default: break;
        }
        if (in_setup && c.kind != K::name && c.kind != K::world && c.kind != K::resolution &&
            c.kind != K::draw_distance) {
            s.setup.push_back(std::move(c));
            continue;
        }
        if (!measured) {
            switch (c.kind) {
            case K::name: s.name = c.arg; continue;
            case K::world: s.world = c.arg; continue;
            case K::resolution: s.width = c.width; s.height = c.height; continue;
            case K::draw_distance: s.draw_distance = c.value; continue;
            case K::start:
                if (s.start) {
                    throw Error(std::format("{}: only one 'start' is allowed", c.where()));
                }
                s.start = c.pose;
                continue;
            case K::player:
                if (s.player) {
                    throw Error(std::format("{}: only one leading 'player' is allowed", c.where()));
                }
                s.player = c.pose;
                continue;
            case K::segment: break;
            default: measured = true; break;
            }
        } else if (c.kind == K::start) {
            throw Error(std::format("{}: 'start' must come before the measured commands", c.where()));
        }
        s.commands.push_back(std::move(c));
    }
    return s;
}

void write_bench_json(const std::filesystem::path& file, const BenchResult& r) {
    std::string j;
    j.reserve(256 + r.frames.size() * 160);
    j += "{\n";
    j += std::format("  \"scenario\": \"{}\",\n", json_escape(r.scenario));
#ifdef SPUD_BUILD_CONFIG
    const char* config = SPUD_BUILD_CONFIG;
#else
    const char* config = "unknown";
#endif
    j += std::format("  \"build\": {{ \"config\": \"{}\", \"compiler\": \"{}\" }},\n", config, compiler());
    j += std::format("  \"machine\": {{ \"gpu\": \"{}\", \"driver\": \"{}\", \"cpu\": \"{}\", \"cores\": {}, "
                     "\"ram_gb\": {:.0f} }},\n",
                     json_escape(r.gpu), json_escape(r.driver), json_escape(cpu_brand()),
                     std::thread::hardware_concurrency(), ram_gb());
    j += std::format("  \"settings\": {{ \"width\": {}, \"height\": {}, \"draw_distance\": {}, "
                     "\"present_mode\": \"{}\", \"headless\": {} }},\n",
                     r.width, r.height, r.draw_distance, r.present_mode, r.headless ? "true" : "false");
    if (r.target_ms > 0) {
        j += std::format("  \"tour\": {{ \"target_ms\": {}, \"fixed_hz\": {}, \"seed\": {}, \"preset\": \"{}\", \"wall_limit_s\": {}, "
                         "\"wall_s\": {:.1f}, \"loading_s\": {:.1f}, \"finished\": {} }},\n",
                         r.target_ms, r.fixed_hz, r.seed, json_escape(r.preset), r.wall_limit_s, r.wall_s, r.loading_s,
                         r.finished ? "true" : "false");
        j += "  \"digests\": [";
        for (size_t i = 0; i < r.digests.size(); ++i) {
            j += std::format("{}\"{:016x}\"", i ? ", " : " ", r.digests[i]);
        }
        j += " ],\n";
    }
    j += std::format("  \"startup_ms\": {:.1f},\n", r.startup_ms);
    j += std::format("  \"warmup_ms\": {:.1f},\n", r.warmup_ms);
    j += "  \"frames\": [\n";
    for (size_t i = 0; i < r.frames.size(); ++i) {
        const auto& f = r.frames[i];
        j += std::format("    {{ \"t\": {:.5f}, \"cpu_ms\": {:.4f}, \"gpu_ms\": {:.4f}, \"segment\": {}{} }}{}\n", f.t,
                         f.cpu_ms, f.gpu_ms, f.segment,
                         f.still ? ", \"still\": true" : std::format(", \"late\": {:.2f}", f.tick_late_ms),
                         i + 1 < r.frames.size() ? "," : "");
    }
    j += "  ],\n";
    j += "  \"segments\": [";
    for (size_t i = 0; i < r.segments.size(); ++i) {
        j += std::format("{}\"{}\"", i ? ", " : " ", json_escape(r.segments[i]));
    }
    j += r.segments.empty() ? "],\n" : " ],\n";
    auto array = [&](auto&& get) {
        std::string a = "[";
        for (size_t i = 0; i < r.frames.size(); ++i) {
            a += std::format("{}{:.4f}", i ? ", " : "", get(r.frames[i]));
        }
        return a + "]";
    };
    j += "  \"gpu_passes_ms\": {\n";
    for (size_t p = 0; p < r.pass_names.size(); ++p) {
        j += std::format("    \"{}\": {}{}\n", r.pass_names[p],
                         array([&](const BenchFrame& f) { return f.passes[p]; }),
                         p + 1 < r.pass_names.size() ? "," : "");
    }
    j += "  },\n";
    j += "  \"cpu_zones_ms\": {\n";
    for (uint32_t z = 0; z < k_zone_count; ++z) {
        j += std::format("    \"{}\": {}{}\n", cpu_zone_name(z), array([&](const BenchFrame& f) { return f.zones[z]; }),
                         z + 1 < k_zone_count ? "," : "");
    }
    j += "  },\n";
    j += std::format("  \"streaming\": {{ \"max_pending_jobs\": {}, \"max_upload_bytes_per_frame\": {}, "
                     "\"cells_loaded\": {}, \"meshes_loaded\": {}, \"textures_loaded\": {} }},\n",
                     r.max_pending_jobs, r.max_upload_bytes_per_frame, r.cells_loaded, r.meshes_loaded,
                     r.textures_loaded);
    j += std::format("  \"memory\": {{ \"peak_rss_mb\": {:.0f}, \"peak_gpu_mb\": {:.0f} }}\n", peak_rss_mb(),
                     double(r.peak_gpu_bytes) / double(1 << 20));
    j += "}\n";
    write_file(file, std::as_bytes(std::span(j.data(), j.size())));
}

std::string bench_summary(const BenchResult& r) {
    std::vector<float> cpu, gpu, jitter;
    cpu.reserve(r.frames.size());
    for (size_t i = 0; i < r.frames.size(); ++i) {
        cpu.push_back(r.frames[i].cpu_ms);
        if (r.frames[i].gpu_ms >= 0) {
            gpu.push_back(r.frames[i].gpu_ms);
        }
        if (i > 0) {
            jitter.push_back(std::abs(r.frames[i].cpu_ms - r.frames[i - 1].cpu_ms));
        }
    }
    double mean = 0;
    for (float v : cpu) {
        mean += v;
    }
    mean = cpu.empty() ? 0 : mean / cpu.size();
    double gmean = 0;
    for (float v : gpu) {
        gmean += v;
    }
    gmean = gpu.empty() ? 0 : gmean / gpu.size();
    // Hitch: > 2x the median of the previous 60 frames and >= 2 ms over it.
    int hitches = 0;
    std::vector<float> window;
    for (size_t i = 60; i < cpu.size(); ++i) {
        window.assign(cpu.begin() + (i - 60), cpu.begin() + i);
        std::nth_element(window.begin(), window.begin() + 30, window.end());
        const float med = window[30];
        if (cpu[i] > 2 * med && cpu[i] >= med + 2) {
            ++hitches;
        }
    }
    const double maxv = cpu.empty() ? 0 : *std::max_element(cpu.begin(), cpu.end());
    return std::format("{}: {} frames, cpu mean {:.3f} ms ({:.0f} fps), p50 {:.3f}, p99 {:.3f}, max {:.3f} ms, "
                       "jitter p99 {:.3f} ms, hitches {}; gpu mean {:.3f} ms, p99 {:.3f} ms",
                       r.scenario, cpu.size(), mean, mean > 0 ? 1000.0 / mean : 0.0, percentile(cpu, 0.5),
                       percentile(cpu, 0.99), maxv, percentile(jitter, 0.99), hitches, gmean, percentile(gpu, 0.99));
}

} // namespace spud::engine
