// skytool fx: particle effects.

#include "commands.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "formats/osm.hpp"
#include "formats/ose.hpp"
#include "fx/effect.hpp"
#include "importer/convert/mesh_import.hpp"
#include "importer/convert/particle_import.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/nif.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <string>

namespace skytool {

namespace {

using namespace spud;

struct Loaded {
    std::string source; // normalised data path or file name
    std::optional<legacy::NifFile> nif;
    ose::Effect effect;
    std::vector<std::string> warnings;
    std::vector<std::string> textures;
    importer::MeshImportResult mesh;
};

bool ends_with_ci(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && path::to_lower(s.substr(s.size() - suffix.size())) == suffix;
}

// A .ose file, a .nif file on disk, or a NIF path in the game's data.
Loaded load(const std::string& arg) {
    Loaded l;
    if (ends_with_ci(arg, ".ose")) {
        l.source = arg;
        l.effect = ose::read(read_file(arg));
        return l;
    }
    std::vector<std::byte> bytes;
    if (std::filesystem::is_regular_file(arg)) {
        l.source = path::normalize(std::filesystem::path(arg).filename().string());
        bytes = read_file(arg);
    } else {
        l.source = path::with_root(path::normalize(arg), "meshes");
        legacy::DataSource data(open_install());
        auto b = data.read(l.source);
        if (!b) {
            throw Error(std::format("{} not found in the install", l.source));
        }
        bytes = std::move(*b);
    }
    l.nif = legacy::read_nif(bytes);
    l.mesh = importer::import_nif(*l.nif, l.source);
    auto fx = importer::import_particles(*l.nif, l.source, l.mesh.submesh_shapes);
    l.effect = std::move(fx.effect);
    l.warnings = std::move(fx.warnings);
    l.textures = std::move(fx.textures);
    return l;
}

float parse_step(const std::string& s) {
    if (auto slash = s.find('/'); slash != std::string::npos) {
        return std::stof(s.substr(0, slash)) / std::stof(s.substr(slash + 1));
    }
    return std::stof(s);
}

const char* modifier_name(ose::ModifierType t) {
    switch (t) {
    case ose::ModifierType::age_death:
        return "age death";
    case ose::ModifierType::lod:
        return "LOD";
    case ose::ModifierType::mesh_emitter:
        return "mesh emitter";
    case ose::ModifierType::spawn:
        return "spawn";
    case ose::ModifierType::simple_color:
        return "simple colour";
    case ose::ModifierType::rotation:
        return "rotation";
    case ose::ModifierType::subtex:
        return "sub-texture";
    case ose::ModifierType::scale:
        return "scale";
    case ose::ModifierType::gravity:
        return "gravity";
    case ose::ModifierType::drag:
        return "drag";
    case ose::ModifierType::position:
        return "position";
    case ose::ModifierType::bound_update:
        return "bound update";
    }
    return "?";
}

const char* controller_name(ose::ControllerType t) {
    switch (t) {
    case ose::ControllerType::modifier_active:
        return "modifier active";
    case ose::ControllerType::emitter:
        return "emitter";
    case ose::ControllerType::update:
        return "update";
    case ose::ControllerType::effect_float:
        return "effect float";
    case ose::ControllerType::effect_color:
        return "effect colour";
    case ose::ControllerType::transform:
        return "transform";
    case ose::ControllerType::other:
        return "other";
    }
    return "?";
}

void dump(const ose::Effect& e) {
    std::printf(
        "%zu nodes, %zu systems, %zu modifiers, %zu emitter meshes, %zu controllers, %zu interpolators, "
        "%zu sequences, %zu materials\n",
        e.nodes.size(), e.systems.size(), e.modifiers.size(), e.emitter_meshes.size(), e.controllers.size(),
        e.interpolators.size(), e.sequences.size(), e.materials.size());
    // Version 3: the scene graph the UI 3D scenes read.
    static constexpr const char* k_kinds[] = {"other", "node", "billboard", "shape"};
    for (size_t i = 0; i < e.node_extras.size(); ++i) {
        const auto& x = e.node_extras[i];
        const auto& n = e.nodes[i];
        std::printf("node %zu '%s' parent %d %s%s flags 0x%x scale %g t (%g %g %g) bound (%g %g %g) r %g\n", i,
                    std::string(e.string(n.name)).c_str(), n.parent == ose::k_none ? -1 : int(n.parent),
                    k_kinds[std::min<size_t>(size_t(x.kind), 3)],
                    x.kind == ose::NodeKind::billboard ? std::format(" mode {}", x.billboard_mode).c_str() : "", n.flags,
                    n.scale, n.translation[0], n.translation[1], n.translation[2], x.bound_center[0], x.bound_center[1],
                    x.bound_center[2], x.bound_radius);
    }
    for (size_t i = 0; i < e.submesh_nodes.size(); ++i) {
        std::printf("submesh %zu: node %d\n", i, e.submesh_nodes[i] == ose::k_none ? -1 : int(e.submesh_nodes[i]));
    }
    for (const auto& m : e.inv_marker) {
        std::printf("INV (%u %u %u) zoom %g\n", m.rotation[0], m.rotation[1], m.rotation[2], m.zoom);
    }
    for (size_t i = 0; i < e.systems.size(); ++i) {
        const auto& s = e.systems[i];
        std::printf("system %zu: node '%s', max %u particles, flags 0x%03x, %u sub-textures, material %u\n",
                    i, std::string(e.string(e.nodes[s.node].name)).c_str(), s.max_particles, s.flags,
                    s.subtexture_count, s.material);
        for (uint32_t k = 0; k < s.modifier_count; ++k) {
            const auto& m = e.modifiers[s.first_modifier + k];
            std::printf("  [%u] %-13s order %4u %s '%s'", s.first_modifier + k, modifier_name(m.type),
                        m.order, m.active ? "on " : "off", std::string(e.string(m.name)).c_str());
            if (m.type == ose::ModifierType::mesh_emitter) {
                std::printf(" speed %g+-%g/2 life %g+-%g/2 radius %g+-%g, velocity %u emission %u, %u meshes",
                            m.f[0], m.f[1], m.f[12], m.f[13], m.f[10], m.f[11], m.u0, m.u1, m.count);
            } else if (m.type == ose::ModifierType::lod) {
                std::printf(" begin %g end %g emit %g size %g", m.f[0], m.f[1], m.f[2], m.f[3]);
            }
            std::puts("");
        }
    }
    for (size_t i = 0; i < e.emitter_meshes.size(); ++i) {
        const auto& m = e.emitter_meshes[i];
        size_t bad = 0;
        for (uint32_t v = 0; v < m.vertex_count; ++v) {
            const auto& p = e.mesh_positions[m.first_vertex + v];
            const auto& n = e.mesh_normals[m.first_vertex + v];
            bad += !std::isfinite(p.x + p.y + p.z + n.x + n.y + n.z);
        }
        std::printf("emitter mesh %zu: node '%s', %u vertices (%zu non-finite), %u triangles\n", i,
                    std::string(e.string(e.nodes[m.node].name)).c_str(), m.vertex_count, bad,
                    m.triangle_count);
    }
    for (size_t i = 0; i < e.controllers.size(); ++i) {
        const auto& c = e.controllers[i];
        std::printf(
            "controller %zu: %-15s flags 0x%02x freq %g phase %g keys %g..%g target %u:%u interp %d/%d "
            "modifier %d var %u '%s'\n",
            i, controller_name(c.type), c.flags, c.frequency, c.phase, c.start_time, c.stop_time,
            static_cast<uint32_t>(c.target_kind), c.target, static_cast<int>(c.interpolator),
            static_cast<int>(c.interpolator2), static_cast<int>(c.modifier), c.variable,
            std::string(e.string(c.name)).c_str());
    }
    for (const auto& s : e.sequences) {
        std::printf("sequence '%s': cycle %u freq %g keys %g..%g weight %g, %u blocks, text keys:",
                    std::string(e.string(s.name)).c_str(), s.cycle_type, s.frequency, s.start_time,
                    s.stop_time, s.weight, s.block_count);
        for (uint32_t k = 0; k < s.text_key_count; ++k) {
            const auto& t = e.text_keys[s.first_text_key + k];
            std::printf(" %g '%s'", t.time, std::string(e.string(t.text)).c_str());
        }
        std::puts("");
    }
    for (size_t i = 0; i < e.materials.size(); ++i) {
        const auto& m = e.materials[i];
        std::printf(
            "material %zu: owner %u:%u base (%g,%g,%g,%g)x%g falloff (%g,%g,%g,%g) soft %g flags1 0x%08x "
            "flags2 0x%08x alpha 0x%04x '%s' '%s'\n",
            i, static_cast<uint32_t>(m.owner_kind), m.owner, m.base_color[0], m.base_color[1],
            m.base_color[2], m.base_color[3], m.base_color_scale, m.falloff[0], m.falloff[1], m.falloff[2],
            m.falloff[3], m.soft_falloff_depth, m.skyrim_flags1, m.skyrim_flags2, m.alpha_property_flags,
            std::string(e.string(m.source_texture)).c_str(),
            std::string(e.string(m.greyscale_texture)).c_str());
    }
}

struct SimOptions {
    std::string sequence = "AutoPlay";
    std::string then; // started when `sequence` reaches its end (CLAMP) — the inferred Autoplay flow
    float seconds = 10.0f;
    float step = 1.0f / 60.0f;
    uint32_t seed = 1;
    float lod = 0.0f;
    float rotate_z = 0.0f;     // degrees, the root's placement
    float stop_effect = -1.0f; // time at which AutoFadeOut replaces the running sequence
    int show = 3;              // particles printed per report
    float report = 1.0f;       // seconds between reports
    bool ms_clock = false;     // times in whole milliseconds, as the Mist Menu's clock hands them to its scene
};

SimOptions sim_options(Args args) {
    SimOptions o;
    if (auto v = option(args, "--sequence")) {
        o.sequence = *v;
    }
    if (auto v = option(args, "--then")) {
        o.then = *v;
    }
    if (auto v = option(args, "--seconds")) {
        o.seconds = std::stof(*v);
    }
    if (auto v = option(args, "--step")) {
        o.step = parse_step(*v);
    }
    if (auto v = option(args, "--seed")) {
        o.seed = static_cast<uint32_t>(std::stoul(*v));
    }
    if (auto v = option(args, "--lod")) {
        o.lod = std::stof(*v);
    }
    if (auto v = option(args, "--rotate-z")) {
        o.rotate_z = std::stof(*v);
    }
    if (auto v = option(args, "--stop-effect")) {
        o.stop_effect = std::stof(*v);
    }
    if (auto v = option(args, "--show")) {
        o.show = std::stoi(*v);
    }
    if (auto v = option(args, "--report")) {
        o.report = parse_step(*v);
    }
    o.ms_clock = has_flag(args, "--ms-clock");
    return o;
}

struct Driver {
    fx::EffectInstance fx;
    SimOptions o;
    std::string current;
    bool chained = false;
    bool stopped = false;

