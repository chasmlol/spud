#pragma once

// Trees whose branches move in the wind (docs/specs/lighting.md section 12; the motion in the notes' tree sway report):
// the nearest trees in view of a model with branch bones are drawn from a posed copy of their skinned model, the
// branch bones turned each tick by the game's wind wave. Others keep their last pose, or the plain model.

#include "formats/osk.hpp"
#include "render/meshes.hpp"
#include "render/view.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <future>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud {

class TreeSway {
public:
    TreeSway(render::MeshManager& meshes, std::filesystem::path cache);
    ~TreeSway();
    TreeSway(const TreeSway&) = delete;
    TreeSway& operator=(const TreeSway&) = delete;

    struct Settings {
        float switch_distance = 0.0f; // fTreesMidLODSwitchDist:Display, game units: no sway beyond
        uint32_t max_trees = 40;      // uiMaxSkinnedTreesToRender:Trees
        float wave_scale = 0.03f;     // the game's tree wind speed
        float fade_mult = 5.0f;       // fLODFadeOutMultObjects:LOD
        float lod_adjust = 1.0f;      // the camera's LOD adjust: world FOV / fDefaultFOV
    };
    // A tree in view this tick, of a model with branch bones.
    struct Candidate {
        uint32_t form_id = 0;      // the reference
        const std::string* mesh = nullptr; // its model's cache path (".osm")
        float branch_flexibility = 0.0f;
        glm::vec3 position{0.0f};  // metres
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        float scale = 1.0f;
        glm::vec3 centre{0.0f};    // its bound's centre, metres: what the bone fade measures from
        float distance_sq = 0.0f;  // the reference's position to the camera, metres squared: the nearest go first
        render::MeshId plain = render::k_invalid_mesh; // the model it's drawn with otherwise, and its material variant
        uint32_t plain_variant = render::k_no_variant;
    };
    // One tick: poses the nearest `settings.max_trees` of `candidates` (in view, this tick) at world time `time`
    // (seconds), adding their shapes and rows to `jobs` and `rows`. `loaded` is every swaying tree still loaded; the
    // copies of the others are let go.
    void update(std::span<Candidate> candidates, std::span<const uint32_t> loaded, const Settings& settings, float time,
                const glm::vec3& camera, std::vector<render::SkinJob>& jobs, std::vector<glm::vec4>& rows);
    // The posed copy a reference is drawn with (its mesh and variant), false when it has none.
    bool drawn_as(uint32_t form_id, render::MeshId& mesh, uint32_t& variant) const;
    // A named node of a posed tree as last posed, in the world: position (metres) and rotation. False when it has none.
    bool node(uint32_t form_id, std::string_view name, glm::vec3& position, glm::mat3& rotation) const;

private:
    struct Node {
        int32_t parent = -1;
        glm::mat3 rotation{1.0f}; // rest, parent frame (column vectors)
        glm::vec3 translation{0.0f};
        float scale = 1.0f;
    };
    struct Branch {
        int32_t node = 0;
        glm::vec2 direction{0.0f}; // its X axis in the model's frame, x and y, normalised; 0 when vertical
        glm::mat3 to_parent{1.0f}; // the model's frame to the parent's: the parent's rest world rotation, transposed
    };
    struct Model {
        render::MeshId mesh = render::k_invalid_mesh;
        std::future<osk::Skeleton> loading;
        bool failed = false;
        bool ready = false;
        std::vector<Node> nodes;
        std::vector<Branch> branches; // by the bone's rest height, lowest first
        std::vector<int32_t> skin_nodes; // parallel to the skinned model's bones: the node each follows, -1 none
        std::vector<std::string> names;  // parallel to `nodes`
    };
    struct Tree {
        uint32_t model = 0;
        uint32_t variant = render::k_no_variant;
        std::vector<uint64_t> targets;
        std::vector<glm::mat3> locals; // each node's rotation as last posed
        std::vector<glm::mat4> world;  // each node in the model's frame, as last posed
        glm::mat4 placed{1.0f};        // the reference's transform
        uint64_t used = 0;             // the tick it was last posed
    };
    Model* model(const std::string& mesh, uint32_t& index);
    bool prepare(Model& m);
    Tree* tree(const Candidate& c, uint32_t model);
    void pose(Tree& t, const Model& m, const Candidate& c, const Settings& settings, float time, const glm::vec3& camera,
              std::vector<render::SkinJob>& jobs, std::vector<glm::vec4>& rows);
    void free(Tree& t);

    render::MeshManager& m_meshes;
    std::filesystem::path m_cache;
    std::unordered_map<std::string, uint32_t> m_model_index;
    std::vector<Model> m_models;
    std::unordered_map<uint32_t, Tree> m_trees; // by reference
    uint64_t m_tick = 0;
};

} // namespace spud
