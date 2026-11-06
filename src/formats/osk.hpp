#pragma once

// .osk: spud skeleton. Spec: docs/formats/osk.md
//
// The node tree of a skeleton model in its rest pose: what skinned models (osm::Skin) name their bones in and
// what equipment attaches to.

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::osk {

inline constexpr container::FourCC k_kind = container::fourcc("SKEL");
inline constexpr uint32_t k_version = 2;

struct Bone {
    std::string name;
    int32_t parent = -1; // earlier in the list; -1 for the root
    // Rows; parent = rotation * (scale * local) + translation.
    float rotation[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float translation[3] = {0, 0, 0}; // metres
    float scale = 1.0f;
    bool lock_translation = false; // an animation skeleton's bone that takes no translation from clips
};

struct Skeleton {
    std::vector<Bone> bones; // parents first
    std::string project;     // the behaviour project a model skeleton's actors use, a cache path without extension
};

std::vector<std::byte> write(const Skeleton& skeleton);
Skeleton read(std::span<const std::byte> file);

} // namespace spud::osk
