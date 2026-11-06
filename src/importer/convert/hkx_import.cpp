#include "importer/convert/hkx_import.hpp"

#include "core/binary.hpp"
#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"
#include "importer/legacy/nif.hpp"

#include <atomic>
#include <charconv>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

namespace spud::importer {

namespace {

struct HkxField {
    const char* name;
    uint32_t offset;
    const char* type;
};
struct HkxClass {
    const char* name;
    const char* parent;
    uint32_t size;
    uint32_t first_field;
    uint32_t field_count;
};
#include "importer/convert/hkx_classes.inc"

const HkxClass* find_class(std::string_view name) {
    static const auto table = [] {
        std::unordered_map<std::string_view, const HkxClass*> t;
        for (const auto& c : k_hkx_classes) {
            t.emplace(c.name, &c);
        }
        return t;
    }();
    auto it = table.find(name);
    return it != table.end() ? it->second : nullptr;
}

// The file's sections, as, "The packfile container".
class Packfile {
public:
    explicit Packfile(std::span<const std::byte> file) : m_file(file) {
        if (file.size() < 64 + 48 || u32(0) != 0x57E0E057u || u32(4) != 0x10C0C010u) {
            throw ParseError("not a packfile");
        }
        if (uint8_t(file[16]) != 8) {
            throw ParseError("packfile pointers aren't 64-bit");
        }
        const uint32_t sections = u32(0x14);
        const auto section = [&](std::string_view tag) -> size_t {
            for (uint32_t i = 0; i < sections; ++i) {
                const size_t at = 64 + size_t(i) * 48;
                if (std::string_view(reinterpret_cast<const char*>(file.data() + at)).substr(0, 19) == tag) {
                    return at;
                }
            }
            throw ParseError(std::format("packfile has no {} section", tag));
        };
        const size_t names = section("__classnames__");
        const size_t data = section("__data__");
        const uint32_t names_start = u32(names + 20);
        std::unordered_map<uint32_t, std::string_view> class_names;
        for (size_t at = names_start, end = names_start + size_t(u32(names + 24)); at + 5 < end;) {
            if (uint8_t(file[at + 4]) != 9) {
                break;
            }
            const char* text = reinterpret_cast<const char*>(file.data() + at + 5);
            const std::string_view name(text, strnlen(text, end - at - 5));
            class_names.emplace(static_cast<uint32_t>(at + 5 - names_start), name);
            at += 5 + name.size() + 1;
        }
        m_start = u32(data + 20);
        const uint32_t local = u32(data + 24), global = u32(data + 28), virt = u32(data + 32), exports = u32(data + 36);
        m_size = local;
        for (uint32_t at = local; at + 8 <= global; at += 8) {
            const int32_t src = i32(m_start + at);
            if (src != -1) {
                m_pointers.emplace(uint32_t(src), uint32_t(i32(m_start + at + 4)));
            }
        }
        for (uint32_t at = global; at + 12 <= virt; at += 12) {
            const int32_t src = i32(m_start + at);
            if (src != -1) {
                m_pointers.emplace(uint32_t(src), uint32_t(i32(m_start + at + 8)));
            }
        }
        for (uint32_t at = virt; at + 12 <= exports; at += 12) {
            const int32_t src = i32(m_start + at);
            if (src != -1) {
                auto it = class_names.find(uint32_t(i32(m_start + at + 8)));
                if (it != class_names.end()) {
                    m_classes.emplace(uint32_t(src), it->second);
                }
            }
        }
    }

    // A pointer slot's target in __data__, ~0u for null.
    uint32_t pointer(uint32_t slot) const {
        auto it = m_pointers.find(slot);
        return it != m_pointers.end() ? it->second : ~0u;
    }
    std::string_view class_at(uint32_t object) const {
        auto it = m_classes.find(object);
        return it != m_classes.end() ? it->second : std::string_view{};
    }
    template <typename T>
    T read(uint32_t at) const {
        if (size_t{at} + sizeof(T) > m_size) {
            throw ParseError("packfile read past its data");
        }
        T v;
        std::memcpy(&v, m_file.data() + m_start + at, sizeof(T));
        return v;
    }
    std::string_view text(uint32_t at) const {
        if (at >= m_size) {
            throw ParseError("packfile string past its data");
        }
        const char* s = reinterpret_cast<const char*>(m_file.data() + m_start + at);
        return {s, strnlen(s, m_size - at)};
    }

private:
    uint32_t u32(size_t at) const {
        uint32_t v;
        std::memcpy(&v, m_file.data() + at, 4);
        return v;
    }
    int32_t i32(size_t at) const { return static_cast<int32_t>(u32(at)); }

    std::span<const std::byte> m_file;
    uint32_t m_start = 0;
    uint32_t m_size = 0;
    std::unordered_map<uint32_t, uint32_t> m_pointers;
    std::unordered_map<uint32_t, std::string_view> m_classes;
};

uint32_t primitive_size(std::string_view t) {
    if (t == "bool" || t == "i8" || t == "u8" || t == "char") return 1;
    if (t == "i16" || t == "u16" || t == "f16") return 2;
    if (t == "i32" || t == "u32" || t == "f32") return 4;
    if (t == "i64" || t == "u64" || t == "ulong") return 8;
    if (t == "vec4" || t == "quat") return 16;
    if (t == "mat3" || t == "rot3" || t == "qstransform") return 48;
    if (t == "transform" || t == "mat4") return 64;
    if (t == "string" || t == "cstring" || t == "ptr") return 8;
    return 0;
}

uint32_t float_count(std::string_view t) {
    if (t == "vec4" || t == "quat") return 4;
    if (t == "mat3" || t == "rot3" || t == "qstransform") return 12;
    if (t == "transform" || t == "mat4") return 16;
    return 0;
}

class GraphReader {
public:
    explicit GraphReader(const Packfile& file) : m_file(file) {}

