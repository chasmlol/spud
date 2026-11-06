#include "importer/pipeline.hpp"

#include <bit>
#include <cstdio>
#include <functional>
#include <mutex>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"
#include "formats/osd.hpp"
#include "formats/osm.hpp"
#include "formats/ose.hpp"
#include "formats/ots.hpp"
#include "formats/osp.hpp"
#include "formats/osr.hpp"
#include "formats/osw.hpp"
#include "importer/convert/actor_import.hpp"
#include "importer/convert/door_import.hpp"
#include "importer/convert/atmosphere_import.hpp"
#include "importer/convert/hkx_import.hpp"
#include "importer/convert/audio_import.hpp"
#include "importer/convert/collision_import.hpp"
#include "importer/convert/mesh_import.hpp"
#include "importer/convert/particle_import.hpp"
#include "importer/convert/rules_import.hpp"
#include "importer/convert/script_import.hpp"
#include "importer/convert/ui_import.hpp"
#include "importer/convert/ui_images.hpp"
#include "importer/convert/ui_loading.hpp"
#include "importer/convert/ui_strings.hpp"
#include "importer/convert/ui_worlds.hpp"
#include "importer/convert/world_tables.hpp"
#include "importer/convert/world_import.hpp"
#include "importer/far/far_view.hpp"
#include "importer/incremental.hpp"
#include "importer/legacy/strings.hpp"
#include "importer/records/forms_import.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/esm.hpp"
#include "importer/legacy/nif.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <cstring>
#include <set>
#include <thread>
#include <unordered_map>

namespace spud::importer {

namespace {

// Water activators (ACTI with WNAM) and the triangles of their models' water shapes, in model units.
struct WaterModel {
    uint32_t water = 0;
    std::string model;
};
// The flag that makes the game take a water shape's depth from its vertices (BSWaterShaderProperty +136, technique 0x80).
constexpr uint32_t k_water_vertex_alpha_depth = 0x200;
// And the noise from their texture coordinates (+136, the vertex shader's cb2[12].x).
constexpr uint32_t k_water_vertex_uv = 0x100;

// A shape with a water shader, in its model's space. Its plane is its Z axis through its origin.
struct WaterShape {
    glm::vec3 up{0.0f, 0.0f, 1.0f};
    glm::vec3 origin{0.0f};
    bool vertex_uv = false;
    std::vector<osw::WaterVertex> vertices;
};

std::vector<WaterShape> water_shapes_of(const legacy::DataSource& data, const std::string& model) {
    using legacy::BSTriShape;
    using legacy::NifRef;
    using legacy::NiNode;
    std::vector<WaterShape> out;
    const auto bytes = data.read(model);
    if (!bytes) {
        return out;
    }
    const auto nif = legacy::read_nif(*bytes);
    struct Xf {
        glm::mat3 r{1.0f};
        glm::vec3 t{0.0f};
        float s = 1.0f;
    };
    std::function<void(NifRef, const Xf&)> walk = [&](NifRef ref, const Xf& parent) {
        if (const auto* node = nif.get<NiNode>(ref)) {
            const Xf w{parent.r * node->rotation, parent.r * (parent.s * node->translation) + parent.t,
                       parent.s * node->scale};
            for (NifRef c : node->children) {
                walk(c, w);
            }
            return;
        }
        const auto* shape = nif.get<BSTriShape>(ref);
        if (!shape || nif.type_name(shape->shader_property) != "BSWaterShaderProperty") {
            return;
        }
        const Xf w{parent.r * shape->rotation, parent.r * (parent.s * shape->translation) + parent.t,
                   parent.s * shape->scale};
        // BSWaterShaderProperty has no decoded form: its flags are its last four bytes.
        const legacy::NifBlock& property = nif.blocks[size_t(shape->shader_property)];
        uint32_t flags = 0;
        std::memcpy(&flags, bytes->data() + property.offset + property.size - sizeof(flags), sizeof(flags));
        const bool vertex_depth = flags & k_water_vertex_alpha_depth;
        const auto& g = shape->geometry;
        WaterShape& water = out.emplace_back();
        water.up = w.r[2];
        water.origin = w.t;
        water.vertex_uv = (flags & k_water_vertex_uv) && !g.uvs.empty();
        for (const auto& tri : g.triangles) {
            for (uint16_t i : tri) {
                const glm::vec3 p = w.r * (w.s * g.positions[i]) + w.t;
                osw::WaterVertex v{};
                v.position[0] = p.x;
                v.position[1] = p.y;
                v.position[2] = p.z;
                if (i < g.uvs.size()) {
                    v.uv[0] = g.uvs[i].x;
                    v.uv[1] = g.uvs[i].y;
                }
                v.depth = !vertex_depth ? -1.0f : i < g.colors.size() ? float(g.colors[i].a) / 255.0f : 1.0f;
                water.vertices.push_back(v);
            }
        }
    };
    for (NifRef root : nif.roots) {
        walk(root, Xf{});
    }
    return out;
}

// The flow maps: 64 x 64 uncompressed DDS, one per cell, named by the cell's coordinates.
void read_flow_maps(const legacy::DataSource& data, osw::World& world) {
    std::vector<std::pair<osw::FlowCell, std::string>> files;
    for (const auto& path : data.list("textures/water/skyrim.esm/flow.", ".dds")) {
        osw::FlowCell c{};
        if (std::sscanf(path.c_str() + path.rfind('/') + 1, "flow.%d.%d.dds", &c.x, &c.y) == 2) {
            files.emplace_back(c, path);
        }
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return std::pair(a.first.y, a.first.x) < std::pair(b.first.y, b.first.x);
    });
    for (const auto& [cell, path] : files) {
        const auto bytes = data.read(path);
        if (!bytes || bytes->size() < 128 + osw::k_flow_cell_bytes) {
            continue;
        }
        const auto* b = reinterpret_cast<const uint8_t*>(bytes->data());
        uint32_t masks[4];
        std::memcpy(masks, b + 92, sizeof(masks)); // DDS_PIXELFORMAT: R, G, B, A
        auto shift = [](uint32_t m) { return m ? uint32_t(std::countr_zero(m)) : 0u; };
        world.flow_cells.push_back(cell);
        const size_t at = world.flow_texels.size();
        world.flow_texels.resize(at + osw::k_flow_cell_bytes);
        for (uint32_t i = 0; i < osw::k_flow_size * osw::k_flow_size; ++i) {
            uint32_t v;
            std::memcpy(&v, b + 128 + i * 4, 4);
            for (int c = 0; c < 4; ++c) {
                world.flow_texels[at + i * 4 + c] = uint8_t((v >> shift(masks[c])) & 0xFF);
            }
        }
    }
    log::info("{}: {} flow maps", world.edid, world.flow_cells.size());
}

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Runs `work(i)` for i in [0, count) on `threads` workers, logging progress.
void parallel_for(size_t count, unsigned threads, std::string_view what, const std::function<void(size_t)>& work) {
    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
    auto start = Clock::now();
    {
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&] {
                for (size_t i; (i = next++) < count;) {
                    work(i);
                    auto n = ++done;
                    if (n % 2000 == 0) {
                        log::info("  {}: {}/{}", what, n, count);
                    }
                }
            });
        }
    }
    log::info("  {}: {} done in {:.1f}s", what, count, seconds_since(start));
}

