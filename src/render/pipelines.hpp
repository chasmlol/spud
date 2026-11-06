#pragma once

// Every graphics pipeline the renderer uses, created up front (in parallel,
// through a persistent VkPipelineCache) so nothing compiles at runtime.
// Cull mode, front face, depth test/write and depth bias are dynamic state;
// blend factors are baked, so blended programs get one pipeline per
// (src, dst) NiAlphaProperty factor pair.

#include "render/device.hpp"
#include "render/jobs.hpp"

#include <array>
#include <filesystem>
#include <vector>

namespace spud::render {

enum Program : uint8_t {
    program_sky,
    program_terrain,
    program_terrain_far, // land beyond the full-resolution ring
    program_impostor,    // tree impostors
    program_grass,       // grass
    program_water,       // the surface seen from above, into the water's own target
    program_water_flow,  // ... with a flow map
    program_water_lod,   // the far surface
    program_water_under, // the surface seen from below
    program_water_under_flow,
    program_water_mask,  // the stencil technique: depth and the view's cosine, before the reflections trace
    program_water_resolve, // the water's target into the scene, at the scene's samples
    program_local_map_water, // the LOD technique into the local map's scene
    // The reflection cube's faces: land and the sky, single-sampled.
    program_cube_terrain,
    program_cube_terrain_far,
    program_cube_sky_alpha,
    program_cube_sky_mask,
    program_cube_sky_add,
    program_water_ripple, // the wading ripples' passes
    program_mesh_opaque,
    program_mesh_alpha_test,
    program_mesh_blend,
    program_world_map_land,   // the world map's land and objects (the game's world-map bit)
    program_world_map_object,
    program_effect_opaque,
    program_effect_alpha_test,
    program_effect_blend,
    program_particle, // a particle system's camera-facing quads
    program_tonemap, // fullscreen: resolved HDR scene -> the 8-bit output target
    program_refraction,    // refraction surfaces -> the refraction target (section 13)
    program_is_refraction, // fullscreen: the scene displaced by the refraction target -> a single-sampled HDR target
    program_effect_sky, // effect_blend in the sky's place in the opaque pass: a weather's sky model (aurora)
    // The sky, in the opaque pass: alpha-blended (atmosphere, clouds), the moon
    // mask (depth only), additive (stars, sun and moon discs).
    program_sky_alpha,
    program_sky_mask,
    program_sky_add,
    program_precipitation, // rain and snow (the game ENVCUBE), additive, after the blended draws
    // Image space: fullscreen passes into their own targets.
    program_ao_minify,    // ISSAOCameraZ, ISMinify / ISMinifyContrast -> R32F mips
    program_ao_raw,       // ISSAORawAONoTemporal -> RGBA8
    program_ao_blur,      // ISSAOBlurH / V -> RGBA8
    program_snow_sss,     // ISSnowSSS: snow pixels of the scene, rewritten
    program_ssr_trace,    // ReflectionsRayTracing -> RGBA16F, half size
    program_ssr_blur,     // ReflectionBlurH / V
    program_lens_down,    // ISDownsampleCS and the first ISIBLensFlares
    program_lens_streak,  // ISDownsample and the second ISIBLensFlares
    program_lens_blur,    // ISBlur over the streaks
    program_composite,    // ISSAOCompositeSAOFog as a blend into the multisampled scene: c * A + S
    program_vl_apply,     // ISApplyVolumetricLighting (raymarched in the pass itself) -> R16F
    program_vl_blur,      // ISVolumetricLightingBlurH / V -> R16F
    program_vl_composite, // the vertical blur and ISCompositeLensFlareVolumetricLighting: additive into the scene
    program_hdr_down4,    // ISHDRDownSample4 -> RGBA16F
    program_hdr_lum4,     // ISHDRDownSample4RGB2Lum
    program_hdr_lum16,    // ISHDRDownSample16Lum
    program_hdr_adapt,    // ISHDRDownSample16LightAdapt -> the 2x2 adapted luminance
    program_bloom_bright, // ISBrightPassBlur (vertical)
    program_bloom_blur,   // ISBlur (horizontal)
    program_radial_blur,  // ISRadialBlurHigh (HDR, before the tonemap)
    program_world_map_blur, // ISWorldMap (HDR, first)
    program_local_map,      // ISLocalMap (an offscreen view's own image)
    program_blur_mix,     // the full-screen blur's last pass
    program_ldr_down4,    // ISHDRDownSample4 of the tonemapped image -> RGBA8 quarter
    program_dof_blur,     // ISDepthOfFieldBlurH / V (bilateral) -> RGBA8 quarter
    program_dof,          // ISDistantBlur -> the output target
    program_double_vision, // ISDoubleVision (IMAD), output -> output
    program_skin_decal,    // skinned decals, over their actors' posed vertices
    program_decal,         // box decals into the multisampled scene, before the composite
    program_screen_blood,  // The game's splats over the output
    program_shadow_opaque,     // shadow casters: depth only into a shadow map
    program_shadow_alpha_test, // shadow casters discarding by the diffuse alpha
    program_shadow_land,       // land into the god-ray map
    program_shadow_land_far,   // ... where it isn't at full resolution
    program_portal_depth,  // depth only, clip-space positions from a buffer: a doorway's depth (renderer_portal.cpp)
    program_portal_copy,   // fullscreen: a door's far scene into the main frame's scene
    program_portal_save,   // fullscreen: the far scene kept at the end of its frame
    program_portal_stamp,  // fullscreen, blended: a door's far image over the finished image
    k_program_count,
};

inline constexpr uint32_t k_blend_factor_count = 11; // NiAlphaProperty: ONE .. SRC_ALPHA_SATURATE

inline bool is_blend_program(Program p) {
    return p == program_mesh_blend || p == program_effect_blend || p == program_effect_sky || p == program_particle;
}

// Specialised pipelines of the lighting shader (k_spec_* in shaders/common.glsl). Each covers a set of shader types,
// technique bits and material flags; the compiler drops the code, and the registers, of everything else. Without the
// multi-pass sparkle, whose discard would cost every draw its early depth test, nothing discards. They nest: each
// covers the ones before it, and the last covers every material. The opaque and alpha-tested lighting programs have
// one pipeline per variant; every other program is unspecialised.
enum MeshVariant : uint8_t {
    mesh_variant_plain,     // default, glow map and LOD objects: no rim, soft or back lighting, no projected UV
    mesh_variant_lit,       // + environment maps, trees, rim, soft and back lighting
    mesh_variant_projected, // + projected UV and snow
    mesh_variant_general,   // + billboards, parallax, multilayer, faces, hair, eyes
    mesh_variant_full,      // + the multi-pass sparkle (type 14): unspecialised
    k_mesh_variant_count,
};

struct MeshVariantSpec {
    uint32_t types;     // bit t: shader type t
    uint32_t technique; // technique bits 0-23 the shader may see set
    uint32_t flags;     // material flags (GpuMaterial::flags)
};
MeshVariantSpec mesh_variant_spec(MeshVariant v);
// The narrowest variant that covers a lighting material.
MeshVariant mesh_variant(uint32_t technique, uint32_t flags);

// Packed per-submesh draw state; sorting by it groups pipeline and dynamic-state changes.
struct DrawState {
    Program program = program_mesh_opaque;
    uint8_t blend_src = 6;
    uint8_t blend_dst = 7;
    MeshVariant variant = mesh_variant_full;
    bool two_sided = false;
    bool decal = false;
    bool z_write = true;
    bool z_test = true;
    bool membrane = false; // a blended run's only: the membrane's depth bias

