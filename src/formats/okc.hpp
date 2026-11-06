#pragma once

// .okc — kill camera models: each `Cameras\*.nif` a CAMS record names, as its root's animated transform sampled at a fixed
// step and its NiCamera's frustum (rules/cameras.okc in the cache). Spec: docs/formats/okc.md.

#include "formats/container.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::okc {

inline constexpr container::FourCC k_kind = container::fourcc("KCAM");
inline constexpr uint32_t k_version = 1;
inline constexpr float k_step = 1.0f / 30.0f; // seconds between frames

struct Frame {
    std::array<float, 3> translation{}; // Skyrim units
    std::array<float, 3> angles{};      // radians, x y z; zero with no rotation keys
    float right = 0;                    // the frustum's right as it animates; 0: Model::frustum_right
};

struct Model {
    std::string path;        // lower case, under meshes, forward slashes: "cameras/base.nif"
    float frustum_right = 0; // the NiCamera's; 4:3, so the vertical half tangent is this x 0.75
    bool rotates = false;    // has rotation keys
    std::vector<Frame> frames;
};

struct Cameras {
    std::vector<Model> models;
};

std::vector<std::byte> write(const Cameras& cameras);
Cameras read(std::span<const std::byte> file);

} // namespace spud::okc