enum class MeshState { ok, empty, missing, failed };

// The game hangs the light under the first node named "AttachLight" (depth-first from the root,
// children in order), else under the root; the root itself takes the reference's transform. Returns that node's
// offset in the root's space.
std::optional<std::array<float, 3>> light_attach_point(const legacy::NifFile& nif) {
    if (nif.roots.empty()) {
        return std::array<float, 3>{0.0f, 0.0f, 0.0f};
    }
    std::vector<legacy::NifRef> chain;
    std::vector<uint8_t> seen(nif.blocks.size(), 0);
    std::function<bool(legacy::NifRef)> find = [&](legacy::NifRef ref) {
        const auto* node = nif.get<legacy::NiNode>(ref);
        if (!node || seen[size_t(ref)]) {
            return false;
        }
        seen[size_t(ref)] = 1;
        chain.push_back(ref);
        if (path::to_lower(node->name) == "attachlight") {
            return true;
        }
        for (auto c : node->children) {
            if (find(c)) {
                return true;
            }
        }
        chain.pop_back();
        return false;
    };
    if (!find(nif.roots[0])) {
        return std::array<float, 3>{0.0f, 0.0f, 0.0f};
    }
    glm::mat3 rot(1.0f);
    glm::vec3 trans(0.0f);
    float scale = 1.0f;
    for (size_t i = 1; i < chain.size(); ++i) {
        const auto* n = nif.av_object(chain[i]);
        trans += scale * (rot * n->translation);
        rot = rot * n->rotation;
        scale *= n->scale;
    }
    return std::array<float, 3>{trans.x, trans.y, trans.z};
}

// Models no worldspace references that the engine loads by name: the Mist Menu's fog behind the main menu
constexpr std::string_view k_interface_models[] = {
    "meshes/interface/intmenufogparticles.nif",
    // Effects the engine spawns by name.
    "meshes/effects/watersplash.nif",
    "meshes/effects/explosionsplash.nif",
    "meshes/water/defaultwaterripples.nif", // rain on water
};

// The screen blood's images, which the game opens by name.
constexpr std::string_view k_effect_textures[] = {
    "textures/blood/screenbloodalpha01opt.dds",
    "textures/blood/screenbloodcolor01opt.dds",
    "textures/blood/fxbloodflare.dds",
};

// What the records' effects draw: no world references these models, and impacts name their decals' textures.
struct EffectAssets {
    std::vector<std::string> models;
    std::set<std::string> textures;
};
EffectAssets effect_assets(const legacy::EsmDatabase& db) {
    EffectAssets out;
    std::set<std::string> models;
    using legacy::record_type;
    for (const legacy::RecordType type : {record_type("IPCT"), record_type("EXPL"), record_type("PROJ"), record_type("HAZD"),
                                          record_type("ARTO"), record_type("ADDN")}) {
        for (auto e : db.of_type(type)) {
            if (e->deleted()) {
                continue;
            }
            const auto rec = db.read(*e);
            if (auto model = path::normalize(rec.string(record_type("MODL"))); !model.empty()) {
                models.insert(path::with_root(model, "meshes"));
            }
        }
    }
    // Debris: each DATA is a percentage, the model and a flag.
    for (auto e : db.of_type(record_type("DEBR"))) {
        if (e->deleted()) {
            continue;
        }
        const auto rec = db.read(*e);
        for (const auto& sub : rec.subrecords()) {
            if (sub.type == record_type("DATA") && sub.data.size() > 2) {
                const auto* text = reinterpret_cast<const char*>(sub.data.data()) + 1;
                const std::string name(text, strnlen(text, sub.data.size() - 1));
                if (auto model = path::normalize(name); !model.empty()) {
                    models.insert(path::with_root(model, "meshes"));
                }
            }
        }
    }
    out.models.assign(models.begin(), models.end());
    out.textures.insert(std::begin(k_effect_textures), std::end(k_effect_textures));
    for (auto e : db.of_type(record_type("IPCT"))) {
        const auto rec = db.read(*e);
        for (const legacy::RecordType sig : {record_type("DNAM"), record_type("ENAM")}) {
            const auto* set = db.find(rec.form(sig));
            if (!set || set->deleted()) {
                continue;
            }
            const auto tx = db.read(*set);
            for (const legacy::RecordType slot : {record_type("TX00"), record_type("TX01"), record_type("TX03")}) {
                if (auto t = path::normalize(tx.string(slot)); !t.empty()) {
                    out.textures.insert(path::with_root(t, "textures"));
                }
            }
        }
    }
    for (auto e : db.of_type(record_type("EFSH"))) {
        const auto rec = db.read(*e);
        for (const legacy::RecordType sig : {record_type("ICON"), record_type("ICO2"), record_type("NAM7"),
                                             record_type("NAM8"), record_type("NAM9")}) {
            if (auto t = path::normalize(rec.string(sig)); !t.empty()) {
                out.textures.insert(path::with_root(t, "textures"));
            }
        }
    }
    return out;
}

struct MeshConversion {
    MeshState state = MeshState::failed;
    bool effect = false;    // a .ose exists for it
    bool collision = false; // a .osp exists for it
    std::vector<std::string> textures;
};

// One NIF -> .osm, its collision (.osp) and, when it has particle systems, its effect (.ose). An output is reused
// unless `force` when its recorded source key is current (incremental.hpp) and it reads with this build's format
// version; the effect is made with the mesh (it names the mesh's submeshes), so it is reused or redone with it. A
// NIF without collision writes no .osp, so reuse runs parse those again (cheap next to the render conversion).
// Destructible records' models: the replacements their stages name, and each stage a model's
// own BSDamageStage nodes show.
struct DestructionModels {
    std::vector<std::string> replacements;
    std::set<std::pair<std::string, int>> stages;
};

