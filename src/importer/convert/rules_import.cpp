#include "importer/convert/rules_import.hpp"

#include "core/path.hpp"
#include "core/units.hpp"
#include "importer/legacy/nif.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <set>

namespace spud::importer {

namespace {

using legacy::record_type;
using legacy::RecordType;

constexpr RecordType k_gmst = record_type("GMST");
constexpr RecordType k_movt = record_type("MOVT");
constexpr RecordType k_race = record_type("RACE");
constexpr RecordType k_edid = record_type("EDID");
constexpr RecordType k_data = record_type("DATA");
constexpr RecordType k_mnam = record_type("MNAM");
constexpr RecordType k_fnam = record_type("FNAM");
constexpr RecordType k_anam = record_type("ANAM");
constexpr RecordType k_sped = record_type("SPED");
constexpr RecordType k_inam = record_type("INAM");
constexpr RecordType k_rnam = record_type("RNAM");
constexpr RecordType k_nam6 = record_type("NAM6");
constexpr RecordType k_acbs = record_type("ACBS");
constexpr RecordType k_coll = record_type("COLL");
constexpr RecordType k_matt = record_type("MATT");
constexpr RecordType k_furn = record_type("FURN");
constexpr RecordType k_modl = record_type("MODL");
constexpr RecordType k_bnam = record_type("BNAM");
constexpr RecordType k_gnam = record_type("GNAM");
constexpr RecordType k_cnam = record_type("CNAM");
constexpr RecordType k_movement_subrecords[6] = {record_type("WKMV"), record_type("RNMV"), record_type("SWMV"),
                                                 record_type("FLMV"), record_type("SNMV"), record_type("SPMV")};

constexpr uint32_t k_player_npc = 0x00000007;
// Not named by any record: the game's first-person skeleton and its camera node.
constexpr const char* k_first_person_skeleton = "meshes/actors/character/_1stperson/skeleton.nif";
constexpr const char* k_camera_node = "Camera1st [Cam1]";
constexpr const char* k_camera3_node = "Camera3rd [Cam3]";

float read_f32(std::span<const std::byte> d, size_t offset) {
    float v = 0;
    if (offset + 4 <= d.size()) {
        std::memcpy(&v, d.data() + offset, 4);
    }
    return v;
}

uint32_t read_u32(std::span<const std::byte> d, size_t offset) {
    uint32_t v = 0;
    if (offset + 4 <= d.size()) {
        std::memcpy(&v, d.data() + offset, 4);
    }
    return v;
}

// The game's the game of a lower-cased string: CRC-32 (reflected polynomial 0xEDB88320) from 0 with no final xor
// (CRC32_StringCase; e.g. "characterbumper" gives 0x5DD8A028, "stairsstone" 0x359D733D).
uint32_t bs_crc32_lower(std::string_view s) {
    uint32_t c = 0;
    for (const char ch : s) {
        c ^= static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(ch)));
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        }
    }
    return c;
}

// Node world transforms in a NIF, by following parents.
class NifPose {
public:
    explicit NifPose(const legacy::NifFile& nif) : m_nif(nif), m_parent(nif.blocks.size(), -1) {
        for (size_t i = 0; i < nif.blocks.size(); ++i) {
            if (const auto* n = std::get_if<legacy::NiNode>(&nif.blocks[i].data)) {
                for (auto c : n->children) {
                    if (nif.valid(c) && m_parent[static_cast<size_t>(c)] == -1) {
                        m_parent[static_cast<size_t>(c)] = static_cast<legacy::NifRef>(i);
                    }
                }
            }
        }
    }

    legacy::NifRef find(std::string_view name) const {
        for (size_t i = 0; i < m_nif.blocks.size(); ++i) {
            const auto* av = m_nif.av_object(static_cast<legacy::NifRef>(i));
            if (av && av->name == name) {
                return static_cast<legacy::NifRef>(i);
            }
        }
        return -1;
    }

