#pragma once

// The UI 3D scenes: what a menu shows in a slot of the game's UI 3D scene manager (g_UI3DSceneManager
//): the 3D item behind the item and magic menus (slots 1 and 2), the book
// (3), the Stats Menu's starfield (6). A menu describes its scene each frame in the game's terms (Skyrim
// units, NiMatrix3 rotations, a Gamebryo frustum); the engine draws it with the renderer's mesh path at the
// menu's place in the stack, before or after the menu's movie as the menu's display function does
// (the game draws straight into the frame after clearing depth).
//
// The models' scene graphs (node names, classes, bounds, the "INV" marker) come from the .ose the importer
// writes beside each UI model's .osm (docs/formats/ose.md, version 3); SceneModels loads them off the render
// thread.

#include "formats/osm.hpp"
#include "formats/ose.hpp"
#include "ui/player/draw.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace spud::ui {

// A point light of a slot, world position in Skyrim units.
struct SceneLight {
    float position[3] = {0, 0, 0};
    float color[3] = {0, 0, 0}; // diffuse
    float radius = 0;
    float dimmer = 0; // the light's fade value
    bool shadows = false;
};

// An NiCamera: world translation (Skyrim units), world rotation as a row-major NiMatrix3 whose columns are
// the view direction, up and right (Gamebryo's camera axes), and the NiFrustum: left, right, top, bottom as
// slopes at unit distance, near, far.
struct SceneCamera {
    float position[3] = {0, 0, 0};
    float rotation[9] = {0, 0, -1, -1, 0, 0, 0, 1, 0}; // looking along -Y, up +Z, right -X
    float frustum[6] = {-1, 1, 1, -1, 15, 20480};
};

// The UI scene manager's own camera as the game makes it: at the origin looking along -Y
// with up +Z, the frustum from fDefaultFOV (80) with the game's horizontal-locked slopes (0.75
// t across, 0.75 t H / W up, t = tan(fov pi / 1200)), near fUICameraNearDistance 15, far fUICameraFarDistance
// 20480.
SceneCamera default_scene_camera(float width, float height);

// One draw: a whole model, or one shape of it (one .osm submesh), at a world transform.
struct SceneDraw {
    uint32_t model = 0;                              // index into UiScene::models
    int submesh = -1;                                // -1: the whole model
    float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // world rotation x scale, row-major (column vectors)
    float translation[3] = {0, 0, 0};                // world, Skyrim units
    // The shape's effect material alpha (base colour alpha), -1: as
    // loaded.
    float alpha = -1;
    // The shape's diffuse replaced by a Book Menu page target (render target 105 + page), -1: none.
    int page = -1;
    // The order blended draws are drawn in, farthest first, when not by distance (a BSOrderedNode draws its
    // children in their order): a key in square metres, larger first; -1: the camera distance.
    float sort = -1;
    // Under an NiBillboardNode the scene turned itself: the renderer must not turn it again.
    bool turned = false;
    // The shape's effect material as the model's controllers leave it, when `effect` is set:
    // BSEffectShaderProperty base colour x its emissive multiple, alpha, and the UV offset and scale. `alpha`
    // still replaces the alpha.
    bool effect = false;
    float base_color[4] = {1, 1, 1, 1};
    float uv_offset[2] = {0, 0};
    float uv_scale[2] = {1, 1};
};

struct UiScene {
    int slot = 0;
    SceneCamera camera;
    std::vector<SceneLight> lights;
    std::vector<std::string> models; // cache .osm paths the draws refer to
    std::vector<SceneDraw> draws;

    // Kept between frames so the lists reuse their storage.
    void clear() {
        slot = 0;
        camera = SceneCamera{};
        lights.clear();
        model_count = 0;
        draws.clear();
    }
    // The index of a model path, added if new this frame.
    uint32_t model(std::string_view path) {
        for (uint32_t i = 0; i < model_count; ++i) {
            if (models[i] == path) {
                return i;
            }
        }
        if (model_count == models.size()) {
            models.emplace_back();
        }
        models[model_count].assign(path);
        return model_count++;
    }
    uint32_t model_count = 0; // models[0, model_count) are this frame's
};

// An off-screen page a menu draws this frame (the Book Menu's render target 105 + page): its movie's draw
// list as the page shows it and where the movie's stage goes on the page target.
struct ScenePage {
    const player::DrawList* list = nullptr;
    player::Matrix2D stage_to_pixels;
    int page = 0;
};

// A UI model's scene graph as the importer wrote it (.ose version 3) and the .osm's materials' textures, with
// each node's transform in the model's space at rest.
struct SceneModel {
    std::string osm; // cache path of the mesh
    ose::Effect graph;
    std::vector<std::array<std::string, osm::k_texture_slots>> submesh_textures; // per .osm submesh
    std::vector<glm::mat3>
        rest_rotation; // per node: model space (the root's own transform included), x scale
    std::vector<glm::vec3> rest_translation;
    std::vector<float> rest_scale;

    uint32_t find(std::string_view name) const;          // a node by name, ose::k_none if absent
    int submesh_of(uint32_t node) const;                 // the .osm submesh drawn from a shape node, -1
    std::vector<uint32_t> children(uint32_t node) const; // in file order
    bool has_inv() const { return !graph.inv_marker.empty(); }
    // The world bound of node `node` as Gamebryo's update computes it (the
    // children's world bounds merged in order, a shape's its stored bound transformed), for the root placed
    // at `root_rotation` (x scale) and `root_translation` instead of its own local transform. Skyrim units.
    // `detached`: a node left out with its subtree (the weapon's scabbard the 3D item manager detaches
    // first).
    void world_bound(uint32_t node, const glm::mat3& root_rotation, const glm::vec3& root_translation,
                     glm::vec3& center, float& radius, uint32_t detached = ose::k_none) const;
};

// The UI models by cache path, each read and parsed on a worker the first time it is asked for: null until
// then (the scene shows what has arrived, never stalling a frame). `submit` runs a job off the render thread.
class SceneModels {
  public:
    SceneModels(std::filesystem::path cache_root, std::function<void(std::function<void()>)> submit);
    ~SceneModels();
    // The model whose .osm is `osm_path` (its .ose beside it); null while it loads, or when it has no scene
    // graph.
    std::shared_ptr<const SceneModel> get(const std::string& osm_path);
    bool failed(const std::string& osm_path) const;

  private:
    struct Shared;
    std::filesystem::path m_root;
    std::function<void(std::function<void()>)> m_submit;
    std::shared_ptr<Shared> m_shared;
};

// Gamebryo's the game: the smallest sphere holding both (`center`, `radius` grows to take the other
// in).
void merge_bound(glm::vec3& center, float& radius, const glm::vec3& other_center, float other_radius);

} // namespace spud::ui
