#pragma once

// NIF -> .osm: flattens a NIF scene graph into spud's static mesh format. Node transforms are baked
// into the vertices (metres), one submesh per drawn shape, materials from the shader and alpha
// properties. Editor-only and hidden geometry, collision, particles, water and sky are dropped.

#include "formats/osm.hpp"
#include "importer/legacy/nif.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spud::importer {

struct MeshImportResult {
    osm::Mesh mesh;                    // no submeshes if nothing renderable
    std::vector<std::string> textures; // unique normalised texture paths referenced by the materials
    std::vector<std::string> warnings;
    std::vector<legacy::NifRef> submesh_shapes; // the shape block each submesh was converted from
};

// `source_path` is the normalised data path ("meshes/x/y.nif"), used for messages.
// Throws ParseError if the file is malformed. Safe to call from many threads at once.
MeshImportResult import_nif(std::span<const std::byte> nif, std::string_view source_path);

// Same, for a file that has already been parsed (tools inspect the parse first).
// `damage_stage` is the stage the model's BSDamageStage nodes show, 0 for intact.
// `harvested`: a NiSwitchNode draws its second child, as a picked plant does.
MeshImportResult import_nif(const legacy::NifFile& nif, std::string_view source_path, int damage_stage = 0, bool harvested = false);

// A model drawn on a skeleton: shapes stay in skin space with their bones and weights (osm::Skin), for the engine
// to pose.
// `keep_vertices`: a head part's shapes stay in the NIF's vertex order, which its .tri morphs are in.
// `object`: an animated object, whose rigid shapes ride on their parent nodes.
MeshImportResult import_skinned_nif(const legacy::NifFile& nif, std::string_view source_path, bool keep_vertices = false,
                                    bool object = false);
// `heavy` (a model's _1 file) takes `light`'s vertices (its _0 file) as its weight morph. False when the two don't
// have the same shapes, so they can't blend.
bool add_weight_morph(osm::Mesh& heavy, osm::Mesh&& light);

// The .osm material for a shader property (BSLightingShaderProperty or BSEffectShaderProperty) and an
// optional NiAlphaProperty; nullopt for any other shader. Texture paths are normalised cache paths.
std::optional<osm::Material> convert_material(const legacy::NifFile& nif, legacy::NifRef shader,
                                              legacy::NifRef alpha, bool has_vertex_colors);

} // namespace spud::importer