    // The object at `at`, once; its class from the object table, or `cls` when it is embedded.
    uint32_t object(uint32_t at, std::string_view cls) {
        auto key = std::pair(at, std::string(cls));
        if (auto it = m_seen.find(key); it != m_seen.end()) {
            return it->second;
        }
        const HkxClass* c = find_class(cls);
        if (!c) {
            return osb::k_no_object;
        }
        const auto index = static_cast<uint32_t>(m_graph.objects.size());
        m_seen.emplace(std::move(key), index);
        m_graph.objects.emplace_back();
        m_graph.objects[index].cls = cls;
        std::vector<const HkxClass*> chain;
        for (; c; c = *c->parent ? find_class(c->parent) : nullptr) {
            chain.push_back(c);
        }
        std::vector<std::pair<std::string, osb::Value>> members;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            for (uint32_t i = 0; i < (*it)->field_count; ++i) {
                const HkxField& f = k_hkx_fields[(*it)->first_field + i];
                members.emplace_back(f.name, value(at + f.offset, f.type));
            }
        }
        m_graph.objects[index].members = std::move(members); // `objects` moves as members read further objects
        return index;
    }

    osb::Graph take(uint32_t root) {
        m_graph.root = root;
        return std::move(m_graph);
    }

private:
    uint32_t size_of(std::string_view t) const {
        if (t.ends_with('*') || t.starts_with("hkRefPtr")) {
            return 8;
        }
        if (t.starts_with("enum:") || t.starts_with("flags:")) {
            return primitive_size(t.substr(t.find(':') + 1));
        }
        if (const uint32_t p = primitive_size(t)) {
            return p;
        }
        if (t.starts_with("array<")) {
            return 16;
        }
        if (const HkxClass* c = find_class(t)) {
            return c->size;
        }
        throw ParseError(std::format("no size for type {}", t));
    }

    osb::Value integer(int64_t v) {
        osb::Value out;
        out.kind = osb::Kind::integer;
        out.integer = v;
        return out;
    }

    osb::Value value(uint32_t at, std::string_view t) {
        osb::Value v;
        if (t.starts_with("enum:") || t.starts_with("flags:")) {
            t = t.substr(t.find(':') + 1);
        }
        if (t == "bool") return integer(m_file.read<uint8_t>(at) != 0);
        if (t == "i8" || t == "char") return integer(m_file.read<int8_t>(at));
        if (t == "u8") return integer(m_file.read<uint8_t>(at));
        if (t == "i16") return integer(m_file.read<int16_t>(at));
        if (t == "u16") return integer(m_file.read<uint16_t>(at));
        if (t == "i32") return integer(m_file.read<int32_t>(at));
        if (t == "u32") return integer(m_file.read<uint32_t>(at));
        if (t == "i64" || t == "u64" || t == "ulong") return integer(m_file.read<int64_t>(at));
        if (t == "f32" || t == "f16") {
            v.kind = osb::Kind::real;
            if (t == "f32") {
                v.real = m_file.read<float>(at);
            } else { // hkHalf: the top 16 bits of a float
                const uint32_t bits = uint32_t(m_file.read<uint16_t>(at)) << 16;
                std::memcpy(&v.real, &bits, 4);
            }
            return v;
        }
        if (t == "string" || t == "cstring") {
            const uint32_t p = m_file.pointer(at);
            if (p != ~0u) {
                v.kind = osb::Kind::text;
                v.text = std::string(m_file.text(p));
            }
            return v;
        }
        if (const uint32_t n = float_count(t)) {
            v.kind = osb::Kind::floats;
            v.floats.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                v.floats[i] = m_file.read<float>(at + i * 4);
            }
            return v;
        }
        if (t.ends_with('*') || t == "ptr") {
            const uint32_t p = m_file.pointer(at);
            if (p != ~0u) {
                const auto cls = m_file.class_at(p);
                const uint32_t o = cls.empty() ? osb::k_no_object : object(p, cls);
                if (o != osb::k_no_object) {
                    v.kind = osb::Kind::object;
                    v.object = o;
                }
            }
            return v;
        }
        if (t.starts_with("array<")) {
            return array(at, t.substr(6, t.size() - 7));
        }
        if (t.ends_with(']')) { // fixed arrays: T[N]
            const size_t open = t.find('[');
            uint32_t count = 0;
            std::from_chars(t.data() + open + 1, t.data() + t.size() - 1, count);
            const auto element = t.substr(0, open);
            const uint32_t stride = size_of(element);
            v.kind = osb::Kind::list;
            for (uint32_t i = 0; i < count; ++i) {
                v.list.push_back(value(at + i * stride, element));
            }
            return v;
        }
        if (find_class(t)) { // embedded
            v.kind = osb::Kind::object;
            v.object = object(at, t);
            return v;
        }
        return v; // a class the tables leave out
    }

