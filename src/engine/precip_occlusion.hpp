#pragma once

// The precipitation occlusion map: the world seen along the fall
// direction, so rain stops under roofs. An offscreen view drawn before the scene; the particles read its depth.

#include "render/renderer.hpp"

#include <span>

namespace spud {

namespace engine {
class Space;
}

class PrecipOcclusion {
public:
    explicit PrecipOcclusion(render::Renderer& renderer);

    // Places the map for this frame's strongest emitter and writes its matrix into `sky`. Empty without precipitation.
    std::span<const render::OffscreenView> frame(engine::Space& space, const render::RenderSettings& rs,
                                                 const render::View& view, render::SkyFrame& sky);

private:
    uint32_t m_target;
    render::View m_view;
    render::DrawList m_draws;
    render::OffscreenView m_offscreen;
};

} // namespace spud
