#pragma once

// .ofm — spud face morphs. Spec: docs/formats/ofm.md
//
// The morphs of one head part, converted from a FaceGen .tri: a position delta per vertex of the shape they go with,
// in that shape's vertex order. Heads share them: one file serves every NPC whose head part names it.

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::ofm {

inline constexpr container::FourCC k_kind = container::fourcc("FMOR");
inline constexpr uint32_t k_version = 1;

struct MorphSet {
    uint32_t vertex_count = 0;
    std::vector<std::string> names;
    std::vector<float> scales;   // per morph: metres per delta unit
    std::vector<int16_t> deltas; // morph-major: x y z 0 per vertex, as the GPU reads them
};

std::vector<std::byte> write(const MorphSet& set);
MorphSet read(std::span<const std::byte> file);

} // namespace spud::ofm