    osb::Value array(uint32_t at, std::string_view element) {
        osb::Value v;
        const uint32_t data = m_file.pointer(at);
        const auto count = m_file.read<int32_t>(at + 8);
        if (data == ~0u || count <= 0) {
            return v;
        }
        const uint32_t stride = size_of(element);
        const auto scalar = element.starts_with("enum:") || element.starts_with("flags:")
                                ? element.substr(element.find(':') + 1)
                                : element;
        if (scalar == "u8" || scalar == "i8" || scalar == "bool") {
            v.kind = osb::Kind::bytes;
            v.bytes.resize(size_t(count));
            for (int32_t i = 0; i < count; ++i) {
                v.bytes[size_t(i)] = m_file.read<uint8_t>(data + uint32_t(i));
            }
            return v;
        }
        if (scalar == "i16" || scalar == "u16" || scalar == "i32" || scalar == "u32") {
            v.kind = osb::Kind::ints;
            v.ints.reserve(size_t(count));
            for (int32_t i = 0; i < count; ++i) {
                const uint32_t e = data + uint32_t(i) * stride;
                v.ints.push_back(scalar == "i16"   ? m_file.read<int16_t>(e)
                                 : scalar == "u16" ? m_file.read<uint16_t>(e)
                                                   : m_file.read<int32_t>(e));
            }
            return v;
        }
        if (scalar == "f32" || float_count(scalar)) {
            v.kind = osb::Kind::floats;
            const uint32_t n = scalar == "f32" ? 1 : float_count(scalar);
            v.floats.reserve(size_t(count) * n);
            for (int32_t i = 0; i < count; ++i) {
                for (uint32_t k = 0; k < n; ++k) {
                    v.floats.push_back(m_file.read<float>(data + uint32_t(i) * stride + k * 4));
                }
            }
            return v;
        }
        v.kind = osb::Kind::list;
        v.list.reserve(size_t(count));
        for (int32_t i = 0; i < count; ++i) {
            v.list.push_back(value(data + uint32_t(i) * stride, element));
        }
        return v;
    }

    const Packfile& m_file;
    osb::Graph m_graph;
    std::map<std::pair<uint32_t, std::string>, uint32_t> m_seen;
};

const osb::Object* follow(const osb::Graph& g, const osb::Value* v) {
    return v && v->kind == osb::Kind::object && v->object < g.objects.size() ? &g.objects[v->object] : nullptr;
}

// The root container's variant of a class.
const osb::Object* variant(const osb::Graph& g, std::string_view cls) {
    const osb::Object& root = g.objects[g.root];
    const auto* variants = root.get("namedVariants");
    if (!variants) {
        return nullptr;
    }
    for (const auto& v : variants->list) {
        const osb::Object* entry = follow(g, &v);
        const osb::Object* target = entry ? follow(g, entry->get("variant")) : nullptr;
        if (target && target->cls == cls) {
            return target;
        }
    }
    return nullptr;
}

int64_t integer(const osb::Object& o, std::string_view name) {
    const auto* v = o.get(name);
    return v ? v->integer : 0;
}
float real(const osb::Object& o, std::string_view name) {
    const auto* v = o.get(name);
    return v ? v->real : 0.0f;
}

osk::Skeleton convert_skeleton(const osb::Graph& g, const osb::Object& skeleton) {
    osk::Skeleton out;
    const auto* parents = skeleton.get("parentIndices");
    const auto* bones = skeleton.get("bones");
    const auto* pose = skeleton.get("referencePose");
    if (!parents || !bones || !pose) {
        throw ParseError("skeleton without bones");
    }
    const size_t n = bones->list.size();
    if (parents->ints.size() != n || pose->floats.size() != n * 12) {
        throw ParseError("skeleton arrays disagree");
    }
    out.bones.resize(n);
    for (size_t i = 0; i < n; ++i) {
        osk::Bone& b = out.bones[i];
        const osb::Object* bone = follow(g, &bones->list[i]);
        if (bone) {
            if (const auto* name = bone->get("name")) {
                b.name = name->text;
            }
            b.lock_translation = integer(*bone, "lockTranslation") != 0;
        }
        b.parent = parents->ints[i];
        const float* qs = pose->floats.data() + i * 12;
        for (int k = 0; k < 3; ++k) {
            b.translation[k] = qs[k] * units::k_skyrim_unit_m;
        }
        const glm::mat3 r = glm::mat3_cast(glm::quat(qs[7], qs[4], qs[5], qs[6]));
        for (int row = 0; row < 3; ++row) {
            for (int c = 0; c < 3; ++c) {
                b.rotation[row][c] = r[c][row];
            }
        }
        b.scale = qs[8];
    }
    return out;
}

