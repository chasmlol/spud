#pragma once

#include "render/instance_table.hpp"
#include "render/renderer.hpp"

#include <string>
#include <vector>

namespace spud::engine {

// What the world map shows in place of the world: the worldspace's level 32 land and
// level 16 objects, as the game locks its LOD, and the cloud bank.
class WorldMapScene {
public:
    WorldMapScene(render::Renderer& renderer, std::string cache_root);
    ~WorldMapScene();

    void open(const std::string& world);
    void close();
    bool active() const { return !m_models.empty(); }

    // Replaces what `space` draws, for this frame's camera.
    void apply(render::SpaceView& space, const render::View& view);

private:
    struct Model {
        render::MeshId mesh = render::k_invalid_mesh;
        glm::vec3 origin{0.0f}; // metres
        uint32_t slot = ~0u;
    };

    render::Renderer& m_renderer;
    std::string m_cache_root;
    render::InstanceTable m_instances;
    render::DrawList m_draws;
    std::vector<Model> m_models;
};

} // namespace spud::engine
