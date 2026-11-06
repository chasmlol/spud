#pragma once

// .oda — spud node animations: how the moving parts of a model move, as rigid poses in mesh space
// (rules/doors.oda in the cache). Spec: docs/formats/oda.md
//
// For now, doors: each DOOR's Open and Close sequences, sampled, with its sounds.

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::oda {

inline constexpr container::FourCC k_kind = container::fourcc("NANM");
inline constexpr uint32_t k_version = 2;
inline constexpr float k_sample_rate = 60.0f; // samples a second

// A rigid pose in mesh space, identity at the shape's place in the file: mesh -> mesh.
struct Pose {
    float rotation[4] = {0, 0, 0, 1}; // quaternion xyzw
    float translation[3] = {0, 0, 0}; // metres
};

// Submeshes that move together.
struct Part {
    std::vector<uint32_t> submeshes;
    std::vector<Pose> open;  // from the Open sequence's start to its end
    std::vector<Pose> close; // ... the Close sequence's
    float lo[3] = {0, 0, 0}; // its box in mesh space where it stands in the file, metres
    float hi[3] = {0, 0, 0};
};

struct Door {
    uint32_t base_form_id = 0;     // the DOOR
    std::string mesh;              // its .osm, cache path
    uint32_t open_sound = 0;       // SNAM, a SNDR; 0 none
    uint32_t close_sound = 0;      // ANAM
    std::vector<Part> parts;
    std::vector<uint32_t> still;   // submeshes that stay where they are: the frame
    std::vector<uint32_t> backing; // what stands behind a shut door, hidden while it is open
    bool has_back = false;         // its moving parts are modelled on both faces, not only the one the game shows
    bool hung = false;             // spud's swing for a plain door with no sequences: load doors only
};

struct File {
    std::vector<Door> doors;
};

std::vector<std::byte> write(const File& file);
File read(std::span<const std::byte> bytes);

} // namespace spud::oda