osn::Clip convert_clip(const osb::Graph& g, const osb::Object& container) {
    const auto* animations = container.get("animations");
    const auto* bindings = container.get("bindings");
    if (!animations || !bindings || animations->list.empty() || bindings->list.empty()) {
        throw ParseError("clip without an animation");
    }
    const osb::Object* a = follow(g, &animations->list[0]);
    const osb::Object* b = follow(g, &bindings->list[0]);
    const bool uncompressed = a && a->cls == "hkaInterleavedUncompressedAnimation";
    if (!a || !b || (a->cls != "hkaSplineCompressedAnimation" && !uncompressed)) {
        throw ParseError("clip of an unknown kind");
    }
    osn::Clip c;
    c.duration = real(*a, "duration");
    c.transform_tracks = static_cast<uint32_t>(integer(*a, "numberOfTransformTracks"));
    c.float_tracks = static_cast<uint32_t>(integer(*a, "numberOfFloatTracks"));
    c.frames = static_cast<uint32_t>(integer(*a, "numFrames"));
    if (const auto* transforms = uncompressed ? a->get("transforms") : nullptr; transforms && c.transform_tracks) {
        c.raw = transforms->floats;
        c.frames = uint32_t(c.raw.size() / (size_t(c.transform_tracks) * 12));
    }
    c.blocks = static_cast<uint32_t>(integer(*a, "numBlocks"));
    c.max_frames_per_block = static_cast<uint32_t>(integer(*a, "maxFramesPerBlock"));
    c.mask_size = static_cast<uint32_t>(integer(*a, "maskAndQuantizationSize"));
    c.block_duration = real(*a, "blockDuration");
    c.block_inverse_duration = real(*a, "blockInverseDuration");
    c.frame_duration = real(*a, "frameDuration");
    c.additive = integer(*b, "blendHint") == 1;
    auto words = [&](std::string_view name) {
        std::vector<uint32_t> out;
        if (const auto* v = a->get(name)) {
            out.assign(v->ints.begin(), v->ints.end());
        }
        return out;
    };
    c.block_offsets = words("blockOffsets");
    c.float_block_offsets = words("floatBlockOffsets");
    if (const auto* data = a->get("data")) {
        c.data = data->bytes;
    }
    auto shorts = [&](std::string_view name) {
        std::vector<int16_t> out;
        if (const auto* v = b->get(name)) {
            out.assign(v->ints.begin(), v->ints.end());
        }
        return out;
    };
    c.track_bones = shorts("transformTrackToBoneIndices");
    c.float_slots = shorts("floatTrackToFloatSlotIndices");
    if (const auto* tracks = a->get("annotationTracks")) {
        for (const auto& t : tracks->list) {
            const osb::Object* track = follow(g, &t);
            const auto* track_name = track ? track->get("trackName") : nullptr;
            c.track_names.push_back(track_name ? track_name->text : std::string());
            const auto* list = track ? track->get("annotations") : nullptr;
            for (const auto& e : list ? list->list : std::vector<osb::Value>{}) {
                if (const osb::Object* note = follow(g, &e)) {
                    const auto* text = note->get("text");
                    if (text && !text->text.empty()) {
                        c.annotations.push_back({real(*note, "time"), text->text});
                    }
                }
            }
        }
    }
    std::stable_sort(c.annotations.begin(), c.annotations.end(),
                     [](const auto& x, const auto& y) { return x.time < y.time; });
    return c;
}

std::vector<float> floats_of(std::string_view l) {
    std::vector<float> out;
    const char* p = l.data();
    const char* end = p + l.size();
    while (p < end) {
        float v = 0.0f;
        const auto r = std::from_chars(p, end, v);
        if (r.ec != std::errc{}) {
            break;
        }
        out.push_back(v);
        p = r.ptr;
        while (p < end && *p == ' ') {
            ++p;
        }
    }
    return out;
}

int64_t number_of(std::string_view l) {
    int64_t v = 0;
    std::from_chars(l.data(), l.data() + l.size(), v);
    return v;
}

// AnimationDataSingleFile.txt: each project's clip motion, by
// clip generator name, as animations/motion/<project>.osb. The game holds key times as half floats.
size_t import_motion(const legacy::DataSource& data, const std::filesystem::path& cache) {
    const auto bytes = data.read("meshes/animationdatasinglefile.txt");
    if (!bytes) {
        return 0;
    }
    std::vector<std::string_view> lines;
    {
        const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        for (size_t at = 0; at < text.size();) {
            const size_t end = std::min(text.find('\n', at), text.size());
            auto l = text.substr(at, end - at);
            if (!l.empty() && l.back() == '\r') {
                l.remove_suffix(1);
            }
            lines.push_back(l);
            at = end + 1;
        }
    }
    size_t at = 0;
    auto next = [&]() -> std::string_view { return at < lines.size() ? lines[at++] : std::string_view{}; };
    std::vector<std::string> projects;
    for (int64_t n = number_of(next()); n > 0; --n) {
        projects.emplace_back(next());
    }
    size_t written = 0;
    for (const std::string& project : projects) {
        const size_t block_end = at + 1 + size_t(number_of(lines[at]));
        ++at;
        next(); // 1
        for (int64_t n = number_of(next()); n > 0; --n) {
            next(); // files
        }
        const bool has_motion = number_of(next()) != 0;
        std::vector<std::pair<std::string, int64_t>> clips;
        while (at < block_end) {
            std::string name(next());
            const int64_t id = number_of(next());
            next(); // speed
            next(); // crop start
            next(); // crop end
            for (int64_t n = number_of(next()); n > 0; --n) {
                next(); // triggers
            }
            next(); // empty
            clips.emplace_back(std::move(name), id);
        }
        at = block_end;
        if (!has_motion) {
            continue;
        }
        const size_t motion_end = at + 1 + size_t(number_of(lines[at]));
        ++at;
        struct Motion {
            std::vector<float> translation, rotation;
        };
        std::unordered_map<int64_t, Motion> motions;
        while (at < motion_end) {
            const int64_t id = number_of(next());
            next(); // duration
            Motion& m = motions[id];
            for (int64_t n = number_of(next()); n > 0; --n) {
                auto k = floats_of(next());
                if (k.size() >= 4) {
                    k[0] = glm::unpackHalf1x16(glm::packHalf1x16(k[0]));
                    m.translation.insert(m.translation.end(), k.begin(), k.begin() + 4);
                }
            }
            for (int64_t n = number_of(next()); n > 0; --n) {
                auto k = floats_of(next());
                if (k.size() >= 5) {
                    k[0] = glm::unpackHalf1x16(glm::packHalf1x16(k[0]));
                    m.rotation.insert(m.rotation.end(), k.begin(), k.begin() + 5);
                }
            }
            next(); // empty
        }
        at = motion_end;
        osb::Graph g;
        for (const auto& [name, id] : clips) {
            auto it = motions.find(id);
            if (it == motions.end()) {
                continue;
            }
            osb::Object o;
            o.cls = "clip_motion";
            osb::Value text, translation, rotation;
            text.kind = osb::Kind::text;
            text.text = name;
            translation.kind = rotation.kind = osb::Kind::floats;
            translation.floats = it->second.translation;
            rotation.floats = it->second.rotation;
            o.members.emplace_back("name", std::move(text));
            o.members.emplace_back("translation", std::move(translation));
            o.members.emplace_back("rotation", std::move(rotation));
            g.objects.push_back(std::move(o));
        }
        const auto file = path::to_lower(path::replace_extension(project, ".osb"));
        write_file(cache / "animations" / "motion" / file, osb::write(g));
        ++written;
    }
    return written;
}

} // namespace

