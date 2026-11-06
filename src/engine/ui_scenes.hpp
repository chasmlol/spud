#pragma once

// The UI 3D scenes as the
// renderer draws them: each frame's ui::UiScene list becomes one render::UiSceneInput per scene, with its own
// camera, instance table and draw list, which the game UI's overlay draws at the scene's place among the
// menus.
//
// Lighting: the game lights these models with its slot's point lights through the game (the lighting
// workstream). Main's mesh shading has one directional light and hemisphere ambient, so the slot's first
// light with a radius stands in as the directional light, from the model's centre, with its colour x dimmer x
// the lighting shader's attenuation there; the ambient is zero. Effect-shader models (the starfield) are
// unlit either way.

#include "render/instance_table.hpp"
#include "render/renderer.hpp"
#include "render/view.hpp"
#include "ui/ui.hpp"

#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace spud::engine {

class UiScenes {
  public:
    explicit UiScenes(render::Renderer& renderer);
    ~UiScenes();
    UiScenes(const UiScenes&) = delete;
    UiScenes& operator=(const UiScenes&) = delete;

    // Each frame: this frame's scenes (Ui::ui_scenes) into the inputs `inputs()` returns, in the same order.
    void update(std::span<const ui::UiScene> scenes, ui::Ui& ui);
    // The directional ambient the world has now (what a scene that writes none of its own sees).
    void set_world_ambient(const glm::vec3& sky, const glm::vec3& ground) {
        m_world_ambient_sky = sky;
        m_world_ambient_ground = ground;
    }
    std::span<const render::UiSceneInput> inputs() const { return m_inputs; }
    // Every scene closed: its models are released (they stay cached).
    void release();

  private:
    struct Draw {
        uint32_t mesh = render::k_invalid_mesh;
        int submesh = -1;
        float transform[12] = {};
        uint32_t slot = ~0u;
        float alpha = -1;
        float effect[8] = {}; // the effect override it was made with (base colour, UV offset, UV scale)
        bool has_effect = false;
        uint32_t texture = ~0u;
        uint32_t group = ~0u;
        uint32_t material = ~0u; // an overriding material slot, or ~0u
    };
    struct View {
        int slot = -1;
        render::InstanceTable instances;
        render::DrawList draws;
        render::View view;
        render::Environment environment;
        std::vector<Draw> cache; // by draw index
        explicit View(render::Device& device);
    };

    render::MeshId mesh(const std::string& path);
    uint32_t group_index(render::MeshId mesh, int submesh) const;
    void update_view(View& v, const ui::UiScene& scene, ui::Ui& ui);

    render::Renderer& m_renderer;
    glm::vec3 m_world_ambient_sky{0.0f}, m_world_ambient_ground{0.0f};
    std::vector<std::unique_ptr<View>> m_views; // by the scene's slot
    std::vector<render::UiSceneInput> m_inputs;
    std::map<std::string, render::MeshId> m_meshes;
};

} // namespace spud::engine
