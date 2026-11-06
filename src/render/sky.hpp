#pragma once

// What the engine's sky hands the renderer each frame: the sky draws (the game techniques,
// in the game's order), the image-space data the composite and HDR chain read, volumetric lighting and
// precipitation. Plain values in fixed arrays: an Environment carries one per space.

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

namespace spud::render {

// The game techniques.
enum SkyTechnique : uint32_t {
    sky_tech_occlude = 0,
    sky_tech_glare = 1,
    sky_tech_mask = 2, // MoonAndStarsMask
    sky_tech_stars = 3,
    sky_tech_clouds = 4,
    sky_tech_clouds_lerp = 5,
    sky_tech_clouds_fade = 6,
    sky_tech_texture = 7, // sun and moon discs
    sky_tech_dome = 8,    // "Sky": the atmosphere
};

struct SkyVertex {
    float position[3]; // game units, camera-centred
    float uv[2];
    uint8_t color[4];
};
static_assert(sizeof(SkyVertex) == 24);

struct SkyDraw {
    uint32_t technique = sky_tech_dome;
    uint32_t first_index = 0; // into the sky mesh (Renderer::set_sky_mesh)
    uint32_t index_count = 0;
    uint32_t texture0 = 0;    // TextureId (bindless slot)
    uint32_t texture1 = 0;    // CloudsLerp's second texture
    float blend = 0.0f;       // PParams.x
    float depth = 0.0f;       // the draw's depth bias as reverse-Z depth (game: 1 - k * 2^-24)
    uint32_t moon = 0;        // sky object type 6: no Sky Scale
    glm::mat4 world{1.0f};    // game units
    glm::vec4 color[3] = {};  // BlendColor[0..2]
    glm::vec4 uv_offset{0.0f}; // TexCoordOff (xy)
};

// The image-space data block after the weather blend and the modifiers.
struct ImageSpaceData {
    float eye_adapt_speed = 0, bloom_blur_radius = 0, bloom_threshold = 0, bloom_scale = 0,
          receive_bloom_threshold = 0, white = 0, sunlight_scale = 0, sky_scale = 0, eye_adapt_strength = 0;
    float saturation = 1, brightness = 1, contrast = 1;
    float tint_amount = 0;
    glm::vec3 tint{0.0f};
    glm::vec4 dof{0.0f}; // strength, distance, range, flags (float)
    float fade_amount = 0;
    glm::vec3 fade{0.0f};
    float blur_radius = 0;
    float double_vision = 0;
    float radial_strength = 0, radial_ramp_up = 0, radial_start = 0, radial_ramp_down = 0, radial_down_start = 0;
    glm::vec2 radial_center{0.5f};
    float motion_blur = 0;
    bool world_map = false; // ImageSpaceEffectWorldMap in place of the distant blur
};

struct VolumetricParams {
    bool enabled = false;
    float intensity = 2, custom_color_contribution = 0;
    glm::vec3 color{1.0f};
    float density_contribution = 0.3f, density_size = 300, wind_speed = 15, falling_speed = 0.3f,
          phase_contribution = 0.83f, phase_scattering = 0.85f, range = 40;
    glm::vec3 wind_offset{0.0f}; // game units, the noise volume's drift
};

// One precipitation emitter at this frame.
struct PrecipitationParams {
    float amount = 0;      // ceil(amount) passes, the last at alpha amount - floor
    uint32_t snow = 0;     // technique ENVCUBE SNOW, else RAIN
    uint32_t texture = 0;  // TextureId
    uint32_t seed = 0;     // the geometry's random stream
    float box = 4096;      // box size, units
    float gravity = 0, rotation_velocity = 0, size_x = 1, size_y = 1, offset_min = 0, offset_max = 0,
          rotation_range = 0;
    uint32_t subtextures_x = 1, subtextures_y = 1;
    glm::vec3 wind{0.0f};   // W, units/s
    glm::vec3 scroll{0.0f}; // fmod(scroll - camera, B) before the camera offset, units
    float rotation = 0;
    glm::vec3 instance_offset[10] = {};
};

struct SkyFrame {
    bool exterior = false; // the sky is drawn and the image space is the weather's
    uint32_t draw_count = 0;
    std::array<SkyDraw, 72> draws;
    float sky_scale = 0;  // PParams.y: g_FramebufferRangeRcp x the image space's Sky Scale
    ImageSpaceData image_space;
    bool image_space_valid = false;
    bool ui_scene = false; // the Mist Menu's scene: HDR and tonemap alone, no composite or distant blur, the eye at the scene
    bool reset_adaptation = false; // the eye adaptation jumps to the scene (forced weather change)
    float dof_center_weight = 2.0f;
    VolumetricParams volumetric;
    std::array<PrecipitationParams, 2> precipitation; // current, outgoing
    glm::mat4 occlusion{1.0f}; // camera-relative metres -> the precipitation occlusion map's clip space
    bool occlusion_valid = false;
    float transition = 1.0f; // T: sky statics fade with it
    glm::vec3 model_offset{0.0f}; // the weather model from the camera, units: the sun's place with DATA[11] & 0x20
    glm::vec3 sky_statics{1.0f}; // their colour (the weather colour Sky Statics)
    glm::vec3 sunlight{0.0f}; // the sun light's diffuse (Sunlight): volumetric lighting's and the sparkles' colour
};

} // namespace spud::render