    Driver(std::shared_ptr<const fx::Effect> effect, const SimOptions& opts)
        : fx(std::move(effect), opts.seed), o(opts) {
        glm::mat3 r = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(o.rotate_z), glm::vec3(0, 0, 1)));
        fx.set_root_transform(r, glm::vec3(0.0f), 1.0f);
        fx.set_lod_distance(o.lod);
        if (!o.sequence.empty() && o.sequence != "none") {
            if (!fx.activate_sequence(o.sequence)) {
                throw Error(std::format("no sequence named '{}'", o.sequence));
            }
            current = o.sequence;
        }
    }

    // One fixed step: time = n * step (the game passes absolute seconds since attach).
    void step(int n) {
        float t = static_cast<float>(n) * o.step;
        if (o.ms_clock) {
            // The menu's integer millisecond clock: at 1/60 s it advances 16, 17, 17,.. ms.
            t = static_cast<float>(static_cast<int64_t>(static_cast<double>(n) * o.step * 1000.0 + 1e-6)) / 1000.0f;
        }
        if (!stopped && o.stop_effect >= 0.0f && t >= o.stop_effect) {
            if (!current.empty()) {
                fx.deactivate_sequence(current);
            }
            fx.activate_sequence("AutoFadeOut");
            current = "AutoFadeOut";
            stopped = true;
        } else if (!chained && !o.then.empty() && !current.empty() && fx.sequence_active(current)) {
            // The sequence reached its last key time (a CLAMP sequence stays there): hand over, as the
            // Autoplay behaviour graph is inferred to do.
            for (const auto& s : fx.data().sequences) {
                if (fx.data().string(s.name) == current && fx.sequence_time(current) >= s.stop_time) {
                    fx.deactivate_sequence(current);
                    fx.activate_sequence(o.then);
                    current = o.then;
                    chained = true;
                    break;
                }
            }
        }
        fx.update(t);
    }
};

