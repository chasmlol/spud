#pragma once

// .osh — spud far view of one worldspace. Spec: docs/formats/osh.md
//
// What the importer generates from the full-detail data for drawing the world beyond the loaded cells: the land's
// macro colour and normal maps, tree impostors and every tree's placement, and the merged proxy meshes of each
// region. docs/ARCHITECTURE.md, "LOD".

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::osh {

inline constexpr container::FourCC k_kind = container::fourcc("FARV");
inline constexpr uint32_t k_version = 1;
inline constexpr uint32_t k_none = 0xFFFFFFFFu;

// A tree model's octahedral impostor: frames x frames views over the upper hemisphere.
struct TreeImpostor {
    std::string colour; // RGB albedo, A coverage
    std::string normal; // RGB model-space normal
    uint32_t frames = 0;
    float centre[3] = {0, 0, 0}; // the model's bound centre, mesh space (metres)
    float radius = 0;            // half the side of every frame (metres)
    float lateral = 1;           // how far the tree reaches from its vertical axis, over radius
};

// A placed tree, sorted by cell: drawn as an impostor wherever its cell's full mesh isn't.
struct TreeInstance {
    float position[3];
    float scale;
    float rotation[4]; // quaternion xyzw
    uint32_t impostor; // index into FarView::impostors
    uint32_t form_id;
};
static_assert(sizeof(TreeInstance) == 40);

// The trees of one cell: FarView::trees[first, first + count).
struct TreeCell {
    int32_t x;
    int32_t y;
    uint32_t first;
    uint32_t count;
};
static_assert(sizeof(TreeCell) == 16);

// One merged mesh of a region: its objects that share a directional material (the reference's, as the engine applies
// it to a whole mesh).
struct Proxy {
    uint32_t mesh;        // index into FarView::meshes
    uint32_t directional; // index into World::directional_materials, or k_none
};
static_assert(sizeof(Proxy) == 8);

// A square of cells whose large static objects are merged and simplified. Level 1 regions
// are 4 x 4 cells and stand for those cells' references; level 2 regions are 8 x 8 and stand for their children.
struct Region {
    int32_t x; // cell of the south-west corner
    int32_t y;
    uint32_t level;
    float error;          // metres: how far the proxies stray from the full-detail shape
    float lo[3];          // bounds (absolute metres)
    float hi[3];
    uint32_t parent;      // index into FarView::regions, or k_none
    uint32_t first_proxy; // FarView::proxies[first_proxy, + proxy_count)
    uint32_t proxy_count;
    float centre[3];      // absolute metres: the proxies' origin
};
static_assert(sizeof(Region) == 64);

struct FarView {
    int32_t cell_min[2] = {0, 0}; // inclusive land bounds the maps cover
    int32_t cell_max[2] = {0, 0};
    uint32_t colour_per_cell = 0; // texels per cell edge
    uint32_t normal_per_cell = 0;
    std::string colour; // macro maps: RGB albedo x vertex colour; RG world normal xy
    std::string normal;
    std::vector<std::string> meshes;
    std::vector<TreeImpostor> impostors;
    std::vector<TreeInstance> trees;
    std::vector<TreeCell> tree_cells; // sorted by (y, x)
    std::vector<Region> regions; // level 1, then level 2
    std::vector<Proxy> proxies;
};

std::vector<std::byte> write(const FarView& view);
FarView read(std::span<const std::byte> file);

} // namespace spud::osh
