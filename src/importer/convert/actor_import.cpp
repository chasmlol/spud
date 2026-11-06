#include "importer/convert/actor_import.hpp"

#include "core/binary.hpp"
#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"
#include "formats/forms_typed.hpp"
#include "formats/ofm.hpp"
#include "formats/osk.hpp"
#include "formats/osm.hpp"
#include "formats/osp.hpp"
#include "importer/convert/collision_import.hpp"
#include "importer/convert/mesh_import.hpp"
#include "importer/legacy/nif.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace spud::importer {

namespace {

std::string model_path(std::string_view raw) {
    auto p = path::normalize(raw);
    return p.empty() ? p : path::with_root(p, "meshes");
}

// The end of a weight-morphed model with this digit (x_0.nif, x_1.nif); "" for a model not named that way.
std::string weight_end(const std::string& nif, char digit) {
    constexpr size_t k_tail = 6; // "_N.nif"
    if (nif.size() <= k_tail || nif[nif.size() - k_tail] != '_') {
        return {};
    }
    std::string s = nif;
    char& d = s[s.size() - k_tail + 1];
    if (d != '0' && d != '1') {
        return {};
    }
    d = digit;
    return s;
}

// Every skinned model and skeleton the records name, and the texture sets that dress them.
void list_models(const forms::Database& db, const legacy::DataSource& data, std::set<std::string>& models,
                 std::set<std::string>& skeletons, std::set<std::string>& textures, std::set<std::string>& head_models,
                 std::set<std::string>& tris) {
    auto texture_set = [&](const forms::Value* v) {
        const auto* e = v && v->form ? db.find(v->form) : nullptr;
        if (!e) {
            return;
        }
        const forms::Form set(db, *e);
        for (const char* field : {"TX00", "TX01", "TX07"}) {
            if (const auto* t = set.get(field); t && !t->text.empty()) {
                textures.insert(path::with_root(path::normalize(t->text), "textures"));
            }
        }
    };
    auto add = [&](const forms::Value* v) {
        if (v && !v->text.empty()) {
            auto p = model_path(v->text);
            // Both ends of a weight-morphed model convert as one file, named after its _1 end.
            if (auto heavy = weight_end(p, '1'); !heavy.empty() && data.exists(heavy)) {
                p = std::move(heavy);
            }
            models.insert(std::move(p));
        }
    };
    for (const auto* e : db.of_type(forms::sig("ARMA"))) {
        const forms::Form f(db, *e);
        for (const char* field : {"MOD2", "MOD3", "MOD4", "MOD5"}) {
            add(f.get(field));
        }
        texture_set(f.get("NAM0"));
        texture_set(f.get("NAM1"));
    }
    for (const auto* e : db.of_type(forms::sig("HDPT"))) {
        const forms::Form f(db, *e);
        add(f.get("MODL"));
        texture_set(f.get("TNAM"));
        if (const forms::Value* model = f.get("MODL"); model && !model->text.empty()) {
            head_models.insert(model_path(model->text));
        }
        for (const auto* file : f.all("NAM1")) {
            if (!file->text.empty()) {
                tris.insert(model_path(file->text));
            }
        }
    }
    for (const auto* e : db.of_type(forms::sig("WEAP"))) {
        const forms::Form f(db, *e);
        add(f.get("MODL"));
        if (const forms::Value* first = f.get("WNAM"); first && first->form) { // the first-person model, a STAT
            if (const auto* stat = db.find(first->form)) {
                add(forms::Form(db, *stat).get("MODL"));
            }
        }
    }
    for (const auto* e : db.of_type(forms::sig("ANIO"))) { // what idles hold: a broom, a hammer
        add(forms::Form(db, *e).get("MODL"));
    }
    for (const auto* e : db.of_type(forms::sig("AMMO"))) { // the quiver
        add(forms::Form(db, *e).get("MODL"));
    }
    for (const auto* e : db.of_type(forms::sig("RACE"))) {
        const forms::Form f(db, *e);
        texture_set(f.get("DFTM"));
        texture_set(f.get("DFTF"));
        for (const auto* mask : f.all("TINT")) { // what a made face is tinted with
            if (!mask->text.empty()) {
                textures.insert(path::with_root(path::normalize(mask->text), "textures"));
            }
        }
        for (const auto* v : f.all("ANAM")) {
            if (!v->text.empty()) {
                skeletons.insert(model_path(v->text));
            }
        }
    }
    for (auto& p : data.list("meshes/actors/character/facegendata/facegeom/", ".nif")) {
        head_models.insert(p);
        models.insert(std::move(p));
    }
    for (auto& p : data.list("textures/actors/character/facegendata/facetint/", ".dds")) {
        textures.insert(std::move(p));
    }
    // The first-person body has its own skeletons; no record names them.
    for (auto& p : data.list("meshes/actors/character/_1stperson/", ".nif")) {
        if (p.find("skeleton") != std::string::npos) {
            skeletons.insert(std::move(p));
        }
    }
}

// A FaceGen.tri. The header, then what the morphs skip over.
ofm::MorphSet convert_tri(std::span<const std::byte> file) {
    BinaryReader r(file);
    const auto magic = r.read_array<char>(8);
    if (std::string_view(magic.data(), magic.size()) != "FRTRI003") {
        throw ParseError("not a FRTRI003 file");
    }
    const auto head = r.read_array<uint32_t>(14);
    const uint32_t vertices = head[0], faces = head[1], quads = head[2], uvs = head[5], flags = head[6], morphs = head[7],
                   added = head[9];
    r.skip((size_t(vertices) + added) * 12 + size_t(faces) * 12 + size_t(quads) * 16);
    if (flags & 1) {
        r.skip(size_t(uvs) * 8 + (size_t(faces) * 12 + size_t(quads) * 16));
    }
    ofm::MorphSet out;
    out.vertex_count = vertices;
    std::set<std::string> seen;
    for (uint32_t m = 0; m < morphs; ++m) {
        const auto length = r.read<uint32_t>();
        const auto name = r.read_array<char>(length);
        std::string text(name.data(), strnlen(name.data(), name.size()));
        const float scale = r.read<float>();
        const auto deltas = r.read_array<int16_t>(size_t(vertices) * 3);
        if (!seen.insert(text).second) {
            continue; // the game's lookup takes the first of a name
        }
        out.names.push_back(std::move(text));
        out.scales.push_back(scale * units::k_skyrim_unit_m);
        const size_t at = out.deltas.size();
        out.deltas.resize(at + size_t(vertices) * 4);
        for (uint32_t v = 0; v < vertices; ++v) {
            std::copy_n(deltas.data() + size_t(v) * 3, 3, out.deltas.data() + at + size_t(v) * 4);
        }
    }
    return out;
}

osk::Skeleton convert_skeleton(const legacy::NifFile& nif) {
    using legacy::NifRef;
    using legacy::NiNode;
    osk::Skeleton out;
    std::vector<uint8_t> seen(nif.blocks.size(), 0);
    std::function<void(NifRef, int32_t, bool)> walk = [&](NifRef ref, int32_t parent, bool root) {
        const auto* node = nif.get<NiNode>(ref);
        if (!node || seen[static_cast<size_t>(ref)]) {
            return;
        }
        seen[static_cast<size_t>(ref)] = 1;
        osk::Bone b;
        b.name = node->name;
        b.parent = parent;
        if (!root) { // the reference's transform takes the root's place, as for any model
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    b.rotation[r][c] = node->rotation[c][r]; // glm is column-major
                }
                b.translation[r] = node->translation[r] * units::k_skyrim_unit_m;
            }
            b.scale = node->scale;
        }
        const auto index = static_cast<int32_t>(out.bones.size());
        out.bones.push_back(std::move(b));
        for (NifRef c : node->children) {
            walk(c, index, false);
        }
    };
    if (!nif.roots.empty()) {
        walk(nif.roots.front(), -1, true);
        // The behaviour project the skeleton's actors run, named on its root.
        if (const auto* root = nif.av_object(nif.roots.front())) {
            for (NifRef e : root->extra_data) {
                if (const auto* bged = nif.get<legacy::BSBehaviorGraphExtraData>(e)) {
                    out.project = path::replace_extension("animations/" + model_path(bged->graph), "");
                }
            }
        }
    }
    return out;
}