DestructionModels destruction_models(const legacy::EsmDatabase& db) {
    DestructionModels out;
    std::set<std::string> seen;
    using legacy::record_type;
    for (const legacy::RecordType type : {record_type("ACTI"), record_type("MSTT"), record_type("FURN"), record_type("CONT"),
                                          record_type("DOOR"), record_type("LIGH"), record_type("MISC"), record_type("FLOR"),
                                          record_type("TREE"), record_type("STAT")}) {
        for (auto e : db.of_type(type)) {
            if (e->deleted()) {
                continue;
            }
            const auto rec = db.read(*e);
            const auto model = path::normalize(rec.string(legacy::record_type("MODL")));
            for (const auto& sub : rec.subrecords()) {
                if (sub.type == legacy::record_type("DSTD") && sub.data.size() > 2 && !model.empty()) {
                    if (const int stage = std::to_integer<int>(sub.data[2]); stage > 0) {
                        out.stages.emplace(path::with_root(model, "meshes"), stage);
                    }
                } else if (sub.type == legacy::record_type("DMDL")) {
                    const char* text = reinterpret_cast<const char*>(sub.data.data());
                    auto replacement = path::with_root(path::normalize(std::string(text, strnlen(text, sub.data.size()))), "meshes");
                    if (seen.insert(replacement).second) {
                        out.replacements.push_back(std::move(replacement));
                    }
                }
            }
        }
    }
    return out;
}

void convert_damage_stage(const legacy::DataSource& data, AssetCache& assets, const std::filesystem::path& cache,
                          const std::string& src, int stage, bool force, std::set<std::string>& textures) {
    const auto rel = osm::damage_stage_path(path::replace_extension(src, ".osm"), stage);
    if (!force && assets.fresh(rel, src, k_mesh_converter_version)) {
        return;
    }
    const auto bytes = data.read(src);
    if (!bytes) {
        return;
    }
    try {
        const legacy::NifFile nif = legacy::read_nif(*bytes);
        auto mesh = import_nif(nif, src, stage);
        if (!mesh.mesh.submeshes.empty()) {
            write_file(cache / rel, osm::write(mesh.mesh));
            textures.insert(mesh.textures.begin(), mesh.textures.end());
        }
        auto collision = import_collision(nif, src, stage);
        if (!collision.collision.shapes.empty()) {
            write_file(cache / path::replace_extension(rel, ".osp"), osp::write(collision.collision));
        }
        assets.record(rel, src, k_mesh_converter_version);
    } catch (const std::exception& e) {
        log::warn("{}: stage {}: {}", src, stage, e.what());
    }
}

MeshConversion convert_mesh(const legacy::DataSource& data, AssetCache& assets, const std::filesystem::path& cache,
                            const std::string& src, bool force) {
    MeshConversion c;
    const auto rel = path::replace_extension(src, ".osm");
    const auto rel_collision = path::replace_extension(src, ".osp");
    const auto out = cache / rel;
    const auto out_fx = cache / path::replace_extension(src, ".ose");
    const auto out_collision = cache / rel_collision;
    try {
        bool reuse_mesh = !force && assets.fresh(rel, src, k_mesh_converter_version);
        // Collision of an older .osp version is redone (its version gains data the engine reads).
        const bool reuse_collision =
            !force && assets.fresh(rel_collision, src, k_collision_converter_version) && [&] {
                try {
                    return container::Reader(read_file(out_collision), osp::k_kind).version() == osp::k_version;
                } catch (const std::exception&) {
                    return false;
                }
            }();
        if (reuse_mesh) {
            // An output that doesn't read with this build's format version (or a damaged one) is converted again.
            try {
                auto mesh = osm::read(read_file(out));
                for (const auto& m : mesh.materials) {
                    for (const auto& t : m.textures) {
                        if (!t.empty()) {
                            c.textures.push_back(t);
                        }
                    }
                }
                if (std::filesystem::exists(out_fx)) {
                    c.effect = true;
                    auto fx = ose::read(read_file(out_fx));
                    for (const auto& m : fx.materials) {
                        for (auto t : {m.source_texture, m.greyscale_texture}) {
                            if (auto p = fx.string(t); !p.empty()) {
                                c.textures.emplace_back(p);
                            }
                        }
                    }
                }
                c.state = mesh.submeshes.empty() ? MeshState::empty : MeshState::ok;
            } catch (const ParseError&) {
                c.textures.clear();
                c.effect = false;
                reuse_mesh = false;
            }
        }
        c.collision = reuse_collision;
        if (!reuse_mesh || !reuse_collision) {
            if (auto bytes = data.read(src)) {
                legacy::NifFile nif;
                try {
                    nif = legacy::read_nif(*bytes);
                } catch (const ParseError& e) {
                    throw ParseError(std::format("{}: {}", src, e.what()));
                }
                if (!reuse_mesh) {
                    auto result = import_nif(nif, src);
                    for (const auto& w : result.warnings) {
                        log::debug("{}: {}", src, w);
                    }
                    if (result.mesh.submeshes.empty()) {
                        c.state = MeshState::empty;
                    } else {
                        write_file(out, osm::write(result.mesh));
                        c.textures = std::move(result.textures);
                        c.state = MeshState::ok;
                    }
                    bool effect = false;
                    const auto addons = read_addon_nodes(nif, *bytes);
                    if (has_particle_systems(nif) || !addons.empty() || has_controller_manager(nif)) {
                        auto fx = import_particles(nif, src, result.submesh_shapes);
                        fx.effect.addon_nodes = addons;
                        for (const auto& w : fx.warnings) {
                            log::debug("{}: {}", src, w);
                        }
                        if (!fx.effect.systems.empty() || !addons.empty() || !fx.effect.sequences.empty()) {
                            for (const std::string& t : read_attach_techniques(nif, *bytes)) {
                                fx.effect.attach_techniques.push_back(fx.effect.add_string(t));
                            }
                            write_file(out_fx, ose::write(fx.effect));
                            c.textures.insert(c.textures.end(), fx.textures.begin(), fx.textures.end());
                            effect = true;
                        }
                    }
                    if (!effect) {
                        // A source that lost its particle systems must not keep the old effect.
                        std::filesystem::remove(out_fx);
                    }
                    c.effect = effect;
                    if (c.state == MeshState::ok) {
                        assets.record(rel, src, k_mesh_converter_version);
                    }
                }
                if (!reuse_collision) {
                    auto result = import_collision(nif, src);
                    for (const auto& w : result.warnings) {
                        log::debug("{}: collision: {}", src, w);
                    }
                    if (!result.collision.shapes.empty()) {
                        write_file(out_collision, osp::write(result.collision));
                        assets.record(rel_collision, src, k_collision_converter_version);
                        c.collision = true;
                    } else {
                        // A source that lost its collision must not keep the old shapes.
                        std::filesystem::remove(out_collision);
                    }
                }
            } else if (!reuse_mesh) {
                c.state = MeshState::missing;
            }
        }
    } catch (const std::exception& e) {
        log::warn("{}: {}", src, e.what());
        c.state = MeshState::failed;
    }
    return c;
}

} // namespace

