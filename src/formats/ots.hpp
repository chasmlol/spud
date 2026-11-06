#pragma once

// .ots — spud texture swaps: what a base object's alternate textures (MODS) put on the shapes of its model
// (rules/texture_swaps.ots in the cache). Spec: docs/formats/ots.md

#include "formats/container.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::ots {

inline constexpr container::FourCC k_kind = container::fourcc("TSWP");
inline constexpr uint32_t k_version = 2;

// A shape of the model and its new texture set, in osm::TextureSlot order; "" keeps the model's.
struct Shape {
    uint32_t submesh = 0; // of the model's .osm
    std::string name;     // the shape's: a skinned model goes by this
    std::array<std::string, 8> textures;
};

// Which of a record's models: an object's one, or an armor addon's four.
enum Variant : uint32_t { variant_model = 0, variant_male = 1, variant_female = 2, variant_male_first = 3, variant_female_first = 4 };

struct Swap {
    uint32_t base_form_id = 0;
    uint32_t variant = variant_model;
    std::vector<Shape> shapes;
};

struct File {
    std::vector<Swap> swaps;
};

std::vector<std::byte> write(const File& file);
File read(std::span<const std::byte> bytes);

} // namespace spud::ots