    // Position of a node's origin in the file's root space (Skyrim units).
    glm::vec3 origin(legacy::NifRef ref) const {
        glm::vec3 p(0.0f);
        for (int depth = 0; ref != -1 && depth < 256; ++depth) {
            const auto* av = m_nif.av_object(ref);
            if (!av) {
                break;
            }
            p = av->rotation * (av->scale * p) + av->translation;
            ref = m_parent[static_cast<size_t>(ref)];
        }
        return p;
    }

private:
    const legacy::NifFile& m_nif;
    std::vector<legacy::NifRef> m_parent;
};

std::optional<legacy::NifFile> load_nif(const legacy::DataSource& data, std::string_view path,
                                        std::vector<std::string>& warnings) {
    auto bytes = data.read(path);
    if (!bytes) {
        warnings.push_back(std::format("{} not found", path));
        return std::nullopt;
    }
    try {
        return legacy::read_nif(*bytes);
    } catch (const std::exception& e) {
        warnings.push_back(std::format("{}: {}", path, e.what()));
        return std::nullopt;
    }
}

uint16_t parse_code(std::string_view tok) {
    // "0x1d", "0x1d" (a chord: kept as its last key), "!0,Activate" (menu references) -> unmapped.
    if (auto plus = tok.rfind('+'); plus != std::string_view::npos) {
        tok = tok.substr(plus + 1);
    }
    if (tok.starts_with("0x") || tok.starts_with("0X")) {
        uint32_t v = 0;
        auto [p, ec] = std::from_chars(tok.data() + 2, tok.data() + tok.size(), v, 16);
        if (ec == std::errc() && p == tok.data() + tok.size()) {
            return static_cast<uint16_t>(std::min<uint32_t>(v, 0xFFFF));
        }
    }
    return 0xFF;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (;;) {
        const size_t at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
        if (at == std::string_view::npos) {
            return out;
        }
        start = at + 1;
    }
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

// Gamebryo's quadratic float keys: Hermite on the stored tangents.
float sample_keys(const legacy::NiFloatData& d, float time) {
    const auto& k = d.keys;
    size_t i = 0;
    while (i + 2 < k.size() && k[i + 1].time < time) {
        ++i;
    }
    if (k.size() < 2 || time <= k[i].time) {
        return k.empty() ? 0.0f : k[i].value;
    }
    const float t = std::min((time - k[i].time) / (k[i + 1].time - k[i].time), 1.0f), t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * k[i].value + (t3 - 2 * t2 + t) * k[i].backward + (-2 * t3 + 3 * t2) * k[i + 1].value +
           (t3 - t2) * k[i + 1].forward;
}

// Kochanek-Bartels tangents at key i, on each side, of a value got by `at`: published form with the time step scaling.
template <class Keys, class Value>
void tcb_tangents(const Keys& k, size_t i, Value (*at)(const Keys&, size_t), Value& incoming, Value& outgoing) {
    const auto& tbc = k[i].tbc;
    const float t = tbc[0], c = tbc[1], b = tbc[2];
    const Value v = at(k, i);
    const Value p = i > 0 ? at(k, i - 1) : v, n = i + 1 < k.size() ? at(k, i + 1) : v;
    const float before = i > 0 ? k[i].time - k[i - 1].time : 0.0f, after = i + 1 < k.size() ? k[i + 1].time - k[i].time : 0.0f;
    const float total = before + after;
    const Value d0 = v - p, d1 = n - v;
    const Value in = 0.5f * ((1 - t) * (1 - c) * (1 + b)) * d0 + 0.5f * ((1 - t) * (1 + c) * (1 - b)) * d1;
    const Value out = 0.5f * ((1 - t) * (1 + c) * (1 + b)) * d0 + 0.5f * ((1 - t) * (1 - c) * (1 - b)) * d1;
    incoming = total > 0.0f ? in * (2.0f * before / total) : in;
    outgoing = total > 0.0f ? out * (2.0f * after / total) : out;
}

template <class Keys, class Value>
Value sample_any(const Keys& k, uint32_t type, float time, Value (*at)(const Keys&, size_t), Value (*forward)(const Keys&, size_t),
                 Value (*backward)(const Keys&, size_t)) {
    if (k.empty()) {
        return Value{};
    }
    size_t i = 0;
    while (i + 2 < k.size() && k[i + 1].time < time) {
        ++i;
    }
    if (k.size() < 2 || time <= k[i].time) {
        return at(k, i);
    }
    const float span = k[i + 1].time - k[i].time;
    const float u = std::min((time - k[i].time) / span, 1.0f), u2 = u * u, u3 = u2 * u;
    const Value a = at(k, i), b = at(k, i + 1);
    if (type == 1) {
        return a + (b - a) * u;
    }
    if (type == 5) {
        return a;
    }
    Value m0 = backward(k, i), m1 = forward(k, i + 1);
    if (type == 3) {
        Value unused{};
        tcb_tangents(k, i, at, unused, m0);
        tcb_tangents(k, i + 1, at, m1, unused);
    }
    return (2 * u3 - 3 * u2 + 1) * a + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * b + (u3 - u2) * m1;
}

float sample_float(const legacy::NiFloatData& d, float time) {
    using K = std::vector<legacy::NiFloatKey>;
    return sample_any<K, float>(
        d.keys, d.key_type, time, +[](const K& k, size_t i) { return k[i].value; },
        +[](const K& k, size_t i) { return k[i].forward; }, +[](const K& k, size_t i) { return k[i].backward; });
}

glm::vec3 sample_vec3(const std::vector<legacy::NiVec3Key>& keys, uint32_t type, float time) {
    using K = std::vector<legacy::NiVec3Key>;
    return sample_any<K, glm::vec3>(
        keys, type, time, +[](const K& k, size_t i) { return k[i].value; },
        +[](const K& k, size_t i) { return k[i].forward; }, +[](const K& k, size_t i) { return k[i].backward; });
}

std::vector<osr::KeyBinding> parse_control_map(std::string_view text) {
    std::vector<osr::KeyBinding> out;
    uint32_t context = 0;
    bool in_context = false;
    for (auto raw : split(text, '\n')) {
        auto line = trim(raw);
        if (line.starts_with("//")) {
            continue;
        }
        if (line.empty()) {
            // A blank line starts the next input context.
            if (in_context) {
                ++context;
                in_context = false;
            }
            continue;
        }
        // Fields are separated by runs of tabs; the event name may contain spaces.
        std::vector<std::string_view> fields;
        for (auto f : split(line, '\t')) {
            f = trim(f);
            if (!f.empty()) {
                fields.push_back(f);
            }
        }
        if (fields.size() < 4) {
            continue;
        }
        in_context = true;
        osr::KeyBinding k;
        k.event = std::string(fields[0]);
        k.context = context;
        auto keys = split(fields[1], ',');
        for (size_t i = 0; i < keys.size() && i < 2; ++i) {
            k.keys[i] = parse_code(trim(keys[i]));
        }
        k.mouse = parse_code(trim(split(fields[2], ',')[0]));
        k.gamepad = parse_code(trim(split(fields[3], ',')[0]));
        out.push_back(std::move(k));
    }
    return out;
}

okc::Cameras import_cameras(const legacy::EsmDatabase& db, const legacy::DataSource& data,
                            std::vector<std::string>& warnings) {
    std::set<std::string> paths;
    for (const auto* e : db.of_type(legacy::record_type("CAMS"))) {
        const auto r = db.read(*e);
        if (const auto* modl = r.find(k_modl)) {
            std::string model(reinterpret_cast<const char*>(modl->data.data()), modl->data.size());
            model.resize(std::strlen(model.c_str()));
            std::ranges::replace(model, char(92), char(47));
            if (!model.empty()) {
                paths.insert(path::to_lower(model));
            }
        }
    }
    okc::Cameras out;
    for (const std::string& p : paths) {
        const auto nif = load_nif(data, std::format("meshes/{}", p), warnings);
        if (!nif) {
            continue;
        }
        // The root carries the controller; its first child is the NiCamera. The one float
        // track is the frustum's right as a game object drives it, except that a path's is how far along the path
        // it is: a path keeps its view constant.
        const legacy::NiTransformData* keys = nullptr;
        const legacy::NiFloatData* floats = nullptr;
        const legacy::NiPosData* path_keys = nullptr;
        okc::Model model;
        model.path = p;
        for (const auto& b : nif->blocks) {
            if (const auto* d = std::get_if<legacy::NiTransformData>(&b.data)) {
                keys = d;
            } else if (const auto* c = std::get_if<legacy::NiCamera>(&b.data)) {
                model.frustum_right = c->frustum_right;
            } else if (const auto* f = std::get_if<legacy::NiFloatData>(&b.data)) {
                floats = f;
            } else if (const auto* s = std::get_if<legacy::NiPosData>(&b.data)) {
                path_keys = s;
            }
        }
        std::vector<legacy::NiVec3Key> path;
        if (path_keys) {
            path.resize(path_keys->keys[0].size());
            for (size_t k = 0; k < path.size(); ++k) {
                path[k].time = path_keys->keys[0][k][0];
                for (int c = 0; c < 3; ++c) {
                    const auto& key = path_keys->keys[size_t(c)][k];
                    path[k].value[c] = key[1];
                    path[k].forward[c] = key[2];
                    path[k].backward[c] = key[3];
                }
                path[k].tbc = {path_keys->keys[0][k][2], path_keys->keys[0][k][3], path_keys->keys[0][k][4]};
            }
        }
        float stop = 0.0f;
        const auto last = [&stop](const auto& list) {
            if (!list.empty()) {
                stop = std::max(stop, list.back().time);
            }
        };
        if (keys) {
            last(keys->translation_keys);
            last(keys->rotation_keys);
            for (const auto& axis : keys->xyz) {
                last(axis.keys);
            }
        }
        if (floats) {
            last(floats->keys);
        }
        if (model.frustum_right <= 0.0f) {
            warnings.push_back(std::format("{}: no NiCamera", p));
            continue;
        }
        model.rotates = keys && (keys->rotation_type == 4 ? !keys->xyz[0].keys.empty() || !keys->xyz[1].keys.empty() ||
                                                                !keys->xyz[2].keys.empty()
                                                          : !keys->rotation_keys.empty());
        if (keys && keys->rotation_type != 4 && !keys->rotation_keys.empty()) {
            warnings.push_back(std::format("{}: quaternion camera keys, type {}", p, keys->rotation_type));
        }
        const legacy::NiAVObject* root = nif->av_object(nif->roots.empty() ? 0 : nif->roots.front());
        for (float t = 0.0f; t <= stop + okc::k_step; t += okc::k_step) {
            okc::Frame f;
            glm::vec3 at = root ? root->translation : glm::vec3(0.0f);
            if (keys && !keys->translation_keys.empty()) {
                at = sample_vec3(keys->translation_keys, keys->translation_type, std::min(t, stop));
            } else if (!path.empty() && floats) {
                const float along = sample_float(*floats, std::min(t, stop));
                const float from = path.front().time, to = path.back().time;
                at = sample_vec3(path, path_keys->key_type, from + along * (to - from));
            }
            if (floats && !path_keys) {
                f.right = sample_float(*floats, std::min(t, stop));
            }
            f.translation = {at.x, at.y, at.z};
            if (keys && keys->rotation_type == 4) {
                f.angles = {sample_float(keys->xyz[0], std::min(t, stop)), sample_float(keys->xyz[1], std::min(t, stop)),
                            sample_float(keys->xyz[2], std::min(t, stop))};
            }
            model.frames.push_back(f);
        }
        out.models.push_back(std::move(model));
    }
    return out;
}

RulesImport import_rules(const legacy::EsmDatabase& db, const legacy::DataSource& data) {
    RulesImport out;
    auto& rules = out.rules;
    const float m = units::k_skyrim_unit_m;

    // Game settings: numeric ones only (string settings are localised UI text).
    for (const auto* e : db.of_type(k_gmst)) {
        auto r = db.read(*e);
        const std::string name(r.edid());
        const auto* d = r.find(k_data);
        if (name.empty() || !d || d->data.size() < 4) {
            continue;
        }
        osr::Setting s;
        s.name = name;
        switch (std::tolower(static_cast<unsigned char>(name[0]))) {
        case 'f': s.value = read_f32(d->data, 0); break;
        case 'i': s.value = static_cast<int32_t>(read_u32(d->data, 0)); break;
        case 'u': s.value = read_u32(d->data, 0); break;
        case 'b': s.value = read_u32(d->data, 0) != 0 ? 1 : 0; break;
        default: continue;
        }
        rules.settings.push_back(std::move(s));
    }
    std::ranges::sort(rules.settings, [](const osr::Setting& a, const osr::Setting& b) {
        return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                                            [](char x, char y) { return std::tolower(x) < std::tolower(y); });
    });

    // Movement types. SPED is in Skyrim units per second and radians per second; INAM uses FLT_MAX for
    // "no threshold".
    for (const auto* e : db.of_type(k_movt)) {
        auto r = db.read(*e);
        osr::MovementType t;
        t.form_id = r.form_id();
        t.edid = std::string(r.edid());
        t.name = std::string(r.string(k_mnam));
        if (const auto* s = r.find(k_sped)) {
            auto f = [&](size_t i) { return read_f32(s->data, i * 4); };
            t.left[0] = f(0) * m;
            t.left[1] = f(1) * m;
            t.right[0] = f(2) * m;
            t.right[1] = f(3) * m;
            t.forward[0] = f(4) * m;
            t.forward[1] = f(5) * m;
            t.back[0] = f(6) * m;
            t.back[1] = f(7) * m;
            t.rotate_in_place[0] = f(8);
            t.rotate_in_place[1] = f(9);
            t.rotate_while_moving = s->data.size() >= 44 ? f(10) : f(9);
        }
        if (const auto* s = r.find(k_inam)) {
            for (int i = 0; i < 3; ++i) {
                const float v = read_f32(s->data, size_t(i) * 4);
                t.thresholds[i] = v >= std::numeric_limits<float>::max() * 0.5f ? -1.0f : (i == 1 ? v * m : v);
            }
        }
        rules.movement_types.push_back(std::move(t));
    }

    // Races: body scale, acceleration and default movement types.
    for (const auto* e : db.of_type(k_race)) {
        auto r = db.read(*e);
        osr::Race race;
        race.form_id = r.form_id();
        race.edid = std::string(r.edid());
        if (const auto* d = r.find(k_data)) {
            const auto& b = d->data;
            race.height[0] = read_f32(b, 16);
            race.height[1] = read_f32(b, 20);
            race.weight[0] = read_f32(b, 24);
            race.weight[1] = read_f32(b, 28);
            race.flags = read_u32(b, 32);
            race.base_mass = read_f32(b, 52);
            race.acceleration_rate = read_f32(b, 56);
            race.deceleration_rate = read_f32(b, 60);
            race.size = read_u32(b, 64);
            race.angular_acceleration_rate = read_f32(b, 116);
            race.angular_tolerance = read_f32(b, 120);
        }
        for (int i = 0; i < 6; ++i) {
            race.movement_types[i] = r.form(k_movement_subrecords[i]);
        }
        // Skeletons: MNAM starts the male section and FNAM the female one; each has its own ANAM.
        int section = -1;
        for (const auto& sub : r.subrecords()) {
            if (sub.type == k_mnam && sub.data.empty()) {
                section = 0;
            } else if (sub.type == k_fnam && sub.data.empty()) {
                section = 1;
            } else if (sub.type == k_anam && section >= 0 && race.skeleton[section].empty()) {
                race.skeleton[section] = path::with_root(path::normalize(sub.zstring()), "meshes");
            }
        }
        rules.races.push_back(std::move(race));
    }

    // Collision layers: the game's runtime collision matrix (each COLL lists the layers it collides with).
    {
        std::vector<std::pair<uint32_t, osr::CollisionLayer>> layers; // FormID, layer
        for (const auto* e : db.of_type(k_coll)) {
            auto r = db.read(*e);
            osr::CollisionLayer l;
            l.edid = std::string(r.edid());
            if (const auto* b = r.find(k_bnam); b && b->data.size() >= 4) {
                l.index = read_u32(b->data, 0);
            }
            if (const auto* g = r.find(k_gnam); g && g->data.size() >= 4) {
                l.flags = read_u32(g->data, 0);
            }
            layers.emplace_back(r.form_id(), std::move(l));
        }
        for (const auto* e : db.of_type(k_coll)) {
            auto r = db.read(*e);
            const auto* c = r.find(k_cnam);
            auto it = std::ranges::find(layers, r.form_id(), &std::pair<uint32_t, osr::CollisionLayer>::first);
            if (!c || it == layers.end()) {
                continue;
            }
            for (size_t o = 0; o + 4 <= c->data.size(); o += 4) {
                const uint32_t other = r.resolve(read_u32(c->data, o));
                auto jt = std::ranges::find(layers, other, &std::pair<uint32_t, osr::CollisionLayer>::first);
                if (jt != layers.end() && jt->second.index < 64) {
                    it->second.collides |= uint64_t(1) << jt->second.index;
                }
            }
        }
        for (auto& [id, l] : layers) {
            rules.collision_layers.push_back(std::move(l));
        }
        std::ranges::sort(rules.collision_layers, {}, &osr::CollisionLayer::index);
    }

    // Stairs materials: a MATT flagged "stair material" (FNAM bit 0) makes every steep contact on a surface of
    // its material a step (the game through the game;
    // the material ID is the game of the lower-cased MNAM).
    for (const auto* e : db.of_type(k_matt)) {
        auto r = db.read(*e);
        const auto* mnam = r.find(k_mnam);
        const auto* f = r.find(k_fnam);
        if (!mnam || !f || f->data.size() < 4 || (read_u32(f->data, 0) & 1) == 0) {
            continue;
        }
        std::string name(reinterpret_cast<const char*>(mnam->data.data()), mnam->data.size());
        name.resize(std::strlen(name.c_str()));
        rules.stairs_materials.push_back(bs_crc32_lower(name));
    }
    std::ranges::sort(rules.stairs_materials);
    rules.stairs_materials.erase(std::ranges::unique(rules.stairs_materials).begin(), rules.stairs_materials.end());

    // The player NPC and its race.
    auto& player = rules.player;
    player.npc = k_player_npc;
    if (auto r = db.get(k_player_npc)) {
        player.race = r->form(k_rnam);
        if (const auto* s = r->find(k_nam6); s && s->data.size() >= 4) {
            player.npc_height = read_f32(s->data, 0);
        }
        if (const auto* s = r->find(k_acbs); s && s->data.size() >= 4) {
            player.female = (read_u32(s->data, 0) & 1) != 0;
        }
    } else {
        out.warnings.push_back("player NPC_ 00000007 not found");
    }

    // First-person eye height: the camera node of the first-person skeleton, in its bind pose.
    if (auto nif = load_nif(data, k_first_person_skeleton, out.warnings)) {
        NifPose pose(*nif);
        const auto cam = pose.find(k_camera_node);
        if (cam != -1) {
            player.eye_height = pose.origin(cam).z * m;
        } else {
            out.warnings.push_back(std::format("{}: no '{}' node", k_first_person_skeleton, k_camera_node));
        }
    }
    // Every race's skeletons' bounds: what its actors' controllers are built from.
    constexpr float k_bound_m = 0.0142875f;
    std::unordered_map<std::string, std::optional<legacy::BSBound>> bounds;
    for (osr::Race& r : rules.races) {
        for (int sex = 0; sex < 2; ++sex) {
            if (r.skeleton[sex].empty()) {
                continue;
            }
            auto [it, fresh] = bounds.try_emplace(r.skeleton[sex]);
            if (fresh) {
                if (const auto nif = load_nif(data, r.skeleton[sex], out.warnings)) {
                    for (const auto& b : nif->blocks) {
                        if (const auto* bound = std::get_if<legacy::BSBound>(&b.data); bound && bound->name == "BBX") {
                            it->second = *bound;
                            break;
                        }
                    }
                }
            }
            if (it->second) {
                for (int a = 0; a < 3; ++a) {
                    r.bound_centre[sex][a] = it->second->center[a] * k_bound_m;
                    r.bound_half[sex][a] = it->second->half_extents[a] * k_bound_m;
                }
            }
        }
    }
    // Body bounds: the BSBound "BBX" of the race's third-person skeleton.
    const osr::Race* race = rules.race(player.race);
    const std::string skeleton = race ? race->skeleton[player.female ? 1 : 0] : std::string();
    if (!skeleton.empty()) {
        if (auto nif = load_nif(data, skeleton, out.warnings)) {
            bool found = false;
            for (const auto& b : nif->blocks) {
                if (const auto* bound = std::get_if<legacy::BSBound>(&b.data); bound && bound->name == "BBX") {
                    for (int i = 0; i < 3; ++i) {
                        player.bound_centre[i] = bound->center[i] * m;
                        player.bound_half[i] = bound->half_extents[i] * m;
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                out.warnings.push_back(std::format("{}: no BBX bound", skeleton));
            }
            // The third-person camera pivot: the game takes the Cam3 node's local z as the height above the
            // feet, unscaled.
            NifPose pose(*nif);
            if (const auto cam = pose.find(k_camera3_node); cam != -1) {
                player.camera3_height = nif->av_object(cam)->translation.z * m;
            } else {
                out.warnings.push_back(std::format("{}: no '{}' node", skeleton, k_camera3_node));
            }
        }
    } else {
        out.warnings.push_back(std::format("player race {:08X} has no skeleton", player.race));
    }

    // The camera shake: the root's euler keys over the controller's loop.
    if (auto nif = load_nif(data, "meshes/actors/character/camerashake.nif", out.warnings)) {
        const legacy::NiTransformData* track = nullptr;
        for (const auto& b : nif->blocks) {
            if (const auto* d = std::get_if<legacy::NiTransformData>(&b.data); d && d->rotation_type == 4) {
                track = d;
            }
        }
        const float length = track && !track->xyz[0].keys.empty() ? track->xyz[0].keys.back().time : 0.0f;
        for (float t = 0.0f; t < length; t += osr::k_camera_shake_step) {
            rules.camera_shake.push_back({sample_keys(track->xyz[0], t), sample_keys(track->xyz[1], t), sample_keys(track->xyz[2], t)});
        }
    }

    // Furniture markers: every FURN's model's BSFurnitureMarkerNode.
    for (const auto* e : db.of_type(k_furn)) {
        auto r = db.read(*e);
        const auto* modl = r.find(k_modl);
        if (!modl) {
            continue;
        }
        std::string model(reinterpret_cast<const char*>(modl->data.data()), modl->data.size());
        model.resize(std::strlen(model.c_str()));
        std::ranges::replace(model, char(92), char(47));
        std::vector<std::string> quiet; // a model that won't read is no marker: nothing here needs it
        const auto nif = load_nif(data, std::format("meshes/{}", path::to_lower(model)), quiet);
        if (!nif) {
            continue;
        }
        for (const auto& b : nif->blocks) {
            if (const auto* m = std::get_if<legacy::BSFurnitureMarker>(&b.data)) {
                for (size_t i = 0; i < m->positions.size(); ++i) {
                    const legacy::FurnitureMarkerPosition& p = m->positions[i];
                    osr::FurnitureMarker out;
                    out.form = e->form_id;
                    out.index = uint32_t(i);
                    out.offset[0] = p.offset.x;
                    out.offset[1] = p.offset.y;
                    out.offset[2] = p.offset.z;
                    out.heading = p.heading;
                    out.animation = p.animation;
                    out.entry = p.entry;
                    rules.furniture_markers.push_back(out);
                }
            }
        }
    }
    std::ranges::sort(rules.furniture_markers, {}, [](const osr::FurnitureMarker& m) { return std::pair(m.form, m.index); });

    // Default key bindings.
    if (auto text = data.read("interface/controls/pc/controlmap.txt")) {
        rules.keys = parse_control_map(
            std::string_view(reinterpret_cast<const char*>(text->data()), text->size()));
    } else {
        out.warnings.push_back("interface/controls/pc/controlmap.txt not found");
    }
    return out;
}

} // namespace spud::importer