void report(const fx::EffectInstance& fx, float t, int show) {
    for (size_t s = 0; s < fx.system_count(); ++s) {
        auto ps = fx.particles(s);
        auto b = fx.bound(s);
        const auto& mat = fx.system_material(s);
        std::printf("t=%7.3f s  system %zu: %3zu particles  bound (%.3f, %.3f, %.3f) r %.3f m  alpha %.3f\n",
                    t, s, ps.size(), b.center.x, b.center.y, b.center.z, b.radius, mat.base_color.a);
        if (!ps.empty()) {
            glm::vec3 lo(1e30f), hi(-1e30f);
            for (const auto& p : ps) {
                lo = glm::min(lo, p.position);
                hi = glm::max(hi, p.position);
            }
            std::printf("    positions (%.3f, %.3f, %.3f) .. (%.3f, %.3f, %.3f) m\n", lo.x, lo.y, lo.z, hi.x,
                        hi.y, hi.z);
        }
        for (int i = 0; i < show && i < static_cast<int>(ps.size()); ++i) {
            const auto& p = ps[static_cast<size_t>(i)];
            std::printf(
                "    #%d pos (%.3f, %.3f, %.3f) vel (%.6f, %.6f, %.6f) radius %.3f size %.3f rot %.3f "
                "rgba (%.3f, %.3f, %.3f, %.3f) frame %u uv (%.3f, %.3f, %.3f, %.3f) age %.3f/%.3f\n",
                i, p.position.x, p.position.y, p.position.z, p.velocity.x, p.velocity.y, p.velocity.z,
                p.radius, p.size, p.rotation, p.color.r, p.color.g, p.color.b, p.color.a, p.frame,
                p.uv_rect.x, p.uv_rect.y, p.uv_rect.z, p.uv_rect.w, p.age, p.life_span);
        }
    }
    for (size_t m = 0; m < fx.material_count(); ++m) {
        const auto& mat = fx.material(m);
        if (mat.source->owner_kind == ose::MaterialOwner::mesh_shape) {
            std::printf("    mesh submesh %d material: alpha %.3f\n", static_cast<int>(mat.source->owner),
                        mat.base_color.a);
        }
    }
}

