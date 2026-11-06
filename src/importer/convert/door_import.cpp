#include "importer/convert/door_import.hpp"

#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"
#include "importer/convert/mesh_import.hpp"
#include "importer/legacy/nif.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace spud::importer {

namespace {

using legacy::NifFile;
using legacy::NifRef;
using legacy::RecordType;

constexpr RecordType k_door = legacy::record_type("DOOR");
constexpr RecordType k_modl = legacy::record_type("MODL");
constexpr RecordType k_snam = legacy::record_type("SNAM");
constexpr RecordType k_anam = legacy::record_type("ANAM");

struct Transform {
    glm::mat3 linear{1.0f};
    glm::vec3 translation{0.0f};
};

Transform compose(const Transform& a, const Transform& b) {
    return {a.linear * b.linear, a.linear * b.translation + a.translation};
}

Transform inverse(const Transform& t) {
    const glm::mat3 i = glm::inverse(t.linear);
    return {i, -(i * t.translation)};
}

float hermite(float t, float p0, float m0, float p1, float m1) {
    const float t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1;
}

// Gamebryo's float keys: linear, quadratic (Hermite on the stored tangents) or constant.
float sample_float(const legacy::NiFloatData& d, float time) {
    const auto& k = d.keys;
    if (k.empty()) {
        return 0.0f;
    }
    if (time <= k.front().time) {
        return k.front().value;
    }
    if (time >= k.back().time) {
        return k.back().value;
    }
    size_t i = 0;
    while (k[i + 1].time < time) {
        ++i;
    }
    const float span = k[i + 1].time - k[i].time;
    const float t = span > 0.0f ? (time - k[i].time) / span : 0.0f;
    if (d.key_type == 2) {
        return hermite(t, k[i].value, k[i].backward, k[i + 1].value, k[i + 1].forward);
    }
    if (d.key_type == 5) {
        return k[i].value;
    }
    return k[i].value + (k[i + 1].value - k[i].value) * t;
}

glm::quat to_quat(const glm::vec4& wxyz) { return glm::quat(wxyz.x, wxyz.y, wxyz.z, wxyz.w); }

// A load door only cracks open before the loading screen (3 to 21 degrees in the game's files), and spud has no
// loading screen: it keeps the door's own hinge, direction and curve and takes them as far as a normal door goes.
// The game's normal doors swing 80 to 100 degrees in about a second (FarmhouseAnimDoor01: 92 in 1.0 s).
constexpr float k_load_door_angle = 30.0f; // degrees: under this a door's swing is a load door's crack
constexpr float k_full_swing = 90.0f;
constexpr float k_full_swing_seconds = 1.0f;

struct Stretch {
    float factor = 1.0f;
    float seconds = 0.0f; // 0: the sequence's own
};

// A node's pose at `time`, from its interpolator; what it doesn't animate stays `bind`'s. `closed`: the euler keys at
// the shut door, which a stretch turns from.
Transform sample_node(const NifFile& nif, const legacy::NiTransformInterpolator& ip, float time,
                      const legacy::NiAVObject& bind, float factor, const glm::vec3& closed) {
    glm::mat3 rotation = bind.rotation;
    glm::vec3 translation = bind.translation;
    constexpr float k_none = -3.0e38f;
    if (ip.rotation.x > k_none) {
        rotation = glm::mat3_cast(to_quat(ip.rotation));
    }
    if (ip.translation.x > k_none) {
        translation = ip.translation;
    }
    if (const auto* d = nif.get<legacy::NiTransformData>(ip.data)) {
        if (d->rotation_type == 4) {
            // Euler angles about x, y and z, in that order of application.
            const float x = closed.x + factor * (sample_float(d->xyz[0], time) - closed.x),
                        y = closed.y + factor * (sample_float(d->xyz[1], time) - closed.y),
                        z = closed.z + factor * (sample_float(d->xyz[2], time) - closed.z);
            rotation = glm::mat3_cast(glm::angleAxis(z, glm::vec3(0, 0, 1)) * glm::angleAxis(y, glm::vec3(0, 1, 0)) *
                                      glm::angleAxis(x, glm::vec3(1, 0, 0)));
        } else if (!d->rotation_keys.empty()) {
            const auto& k = d->rotation_keys;
            size_t i = 0;
            while (i + 2 < k.size() && k[i + 1].time < time) {
                ++i;
            }
            if (k.size() == 1 || time <= k[i].time) {
                rotation = glm::mat3_cast(to_quat(k[i].value));
            } else {
                const size_t j = std::min(i + 1, k.size() - 1);
                const float span = k[j].time - k[i].time;
                const float t = span > 0.0f ? std::clamp((time - k[i].time) / span, 0.0f, 1.0f) : 0.0f;
                rotation = glm::mat3_cast(glm::slerp(to_quat(k[i].value), to_quat(k[j].value), t));
            }
        }
        if (!d->translation_keys.empty()) {
            const auto& k = d->translation_keys;
            size_t i = 0;
            while (i + 2 < k.size() && k[i + 1].time < time) {
                ++i;
            }
            if (k.size() == 1 || time <= k[i].time) {
                translation = k[i].value;
            } else {
                const size_t j = std::min(i + 1, k.size() - 1);
                const float span = k[j].time - k[i].time;
                const float t = span > 0.0f ? std::clamp((time - k[i].time) / span, 0.0f, 1.0f) : 0.0f;
                if (d->translation_type == 2) {
                    for (int c = 0; c < 3; ++c) {
                        translation[c] = hermite(t, k[i].value[c], k[i].backward[c], k[j].value[c], k[j].forward[c]);
                    }
                } else {
                    translation = k[i].value + (k[j].value - k[i].value) * t;
                }
            }
        }
    }
    return {rotation * bind.scale, translation};
}

struct Sequence {
    const legacy::NiControllerSequence* seq = nullptr;
    // Node name (lower case) -> its interpolator.
    std::unordered_map<std::string, const legacy::NiTransformInterpolator*> nodes;
};

Sequence find_sequence(const NifFile& nif, std::string_view name) {
    Sequence s;
    for (size_t i = 0; i < nif.blocks.size(); ++i) {
        const auto* q = nif.get<legacy::NiControllerSequence>(NifRef(i));
        if (!q || path::to_lower(q->name) != name) {
            continue;
        }
        s.seq = q;
        for (const auto& b : q->blocks) {
            if (const auto* ip = nif.get<legacy::NiTransformInterpolator>(b.interpolator)) {
                s.nodes[path::to_lower(b.node_name)] = ip;
            }
        }
        break;
    }
    return s;
}

// Parents of every block in the scene tree.
std::unordered_map<NifRef, NifRef> parents(const NifFile& nif) {
    std::unordered_map<NifRef, NifRef> out;
    for (size_t i = 0; i < nif.blocks.size(); ++i) {
        if (const auto* n = nif.get<legacy::NiNode>(NifRef(i))) {
            for (NifRef c : n->children) {
                out[c] = NifRef(i);
            }
        }
    }
    return out;
}

// The chain of `ref` from the root's child down, as `pose` gives each node: mesh space, in Skyrim units.
template <typename F>
Transform chain(const NifFile& nif, const std::unordered_map<NifRef, NifRef>& up, NifRef ref, const F& pose) {
    std::vector<NifRef> path;
    for (NifRef r = ref; r >= 0;) {
        path.push_back(r);
        auto it = up.find(r);
        r = it == up.end() ? -1 : it->second;
    }
    Transform t;
    // The root's own transform is the reference's: the mesh is flattened without it.
    for (size_t i = path.size() - 1; i-- > 0;) {
        t = compose(t, pose(path[i]));
    }
    return t;
}

Transform bind_of(const legacy::NiAVObject& o) { return {o.rotation * o.scale, o.translation}; }

float swing_degrees(const oda::Pose& p) {
    return 2.0f * std::acos(std::min(std::abs(p.rotation[3]), 1.0f)) * 57.29578f;
}

// What stands behind a load door until it opens: DoorBlack, DoorToBlack, BlackPlane, BackPlane01, DoorBlackBG.
bool is_backing(std::string_view name) {
    const std::string n = path::to_lower(name);
    return n.find("black") != std::string::npos || n.find("backplane") != std::string::npos;
}

// The euler keys of a node at the start of Open: the door shut.
glm::vec3 closed_euler(const NifFile& nif, const Sequence& open, const std::string& node) {
    auto it = open.nodes.find(node);
    if (it == open.nodes.end()) {
        return glm::vec3(0.0f);
    }
    const auto* d = nif.get<legacy::NiTransformData>(it->second->data);
    if (!d || d->rotation_type != 4) {
        return glm::vec3(0.0f);
    }
    const float t = open.seq->start_time;
    return {sample_float(d->xyz[0], t), sample_float(d->xyz[1], t), sample_float(d->xyz[2], t)};
}

std::vector<oda::Pose> sample(const NifFile& nif, const std::unordered_map<NifRef, NifRef>& up, NifRef shape,
                              const Sequence& s, const Sequence& open, const Stretch& stretch) {
    const float start = s.seq->start_time, stop = s.seq->stop_time;
    const float seconds = stretch.seconds > 0.0f ? stretch.seconds : stop - start;
    const int count = std::max(int(std::ceil(seconds * oda::k_sample_rate)) + 1, 2);
    const Transform bind = chain(nif, up, shape, [&](NifRef r) { return bind_of(*nif.av_object(r)); });
    const Transform to_bind = inverse(bind);
    std::vector<oda::Pose> out;
    for (int i = 0; i < count; ++i) {
        const float time = start + (stop - start) * float(i) / float(count - 1);
        const Transform now = chain(nif, up, shape, [&](NifRef r) {
            const auto* av = nif.av_object(r);
            const std::string name = path::to_lower(av->name);
            auto it = s.nodes.find(name);
            return it == s.nodes.end() ? bind_of(*av)
                                       : sample_node(nif, *it->second, time, *av, stretch.factor,
                                                     closed_euler(nif, open, name));
        });
        const Transform m = compose(now, to_bind);
        const glm::quat q = glm::quat_cast(m.linear);
        oda::Pose p;
        p.rotation[0] = q.x;
        p.rotation[1] = q.y;
        p.rotation[2] = q.z;
        p.rotation[3] = q.w;
        for (int c = 0; c < 3; ++c) {
            p.translation[c] = m.translation[c] * units::k_skyrim_unit_m;
        }
        out.push_back(p);
    }
    return out;
}

// A moving part's box in mesh space.
void part_box(const osm::Mesh& mesh, oda::Part& part) {
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (uint32_t si : part.submeshes) {
        const osm::Submesh& s = mesh.submeshes[si];
        for (uint32_t i = 0; i < s.vertex_count; ++i) {
            const float* v = mesh.vertices[s.base_vertex + i].position;
            glm::vec3 p(0.0f);
            for (int r = 0; r < 3; ++r) {
                p[r] = s.transform[r][0] * v[0] + s.transform[r][1] * v[1] + s.transform[r][2] * v[2] + s.transform[r][3];
            }
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    for (int c = 0; c < 3; ++c) {
        part.lo[c] = lo[c];
        part.hi[c] = hi[c];
    }
}

// A plain door with no sequences: the game fades to the loading screen, and spud has none. A leaf standing up, a door's
// size, is hung as the game's own doors are: all of the model moves but what stands behind it. The rest (ladders, hatches,
// holes) have nothing to open.
constexpr float k_hung_thin_m = 60.0f * units::k_skyrim_unit_m;
constexpr float k_hung_wide_m = 80.0f * units::k_skyrim_unit_m;
constexpr float k_hung_low_m = 150.0f * units::k_skyrim_unit_m, k_hung_high_m = 250.0f * units::k_skyrim_unit_m;

bool has_back(const osm::Mesh& mesh, const oda::Door& door);

bool hang(const NifFile& nif, const MeshImportResult& mesh, oda::Door& door) {
    oda::Part part;
    for (size_t si = 0; si < mesh.submesh_shapes.size(); ++si) {
        const auto* av = nif.av_object(mesh.submesh_shapes[si]);
        (av && is_backing(av->name) ? door.backing : part.submeshes).push_back(uint32_t(si));
    }
    if (part.submeshes.empty()) {
        return false;
    }
    part_box(mesh.mesh, part);
    const glm::vec3 lo(part.lo[0], part.lo[1], part.lo[2]), hi(part.hi[0], part.hi[1], part.hi[2]), size = hi - lo;
    const int wide = size.x > size.y ? 0 : 1, thin = 1 - wide;
    if (size[thin] > k_hung_thin_m || size[wide] < k_hung_wide_m || size.z < k_hung_low_m || size.z > k_hung_high_m) {
        return false;
    }
    // Measured from the plugins' arrival markers: these models face -y, or -x when they lie along y. As on the game's own
    // doors the hinge is on the right of whoever faces it, and it opens away from them: a clockwise turn either way.
    glm::vec3 hinge = (lo + hi) * 0.5f;
    hinge[wide] = wide == 0 ? hi.x : lo.y;
    const int count = int(std::ceil(k_full_swing_seconds * oda::k_sample_rate)) + 1;
    for (int i = 0; i < count; ++i) {
        const float t = float(i) / float(count - 1), eased = t * t * (3.0f - 2.0f * t);
        const glm::quat q = glm::angleAxis(glm::radians(-k_full_swing * eased), glm::vec3(0, 0, 1));
        const glm::vec3 moved = hinge - q * hinge;
        oda::Pose p;
        p.rotation[0] = q.x;
        p.rotation[1] = q.y;
        p.rotation[2] = q.z;
        p.rotation[3] = q.w;
        for (int c = 0; c < 3; ++c) {
            p.translation[c] = moved[c];
        }
        part.open.push_back(p);
    }
    part.close.assign(part.open.rbegin(), part.open.rend());
    door.parts.push_back(std::move(part));
    door.has_back = has_back(mesh.mesh, door);
    door.hung = true;
    return true;
}

// Whether a door's moving parts are modelled on both faces. Most are; 33 of the 186 models in the game are a front alone
// (under a tenth of the area facing the other way, against over 0.6 for all but one of the rest).
bool has_back(const osm::Mesh& mesh, const oda::Door& door) {
    glm::vec3 lo(1e30f), hi(-1e30f);
    std::vector<glm::vec3> corners;
    for (const oda::Part& part : door.parts) {
        for (uint32_t si : part.submeshes) {
            const osm::Submesh& s = mesh.submeshes[si];
            for (uint32_t i = 0; i < s.index_count; ++i) {
                const float* v = mesh.vertices[s.base_vertex + mesh.indices[s.first_index + i]].position;
                glm::vec3 p(0.0f);
                for (int r = 0; r < 3; ++r) {
                    p[r] = s.transform[r][0] * v[0] + s.transform[r][1] * v[1] + s.transform[r][2] * v[2];
                }
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
                corners.push_back(p);
            }
        }
    }
    const int thin = hi.x - lo.x < hi.y - lo.y ? 0 : 1;
    float faces[2] = {0.0f, 0.0f};
    for (size_t i = 0; i + 2 < corners.size(); i += 3) {
        const float n = glm::cross(corners[i + 1] - corners[i], corners[i + 2] - corners[i])[thin];
        faces[n > 0.0f ? 1 : 0] += std::abs(n);
    }
    return std::min(faces[0], faces[1]) > 0.3f * std::max(faces[0], faces[1]);
}

} // namespace

oda::File import_doors(const legacy::EsmDatabase& db, const legacy::DataSource& data) {
    oda::File file;
    std::map<std::string, int> skipped;
    size_t doors = 0, stretched = 0, hung = 0;
    for (const legacy::RecordEntry* entry : db.of_type(k_door)) {
        ++doors;
        const legacy::Record rec = db.read(*entry);
        const std::string model = path::with_root(path::normalize(rec.string(k_modl)), "meshes");
        if (model.empty()) {
            ++skipped["no model"];
            continue;
        }
        const auto bytes = data.read(model);
        if (!bytes) {
            ++skipped["model missing"];
            continue;
        }
        NifFile nif;
        MeshImportResult mesh;
        try {
            nif = legacy::read_nif(*bytes);
            mesh = import_nif(nif, model);
        } catch (const std::exception& e) {
            log::warn("doors: {}: {}", model, e.what());
            ++skipped["unreadable"];
            continue;
        }
        const Sequence open = find_sequence(nif, "open"), close = find_sequence(nif, "close");
        oda::Door door;
        door.base_form_id = rec.form_id();
        door.mesh = path::replace_extension(model, ".osm");
        door.open_sound = rec.form(k_snam);
        door.close_sound = rec.form(k_anam);
        if (!open.seq || !close.seq || open.nodes.empty()) {
            if (hang(nif, mesh, door)) {
                file.doors.push_back(std::move(door));
                ++hung;
            } else {
                ++skipped["no Open and Close"];
            }
            continue;
        }
        const auto up = parents(nif);
        // How far the sequence turns its shapes decides whether it is a load door's crack.
        Stretch stretch;
        for (NifRef shape : mesh.submesh_shapes) {
            const float degrees = swing_degrees(sample(nif, up, shape, open, open, {}).back());
            stretch.factor = std::max(stretch.factor, degrees);
        }
        if (stretch.factor > 1.0f && stretch.factor < k_load_door_angle) {
            stretch.factor = k_full_swing / stretch.factor;
            stretch.seconds = k_full_swing_seconds;
            ++stretched;
        } else {
            stretch = {};
        }
        // Shapes under the same animated node move as one.
        std::map<NifRef, size_t> part_of;
        for (size_t si = 0; si < mesh.submesh_shapes.size(); ++si) {
            const NifRef shape = mesh.submesh_shapes[si];
            NifRef moved = -1;
            for (NifRef r = shape; r >= 0;) {
                if (const auto* av = nif.av_object(r); av && open.nodes.contains(path::to_lower(av->name))) {
                    moved = r;
                    break;
                }
                auto it = up.find(r);
                r = it == up.end() ? -1 : it->second;
            }
            if (moved < 0) {
                const auto* av = nif.av_object(shape);
                (av && is_backing(av->name) ? door.backing : door.still).push_back(uint32_t(si));
                continue;
            }
            auto [it, fresh] = part_of.try_emplace(moved, door.parts.size());
            if (fresh) {
                // Each leaf goes the whole way from its own crack: a double door's needn't crack alike, and one stretch for
                // both left the Whiterun gate's second leaf at 40 degrees, its bare back to the passage.
                Stretch own = stretch;
                if (const float degrees = swing_degrees(sample(nif, up, shape, open, open, {}).back());
                    own.seconds > 0.0f && degrees > 1.0f) {
                    own.factor = k_full_swing / degrees;
                }
                oda::Part part;
                part.open = sample(nif, up, shape, open, open, own);
                part.close = sample(nif, up, shape, close, open, own);
                door.parts.push_back(std::move(part));
            }
            door.parts[it->second].submeshes.push_back(uint32_t(si));
        }
        if (door.parts.empty()) {
            ++skipped["nothing moves"];
            continue;
        }
        door.has_back = has_back(mesh.mesh, door);
        for (oda::Part& part : door.parts) {
            part_box(mesh.mesh, part);
        }
        file.doors.push_back(std::move(door));
    }
    log::info("doors: {} DOOR records, {} animated, {} load doors taken to a full swing, {} hung", doors, file.doors.size(),
              stretched, hung);
    for (const auto& [k, v] : skipped) {
        log::info("  doors skipped, {}: {}", k, v);
    }
    return file;
}

} // namespace spud::importer
