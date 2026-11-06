#include "engine/precip_occlusion.hpp"

#include "core/units.hpp"
#include "engine/space.hpp"
#include "render/bindless.hpp"

#include <cmath>

namespace spud {

namespace {
constexpr uint32_t k_size = 512; // depth target 10
}

PrecipOcclusion::PrecipOcclusion(render::Renderer& renderer) {
    m_target = renderer.create_offscreen_target({k_size, k_size}, "precipitation occlusion");
    renderer.expose_offscreen_depth(m_target, render::Bindless::k_precip_occlusion_slot);
}

std::span<const render::OffscreenView> PrecipOcclusion::frame(engine::Space& space, const render::RenderSettings& rs,
                                                              const render::View& view, render::SkyFrame& sky) {
    sky.occlusion_valid = false;
    const render::PrecipitationParams* e = nullptr;
    for (const auto& p : sky.precipitation) {
        if (p.amount > 0.0f && (!e || p.amount > e->amount)) {
            e = &p;
        }
    }
    if (!e) {
        return {};
    }
    // orthographic along the fall direction, +-B/2, near 1, far 4B,
    // half a box ahead of the camera and 2B back up the fall, snapped to B/128 along its own axes. Units here.
    const float k = units::k_skyrim_unit_m;
    const float b = e->box;
    glm::vec3 d = e->wind + glm::vec3(0.0f, 0.0f, -e->gravity);
    d = std::abs(d.x) + std::abs(d.y) + std::abs(d.z) < 0.001f ? glm::vec3(0, 0, -1) : glm::normalize(d);
    const glm::vec3 right = glm::normalize(glm::cross(d, glm::vec3(0, 1, 0)));
    const glm::vec3 up = glm::normalize(glm::cross(right, d));
    const glm::vec3 forward(-view.rotation[0][2], -view.rotation[1][2], -view.rotation[2][2]);
    glm::vec3 position = view.position / k + forward * (b * 0.5f) - d * (2.0f * b);
    const float step = b / 128.0f;
    const float pr = std::floor(glm::dot(position, right) / step) * step;
    const float pu = std::floor(glm::dot(position, up) / step) * step;
    const float pd = std::floor(glm::dot(position, d) / step) * step;
    position = right * pr + up * pu + d * pd;
    m_view = render::View::make_ortho(position * k, right, up, b * 0.5f * k, b * 0.5f * k, k, 4.0f * b * k);
    // What falls inside the box: the cells around it.
    const float cell = space.cell_size();
    const glm::vec3 c = view.position + forward * (b * 0.5f * k);
    const float reach = b * k;
    space.build_area_draws(m_view, int32_t(std::floor((c.x - reach) / cell)), int32_t(std::floor((c.y - reach) / cell)),
                           int32_t(std::floor((c.x + reach) / cell)), int32_t(std::floor((c.y + reach) / cell)), m_draws);
    m_offscreen.view = &m_view;
    m_offscreen.space = space.space_view();
    m_offscreen.space.draws = &m_draws;
    m_offscreen.settings = rs;
    m_offscreen.settings.draw_water = false;
    m_offscreen.settings.draw_blended = false;
    m_offscreen.clear = glm::vec4(0.0f);
    m_offscreen.target = m_target;
    m_offscreen.keep_depth = true;
    // Camera-relative metres in the main view to the map's clip space.
    sky.occlusion = m_view.view_proj * glm::translate(glm::mat4(1.0f), view.position - m_view.position);
    sky.occlusion_valid = true;
    return {&m_offscreen, 1};
}

} // namespace spud
