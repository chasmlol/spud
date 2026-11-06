#pragma once

// The Graphics page of the Journal's Settings (docs/UI.md): the game's Detail presets with the values of its Low.ini,
// Medium.ini, High.ini and Ultra.ini, and steps above Ultra where spud can go further. No temporal antialiasing or
// upscaling of any kind: the game's TAA row is MSAA here.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace spud::engine {

struct GraphicsRow {
    int id;                         // the option's ID in the movie's list
    const char* key;                // its setting: the step's index
    const char* label;
    std::vector<const char*> steps; // empty: a checkbox
    std::array<int, 4> preset;      // the step at Low, Medium, High, Ultra
};

inline constexpr int k_graphics_detail_id = 100; // the preset row; every other row follows it
inline constexpr int k_graphics_custom = 4;      // its step when the rows match no preset

const std::vector<GraphicsRow>& graphics_rows();
const GraphicsRow* graphics_row(int id);

// What each row's step means.
inline constexpr std::array<uint32_t, 4> k_msaa_steps{1, 2, 4, 8};
inline constexpr std::array<uint32_t, 4> k_shadow_resolutions{1024, 2048, 4096, 8192}; // iShadowMapResolution
inline constexpr std::array<float, 5> k_shadow_distances{3000, 8000, 10000, 16000, 24000}; // fShadowDistance
struct ObjectDetail {
    float objects, items, actors; // fLODFadeOutMultObjects, Items, Actors
};
inline constexpr std::array<ObjectDetail, 5> k_object_detail{
    {{5, 1.5f, 5}, {7, 3, 7}, {9, 6, 9}, {30, 10, 15}, {45, 15, 25}}};
struct GrassDetail {
    float fade_start; // fGrassStartFadeDistance
    int cell_radius;  // iGrassCellRadius
};
inline constexpr std::array<GrassDetail, 4> k_grass_detail{{{1000, 2}, {2300, 2}, {7000, 2}, {10000, 3}}};
inline constexpr std::array<float, 6> k_distant_detail{6, 8, 10, 12, 16, 20}; // cells: uLargeRefLODGridSize + 1, then ours
inline constexpr std::array<size_t, 3> k_decal_steps{0, 100, 1000};           // uMaxDecals

} // namespace spud::engine
