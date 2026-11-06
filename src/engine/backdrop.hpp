#pragma once

// The Mist Menu's 3D backdrop (the logo behind the main menu, the loading screens' models) as a scene of its own.
// The menu describes it;
// this draws it through the renderer's mesh path as the frame's only scene, under the menu movies.
//
// Lighting: the game lights the model with three point lights and no ambient through the game: the same here.
// The fog is INTMenuFogParticles' particles, in the Mist Menu's slot of the scene.

#include "engine/world_fx.hpp"
#include "render/instance_table.hpp"
#include "render/renderer.hpp"
#include "render/view.hpp"
#include "ui/ui.hpp"

#include <string>

namespace spud::engine {

class Backdrop {
public:
    Backdrop(render::Renderer& renderer, const std::filesystem::path& cache);
    ~Backdrop();
    Backdrop(const Backdrop&) = delete;
    Backdrop& operator=(const Backdrop&) = delete;

    // Each frame while the Mist Menu is open; `aspect` is the render target's width / height.
    void update(const ui::BackdropScene& scene, float aspect, float dt);
    // The scene's HDR and tone: hdr[9], cinematic[3], tint[4] and dof[4] of an IMGS, 20 contiguous floats.
    void set_image_space(const float* block);
    // The menu closed: the model is released (it stays cached) and the fog ends.
    void release();

    bool shown() const { return m_model_shown; } // the model is on screen
    const render::View& view() const { return m_view; }
    render::SpaceView space_view() const;

private:
    void place(const glm::mat4& transform);
    void release_model();
    void update_fog(float dt);

    render::Renderer& m_renderer;
    render::InstanceTable m_instances;
    render::DrawList m_draws;
    render::View m_view;
    render::Environment m_environment;
    std::string m_model;
    render::MeshId m_mesh = render::k_invalid_mesh;
    uint32_t m_slot = ~0u;
    glm::mat4 m_placed{0.0f}; // the transform m_slot holds
    std::vector<render::PointLight> m_lights;
    WorldFx m_fog;
    osw::World m_no_world;
    std::vector<uint32_t> m_fog_textures;
    uint32_t m_fog_id = 0;
    bool m_model_shown = false;
    float m_fog_seconds = -1.0f; // below 0: not started
    bool m_fog_looping = false;
};

} // namespace spud::engine
