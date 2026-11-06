#include "formats/osk.hpp"

#include <cstring>

namespace spud::osk {

namespace {

constexpr auto k_bone = container::fourcc("BONE");
constexpr auto k_strs = container::fourcc("STRS");
constexpr auto k_proj = container::fourcc("PROJ");

struct BoneRecord {
    uint32_t name;
    int32_t parent;
    float rotation[3][3];
    float translation[3];
    float scale;
    uint32_t flags; // 1: lock translation
};
static_assert(sizeof(BoneRecord) == 64);

} // namespace

std::vector<std::byte> write(const Skeleton& skeleton) {
    container::StringTableBuilder strings;
    std::vector<BoneRecord> bones;
    bones.reserve(skeleton.bones.size());
    for (const auto& b : skeleton.bones) {
        BoneRecord r{};
        r.name = strings.add(b.name);
        r.parent = b.parent;
        std::memcpy(r.rotation, b.rotation, sizeof r.rotation);
        std::memcpy(r.translation, b.translation, sizeof r.translation);
        r.scale = b.scale;
        r.flags = b.lock_translation ? 1 : 0;
        bones.push_back(r);
    }
    container::Writer w(k_kind, k_version);
    w.add_array(k_bone, bones);
    w.add_struct(k_proj, strings.add(skeleton.project));
    w.add(k_strs, strings.bytes());
    return w.finish();
}

Skeleton read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() != k_version) {
        throw ParseError("unsupported .osk version " + std::to_string(r.version()));
    }
    container::StringTable strings(r.require(k_strs));
    Skeleton s;
    s.project = std::string(strings.get(r.read_struct<uint32_t>(k_proj)));
    for (const auto& rec : r.read_array<BoneRecord>(k_bone)) {
        if (rec.parent >= static_cast<int32_t>(s.bones.size())) {
            throw ParseError("skeleton bone before its parent");
        }
        Bone b;
        b.name = std::string(strings.get(rec.name));
        b.parent = rec.parent;
        std::memcpy(b.rotation, rec.rotation, sizeof b.rotation);
        std::memcpy(b.translation, rec.translation, sizeof b.translation);
        b.scale = rec.scale;
        b.lock_translation = rec.flags & 1;
        s.bones.push_back(std::move(b));
    }
    return s;
}

} // namespace spud::osk