void parallel(size_t count, unsigned threads, const std::function<void(size_t)>& work) {
    std::atomic<size_t> next{0};
    std::vector<std::jthread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < count;) {
                work(i);
            }
        });
    }
}

} // namespace

ActorImport import_actors(const legacy::DataSource& data, AssetCache& assets, const std::filesystem::path& cache,
                          unsigned threads, bool force) {
    forms::Database db(cache / "forms.osf");
    std::set<std::string> model_set, skeleton_set, textures, head_models, tris;
    list_models(db, data, model_set, skeleton_set, textures, head_models, tris);
    // Objects with a behaviour graph: only their models say so.
    std::set<std::string> object_models;
    {
        std::vector<std::string> candidates;
        std::set<std::string> seen;
        for (const forms::Sig type : {forms::sig("ACTI"), forms::sig("MSTT"), forms::sig("CONT"), forms::sig("STAT"), forms::sig("FURN")}) {
            for (const auto* e : db.of_type(type)) {
                const forms::Form form(db, *e);
                const forms::Value* model = form.get("MODL");
                if (model && !model->text.empty() && seen.insert(model_path(model->text)).second) {
                    candidates.push_back(model_path(model->text));
                }
            }
        }
        std::mutex found;
        parallel(candidates.size(), threads, [&](size_t i) {
            try {
                const auto bytes = data.read(candidates[i]);
                if (!bytes) {
                    return;
                }
                const legacy::NifFile nif = legacy::read_nif(*bytes);
                const auto* root = nif.roots.empty() ? nullptr : nif.av_object(nif.roots.front());
                bool graph = false, animated = false;
                for (legacy::NifRef e : root ? root->extra_data : std::vector<legacy::NifRef>{}) {
                    graph = graph || nif.get<legacy::BSBehaviorGraphExtraData>(e);
                    if (const auto* bsx = nif.get<legacy::NiIntegerExtraData>(e); bsx && bsx->name == "BSX") {
                        animated = (bsx->value & 1) != 0;
                    }
                }
                // No graph, but it never stops: a banner's first sequence loops from the moment it loads.
                bool loops = false;
                for (size_t b = 0; animated && !graph && b < nif.blocks.size() && !loops; ++b) {
                    if (const auto* manager = std::get_if<legacy::NiControllerManager>(&nif.blocks[b].data)) {
                        const auto* first = manager->sequences.empty() ? nullptr : nif.get<legacy::NiControllerSequence>(manager->sequences[0]);
                        loops = first && first->cycle_type == 0;
                    }
                }
                // A secret door and its like: a sequence moves a node that has collision, so the nodes must drive it.
                bool moves = false;
                if (!graph && !loops) {
                    std::set<std::string> solid;
                    for (size_t b = 0; b < nif.blocks.size(); ++b) {
                        if (const auto* av = nif.av_object(legacy::NifRef(b)); av && av->collision >= 0) {
                            solid.insert(av->name);
                        }
                    }
                    for (size_t b = 0; b < nif.blocks.size() && !moves; ++b) {
                        if (const auto* s = std::get_if<legacy::NiControllerSequence>(&nif.blocks[b].data)) {
                            moves = std::ranges::any_of(s->blocks, [&](const auto& c) { return solid.contains(c.node_name); });
                        }
                    }
                }
                if (graph || loops || moves) {
                    std::lock_guard lock(found);
                    object_models.insert(candidates[i]);
                }
            } catch (const std::exception&) {
                // The mesh pass reports it.
            }
        });
        model_set.insert(object_models.begin(), object_models.end());
        skeleton_set.insert(object_models.begin(), object_models.end());
        log::info("{} of {} models are animated objects", object_models.size(), candidates.size());
    }
    const std::vector<std::string> models(model_set.begin(), model_set.end());
    log::info("converting {} actor models, {} skeletons", models.size(), skeleton_set.size());

    ActorImport out;
    std::mutex mutex;
    std::atomic<size_t> ok{0}, failed{0};
    parallel(models.size(), threads, [&](size_t i) {
        const std::string& src = models[i];
        const auto rel = path::replace_extension("skinned/" + src, ".osm");
        try {
            std::vector<std::string> used;
            bool reuse = !force && assets.fresh(rel, src, k_skinned_converter_version);
            if (reuse) {
                try {
                    const auto mesh = osm::read(read_file(cache / rel));
                    for (const auto& m : mesh.materials) {
                        for (const auto& t : m.textures) {
                            if (!t.empty()) {
                                used.push_back(t);
                            }
                        }
                    }
                } catch (const ParseError&) {
                    reuse = false;
                }
            }
            if (!reuse) {
                const auto bytes = data.read(src);
                if (!bytes) {
                    return; // records name models that don't exist
                }
                const legacy::NifFile nif = legacy::read_nif(*bytes);
                const bool object = object_models.contains(src);
                auto converted = import_skinned_nif(nif, src, head_models.contains(src), object);
                if (object) {
                    // Each body rides on its node.
                    const osk::Skeleton skeleton = convert_skeleton(nif);
                    std::unordered_map<std::string, uint32_t> nodes;
                    for (size_t i = 0; i < skeleton.bones.size(); ++i) {
                        nodes.emplace(skeleton.bones[i].name, static_cast<uint32_t>(i));
                    }
                    auto collision = import_collision(nif, src, 0, &nodes);
                    if (!collision.collision.shapes.empty()) {
                        write_file(cache / path::replace_extension(rel, ".osp"), osp::write(collision.collision));
                    }
                }
                if (converted.mesh.submeshes.empty()) {
                    return;
                }
                if (const auto light = weight_end(src, '0'); !light.empty() && light != src) {
                    if (const auto light_bytes = data.read(light)) {
                        auto other = import_skinned_nif(legacy::read_nif(*light_bytes), light);
                        if (!add_weight_morph(converted.mesh, std::move(other.mesh))) {
                            log::debug("{}: its _0 and _1 files don't match", src);
                        }
                    }
                }
                write_file(cache / rel, osm::write(converted.mesh));
                assets.record(rel, src, k_skinned_converter_version);
                used = std::move(converted.textures);
            }
            ++ok;
            std::lock_guard lock(mutex);
            textures.insert(used.begin(), used.end());
        } catch (const std::exception& e) {
            log::warn("{}: {}", src, e.what());
            ++failed;
        }
    });
    for (const auto& src : tris) {
        const auto rel = "skinned/" + path::replace_extension(src, ".ofm");
        try {
            if (!force && assets.fresh(rel, src, k_skinned_converter_version)) {
                continue;
            }
            if (const auto bytes = data.read(src)) {
                write_file(cache / rel, ofm::write(convert_tri(*bytes)));
                assets.record(rel, src, k_skinned_converter_version);
            }
        } catch (const std::exception& e) {
            log::warn("{}: {}", src, e.what());
        }
    }
    for (const auto& src : skeleton_set) {
        const auto rel = path::replace_extension("skinned/" + src, ".osk");
        try {
            if (!force && assets.fresh(rel, src, k_skinned_converter_version)) {
                ++out.skeletons;
                continue;
            }
            if (const auto bytes = data.read(src)) {
                write_file(cache / rel, osk::write(convert_skeleton(legacy::read_nif(*bytes))));
                assets.record(rel, src, k_skinned_converter_version);
                ++out.skeletons;
            }
        } catch (const std::exception& e) {
            log::warn("{}: {}", src, e.what());
            ++failed;
        }
    }
    out.models = ok;
    out.failed = failed;
    out.textures.assign(textures.begin(), textures.end());
    return out;
}

} // namespace spud::importer