namespace {

// A Havok binary tagfile, as a few Creation Club files are: classes defined in the
// stream, members by name, so it goes straight to the same graph as a packfile.
class Tagfile {
public:
    explicit Tagfile(std::span<const std::byte> file) : m_file(file), m_at(8) {}

    osb::Graph read() {
        m_strings = {"", ""};
        m_classes.emplace_back();
        uint32_t root = osb::k_no_object;
        while (m_at < m_file.size()) { // version 0 files just stop
            const size_t before = m_at;
            if (next_tag() == k_end) {
                break;
            }
            m_at = before;
            const uint32_t object = read_object();
            root = root == osb::k_no_object ? object : root;
        }
        // Forward references, by the order objects were remembered.
        for (osb::Object& o : m_graph.objects) {
            for (auto& [name, v] : o.members) {
                resolve(v);
            }
        }
        m_graph.root = root;
        return std::move(m_graph);
    }

private:
    enum : int64_t { k_info = 1, k_class = 2, k_object = 3, k_remember = 4, k_back = 5, k_null = 6, k_end = 7 };
    enum : uint32_t { t_byte = 1, t_int = 2, t_real = 3, t_vec4 = 4, t_vec8 = 5, t_vec12 = 6, t_vec16 = 7, t_object = 8,
                      t_struct = 9, t_string = 10, t_array = 0x10, t_tuple = 0x20 };
    struct Type {
        uint32_t flags = 0;
        uint32_t tuple = 0;
        std::string cls;
        uint32_t base() const { return flags & 0xF; }
    };
    struct Class {
        std::string name;
        int64_t parent = 0;
        std::vector<std::pair<std::string, Type>> members;
    };

