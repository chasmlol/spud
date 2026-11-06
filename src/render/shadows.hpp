#pragma once

// The sun's (and moon's) shadow cascades as Skyrim SE fits them: the split distances, each
// cascade's light-space box and its shadow-map projection, from the camera's view and the directional light.
// Everything that decides which texel a point reads is the game's; the renderer draws the maps and the lighting
// shaders evaluate the game's shadow mask with these values.

#include "render/view.hpp"

#include <cstdint>

#include <glm/glm.hpp>

namespace spud::render {

struct ShadowCascade {
    glm::mat4 clip{1.0f};   // camera-relative metres -> the map's Vulkan clip space (x 2u-1, y 2v-1, z depth)
    glm::vec4 rows[3]{};    // u, v and depth of a camera-relative point (metres), as the mask shader reads them
    float bias = 0.0f;      // subtracted from the depth before the comparison
    float end_z = 0.0f;     // view depth (metres) where the cascade ends
    // The light-space box of the map: x, y across the light, z along it (game units from the light camera, which
    // stands 15000 units before the camera, sunwards); lo.z, hi.z are the map's near and far planes.
    glm::vec3 axis_x{0.0f}, axis_y{0.0f}, axis_z{0.0f};
    glm::vec3 lo{0.0f}, hi{0.0f};
    // Where the fit stands relative to the camera, along those axes: the camera rounded to whole units.
    glm::vec3 shift{0.0f};
    // The caster cull's side planes: edges of the 2D convex hull of the slice's corners
    // in the light's x, y (at most the 5 longest), each n.xy . p + w >= 0 inside, units from the light camera.
    glm::vec3 sides[5]{};
    uint32_t side_count = 0;

    // Whether a camera-relative sphere (metres) can cast into the map.
    bool sphere(const glm::vec3& centre, float radius) const;
};

// The direction the sun's shadows use: the current one whenever the camera moves, and
// otherwise refreshed every fSunStaticTimeUpdateScale seconds.
struct SunShadowDirection {
    glm::vec3 direction{0.0f};
    glm::vec3 camera{0.0f}; // metres
    float wait = 0.0f;      // reaches 1 when the direction is due
    bool valid = false;     // false: take the current direction on the next update (a time set by hand)

    glm::vec3 update(const glm::vec3& to_light, const glm::vec3& camera_position, float dt, float update_scale);
};

struct SunShadows {
    uint32_t count = 0; // 0: no sun shadows
    ShadowCascade cascades[k_max_cascades];
    bool hold_last = false; // the last cascade's map stands from an earlier frame: nothing draws into it
    uint32_t drawn() const { return count - (hold_last ? 1u : 0u); }
    float blend_start_z = 0.0f; // view depth (metres) where cascade 0 starts blending into cascade 1
    float fade_distance = 0.0f; // metres: the mask fades to lit as (d / fade)^16 approaches 1
    float filter_radius = 0.0f; // map UV: the 16 taps' radius
};

enum class ShadowLightType : uint32_t { spot, hemisphere, omni };

// A shadow-casting point light this frame.
struct ShadowLight {
    uint32_t light = 0; // index into SpaceView::lights
    ShadowLightType type = ShadowLightType::omni;
    // What the mask shader projects a camera-relative point (metres) with: the spot's clip position with y towards
    // the texture's v, or the paraboloid's light-space vector (game units).
    glm::vec4 rows[4]{};
    // What the caster passes project with: Vulkan clip space for a spot; for a paraboloid the same vector, with y up
    // for the front half and, in [1], y down for the back half.
    glm::mat4 render[2]{glm::mat4(1.0f), glm::mat4(1.0f)};
    float bias = 0.0f;  // subtracted from the depth before the comparison
    float param = 0.0f; // paraboloid: radius (game units, depth = distance / radius); spot: falloff exponent
    float fade_distance = 0.0f; // metres: the mask's distance fade
};

// `position` camera-relative (metres), `rotation` the light's world rotation (xyzw), `radius` and `near_clip` metres,
// `distance` the shadow distance in game units.
ShadowLight compute_shadow_light(const glm::vec3& position, const glm::vec4& rotation, float radius, float fov,
                                 float falloff, float near_clip, float bias, bool fades, float distance);

// Whether a spot's pyramid can meet the view's: `position` camera-relative, `radius` metres, the
// pyramids tested against each other's faces.
bool spot_light_visible(const View& view, const glm::vec3& position, const glm::vec4& rotation, float radius,
                        float fov);

// `to_light` points from the scene to the light (Environment::sun_dir), `near_units` / `far_units` are the game's
// projection planes (Environment::near_units / far_units).
SunShadows compute_sun_shadows(const View& view, const glm::vec3& to_light, float near_units, float far_units,
                               const ShadowSettings& settings, bool interior);

} // namespace spud::render