int sim(Args args, const std::string& input) {
    auto loaded = load(input);
    auto o = sim_options(args);
    auto effect = fx::make_effect(std::move(loaded.effect));
    Driver d(effect, o);
    int steps = static_cast<int>(std::lround(o.seconds / o.step));
    int every = std::max(1, static_cast<int>(std::lround(o.report / o.step)));
    std::printf(
        "%s: sequence %s%s%s, %d steps of %.6f s, seed %u, LOD distance %g, root rotated %g deg about Z\n",
        loaded.source.c_str(), o.sequence.c_str(), o.then.empty() ? "" : " then ", o.then.c_str(), steps,
        o.step, o.seed, o.lod, o.rotate_z);
    for (int n = 0; n <= steps; ++n) {
        d.step(n);
        if (n % every == 0) {
            report(d.fx, static_cast<float>(n) * o.step, o.show);
        }
    }
    std::printf("state hash %016llx\n", static_cast<unsigned long long>(d.fx.state_hash()));
    return 0;
}

// Headless acceptance check: determinism (two runs, every step) and particle counts over time.
int check(Args args, const std::string& input) {
    auto loaded = load(input);
    auto o = sim_options(args);
    if (!option(args, "--seconds")) {
        o.seconds = 40.0f;
    }
    if (!option(args, "--then")) {
        o.then = "AutoLoop";
    }
    auto effect = fx::make_effect(std::move(loaded.effect));
    if (effect->data.systems.empty()) {
        log::error("{}: no particle systems", loaded.source);
        return 1;
    }
    Driver a(effect, o);
    Driver b(effect, o);
    int steps = static_cast<int>(std::lround(o.seconds / o.step));
    int failures = 0;
    auto fail = [&](std::string msg) {
        if (failures++ < 20) {
            log::error("{}", msg);
        }
    };
    bool identical = true;
    size_t max_seen = 0;
    double window_sum = 0.0;
    int window_n = 0;
    float window_start = o.seconds * 0.5f;
    for (int n = 0; n <= steps; ++n) {
        a.step(n);
        b.step(n);
        if (a.fx.state_hash() != b.fx.state_hash()) {
            identical = false;
            fail(std::format("step {}: runs differ (state hash)", n));
        }
        for (size_t s = 0; s < a.fx.system_count(); ++s) {
            auto pa = a.fx.particles(s);
            auto pb = b.fx.particles(s);
            if (pa.size() != pb.size() ||
                (!pa.empty() && std::memcmp(pa.data(), pb.data(), pa.size_bytes()) != 0)) {
                identical = false;
                fail(std::format("step {}: system {} particle output differs between runs", n, s));
            }
            max_seen = std::max(max_seen, pa.size());
            if (pa.size() > effect->data.systems[s].max_particles) {
                fail(std::format("step {}: {} particles exceed the maximum", n, pa.size()));
            }
            for (const auto& p : pa) {
                if (!std::isfinite(p.position.x) || !std::isfinite(p.position.y) ||
                    !std::isfinite(p.position.z) || !std::isfinite(p.radius) || !(p.age <= p.life_span)) {
                    fail(std::format("step {}: invalid particle", n));
                    break;
                }
            }
        }
        float t = static_cast<float>(n) * o.step;
        if (t >= window_start) {
            window_sum += static_cast<double>(a.fx.particle_count(0));
            ++window_n;
        }
    }
    // Expected steady state of system 0 (derived): birth rate x mean life span x LOD emit scale.
    const auto& e = effect->data;
    double expected = 0.0;
    for (const auto& c : e.controllers) {
        if (c.type != ose::ControllerType::emitter || c.target != 0 || c.modifier == ose::k_none) {
            continue;
        }
        double rate = 0.0;
        for (const auto& seq : e.sequences) {
            if (e.string(seq.name) != (o.then.empty() ? o.sequence : o.then)) {
                continue;
            }
            for (uint32_t k = 0; k < seq.block_count; ++k) {
                const auto& blk = e.blocks[seq.first_block + k];
                if (blk.interpolator != ose::k_none && e.string(blk.interpolator_id) == "BirthRate") {
                    rate = e.interpolators[blk.interpolator].float_value;
                }
            }
        }
        expected = rate * e.modifiers[c.modifier].f[12];
    }
    double mean = window_n ? window_sum / window_n : 0.0;
    std::printf("%s: %d steps x 2 runs, deterministic: %s; max %zu particles; mean %.2f over t >= %.1f s "
                "(expected about %.2f = birth rate x mean life span)\n",
                loaded.source.c_str(), steps, identical ? "yes" : "NO", max_seen, mean, window_start,
                expected);
    if (expected > 0.0 && (mean < expected * 0.75 || mean > expected * 1.25)) {
        fail(std::format("mean particle count {:.2f} is not within 25% of {:.2f}", mean, expected));
    }
    if (max_seen == 0) {
        fail("no particles were emitted");
    }
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

// Every NIF with particle systems under a prefix: decoding, conversion warnings, a write/read round trip and
// a short simulation of each sequence (or none), checking the output stays finite and within the maximum.
int scan(const std::vector<std::string>& pos) {
    auto prefix = pos.size() >= 2 ? path::normalize(pos[1]) : std::string("meshes/");
    legacy::DataSource data(open_install());
    auto files = data.list(prefix, ".nif");
    size_t with_particles = 0, converted = 0, failed = 0, bad_sim = 0, systems = 0, nan_data = 0;
    std::map<std::string, size_t> mismatches;
    std::map<std::string, std::pair<size_t, std::string>> warnings;
    std::map<std::string, std::string> mismatch_example;
    for (const auto& f : files) {
        try {
            auto bytes = data.read(f);
            if (!bytes) {
                continue;
            }
            auto nif = legacy::read_nif(*bytes);
            if (!importer::has_particle_systems(nif)) {
                continue;
            }
            ++with_particles;
            for (size_t i = 0; i < nif.blocks.size(); ++i) {
                if (nif.blocks[i].status == legacy::NifBlockStatus::mismatch) {
                    std::string type(nif.type_name(static_cast<legacy::NifRef>(i)));
                    ++mismatches[type];
                    mismatch_example.emplace(type, f + ": " + nif.blocks[i].error);
                }
            }
            auto mesh = importer::import_nif(nif, f);
            auto fx = importer::import_particles(nif, f, mesh.submesh_shapes);
            for (const auto& w : fx.warnings) {
                std::string key;
                for (char ch : w) {
                    if (!(ch >= '0' && ch <= '9') || key.empty() || key.back() != '#') {
                        key.push_back(ch >= '0' && ch <= '9' ? '#' : ch);
                    }
                }
                auto& [n, example] = warnings[key];
                if (n++ == 0) {
                    example = f + ": " + w;
                }
            }
            if (fx.effect.systems.empty()) {
                continue;
            }
            ++converted;
            systems += fx.effect.systems.size();
            auto effect = fx::make_effect(ose::read(ose::write(fx.effect)));
            std::vector<std::string> seqs{""};
            for (const auto& s : effect->data.sequences) {
                seqs.emplace_back(effect->data.string(s.name));
            }
            for (const auto& name : seqs) {
                fx::EffectInstance inst(effect);
                if (!name.empty()) {
                    inst.activate_sequence(name);
                }
                bool ok = true;
                for (int n = 0; n <= 300 && ok; ++n) {
                    inst.update(static_cast<float>(n) / 60.0f);
                    for (size_t s = 0; s < inst.system_count() && ok; ++s) {
                        auto ps = inst.particles(s);
                        ok = ps.size() <= effect->data.systems[s].max_particles;
                        for (const auto& p : ps) {
                            ok = ok && std::isfinite(p.position.x + p.position.y + p.position.z + p.radius);
                        }
                    }
                }
                if (!ok) {
                    // The game computes the same NaNs when a mesh emitter's particle data holds NaN normals
                    // (velocity type 0); anything else is a simulation fault.
                    bool nan_normals = false;
                    for (const auto& nrm : effect->data.mesh_normals) {
                        nan_normals = nan_normals || !std::isfinite(nrm.x + nrm.y + nrm.z);
                    }
                    if (nan_normals) {
                        ++nan_data;
                        log::info("{}: sequence '{}': non-finite particles from NaN normals in the emitter "
                                  "mesh's particle data",
                                  f, name);
                    } else {
                        ++bad_sim;
                        log::warn("{}: sequence '{}' produced invalid particles", f, name);
                    }
                }
            }
        } catch (const std::exception& e) {
            ++failed;
            log::warn("{}: {}", f, e.what());
        }
    }
    std::printf(
        "%zu NIFs under %s, %zu with particle systems, %zu converted (%zu systems), %zu failed, %zu with "
        "invalid simulation output, %zu runs with NaN normals in the source data\n",
        files.size(), prefix.c_str(), with_particles, converted, systems, failed, bad_sim, nan_data);
    std::printf("size mismatches by block type:\n");
    for (const auto& [type, n] : mismatches) {
        std::printf("  %6zu  %-36s e.g. %s\n", n, type.c_str(), mismatch_example[type].c_str());
    }
    std::vector<std::pair<size_t, std::string>> sorted;
    for (const auto& [key, v] : warnings) {
        sorted.emplace_back(v.first, v.second);
    }
    std::ranges::sort(sorted, std::greater{});
    std::printf("conversion warnings:\n");
    for (const auto& [n, example] : sorted) {
        std::printf("  %6zu  e.g. %s\n", n, example.c_str());
    }
    return failed == 0 && bad_sim == 0 ? 0 : 1;
}

} // namespace

