#include "importer/convert/tree_import.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"
#include "importer/convert/mesh_import.hpp"

#include <format>
#include <functional>
#include <unordered_set>

namespace spud::importer {

using legacy::NifFile;
using legacy::NifRef;
using legacy::NiNode;

namespace {

const NiNode* tree_root(const NifFile& nif) {
    if (nif.roots.empty() || nif.type_name(nif.roots.front()) != "BSTreeNode") {
        return nullptr;
    }
    const auto* root = nif.get<NiNode>(nif.roots.front());
    return root && !root->tree_bones.empty() ? root : nullptr;
}

// Every node of the model, parents first, the root's own transform left out as for any model (a reference's takes its
// place); the root's branch bones flagged.
osk::Skeleton tree_skeleton(const NifFile& nif, const NiNode& root) {
    osk::Skeleton out;
    const std::unordered_set<NifRef> branches(root.tree_bones.begin(), root.tree_bones.end());
    std::vector<uint8_t> seen(nif.blocks.size(), 0);
    std::function<void(NifRef, int32_t, bool)> walk = [&](NifRef ref, int32_t parent, bool is_root) {
        const auto* node = nif.get<NiNode>(ref);
        if (!node || seen[static_cast<size_t>(ref)]) {
            return;
        }
        seen[static_cast<size_t>(ref)] = 1;
        osk::Bone b;
        b.name = node->name;
        b.parent = parent;
        if (!is_root) {
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    b.rotation[r][c] = node->rotation[c][r]; // glm is column-major
                }
                b.translation[r] = node->translation[r] * units::k_skyrim_unit_m;
            }
            b.scale = node->scale;
        }
        b.tree_branch = branches.contains(ref);
        const auto index = static_cast<int32_t>(out.bones.size());
        out.bones.push_back(std::move(b));
        for (NifRef c : node->children) {
            walk(c, index, false);
        }
    };
    walk(nif.roots.front(), -1, true);
    return out;
}

} // namespace

bool has_branch_bones(const NifFile& nif) { return tree_root(nif) != nullptr; }

std::optional<TreeSwayModel> import_tree_sway(const NifFile& nif, std::string_view source_path) {
    const NiNode* root = tree_root(nif);
    if (!root) {
        return std::nullopt;
    }
    // The switch node's skinned child, as the static model draws it: the engine poses these shapes on the nodes.
    MeshImportResult skinned = import_skinned_nif(nif, source_path);
    if (skinned.mesh.submeshes.empty() || skinned.mesh.skin.vertices.size() != skinned.mesh.vertices.size()) {
        return std::nullopt;
    }
    TreeSwayModel out;
    out.mesh = std::move(skinned.mesh);
    out.textures = std::move(skinned.textures);
    out.skeleton = tree_skeleton(nif, *root);
    return out;
}

std::string tree_sway_mesh_path(std::string_view mesh) {
    return std::string(mesh.substr(0, mesh.size() - 4)) + ".sway.osm";
}

std::string tree_sway_skeleton_path(std::string_view mesh) {
    return std::string(mesh.substr(0, mesh.size() - 4)) + ".sway.osk";
}

void convert_tree_sway(const legacy::DataSource& data, AssetCache& assets, const std::filesystem::path& cache,
                       const std::string& src, bool force, std::set<std::string>& textures) {
    const std::string mesh = path::replace_extension(src, ".osm");
    const std::string rel = tree_sway_mesh_path(mesh), rel_skeleton = tree_sway_skeleton_path(mesh);
    if (!force && assets.fresh(rel, src, k_tree_sway_converter_version) &&
        assets.fresh(rel_skeleton, src, k_tree_sway_converter_version)) {
        return;
    }
    const auto bytes = data.read(src);
    if (!bytes) {
        return;
    }
    try {
        const NifFile nif = legacy::read_nif(*bytes);
        if (auto sway = import_tree_sway(nif, src)) {
            write_file(cache / rel, osm::write(sway->mesh));
            write_file(cache / rel_skeleton, osk::write(sway->skeleton));
            textures.insert(sway->textures.begin(), sway->textures.end());
        }
        assets.record(rel, src, k_tree_sway_converter_version);
        assets.record(rel_skeleton, src, k_tree_sway_converter_version);
    } catch (const std::exception& e) {
        log::warn("{}: tree sway: {}", src, e.what());
    }
}

} // namespace spud::importer
