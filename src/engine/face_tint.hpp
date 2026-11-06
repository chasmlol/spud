#pragma once

// A face's tint map from its layers, as the game renders it.

#include "game/face.hpp"

#include <cstddef>
#include <filesystem>
#include <glm/glm.hpp>
#include <span>
#include <vector>

namespace spud::engine {

// A DDS file in memory, for TextureManager::provide. Empty when no layer shows. Worker threads.
std::vector<std::byte> compose_face_tint(const std::filesystem::path& cache, std::span<const game::TintLayer> layers);
// One colour all over: a head with no layers to build from has its skin tone, as shipped FaceTint files have under theirs.
std::vector<std::byte> plain_face_tint(const glm::vec3& colour);

} // namespace spud::engine