int cmd_fx(Args args) {
    auto pos = positionals(args, {"--sequence", "--then", "--seconds", "--step", "--seed", "--lod",
                                  "--rotate-z", "--stop-effect", "--show", "--report"});
    if (pos.size() < 2 && !(pos.size() == 1 && pos[0] == "scan")) {
        std::puts(
            "fx convert <nif> [out.ose]  convert a NIF's particle systems; without out.ose the .osm, .ose "
            "and\n"
            "                            their textures go into the cache next to the other meshes\n"
            "fx dump <nif|ose>           systems, modifiers, controllers, sequences, materials\n"
            "fx sim <nif|ose> [--sequence AutoPlay] [--then AutoLoop] [--stop-effect T] [--seconds S]\n"
            "       [--step 1/60] [--ms-clock] [--seed N] [--lod D] [--rotate-z DEG] [--show N] [--report S]\n"
            "                            fixed-step simulation, reports counts, bounds and particles\n"
            "                            (--ms-clock: whole-millisecond times, as the Mist Menu's clock)\n"
            "fx check <nif|ose> [same options; default 40 s, AutoPlay then AutoLoop]\n"
            "                            two runs must be bit-identical; the steady-state count must match\n"
            "                            the emitter's birth rate x mean life span\n"
            "fx scan [prefix]            every NIF with particle systems: decode, convert, round trip and\n"
            "                            simulate each sequence for 5 s");
        return 1;
    }
    const auto& sub = pos[0];
    if (sub == "convert") {
        auto loaded = load(pos[1]);
        for (const auto& w : loaded.warnings) {
            log::warn("{}: {}", loaded.source, w);
        }
        if (loaded.effect.systems.empty()) {
            log::error("{}: no particle systems converted", loaded.source);
            return 1;
        }
        auto bytes = ose::write(loaded.effect);
        if (pos.size() >= 3) {
            write_file(pos[2], bytes);
            log::info("wrote {} bytes to {}", bytes.size(), pos[2]);
        } else {
            auto cache = cache_dir();
            auto out = cache / path::replace_extension(loaded.source, ".ose");
            write_file(out, bytes);
            log::info("wrote {}", out.string());
            if (!loaded.mesh.mesh.submeshes.empty()) {
                auto mesh_out = cache / path::replace_extension(loaded.source, ".osm");
                write_file(mesh_out, osm::write(loaded.mesh.mesh));
                log::info("wrote {}", mesh_out.string());
            }
            std::vector<std::string> textures = loaded.textures;
            textures.insert(textures.end(), loaded.mesh.textures.begin(), loaded.mesh.textures.end());
            legacy::DataSource data(open_install());
            for (const auto& t : textures) {
                auto out_t = cache / t;
                if (std::filesystem::exists(out_t)) {
                    continue;
                }
                if (auto b = data.read(t)) {
                    write_file(out_t, *b);
                    log::info("wrote {}", out_t.string());
                } else {
                    log::warn("texture {} not found", t);
                }
            }
        }
        auto back = ose::read(bytes);
        dump(back);
        return 0;
    }
    if (sub == "dump") {
        auto loaded = load(pos[1]);
        for (const auto& w : loaded.warnings) {
            log::warn("{}: {}", loaded.source, w);
        }
        dump(loaded.effect);
        return 0;
    }
    if (sub == "sim") {
        return sim(args, pos[1]);
    }
    if (sub == "check") {
        return check(args, pos[1]);
    }
    if (sub == "scan") {
        return scan(pos);
    }
    log::error("unknown fx subcommand");
    return 1;
}

} // namespace skytool
