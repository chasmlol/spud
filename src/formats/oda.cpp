#include "formats/oda.hpp"

#include "core/binary.hpp"

#include <cstring>

namespace spud::oda {

namespace {

constexpr auto k_door = container::fourcc("DOOR");
constexpr auto k_part = container::fourcc("PART");
constexpr auto k_subs = container::fourcc("SUBS");
constexpr auto k_pose = container::fourcc("POSE");
constexpr auto k_strs = container::fourcc("STRS");

struct DoorRecord {
    uint32_t base_form_id;
    uint32_t mesh; // string offset
    uint32_t open_sound;
    uint32_t close_sound;
    uint32_t first_part;
    uint32_t part_count;
    uint32_t first_still; // into SUBS
    uint32_t still_count;
    uint32_t first_backing;
    uint32_t backing_count;
    uint32_t flags; // 1: has_back, 2: hung
};

struct PartRecord {
    uint32_t first_submesh; // into SUBS
    uint32_t submesh_count;
    uint32_t first_open; // into POSE
    uint32_t open_count;
    uint32_t first_close;
    uint32_t close_count;
    float lo[3];
    float hi[3];
};

} // namespace

std::vector<std::byte> write(const File& file) {
    container::StringTableBuilder strings;
    std::vector<DoorRecord> doors;
    std::vector<PartRecord> parts;
    std::vector<uint32_t> subs;
    std::vector<Pose> poses;
    for (const Door& d : file.doors) {
        DoorRecord r{d.base_form_id, strings.add(d.mesh), d.open_sound, d.close_sound, uint32_t(parts.size()),
                     uint32_t(d.parts.size()), uint32_t(subs.size()), uint32_t(d.still.size()),
                     uint32_t(subs.size() + d.still.size()), uint32_t(d.backing.size()),
                     (d.has_back ? 1u : 0u) | (d.hung ? 2u : 0u)};
        subs.insert(subs.end(), d.still.begin(), d.still.end());
        subs.insert(subs.end(), d.backing.begin(), d.backing.end());
        for (const Part& p : d.parts) {
            PartRecord q{uint32_t(subs.size()), uint32_t(p.submeshes.size()), uint32_t(poses.size()),
                         uint32_t(p.open.size()), 0, uint32_t(p.close.size()), {}, {}};
            std::memcpy(q.lo, p.lo, sizeof(q.lo));
            std::memcpy(q.hi, p.hi, sizeof(q.hi));
            subs.insert(subs.end(), p.submeshes.begin(), p.submeshes.end());
            poses.insert(poses.end(), p.open.begin(), p.open.end());
            q.first_close = uint32_t(poses.size());
            poses.insert(poses.end(), p.close.begin(), p.close.end());
            parts.push_back(q);
        }
        doors.push_back(r);
    }
    container::Writer w(k_kind, k_version);
    w.add_array(k_door, doors);
    w.add_array(k_part, parts);
    w.add_array(k_subs, subs);
    w.add_array(k_pose, poses);
    w.add(k_strs, strings.bytes());
    return w.finish();
}

File read(std::span<const std::byte> bytes) {
    container::Reader r(bytes, k_kind);
    container::StringTable strings(r.require(k_strs));
    const auto doors = r.read_array<DoorRecord>(k_door);
    const auto parts = r.read_array<PartRecord>(k_part);
    const auto subs = r.read_array<uint32_t>(k_subs);
    const auto poses = r.read_array<Pose>(k_pose);
    File f;
    for (const DoorRecord& d : doors) {
        Door out;
        out.base_form_id = d.base_form_id;
        out.mesh = std::string(strings.get(d.mesh));
        out.open_sound = d.open_sound;
        out.close_sound = d.close_sound;
        out.has_back = (d.flags & 1) != 0;
        out.hung = (d.flags & 2) != 0;
        out.still.assign(subs.begin() + d.first_still, subs.begin() + d.first_still + d.still_count);
        out.backing.assign(subs.begin() + d.first_backing, subs.begin() + d.first_backing + d.backing_count);
        for (uint32_t i = 0; i < d.part_count; ++i) {
            const PartRecord& p = parts[d.first_part + i];
            Part part;
            part.submeshes.assign(subs.begin() + p.first_submesh, subs.begin() + p.first_submesh + p.submesh_count);
            part.open.assign(poses.begin() + p.first_open, poses.begin() + p.first_open + p.open_count);
            part.close.assign(poses.begin() + p.first_close, poses.begin() + p.first_close + p.close_count);
            std::memcpy(part.lo, p.lo, sizeof(part.lo));
            std::memcpy(part.hi, p.hi, sizeof(part.hi));
            out.parts.push_back(std::move(part));
        }
        f.doors.push_back(std::move(out));
    }
    return f;
}

} // namespace spud::oda