    uint32_t pack() const {
        return (uint32_t(membrane) << 24) | (uint32_t(program) << 16) | (uint32_t(blend_src) << 12) | (uint32_t(blend_dst) << 8) |
               (uint32_t(variant) << 4) | (uint32_t(two_sided) << 3) | (uint32_t(decal) << 2) |
               (uint32_t(z_write) << 1) | uint32_t(z_test);
    }
    static DrawState unpack(uint32_t k) {
        DrawState s;
        s.program = static_cast<Program>((k >> 16) & 0xFF);
        s.blend_src = static_cast<uint8_t>((k >> 12) & 0xF);
        s.blend_dst = static_cast<uint8_t>((k >> 8) & 0xF);
        s.variant = static_cast<MeshVariant>((k >> 4) & 0xF);
        s.two_sided = (k >> 3) & 1;
        s.decal = (k >> 2) & 1;
        s.z_write = (k >> 1) & 1;
        s.z_test = k & 1;
        s.membrane = (k >> 24) & 1;
        return s;
    }
};

struct PipelineFormats {
    VkFormat color;  // the scene's HDR target
    VkFormat depth;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkFormat output; // tonemap target
    VkFormat refraction = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat normal = VK_FORMAT_UNDEFINED; // the opaque pass's normal target (none: a single colour attachment)
    bool image_space = true;               // build the sky, precipitation and image-space programs
    VkFormat shadow = VK_FORMAT_D16_UNORM; // shadow maps (the game's R16 depth)
};

// The normals of the game's RT 68 in xy, its snow target (RT 112) in zw: snow specular, snow mask.
inline constexpr VkFormat k_normal_format = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat k_ao_format = VK_FORMAT_R8G8B8A8_UNORM;     // RawAO / blur targets (RT 62, 46)
inline constexpr VkFormat k_depth_z_format = VK_FORMAT_R32_SFLOAT;     // camera-Z pyramid (RT 50..)
inline constexpr VkFormat k_vl_format = VK_FORMAT_R16G16_SFLOAT; // volumetric lighting (RT 43, 44) and 1 - depth
inline constexpr VkFormat k_ldr_format = VK_FORMAT_R8G8B8A8_UNORM;     // the tonemapped image (RT 116) and DOF

class Pipelines {
public:
    Pipelines(Device& device, JobSystem& jobs, VkDescriptorSetLayout bindless_layout,
              const std::filesystem::path& shader_dir, const std::filesystem::path& cache_file,
              PipelineFormats formats);
    ~Pipelines();
    Pipelines(const Pipelines&) = delete;
    Pipelines& operator=(const Pipelines&) = delete;

