#pragma once

// One bindless descriptor set per frame in flight: binding 0 is a large
// sampled-image array indexed by TextureId, binding 1 the immutable samplers.
// A slot is rewritten (fallback -> loaded texture) in the set of the frame
// being recorded immediately and in the other sets when their frame comes
// up, so a descriptor is never changed while a submitted frame may read it.

#include "render/device.hpp"

#include <array>
#include <vector>

namespace spud::render {

// Added to every material sampler's LOD: measured, D3D11 versus Vulkan.
inline constexpr float k_texture_lod_bias = -0.25f;

enum SamplerIndex : uint32_t {
    // Matches osm::MaterialParams::texture_clamp (S = u, T = v).
    sampler_clamp_clamp = 0,
    sampler_clamp_wrap = 1,
    sampler_wrap_clamp = 2,
    sampler_wrap_wrap = 3,
    sampler_wrap_isotropic = 4, // trilinear without anisotropy
    // Shadow maps: the game's comparison sampler, bilinear, clamped, LESS (lit where the
    // reference is nearer than the stored depth).
    sampler_shadow = 5,
    k_sampler_count = 6,
};

class Bindless {
public:
    static constexpr uint32_t k_max_textures = 65536;
    // Slots the renderer keeps for its own render targets; textures use the ones below.
    static constexpr uint32_t k_scene_slot = k_max_textures - 1; // the resolved HDR scene, read by tonemapping
    static constexpr uint32_t k_depth_slot = k_max_textures - 2; // the opaque scene's depth (soft effects)
    static constexpr uint32_t k_refraction_slot = k_max_textures - 3; // the refraction target (ISRefraction)
    static constexpr uint32_t k_refracted_slot = k_max_textures - 4;  // the scene after ISRefraction
    static constexpr uint32_t k_sun_shadow_slot = k_max_textures - 5;   // the sun's cascades (a 2D array view)
    static constexpr uint32_t k_light_shadow_slot = k_max_textures - 6; // the shadow lights' maps (a 2D array view)
    static constexpr uint32_t k_god_ray_slot = k_max_textures - 7;      // the god-ray map (a 2D array view)
    // The image-space chain's targets (renderer_post.cpp).
    static constexpr uint32_t k_post_base = k_max_textures - 7 - 64;
    static constexpr uint32_t k_normal_slot = k_post_base;          // resolved normals
    static constexpr uint32_t k_ao_raw_slot = k_post_base + 1;
    static constexpr uint32_t k_ao_tmp_slot = k_post_base + 2;
    static constexpr uint32_t k_ao_slot = k_post_base + 3;
    static constexpr uint32_t k_camz_chain_slot = k_post_base + 4;  // every level of the camera-Z pyramid
    static constexpr uint32_t k_camz_slot = k_post_base + 5;        // one per level (up to 14)
    static constexpr uint32_t k_quarter_slot = k_post_base + 19;
    static constexpr uint32_t k_lum_slot = k_post_base + 20;        // up to 6
    static constexpr uint32_t k_adapt_slot = k_post_base + 26;      // two
    static constexpr uint32_t k_bloom_slot = k_post_base + 28;      // two
    static constexpr uint32_t k_ldr_slot = k_post_base + 30;
    static constexpr uint32_t k_dof_slot = k_post_base + 31;        // two
    static constexpr uint32_t k_radial_slot = k_post_base + 33;
    static constexpr uint32_t k_scene_depth_slot = k_post_base + 34; // the multisampled scene depth
    static constexpr uint32_t k_vl_slot = k_post_base + 35;         // two
    static constexpr uint32_t k_blurred_slot = k_post_base + 37;
    static constexpr uint32_t k_precip_occlusion_slot = k_post_base + 38;
    static constexpr uint32_t k_vl_volume_slot = k_post_base + 39;
    static constexpr uint32_t k_vl_noise_slot = k_post_base + 40;
    static constexpr uint32_t k_snow_source_slot = k_post_base + 41;
    static constexpr uint32_t k_ssr_slot = k_post_base + 42;        // two
    static constexpr uint32_t k_lens_slot = k_post_base + 44;       // four
    static constexpr uint32_t k_ssr_raw_slot = k_post_base + 48;     // the reflections before their blur
    static constexpr uint32_t k_water_mask_slot = k_post_base + 49;
    static constexpr uint32_t k_local_map_slot = k_post_base + 61;  // the local map's scene
    static constexpr uint32_t k_portal_scene_slot = k_post_base + 62; // a door's far frame: its scene,
    static constexpr uint32_t k_portal_image_slot = k_post_base + 63; // and its finished image
    static constexpr uint32_t k_water_depth_slot = k_post_base + 50; // the scene's depth with the water in it
    static constexpr uint32_t k_water_history_slot = k_post_base + 51; // two
    static constexpr uint32_t k_water_cube_slot = k_post_base + 53;
    static constexpr uint32_t k_water_flow_slot = k_post_base + 54;
    static constexpr uint32_t k_water_ripple_slot = k_post_base + 55; // two
    static constexpr uint32_t k_water_door_history_slot = k_post_base + 57; // a door's far view: two,
    static constexpr uint32_t k_water_door_cube_slot = k_post_base + 59;    // and its reflection
    static constexpr uint32_t k_texture_slots = k_post_base;

    explicit Bindless(Device& device);
    ~Bindless();
    Bindless(const Bindless&) = delete;
    Bindless& operator=(const Bindless&) = delete;

    VkDescriptorSetLayout layout() const { return m_layout; }
    VkDescriptorSet set(uint32_t frame) const { return m_sets[frame]; }

    // Points every slot of every set at `view` (startup).
    void fill_all(VkImageView view);
    // Applies queued writes to `frame`'s set; call after that frame's fence wait.
    void begin_frame(uint32_t frame);
    void write(uint32_t slot, VkImageView view, VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

private:
    Device& m_device;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    std::array<VkSampler, k_sampler_count> m_samplers{};
    std::array<VkDescriptorSet, k_frames_in_flight> m_sets{};
    struct Pending {
        uint32_t slot;
        VkImageView view;
        VkImageLayout layout;
    };
    std::array<std::vector<Pending>, k_frames_in_flight> m_pending;
    uint32_t m_frame = 0;
};

} // namespace spud::render
