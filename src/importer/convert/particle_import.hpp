#pragma once

// NIF -> .ose: a NIF's particle systems with their modifiers, emitter meshes, controllers, interpolators,
// keys, controller sequences and effect-shader materials (docs/formats/ose.md). The NIF's ordinary geometry
// still goes to .osm (mesh_import.hpp); materials of mesh shapes that the effect's controllers animate
// point at the .osm submesh they became.

#include "formats/ose.hpp"
#include "importer/legacy/nif.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spud::importer {

struct ParticleImportResult {
    ose::Effect effect;                // no systems if the NIF has none (or none that could be decoded)
    std::vector<std::string> textures; // unique normalised texture paths the materials use
    std::vector<std::string> warnings;
};

// True if the file holds at least one NiParticleSystem block.
bool has_controller_manager(const legacy::NifFile& nif);
bool has_particle_systems(const legacy::NifFile& nif);

// True if the file animates nodes (a NiMultiTargetTransformController).
bool has_node_animation(const legacy::NifFile& nif);

// A NIF whose content is animated nodes (the loading screens' camera paths): its nodes, the controllers on its
// scene graph and its controller sequences, with no particle systems (docs/formats/ose.md, version 2).
ParticleImportResult import_node_animation(const legacy::NifFile& nif, std::string_view source_path);

// A model a UI 3D scene shows: its whole scene graph with node classes, shape bounds and
// the node of every .osm submesh (version 3), its controllers and sequences, the effect materials of the shapes they
// animate, any particle systems, and the root's "INV" marker. `bytes` is the file `nif` was read from.
ParticleImportResult import_scene_model(const legacy::NifFile& nif, std::span<const std::byte> bytes,
                                        std::string_view source_path, std::span<const legacy::NifRef> submesh_shapes);

// The root's BSInvMarker named "INV" (rotation as stored and zoom), as the game's 3D views look it up by name.
std::optional<ose::InvMarker> read_inv_marker(const legacy::NifFile& nif, std::span<const std::byte> bytes);
// The file's AddOnNode<index> nodes (docs/formats/ose.md).
std::vector<ose::AddonNode> read_addon_nodes(const legacy::NifFile& nif, std::span<const std::byte> bytes);
// The root's NiStringsExtraData "AttachT".
std::vector<std::string> read_attach_techniques(const legacy::NifFile& nif, std::span<const std::byte> bytes);

// `submesh_shapes` is MeshImportResult::submesh_shapes for the same file (may be empty).
// `source_path` is used for messages. Safe to call from many threads at once.
ParticleImportResult import_particles(const legacy::NifFile& nif, std::string_view source_path,
                                      std::span<const legacy::NifRef> submesh_shapes = {}, bool animation = false);

} // namespace spud::importer