void import_effects(const legacy::EsmDatabase& db, const legacy::DataSource& data, AssetCache& assets,
                    const std::filesystem::path& cache, unsigned threads, bool force) {
    const EffectAssets effects = effect_assets(db);
    std::set<std::string> found = effects.textures;
    std::mutex mutex;
    parallel_for(effects.models.size(), threads, "effect models", [&](size_t i) {
        auto c = convert_mesh(data, assets, cache, effects.models[i], force);
        std::lock_guard lock(mutex);
        found.insert(c.textures.begin(), c.textures.end());
    });
    const std::vector<std::string> textures(found.begin(), found.end());
    parallel_for(textures.size(), threads, "effect textures", [&](size_t i) {
        const auto& t = textures[i];
        try {
            if (!force && assets.fresh(t, t, k_texture_copy_version)) {
                return;
            }
            if (auto bytes = data.read(t)) {
                write_file(cache / t, *bytes);
                assets.record(t, t, k_texture_copy_version);
            }
        } catch (const std::exception& e) {
            log::warn("{}: {}", t, e.what());
        }
    });
    log::info("effects: {} models, {} textures", effects.models.size(), textures.size());
}

// What PlayGamebryoAnimation plays: objects whose root has an NiControllerManager keep their sequences in a
// .ose, whether or not they have particles. Written beside models that came through before there was a reason to.
void import_object_sequences(const legacy::EsmDatabase& db, const legacy::DataSource& data, const std::filesystem::path& cache,
                             unsigned threads) {
    using legacy::record_type;
    std::set<std::string> found;
    for (const legacy::RecordType type : {record_type("ACTI"), record_type("STAT"), record_type("MSTT"), record_type("FURN"), record_type("DOOR"),
                                          record_type("CONT"), record_type("TACT"), record_type("MISC"), record_type("FLOR"), record_type("TREE")}) {
        for (auto e : db.of_type(type)) {
            if (!e->deleted()) {
                if (auto model = path::normalize(db.read(*e).string(record_type("MODL"))); !model.empty()) {
                    found.insert(path::with_root(model, "meshes"));
                }
            }
        }
    }
    const std::vector<std::string> models(found.begin(), found.end());
    std::atomic<uint32_t> written = 0;
    parallel_for(models.size(), threads, "object sequences", [&](size_t i) {
        const std::string& src = models[i];
        const auto out = cache / path::replace_extension(src, ".ose");
        std::error_code ec;
        if (std::filesystem::exists(out, ec)) {
            return;
        }
        try {
            const auto bytes = data.read(src);
            constexpr std::string_view k_manager = "NiControllerManager";
            if (!bytes || std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()).find(k_manager) == std::string_view::npos) {
                return;
            }
            const legacy::NifFile nif = legacy::read_nif(*bytes);
            const auto mesh = import_nif(nif, src);
            auto fx = import_particles(nif, src, mesh.submesh_shapes, true);
            if (!fx.effect.sequences.empty()) {
                write_file(out, ose::write(fx.effect));
                log::debug("object sequences: {}", src);
                ++written;
            }
        } catch (const std::exception& e) {
            log::warn("{}: {}", src, e.what());
        }
    });
    log::info("object sequences: {} of {} models", written.load(), models.size());
}

// Alternate textures (docs/formats/ots.md). The textures of a set no model names come along here.
void import_texture_swaps(const legacy::EsmDatabase& db, const legacy::DataSource& data, AssetCache& assets,
                          const std::filesystem::path& cache, unsigned threads, bool force) {
    using legacy::record_type;
    const legacy::RecordType mods = record_type("MODS");
    std::vector<const legacy::RecordEntry*> entries;
    // Armor on the ground: the world model and its list, the male's.
    const legacy::RecordType armor = record_type("ARMO");
    for (auto e : db.of_type(armor)) {
        if (!e->deleted() && db.read(*e).has(record_type("MO2S"))) {
            entries.push_back(e);
        }
    }
    for (const legacy::RecordType type : {record_type("WEAP"), record_type("ACTI"), record_type("STAT"), record_type("MSTT"), record_type("FURN"), record_type("DOOR"),
                                          record_type("CONT"), record_type("FLOR"), record_type("TREE"), record_type("LIGH")}) {
        for (auto e : db.of_type(type)) {
            if (!e->deleted() && db.read(*e).has(mods)) {
                entries.push_back(e);
            }
        }
    }
    ots::File file;
    std::set<std::string> textures;
    std::mutex mutex;
    parallel_for(entries.size(), threads, "texture swaps", [&](size_t i) {
        try {
            const auto rec = db.read(*entries[i]);
            const bool worn = rec.has(record_type("MO2S"));
            const std::string src = path::with_root(path::normalize(rec.string(record_type(worn ? "MOD2" : "MODL"))), "meshes");
            const auto bytes = data.read(src);
            if (!bytes) {
                return;
            }
            const legacy::NifFile nif = legacy::read_nif(*bytes);
            const auto mesh = import_nif(nif, src);
            BinaryReader r(rec.find(worn ? record_type("MO2S") : mods)->data);
            ots::Swap swap;
            swap.base_form_id = entries[i]->form_id;
            std::set<std::string> found;
            for (auto n = r.read<uint32_t>(); n > 0; --n) {
                const auto length = r.read<uint32_t>();
                std::string name(length, '\0');
                r.read_into(std::span<char>(name));
                const auto* set = db.find(rec.resolve(r.read<uint32_t>()));
                r.read<int32_t>(); // the 3D index
                if (!set || set->deleted()) {
                    continue;
                }
                const auto tx = db.read(*set);
                ots::Shape shape;
                shape.name = name;
                const legacy::RecordType k_slots[8] = {record_type("TX00"), record_type("TX01"), record_type("TX02"), record_type("TX03"),
                                                       record_type("TX04"), record_type("TX05"), record_type("TX06"), record_type("TX07")};
                for (size_t t = 0; t < 8; ++t) {
                    if (auto p = path::normalize(tx.string(k_slots[t])); !p.empty()) {
                        shape.textures[t] = path::with_root(p, "textures");
                        found.insert(shape.textures[t]);
                    }
                }
                bool named = false;
                for (size_t k = 0; k < mesh.submesh_shapes.size(); ++k) {
                    const auto* av = nif.av_object(mesh.submesh_shapes[k]);
                    if (av && path::to_lower(av->name) == path::to_lower(name)) {
                        shape.submesh = uint32_t(k);
                        swap.shapes.push_back(shape);
                        named = true;
                    }
                }
                if (!named) { // a skinned shape the static model doesn't have: it still goes by the name
                    shape.submesh = ~0u;
                    swap.shapes.push_back(shape);
                }
            }
            if (!swap.shapes.empty()) {
                std::lock_guard lock(mutex);
                file.swaps.push_back(std::move(swap));
                textures.insert(found.begin(), found.end());
            }
        } catch (const std::exception& e) {
            log::warn("texture swaps: {:08X}: {}", entries[i]->form_id, e.what());
        }
    });
    // Armor addons: one list per model. They draw as skinned models, which go by the shape's name.
    const std::pair<legacy::RecordType, uint32_t> addon_lists[] = {{record_type("MO2S"), ots::variant_male}, {record_type("MO3S"), ots::variant_female},
                                                                   {record_type("MO4S"), ots::variant_male_first}, {record_type("MO5S"), ots::variant_female_first}};
    for (auto e : db.of_type(record_type("ARMA"))) {
        if (e->deleted()) {
            continue;
        }
        const auto rec = db.read(*e);
        for (const auto& [sig, variant] : addon_lists) {
            const auto* list = rec.find(sig);
            if (!list) {
                continue;
            }
            BinaryReader r(list->data);
            ots::Swap swap;
            swap.base_form_id = e->form_id;
            swap.variant = variant;
            for (auto n = r.read<uint32_t>(); n > 0; --n) {
                std::string name(r.read<uint32_t>(), '\0');
                r.read_into(std::span<char>(name));
                const auto* set = db.find(rec.resolve(r.read<uint32_t>()));
                r.read<int32_t>();
                if (!set || set->deleted()) {
                    continue;
                }
                const auto tx = db.read(*set);
                ots::Shape shape;
                shape.name = name;
                shape.submesh = ~0u;
                const legacy::RecordType slots[8] = {record_type("TX00"), record_type("TX01"), record_type("TX02"), record_type("TX03"),
                                                     record_type("TX04"), record_type("TX05"), record_type("TX06"), record_type("TX07")};
                for (size_t t = 0; t < 8; ++t) {
                    if (auto p = path::normalize(tx.string(slots[t])); !p.empty()) {
                        shape.textures[t] = path::with_root(p, "textures");
                        textures.insert(shape.textures[t]);
                    }
                }
                swap.shapes.push_back(std::move(shape));
            }
            if (!swap.shapes.empty()) {
                file.swaps.push_back(std::move(swap));
            }
        }
    }
    std::ranges::sort(file.swaps, [](const ots::Swap& a, const ots::Swap& b) {
        return std::pair(a.base_form_id, a.variant) < std::pair(b.base_form_id, b.variant);
    });
    write_file(cache / "rules" / "texture_swaps.ots", ots::write(file));
    const std::vector<std::string> list(textures.begin(), textures.end());
    parallel_for(list.size(), threads, "swapped textures", [&](size_t i) {
        const auto& t = list[i];
        try {
            if (!force && assets.fresh(t, t, k_texture_copy_version)) {
                return;
            }
            if (auto bytes = data.read(t)) {
                write_file(cache / t, *bytes);
                assets.record(t, t, k_texture_copy_version);
            }
        } catch (const std::exception& e) {
            log::warn("{}: {}", t, e.what());
        }
    });
    log::info("texture swaps: {} objects, {} textures", file.swaps.size(), list.size());
}

