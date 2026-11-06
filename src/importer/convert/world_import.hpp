#pragma once

// Worldspace import: one WRLD of the load order (as of game start) -> osw::World.

#include "formats/osw.hpp"
#include "importer/legacy/esm.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::importer {

struct WorldImport {
    osw::World world;
    // Parallel to world.meshes: normalised source NIF path ("meshes/x/y.nif").
    std::vector<std::string> source_meshes;
    // Unique normalised texture paths used by land_textures + default_land_texture.
    std::vector<std::string> textures;

    // Diagnostics: references not exported, by reason ("actor", "marker", "base type SOUN", ...).
    std::map<std::string, size_t> skipped;
    // Merged cities: the parent's references kept in cells a child world shares, by base FormID.
    std::map<uint32_t, size_t> merge_kept;
    size_t skipped_total() const;
};

std::vector<std::string> list_worldspaces(const legacy::EsmDatabase& db); // EditorIDs with >= 1 exterior cell

// A child worldspace that is a city (its location has the LocTypeCity keyword) and shares its parent's coordinates
// (a load door between the two stands within k_shared_frame_units of the door it leads to), merged into the parent
// at import.
struct MergedChild {
    std::string edid;
    uint32_t form_id;
};
inline constexpr float k_shared_frame_units = 4096.0f;
std::vector<MergedChild> merged_children(const legacy::EsmDatabase& db, std::string_view parent_edid);

// Where a light hangs in its LIGH's model (normalised "meshes/..." path): the AttachLight node's offset from the
// model root, in game units. nullopt when the model can't be read (the light then sits at the reference origin).
using LightAttachFn = std::function<std::optional<std::array<float, 3>>(const std::string& model)>;

// Whether a model's root is a leaf-animation node (BSLeafAnimNode, BSTreeNode).
using LeafAnimFn = std::function<bool(const std::string& model)>;

// world_edid is case-insensitive. attach is optional (lights with a model then sit at the reference origin);
// without leaf_anim no tree animates.
// Child world FormID -> the lowercase EditorID of the world it is merged into, for every parent.
using MergedWorlds = std::unordered_map<uint32_t, std::string>;
MergedWorlds merged_worlds(const legacy::EsmDatabase& db);

// The space name of an interior cell (interiors/<name>.osw): its lowercase EditorID, or "cell_<formid>".
std::string interior_space(const legacy::EsmDatabase& db, uint32_t cell);

// Every interior cell of the load order (CELL DATA 0x1, in no worldspace), by FormID.
std::vector<uint32_t> list_interiors(const legacy::EsmDatabase& db);

// One interior cell as a space: its references on the exterior grid by position, its lighting, its load doors.
WorldImport import_interior(const legacy::EsmDatabase& db, uint32_t cell, const LightAttachFn& attach = {},
                            const LeafAnimFn& leaf_anim = {}, const MergedWorlds* merged = nullptr);

// The world, with the child worlds `merged` names for it merged in (nullptr: computed here).
WorldImport import_world(const legacy::EsmDatabase& db, std::string_view world_edid,
                         const LightAttachFn& attach = {}, const LeafAnimFn& leaf_anim = {},
                         const MergedWorlds* merged = nullptr);

// For the pipeline, after mesh conversion: drops references whose mesh has nothing to draw (keep[i] false
// for world.meshes[i]) and compacts the mesh table. Meant for NIFs made only of EditorMarker geometry
// (critter landing spots, pickaxe/wounded furniture markers, auto-load doors), which carry no marker
// flag in the plugin. Returns the number of references dropped (also counted in skipped["empty mesh"]).
size_t drop_meshes(WorldImport& import, const std::vector<bool>& keep);

} // namespace spud::importer