    uint8_t byte() {
        if (m_at >= m_file.size()) {
            throw ParseError("tagfile ends early");
        }
        return uint8_t(m_file[m_at++]);
    }
    // Sign in the lowest bit, six bits here, then seven to a byte.
    int64_t integer() {
        uint8_t b = byte();
        const bool negative = b & 1;
        int64_t v = (b >> 1) & 0x3F;
        for (int shift = 6; b & 0x80; shift += 7) {
            b = byte();
            v |= int64_t(b & 0x7F) << shift;
        }
        return negative ? -v : v;
    }
    float real() {
        if (m_at + 4 > m_file.size()) {
            throw ParseError("tagfile ends early");
        }
        float v;
        std::memcpy(&v, m_file.data() + m_at, 4);
        m_at += 4;
        return v;
    }
    // A length and the characters, or minus the index of one seen before.
    std::string text() {
        const int64_t n = integer();
        if (n <= 0) {
            if (size_t(-n) >= m_strings.size()) {
                throw ParseError("tagfile string out of range");
            }
            return m_strings[size_t(-n)];
        }
        if (m_at + size_t(n) > m_file.size()) {
            throw ParseError("tagfile ends early");
        }
        std::string s(reinterpret_cast<const char*>(m_file.data() + m_at), size_t(n));
        m_at += size_t(n);
        m_strings.push_back(s);
        return s;
    }
    void read_class() {
        Class c;
        c.name = text();
        integer(); // its version
        c.parent = integer();
        const int64_t members = integer();
        for (int64_t i = 0; i < members; ++i) {
            std::string name = text();
            Type t;
            t.flags = uint32_t(integer());
            if (t.flags & t_tuple) {
                t.tuple = uint32_t(integer());
            }
            if (t.base() == t_object || t.base() == t_struct) {
                t.cls = text();
            }
            c.members.emplace_back(std::move(name), std::move(t));
        }
        m_classes.push_back(std::move(c));
    }
    // Information and classes come between objects; what they leave is the next tag.
    int64_t next_tag() {
        for (;;) {
            const int64_t tag = integer();
            if (tag == k_info) {
                m_version = integer();
            } else if (tag == k_class) {
                read_class();
            } else {
                return tag;
            }
        }
    }
    std::vector<const std::pair<std::string, Type>*> members_of(const Class& c) const {
        std::vector<const std::pair<std::string, Type>*> out;
        if (c.parent > 0) {
            out = members_of(m_classes.at(size_t(c.parent)));
        }
        for (const auto& m : c.members) {
            out.push_back(&m);
        }
        return out;
    }
    const Class& class_named(std::string_view name) const {
        for (const Class& c : m_classes) {
            if (c.name == name) {
                return c;
            }
        }
        throw ParseError(std::format("tagfile has no class {}", name));
    }
    std::vector<bool> present(size_t members) {
        std::vector<bool> out(members);
        const size_t bytes = (members + 7) / 8;
        for (size_t i = 0; i < bytes; ++i) {
            const uint8_t b = byte();
            for (size_t k = 0; k < 8 && i * 8 + k < members; ++k) {
                out[i * 8 + k] = (b >> k) & 1;
            }
        }
        return out;
    }
    uint32_t new_object(std::string cls) {
        m_graph.objects.emplace_back();
        m_graph.objects.back().cls = std::move(cls);
        return uint32_t(m_graph.objects.size() - 1);
    }
    uint32_t read_object() {
        const int64_t tag = next_tag();
        if (tag == k_null) {
            return osb::k_no_object;
        }
        if (tag == k_back) {
            return m_remembered.at(size_t(integer()));
        }
        if (tag != k_object && tag != k_remember) {
            throw ParseError(std::format("tagfile tag {}", tag));
        }
        const Class& c = m_classes.at(size_t(integer()));
        const uint32_t index = new_object(c.name);
        if (tag == k_remember) {
            m_remembered.push_back(index);
        }
        read_members(c, index);
        return index;
    }
    void read_members(const Class& c, uint32_t index) {
        const auto members = members_of(c);
        const std::vector<bool> here = present(members.size());
        std::vector<std::pair<std::string, osb::Value>> out;
        for (size_t i = 0; i < members.size(); ++i) {
            if (here[i]) {
                out.emplace_back(members[i]->first, value(members[i]->second));
            }
        }
        m_graph.objects[index].members = std::move(out); // `objects` moves as members read further objects
    }
    static uint32_t vector_floats(uint32_t base) { return base == t_vec4 ? 4 : base == t_vec8 ? 8 : base == t_vec12 ? 12 : 16; }
    osb::Value object_value() {
        osb::Value v;
        // From version 3 a pointer is the number of an object remembered in order, perhaps later on: 0 for none.
        if (m_version >= 3) {
            const int64_t n = integer();
            v.kind = n ? osb::Kind::object : osb::Kind::none;
            v.integer = n; // `resolve` turns it into an object
            v.object = n ? k_forward : osb::k_no_object;
        } else {
            v.object = read_object();
            v.kind = v.object == osb::k_no_object ? osb::Kind::none : osb::Kind::object;
        }
        return v;
    }
    void resolve(osb::Value& v) {
        if (v.kind == osb::Kind::object && v.object == k_forward) {
            v.object = m_remembered.at(size_t(v.integer - 1));
        }
        for (osb::Value& e : v.list) {
            resolve(e);
        }
    }
    osb::Value struct_value(const std::string& cls) {
        osb::Value v;
        v.kind = osb::Kind::object;
        const Class& c = class_named(cls);
        v.object = new_object(c.name);
        read_members(c, v.object);
        return v;
    }
    osb::Value one(const Type& t) {
        osb::Value v;
        switch (t.base()) {
        case t_byte:
            v.kind = osb::Kind::integer;
            v.integer = byte();
            break;
        case t_int:
            v.kind = osb::Kind::integer;
            v.integer = integer();
            break;
        case t_real:
            v.kind = osb::Kind::real;
            v.real = real();
            break;
        case t_vec4:
        case t_vec8:
        case t_vec12:
        case t_vec16:
            v.kind = osb::Kind::floats;
            for (uint32_t i = 0; i < vector_floats(t.base()); ++i) {
                v.floats.push_back(real());
            }
            break;
        case t_string:
            v.kind = osb::Kind::text;
            v.text = text();
            break;
        case t_object:
            return object_value();
        case t_struct:
            return struct_value(t.cls);
        default:
            throw ParseError(std::format("tagfile type {}", t.flags));
        }
        return v;
    }
    // Arrays of structs go by column: which members are there, then each member for every row.
    osb::Value rows(const Type& t, size_t count) {
        osb::Value v;
        v.kind = osb::Kind::list;
        const Class& c = class_named(t.cls);
        const auto members = members_of(c);
        const std::vector<bool> here = present(members.size());
        for (size_t r = 0; r < count; ++r) {
            osb::Value row;
            row.kind = osb::Kind::object;
            row.object = new_object(c.name);
            v.list.push_back(std::move(row));
        }
        for (size_t i = 0; i < members.size(); ++i) {
            if (!here[i]) {
                continue;
            }
            const Type& m = members[i]->second;
            for (size_t r = 0; r < count; ++r) {
                osb::Value cell = value(m);
                m_graph.objects[v.list[r].object].members.emplace_back(members[i]->first, std::move(cell));
            }
        }
        return v;
    }
    osb::Value array(const Type& t, size_t count) {
        osb::Value v;
        Type element = t;
        element.flags &= ~uint32_t(t_array);
        switch (t.base()) {
        case t_struct:
            return rows(t, count);
        case t_byte:
            v.kind = osb::Kind::bytes;
            for (size_t i = 0; i < count; ++i) {
                v.bytes.push_back(byte());
            }
            break;
        case t_int:
            v.kind = osb::Kind::ints;
            for (size_t i = 0; i < count; ++i) {
                v.ints.push_back(int32_t(integer()));
            }
            break;
        case t_real:
        case t_vec4:
        case t_vec8:
        case t_vec12:
        case t_vec16:
            v.kind = osb::Kind::floats;
            for (size_t i = 0; i < count; ++i) {
                osb::Value e = one(element);
                if (e.kind == osb::Kind::real) {
                    v.floats.push_back(e.real);
                } else {
                    v.floats.insert(v.floats.end(), e.floats.begin(), e.floats.end());
                }
            }
            break;
        default:
            v.kind = osb::Kind::list;
            for (size_t i = 0; i < count; ++i) {
                v.list.push_back(one(element));
            }
            break;
        }
        return v;
    }
    osb::Value value(const Type& t) {
        if (t.flags & t_array) {
            return array(t, size_t(integer()));
        }
        if (t.flags & t_tuple) {
            return array(t, t.tuple);
        }
        return one(t);
    }

    static constexpr uint32_t k_forward = osb::k_no_object - 1;
    std::span<const std::byte> m_file;
    size_t m_at;
    int64_t m_version = 0;
    std::vector<std::string> m_strings;
    std::vector<Class> m_classes;
    std::vector<uint32_t> m_remembered;
    osb::Graph m_graph;
};

bool is_tagfile(std::span<const std::byte> file) {
    constexpr uint8_t k_magic[8] = {0x1E, 0x0D, 0xB0, 0xCA, 0xCE, 0xFA, 0x11, 0xD0};
    return file.size() > 8 && std::memcmp(file.data(), k_magic, 8) == 0;
}

} // namespace