ImportSummary run_import(const legacy::Install& install, const ImportOptions& options) {
    auto start = Clock::now();
    auto threads = options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency());
    const auto& cache = options.cache_dir;
    std::filesystem::create_directories(cache);
    log::set_file(cache / "import.log");
    log::info("importing {} into {} ({} threads)", install.game_dir.string(), cache.string(), threads);

    legacy::DataSource data(install);
    AssetCache assets(cache, data);
    ImportSummary summary;

    // 0. Records: the forms database, recompiling only plugins whose inputs changed.
    if (options.forms && !options.only_audio) {
        records::FormsImportOptions fo;
        fo.cache_dir = cache;
        fo.language = legacy::read_language(install.skyrim_ini, install.game_dir / "Skyrim_Default.ini");
        fo.threads = threads;
        fo.force = options.force;
        auto forms = records::import_forms(install, data, records::load_schema(), fo);
        summary.forms = forms.link.forms;
        summary.forms_units_compiled = forms.compiled;
        summary.forms_units_reused = forms.reused;
    }

    auto esm_start = Clock::now();
    legacy::EsmDatabase db(install);
    log::info("plugins loaded in {:.1f}s", seconds_since(esm_start));
    if (options.only_effects) {
        import_effects(db, data, assets, cache, threads, options.force);
        assets.save();
        summary.seconds = seconds_since(start);
        return summary;
    }

    if (options.audio) {
        auto audio_start = Clock::now();
        auto records = import_audio_records(db, data, install.game_dir);
        for (const auto& [kind, n] : records.counts) {
            log::info("  audio {}: {}", kind, n);
        }
        for (const auto& [kind, n] : records.missing) {
            log::warn("  audio {}: {} files named by records are missing from the data", kind, n);
        }
        const auto osd_path = cache / "audio" / "sound.osd";
        write_file(osd_path, osd::write(records.db));
        log::info("wrote {} ({:.1f}s)", osd_path.string(), seconds_since(audio_start));
        auto files = convert_audio_files(data, cache, threads, options.force);
        log::info("audio files: {} converted, {} copied, {} already present, {} failed, {} with lip data; "
                  "{:.0f} MB in, {:.0f} MB out ({:.1f}s)",
                  files.converted, files.copied, files.skipped, files.failed, files.lips,
                  files.source_bytes / 1e6, files.output_bytes / 1e6, seconds_since(audio_start));
        summary.audio_files = files.converted + files.copied + files.skipped;
        summary.audio_failed = files.failed;
    }
    // Door animations: small, always rewritten.
    if (!options.only_audio) {
        write_file(cache / "rules" / "doors.oda", oda::write(import_doors(db, data)));
        std::vector<std::string> camera_warnings;
        const okc::Cameras cameras = import_cameras(db, data, camera_warnings);
        for (const auto& w : camera_warnings) {
            log::warn("cameras: {}", w);
        }
        log::info("cameras: {} kill camera models", cameras.models.size());
        write_file(cache / "rules" / "cameras.okc", okc::write(cameras));
    }
    if (options.only_doors) {
        summary.seconds = seconds_since(start);
        return summary;
    }
    if (options.only_audio) {
        summary.seconds = seconds_since(start);
        return summary;
    }

    // 0. Rules: game settings, movement and input data. Small, so always rewritten.
    {
        auto rules = import_rules(db, data);
        for (const auto& w : rules.warnings) {
            log::warn("rules: {}", w);
        }
        const auto& r = rules.rules;
        const auto* race = r.race(r.player.race);
        log::info("rules: {} settings, {} movement types, {} races; player race {} (height {:.2f}), eye {:.3f} m, "
                  "third-person pivot {:.3f} m, bound half ({:.3f}, {:.3f}, {:.3f}) m, {} key bindings",
                  r.settings.size(), r.movement_types.size(), r.races.size(), race ? race->edid : "?",
                  race ? race->height[r.player.female ? 1 : 0] : 0.0f, r.player.eye_height, r.player.camera3_height,
                  r.player.bound_half[0], r.player.bound_half[1], r.player.bound_half[2], r.keys.size());
        write_file(cache / "rules" / "game.osr", osr::write(r));
    }
    // What a plant looks like picked: FLOR and TREE models with a switch get a second mesh with its other child.
    {
        size_t harvested = 0;
        for (const auto type : {legacy::record_type("FLOR"), legacy::record_type("TREE")}) {
            for (const auto* e : db.of_type(type)) {
                auto r = db.read(*e);
                const auto* modl = r.find(legacy::record_type("MODL"));
                if (!modl) {
                    continue;
                }
                std::string model(reinterpret_cast<const char*>(modl->data.data()), modl->data.size());
                model.resize(std::strlen(model.c_str()));
                std::ranges::replace(model, char(92), char(47));
                const std::string src = "meshes/" + path::to_lower(model);
                const auto bytes = data.read(src);
                if (!bytes) {
                    continue;
                }
                try {
                    const legacy::NifFile nif = legacy::read_nif(*bytes);
                    const bool has_switch = std::ranges::any_of(nif.blocks, [](const auto& b) {
                        const auto* n = std::get_if<legacy::NiNode>(&b.data);
                        return n && n->switch_index && n->children.size() > 1;
                    });
                    if (has_switch) {
                        auto picked = import_nif(nif, src, 0, true);
                        write_file(cache / path::replace_extension(src, ".harvested.osm"), osm::write(picked.mesh));
                        ++harvested;
                    }
                } catch (const std::exception& ex) {
                    log::warn("harvested {}: {}", src, ex.what());
                }
            }
        }
        log::info("harvested plants: {} models", harvested);
    }
    if (options.only_rules) {
        summary.seconds = seconds_since(start);
        return summary;
    }

    // 0b. What actors are drawn with. No world references these models.
    {
        auto actors_start = Clock::now();
        auto actors = import_actors(data, assets, cache, threads, options.force);
        // ... and what destructibles turn into, which no world references either.
        {
            const DestructionModels destruction = destruction_models(db);
            const std::vector<std::pair<std::string, int>> stages(destruction.stages.begin(), destruction.stages.end());
            std::set<std::string> stage_textures;
            std::mutex mutex;
            parallel_for(destruction.replacements.size(), threads, "destruction models", [&](size_t i) {
                auto c = convert_mesh(data, assets, cache, destruction.replacements[i], options.force);
                std::lock_guard lock(mutex);
                stage_textures.insert(c.textures.begin(), c.textures.end());
            });
            parallel_for(stages.size(), threads, "damage stages", [&](size_t i) {
                std::set<std::string> found;
                convert_damage_stage(data, assets, cache, stages[i].first, stages[i].second, options.force, found);
                std::lock_guard lock(mutex);
                stage_textures.insert(found.begin(), found.end());
            });
            log::info("destruction: {} replacement models, {} model stages", destruction.replacements.size(), stages.size());
            actors.textures.insert(actors.textures.end(), stage_textures.begin(), stage_textures.end());
        }
        parallel_for(actors.textures.size(), threads, "actor textures", [&](size_t i) {
            const auto& t = actors.textures[i];
            try {
                if (!options.force && assets.fresh(t, t, k_texture_copy_version)) {
                    return;
                }
                if (auto bytes = data.read(t)) {
                    write_file(cache / t, *bytes);
                    assets.record(t, t, k_texture_copy_version);
                }
            } catch (const std::exception& e) {
                log::warn("{}: {}", t, e.what());
            }
        });
        const auto hkx = import_hkx(data, assets, cache, threads, options.force);
        log::info("animation: {} graphs, {} skeletons, {} clips, {} failed", hkx.graphs, hkx.skeletons, hkx.clips, hkx.failed);
        summary.actor_failed += hkx.failed;
        summary.actor_models = actors.models;
        summary.actor_failed += actors.failed;
        log::info("actors: {} models ({} failed), {} skeletons, {} textures ({:.1f}s)", actors.models, actors.failed,
                  actors.skeletons, actors.textures.size(), seconds_since(actors_start));
    }
    if (options.only_actors) {
        import_object_sequences(db, data, cache, threads);
        import_texture_swaps(db, data, assets, cache, threads, options.force);
        assets.save();
        summary.seconds = seconds_since(start);
        return summary;
    }
    import_effects(db, data, assets, cache, threads, options.force);
    import_object_sequences(db, data, cache, threads);
    import_texture_swaps(db, data, assets, cache, threads, options.force);
    // Papyrus: every compiled script and what records attach. Small next to the rest, so always rewritten.
    {
        const auto scripts_start = Clock::now();
        auto scripts = import_scripts(db, data);
        for (const auto& w : scripts.warnings) {
            log::warn("scripts: {}", w);
        }
        write_file(cache / "scripts" / "scripts.oss", oss::write(scripts.scripts));
        log::info("scripts: {} scripts, {} functions, {} instructions, {} attachments ({:.1f}s)",
                  scripts.scripts.scripts.size(), scripts.scripts.functions.size(),
                  scripts.scripts.instructions.size(), scripts.scripts.attachments.size(),
                  seconds_since(scripts_start));
    }

    auto world_ids = options.worlds.empty() ? list_worldspaces(db) : options.worlds;
    const MergedWorlds merged = merged_worlds(db);

    // Light attach points and leaf-animation roots of models, read once each (interiors import in parallel).
    std::mutex nif_mutex;
    std::unordered_map<std::string, std::optional<std::array<float, 3>>> attach_cache;
    std::unordered_map<std::string, bool> leaf_cache;
    const LightAttachFn attach = [&](const std::string& model) -> std::optional<std::array<float, 3>> {
        {
            std::lock_guard lock(nif_mutex);
            if (auto it = attach_cache.find(model); it != attach_cache.end()) {
                return it->second;
            }
        }
        std::optional<std::array<float, 3>> point;
        try {
            if (auto bytes = data.read(model)) {
                point = light_attach_point(legacy::read_nif(*bytes));
            }
        } catch (const std::exception& e) {
            log::warn("{}: {}", model, e.what());
        }
        std::lock_guard lock(nif_mutex);
        return attach_cache.emplace(model, point).first->second;
    };
    const LeafAnimFn leaf_anim = [&](const std::string& model) {
        {
            std::lock_guard lock(nif_mutex);
            if (auto it = leaf_cache.find(model); it != leaf_cache.end()) {
                return it->second;
            }
        }
        bool leaf = false;
        try {
            if (auto bytes = data.read(model)) {
                const auto nif = legacy::read_nif(*bytes);
                if (!nif.roots.empty()) {
                    const auto type = nif.type_name(nif.roots[0]);
                    leaf = type == "BSLeafAnimNode" || type == "BSTreeNode";
                }
            }
        } catch (const std::exception& e) {
            log::warn("{}: {}", model, e.what());
        }
        std::lock_guard lock(nif_mutex);
        return leaf_cache.emplace(model, leaf).first->second;
    };

    // Placed water: the references of water activators keep their models' water shapes.
    std::unordered_map<uint32_t, WaterModel> water_bases;
    for (auto e : db.of_type(legacy::record_type("ACTI"))) {
        if (e->deleted()) {
            continue;
        }
        const auto rec = db.read(*e);
        if (const uint32_t water = rec.form(legacy::record_type("WNAM"))) {
            auto model = path::normalize(rec.string(legacy::record_type("MODL")));
            if (!model.empty()) {
                water_bases.emplace(rec.form_id(), WaterModel{water, path::with_root(model, "meshes")});
            }
        }
    }
    std::unordered_map<std::string, std::vector<WaterShape>> water_shapes;
    std::mutex water_mutex;
    auto add_placed_water = [&](osw::World& world) {
        constexpr float k = units::k_skyrim_unit_m;
        for (const auto& ref : world.refs) {
            const auto base = water_bases.find(ref.base_form_id);
            if (base == water_bases.end()) {
                continue;
            }
            const std::vector<WaterShape>* shapes = nullptr;
            {
                std::lock_guard lock(water_mutex);
                auto it = water_shapes.find(base->second.model);
                if (it == water_shapes.end()) {
                    std::vector<WaterShape> v;
                    try {
                        v = water_shapes_of(data, base->second.model);
                    } catch (const std::exception& e) {
                        log::warn("{}: {}", base->second.model, e.what());
                    }
                    it = water_shapes.emplace(base->second.model, std::move(v)).first;
                }
                shapes = &it->second;
            }
            const glm::quat q(ref.rotation[3], ref.rotation[0], ref.rotation[1], ref.rotation[2]);
            const glm::vec3 position(ref.position[0], ref.position[1], ref.position[2]);
            for (const WaterShape& shape : *shapes) {
                osw::WaterSurface s{};
                s.water = base->second.water;
                s.first_vertex = uint32_t(world.water_vertices.size());
                s.vertex_count = uint32_t(shape.vertices.size());
                glm::vec3 lo(1e30f), hi(-1e30f);
                for (osw::WaterVertex v : shape.vertices) {
                    const glm::vec3 p =
                        q * (glm::vec3(v.position[0], v.position[1], v.position[2]) * (ref.scale * k)) + position;
                    std::copy_n(&p.x, 3, v.position);
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                    world.water_vertices.push_back(v);
                }
                const glm::vec3 c = (lo + hi) * 0.5f;
                std::copy_n(&c.x, 3, s.center);
                s.radius = glm::length(hi - c);
                const glm::vec3 up = glm::normalize(q * shape.up);
                std::copy_n(&up.x, 3, s.plane);
                s.plane[3] = glm::dot(up, q * (shape.origin * (ref.scale * k)) + position);
                s.vertex_uv = shape.vertex_uv;
                world.water_surfaces.push_back(s);
            }
        }
    };

    // 1. Worldspaces and interiors -> in-memory spaces, collecting every mesh they reference.
    std::vector<WorldImport> worlds;
    std::vector<std::filesystem::path> world_files; // parallel to worlds, relative to the cache
    std::map<std::string, size_t> mesh_index; // source nif -> index into `sources`
    std::vector<std::string> sources;
    std::set<std::string> textures{"textures/terrain/noise.dds"}; // the LOD landscape's
    auto collect = [&](WorldImport&& imported, std::filesystem::path file) {
        for (const auto& src : imported.source_meshes) {
            if (mesh_index.emplace(src, sources.size()).second) {
                sources.push_back(src);
            }
        }
        textures.insert(imported.textures.begin(), imported.textures.end());
        worlds.push_back(std::move(imported));
        world_files.push_back(std::move(file));
    };
    for (const auto& id : world_ids) {
        auto world_start = Clock::now();
        auto imported = import_world(db, id, attach, leaf_anim, &merged);
        if (imported.world.edid == "Tamriel") {
            read_flow_maps(data, imported.world);
        }
        add_placed_water(imported.world);
        log::info("world {}: {} cells, {} lands, {} refs, {} meshes, {} load doors ({:.1f}s)", imported.world.edid,
                  imported.world.cells.size(), imported.world.lands.size(), imported.world.refs.size(),
                  imported.world.meshes.size(), imported.world.doors.size(), seconds_since(world_start));
        auto file = std::filesystem::path("worlds") / (path::to_lower(imported.world.edid) + ".osw");
        collect(std::move(imported), std::move(file));
    }
    if (options.interiors) {
        auto interiors_start = Clock::now();
        std::vector<uint32_t> cells = list_interiors(db);
        if (!options.interior_cells.empty()) {
            std::erase_if(cells, [&](uint32_t c) {
                const auto name = interior_space(db, c);
                return std::none_of(options.interior_cells.begin(), options.interior_cells.end(),
                                    [&](const std::string& want) { return path::to_lower(want) == name; });
            });
        }
        std::vector<std::optional<WorldImport>> imported(cells.size());
        parallel_for(cells.size(), threads, "interiors", [&](size_t i) {
            try {
                imported[i] = import_interior(db, cells[i], attach, leaf_anim, &merged);
                add_placed_water(imported[i]->world);
            } catch (const std::exception& e) {
                log::warn("interior {:08X}: {}", cells[i], e.what());
            }
        });
        size_t refs = 0, doors = 0;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (imported[i]) {
                refs += imported[i]->world.refs.size();
                doors += imported[i]->world.doors.size();
                collect(std::move(*imported[i]),
                        std::filesystem::path("interiors") / (interior_space(db, cells[i]) + ".osw"));
            }
        }
        log::info("{} interiors: {} refs, {} load doors ({:.1f}s)", cells.size(), refs, doors,
                  seconds_since(interiors_start));
    }

    // The projected-UV materials' global textures, read by the renderer itself.
    for (const auto& w : worlds) {
        if (!w.world.directional_materials.empty()) {
            for (const char* t : {"textures/effects/projectednoise.dds", "textures/effects/projecteddiffuse.dds",
                                  "textures/effects/projectednormal.dds", "textures/effects/projectednormaldetail.dds"}) {
                textures.insert(t);
            }
            break;
        }
    }

    // 2. Meshes and their collision, in parallel. Existing outputs are reused unless --force. A NIF without
    //    collision writes no .osp, so reuse runs parse those again (cheap next to the render conversion).
    log::info("converting {} meshes", sources.size());
    std::vector<MeshState> states(sources.size(), MeshState::failed);
    std::vector<uint8_t> has_collision(sources.size(), 0);
    std::mutex texture_mutex;
    std::atomic<size_t> effects{0};
    parallel_for(sources.size(), threads, "meshes", [&](size_t i) {
        auto c = convert_mesh(data, assets, cache, sources[i], options.force);
        states[i] = c.state;
        has_collision[i] = c.collision;
        effects += c.effect ? 1 : 0;
        std::lock_guard lock(texture_mutex);
        textures.insert(c.textures.begin(), c.textures.end());
    });
    size_t collision_meshes = 0, collision_only = 0;
    for (size_t i = 0; i < sources.size(); ++i) {
        collision_meshes += has_collision[i];
        collision_only += has_collision[i] && states[i] == MeshState::empty;
    }
    log::info("collision: {} of {} meshes ({} with nothing to draw)", collision_meshes, sources.size(),
              collision_only);

    // 2b. Interface models and the weathers' sky models (not referenced by any worldspace); the sky's textures.
    auto atmosphere = import_atmosphere(db, data);
    textures.insert(atmosphere.textures.begin(), atmosphere.textures.end());
    std::vector<std::string> extra_models(std::begin(k_interface_models), std::end(k_interface_models));
    extra_models.insert(extra_models.end(), atmosphere.meshes.begin(), atmosphere.meshes.end());
    for (const auto& model : extra_models) {
        std::string src(model);
        if (mesh_index.contains(src)) {
            continue;
        }
        auto c = convert_mesh(data, assets, cache, src, options.force);
        effects += c.effect ? 1 : 0;
        if (c.state == MeshState::missing || c.state == MeshState::failed) {
            log::warn("interface model {} could not be converted", src);
        }
        textures.insert(c.textures.begin(), c.textures.end());
    }
    summary.effects = effects;
    for (auto s : states) {
        summary.meshes += s == MeshState::ok;
        summary.meshes_empty += s == MeshState::empty;
        summary.meshes_missing += s == MeshState::missing;
        summary.meshes_failed += s == MeshState::failed;
    }

    // 3. Textures: DDS is a generic GPU format, so they are extracted as-is.
    std::vector<std::string> texture_list(textures.begin(), textures.end());
    log::info("extracting {} textures", texture_list.size());
    std::atomic<size_t> missing_textures{0};
    parallel_for(texture_list.size(), threads, "textures", [&](size_t i) {
        const auto& t = texture_list[i];
        auto out = cache / t;
        try {
            if (!options.force && assets.fresh(t, t, k_texture_copy_version)) {
                return;
            }
            if (auto bytes = data.read(t)) {
                write_file(out, *bytes);
                assets.record(t, t, k_texture_copy_version);
            } else {
                log::debug("missing texture {}", t);
                ++missing_textures;
            }
        } catch (const std::exception& e) {
            log::warn("{}: {}", t, e.what());
            ++missing_textures;
        }
    });
    summary.textures = texture_list.size() - missing_textures;
    summary.textures_missing = missing_textures;

    // 4. Worlds, with refs to unrenderable meshes removed.
    size_t door_count = 0, one_way_doors = 0, automatic_doors = 0;
    for (auto& imported : worlds) {
        std::vector<bool> mesh_ok(imported.world.meshes.size());
        std::vector<bool> collision_only_mesh(mesh_ok.size());
        for (size_t i = 0; i < mesh_ok.size(); ++i) {
            const size_t m = mesh_index.at(imported.source_meshes[i]);
            mesh_ok[i] = states[m] == MeshState::ok;
            collision_only_mesh[i] = !mesh_ok[i] && has_collision[m];
        }
        size_t collision_only_refs = 0;
        for (const auto& r : imported.world.refs) {
            collision_only_refs += r.mesh < collision_only_mesh.size() && collision_only_mesh[r.mesh];
        }
        if (collision_only_refs > 0) {
            log::info("{}: {} references to collision-only meshes are not exported (no render mesh)",
                      imported.world.edid, collision_only_refs);
        }
        summary.refs_dropped += drop_meshes(imported, mesh_ok);
        summary.refs += imported.world.refs.size();
        auto out = cache / world_files[size_t(&imported - worlds.data())];
        write_if_changed(out, osw::write(imported.world));
        log::debug("wrote {} ({} refs)", out.string(), imported.world.refs.size());
        for (const auto& d : imported.world.doors) {
            ++door_count;
            one_way_doors += (d.flags & osw::door_has_marker) ? 0 : 1;
            automatic_doors += (d.flags & osw::door_automatic) ? 1 : 0;
        }
    }

    log::info("{} load doors: {} automatic, {} whose destination door doesn't lead back", door_count, automatic_doors,
              one_way_doors);

    // 5. Atmosphere: what exterior lighting is computed from (docs/formats/osa.md).
    {
        auto out = cache / "atmosphere" / "weather.osa";
        std::filesystem::create_directories(out.parent_path());
        write_if_changed(out, osa::write(atmosphere.atmosphere));
    }
    summary.worlds = worlds.size();

    // 6. The interface: menu movies, font map and translations (docs/formats/osu.md).
    import_interface(data, cache.string(), options.force);
    write_game_strings(db, data, cache.string());
    write_loading_screens(db, data, cache.string(), options.force);
    write_interface_images(db, data, cache.string(), options.force);
    write_gfx_default_image(install.game_dir, cache.string());
    write_worldspace_maps(db, cache.string());
    write_world_tables(db, data, cache.string());
    summary.reused = assets.reused();
    assets.save();
    // 7. The far view, from what has just been written.
    far::build_far_views({cache, options.worlds, options.threads});
    summary.seconds = seconds_since(start);

    log::info("import done in {:.1f}s: {} worlds, {} refs ({} dropped), meshes {} ok / {} empty / {} missing / {} "
              "failed, {} particle effects, textures {} ok / {} missing",
              summary.seconds, summary.worlds, summary.refs, summary.refs_dropped, summary.meshes,
              summary.meshes_empty, summary.meshes_missing, summary.meshes_failed, summary.effects,
              summary.textures, summary.textures_missing);
    return summary;
}

} // namespace spud::importer
