#pragma once

// NIF -> .osp: Havok collision (bhkCollisionObject trees) flattened into spud physics shapes in mesh
// space, metres. Triangle soups (compressed mesh shapes, LE packed strips) are welded and split by
// collision layer and material; primitives keep their exact form. Phantoms and ragdoll bodies are skipped.

#include "formats/osp.hpp"
#include "importer/legacy/nif.hpp"

#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>

namespace spud::importer {

struct CollisionImportResult {
    osp::Collision collision; // no shapes if the NIF has no solid collision
    std::vector<std::string> warnings;
};

// Safe to call from many threads at once.
// `nodes` (name to .osk index) makes an animated object's collision: each body in the space of the node it rides on.
CollisionImportResult import_collision(const legacy::NifFile& nif, std::string_view source_path, int damage_stage = 0,
                                       const std::unordered_map<std::string, uint32_t>* nodes = nullptr);

} // namespace spud::importer