osb::Graph read_hkx(std::span<const std::byte> file) {
    if (is_tagfile(file)) {
        return Tagfile(file).read();
    }
    Packfile pack(file);
    GraphReader reader(pack);
    const uint32_t root = reader.object(0, "hkRootLevelContainer");
    return reader.take(root);
}

namespace {

osb::Value* member(osb::Graph& g, const osb::Value* at, std::string_view name) {
    if (!at || at->kind != osb::Kind::object || at->object >= g.objects.size()) {
        return nullptr;
    }
    for (auto& [n, v] : g.objects[at->object].members) {
        if (n == name) {
            return &v;
        }
    }
    return nullptr;
}

void set_real(osb::Value* v, float value) {
    if (v) {
        v->kind = osb::Kind::real;
        v->real = value;
    }
}

// After the joints, which go by the .hkx's names: a body is known by its NIF node from here on.
void name_bodies_by_node(osb::Graph& g, const std::vector<std::pair<osb::Value, std::string>>& names) {
    for (const auto& [entry, node] : names) {
        if (osb::Value* name = member(g, &entry, "name")) {
            name->text = node;
        }
    }
}

// A character's bodies and joints are the skeleton NIF's: the game swaps them in for the .hkx's, by name, when it binds a
// graph to its model. The NIF's lengths are Havok units, the.hkx's game units.
void apply_nif_physics(osb::Graph& g, const legacy::NifFile& nif) {
    std::unordered_map<std::string, const legacy::bhkRigidBody*> bodies;
    for (const auto& block : nif.blocks) {
        const auto* node = std::get_if<legacy::NiNode>(&block.data);
        const auto* collision = node ? nif.get<legacy::bhkCollisionObject>(node->collision) : nullptr;
        const auto* body = collision ? nif.get<legacy::bhkRigidBody>(collision->body) : nullptr;
        if (body) {
            bodies.emplace(path::to_lower(node->name), body);
        }
    }
    const osb::Object* instance = variant(g, "hkaRagdollInstance");
    const osb::Value* list = instance->get("rigidBodies");
    const osb::Value* joints = instance->get("constraints");
    if (!list) {
        return;
    }
    // By the animation bone its ragdoll bone maps to, else its own name without the prefix.
    const osb::Value* ragdoll_skeleton = instance->get("skeleton");
    std::vector<std::string> mapped(list->list.size());
    const osb::Value* bone_to_body = instance->get("boneToRigidBodyMap");
    for (const auto& data : g.objects) {
        const osb::Value* skeleton_a = data.get("skeletonA");
        const osb::Value* skeleton_b = data.get("skeletonB");
        const osb::Value* simple = data.get("simpleMappings");
        if (data.cls != "hkaSkeletonMapperData" || !skeleton_a || !skeleton_b || !simple || !ragdoll_skeleton || !bone_to_body ||
            skeleton_b->object != ragdoll_skeleton->object) {
            continue;
        }
        const osb::Object* animation = follow(g, skeleton_a);
        const osb::Value* bones = animation ? animation->get("bones") : nullptr;
        for (const osb::Value& m : simple->list) {
            const osb::Object* mapping = follow(g, &m);
            const size_t bone_a = size_t(mapping->get("boneA")->integer), bone_b = size_t(mapping->get("boneB")->integer);
            if (bones && bone_a < bones->list.size() && bone_b < bone_to_body->ints.size()) {
                const size_t body = size_t(bone_to_body->ints[bone_b]);
                const osb::Object* bone = follow(g, &bones->list[bone_a]);
                if (body < mapped.size() && bone) {
                    mapped[body] = path::to_lower(bone->get("name")->text);
                }
            }
        }
    }
    constexpr std::string_view prefix = "ragdoll_";
    constexpr float units = 69.99125f; // per Havok unit: 1 / 0.0142875
    const std::vector<osb::Value> entries = list->list;
    std::vector<std::pair<osb::Value, std::string>> renamed;
    for (size_t index = 0; index < entries.size(); ++index) {
        const osb::Value& entry = entries[index];
        const osb::Value* name = member(g, &entry, "name");
        const std::string own = name ? path::to_lower(name->text) : std::string();
        auto it = bodies.find(mapped[index]);
        if (it == bodies.end() && own.starts_with(prefix)) {
            it = bodies.find(own.substr(prefix.size()));
        }
        if (it == bodies.end()) {
            log::warn("ragdoll body {} has no NIF body", own);
            continue;
        }
        renamed.emplace_back(entry, it->first);
        const legacy::bhkRigidBody& b = *it->second;
        osb::Value* collidable = member(g, &entry, "collidable");
        if (const auto* capsule = nif.get<legacy::bhkCapsuleShape>(b.shape)) {
            const osb::Value* shape = member(g, collidable, "shape");
            set_real(member(g, shape, "radius"), capsule->radius * units);
            for (const auto& [field, point] : {std::pair{"vertexA", capsule->a}, std::pair{"vertexB", capsule->b}}) {
                if (osb::Value* v = member(g, shape, field); v && v->floats.size() >= 3) {
                    v->floats[0] = point.x * units;
                    v->floats[1] = point.y * units;
                    v->floats[2] = point.z * units;
                }
            }
        }
        if (osb::Value* filter = member(g, member(g, collidable, "broadPhaseHandle"), "collisionFilterInfo")) {
            filter->integer = b.filter.layer | uint32_t(b.filter.flags_and_part) << 8;
        }
        const osb::Value* material = member(g, &entry, "material");
        set_real(member(g, material, "friction"), b.friction);
        set_real(member(g, material, "restitution"), b.restitution);
        const osb::Value* motion = member(g, &entry, "motion");
        if (osb::Value* i = member(g, motion, "inertiaAndMassInv"); i && i->floats.size() >= 4) {
            for (int axis = 0; axis < 3; ++axis) {
                i->floats[size_t(axis)] = b.inertia[axis] > 0.0f ? 1.0f / (b.inertia[axis] * units * units) : 0.0f;
            }
            i->floats[3] = b.mass > 0.0f ? 1.0f / b.mass : 0.0f;
        }
        const osb::Value* state = member(g, motion, "motionState");
        set_real(member(g, state, "linearDamping"), b.linear_damping);
        set_real(member(g, state, "angularDamping"), b.angular_damping);
        set_real(member(g, state, "maxLinearVelocity"), b.max_linear_velocity);
        set_real(member(g, state, "maxAngularVelocity"), b.max_angular_velocity);

        // The joint a body owns ties it to its parent, as the .hkx's constraint of the same name does.
        const legacy::bhkJoint* joint = nullptr;
        for (legacy::NifRef ref : b.constraints) {
            joint = joint ? joint : nif.get<legacy::bhkJoint>(ref);
        }
        if (!joint || !joints) {
            continue;
        }
        for (const osb::Value& c : joints->list) {
            const osb::Value* cname = member(g, &c, "name");
            if (!cname || path::to_lower(cname->text) != own) {
                continue;
            }
            const osb::Value* atoms = member(g, member(g, &c, "data"), "atoms");
            const auto limit = [&](const char* atom, float low, float high) {
                const osb::Value* a = member(g, atoms, atom);
                set_real(member(g, a, "minAngle"), low);
                set_real(member(g, a, "maxAngle"), high);
            };
            if (joint->hinge) {
                limit("angLimit", joint->hinge_min, joint->hinge_max);
            } else {
                limit("twistLimit", joint->twist_min, joint->twist_max);
                limit("planesLimit", joint->plane_min, joint->plane_max);
                set_real(member(g, member(g, atoms, "coneLimit"), "maxAngle"), joint->cone_max);
            }
            set_real(member(g, member(g, atoms, "angFriction"), "maxFrictionTorque"), joint->max_friction);
        }
    }
    name_bodies_by_node(g, renamed);
}

} // namespace