    VkPipelineLayout layout() const { return m_layout; }
    // Blend programs ignore src/dst when wireframe (debug view uses standard alpha blending). The variant selects the
    // specialised pipeline of the opaque and alpha-tested lighting programs (solid only).
    VkPipeline get(Program program, bool wireframe, uint32_t src = 6, uint32_t dst = 7,
                   MeshVariant variant = mesh_variant_full) const;
    // The volumetric lighting volume (shaders/vl_volume.comp); buffers only, so a layout of its own.
    VkPipeline volume_pipeline() const { return m_volume; }
    VkPipelineLayout volume_layout() const { return m_volume_layout; }
    VkDescriptorSetLayout volume_set_layout() const { return m_volume_set_layout; } // its two storage images
    // Posing skinned shapes (shaders/skin.comp): buffer addresses only.
    VkPipeline skin_pipeline() const { return m_skin; }
    VkPipeline skin_decal_pipeline() const { return m_skin_decal; }
    VkPipelineLayout skin_decal_layout() const { return m_skin_decal_layout; }
    VkPipelineLayout skin_layout() const { return m_skin_layout; }
    uint32_t count() const { return m_count; }
    double creation_ms() const { return m_creation_ms; }

private:
    void log_statistics(VkDevice dev) const;

    Device& m_device;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_volume = VK_NULL_HANDLE;
    VkPipelineLayout m_volume_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_volume_set_layout = VK_NULL_HANDLE;
    VkPipeline m_skin = VK_NULL_HANDLE;
    VkPipeline m_skin_decal = VK_NULL_HANDLE;
    VkPipelineLayout m_skin_decal_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_skin_layout = VK_NULL_HANDLE;
    VkPipelineCache m_cache = VK_NULL_HANDLE;
    std::filesystem::path m_cache_file;
    // [program][wire] for non-blend; blend solid at [program] in m_blend.
    std::array<std::array<VkPipeline, 2>, k_program_count> m_basic{};
    std::array<std::vector<VkPipeline>, k_program_count> m_blend; // src * 11 + dst
    // Specialised solid pipelines of the variant programs; the full variant is m_basic's.
    std::array<std::array<VkPipeline, k_mesh_variant_count>, k_program_count> m_variants{};
    uint32_t m_count = 0;
    double m_creation_ms = 0;
};

} // namespace spud::render