HkxImport import_hkx(const legacy::DataSource& data, AssetCache& assets, const std::filesystem::path& cache,
                     unsigned threads, bool force) {
    const auto files = data.list("meshes/", ".hkx");
    log::info("converting {} behaviour files", files.size());
    std::atomic<size_t> next{0}, graphs{0}, skeletons{0}, clips{0}, failed{0};
    auto work = [&] {
        for (size_t i; (i = next++) < files.size();) {
            const std::string& src = files[i];
            const auto stem = path::replace_extension("animations/" + src, "");
            try {
                // What a file holds shows only inside it, so any of the three outputs may be its own.
                bool fresh = false;
                for (const char* ext : {"osb", "osk", "osn"}) {
                    const std::string out = stem + "." + ext;
                    if (!force && assets.fresh(out, src, k_hkx_converter_version)) {
                        fresh = true;
                        (ext[2] == 'n' ? clips : ext[2] == 'k' ? skeletons : graphs)++;
                    }
                }
                if (fresh) {
                    continue;
                }
                const auto bytes = data.read(src);
                if (!bytes) {
                    continue;
                }
                const osb::Graph g = read_hkx(*bytes);
                std::string out;
                if (const osb::Object* container = variant(g, "hkaAnimationContainer")) {
                    const auto* skeleton_list = container->get("skeletons");
                    const auto* animation_list = container->get("animations");
                    // A tagfile clip brings its skeleton along; what it's for is the clip.
                    const bool clip = animation_list && !animation_list->list.empty();
                    if (!clip && skeleton_list && !skeleton_list->list.empty()) {
                        const osb::Object* s = follow(g, &skeleton_list->list[0]);
                        if (!s) {
                            continue;
                        }
                        out = stem + ".osk";
                        write_file(cache / out, osk::write(convert_skeleton(g, *s)));
                        ++skeletons;
                        // Everything about a ragdoll stays as objects beside it, for the engine's physics to build from.
                        if (variant(g, "hkaRagdollInstance")) {
                            osb::Graph ragdoll = g;
                            if (const auto nif = data.read(path::replace_extension(src, ".nif"))) {
                                apply_nif_physics(ragdoll, legacy::read_nif(*nif));
                            } else {
                                log::warn("{}: no skeleton NIF for its ragdoll", src);
                            }
                            write_file(cache / (stem + ".ragdoll.osb"), osb::write(ragdoll));
                        }
                    } else if (clip) {
                        out = stem + ".osn";
                        write_file(cache / out, osn::write(convert_clip(g, *container)));
                        ++clips;
                    } else {
                        continue; // nothing bound
                    }
                } else {
                    out = stem + ".osb";
                    write_file(cache / out, osb::write(g));
                    ++graphs;
                }
                assets.record(out, src, k_hkx_converter_version);
            } catch (const std::exception& e) {
                log::warn("{}: {}", src, e.what());
                ++failed;
            }
        }
    };
    {
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back(work);
        }
    }
    const size_t motion = import_motion(data, cache);
    log::info("clip motion for {} projects", motion);
    return {graphs, skeletons, clips, failed};
}

} // namespace spud::importer
