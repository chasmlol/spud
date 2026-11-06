#include "render/pipelines.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "render/gpu_types.hpp"

#include <chrono>
#include <cstddef>
#include <format>
#include <map>
#include <string>

namespace spud::render {

namespace {

VkBlendFactor ni_blend_factor(uint32_t f) {
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ONE;
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_SRC_COLOR;
    case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_DST_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

struct ProgramInfo {
    const char* vert;
    const char* frag;
    enum class Input { none, mesh, sky } input; // terrain pulls its vertices from a buffer
    enum class Target { scene, output, refraction, hdr, post, scene_colour, shadow, water } target = Target::scene;
    // output: the final 8-bit target, no depth; refraction: the refraction target with the single-sampled depth;
    // hdr: a single-sampled HDR target, no depth; post: a single-sampled target of `format`, no depth;
    // scene_colour: the multisampled scene colour alone; shadow: a shadow map's depth alone, standard Z (frag may be
    // null); water: a single-sampled target of `format` with the water's own depth.
    enum class Blend { none, alpha, add_alpha, add, one_src_alpha, no_colour } blend = Blend::none;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool opaque_pass = false; // drawn in the opaque pass: two colour attachments when the set has a normal format
    bool normals = false;     // ... and writes the second one
};

ProgramInfo post(const char* frag, VkFormat format) {
    ProgramInfo i{"tonemap.vert", frag, ProgramInfo::Input::none, ProgramInfo::Target::post};
    i.format = format;
    return i;
}

ProgramInfo program_info(Program p) {
    switch (p) {
    case program_sky: {
        ProgramInfo i{"sky.vert", "sky.frag", ProgramInfo::Input::none};
        i.opaque_pass = true;
        return i;
    }
    case program_terrain: {
        ProgramInfo i{"terrain.vert", "terrain.frag", ProgramInfo::Input::none}; // vertex pulling
        i.opaque_pass = i.normals = true;
        return i;
    }
    case program_terrain_far: {
        ProgramInfo i{"terrain_far.vert", "terrain_far.frag", ProgramInfo::Input::none};
        i.opaque_pass = i.normals = true;
        return i;
    }
    case program_grass: {
        ProgramInfo i{"grass.vert", "grass.frag", ProgramInfo::Input::mesh};
        i.opaque_pass = i.normals = true;
        return i;
    }
    case program_impostor: {
        ProgramInfo i{"impostor.vert", "impostor.frag", ProgramInfo::Input::none};
        i.opaque_pass = i.normals = true;
        return i;
    }
    case program_water:
    case program_water_flow:
    case program_water_lod:
    case program_water_under:
    case program_water_under_flow:
    case program_water_mask: {
        const char* frag = p == program_water        ? "water.frag"
                           : p == program_water_flow ? "water_flow.frag"
                           : p == program_water_lod  ? "water_lod.frag"
                           : p == program_water_under ? "water_under.frag"
                           : p == program_water_under_flow ? "water_under_flow.frag"
                                                      : "water_mask.frag";
        ProgramInfo i{"water.vert", frag, ProgramInfo::Input::none, ProgramInfo::Target::water};
        i.format = p == program_water_mask ? VK_FORMAT_R8_UNORM : VK_FORMAT_R16G16B16A16_SFLOAT;
        return i;
    }
    case program_water_resolve: return {"water.vert", "water_resolve.frag", ProgramInfo::Input::none};
    case program_local_map_water: return {"water.vert", "water_lod.frag", ProgramInfo::Input::none};
    case program_water_ripple: return post("water_ripple.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_cube_terrain_far: {
        ProgramInfo i{"terrain_far.vert", "terrain_far.frag", ProgramInfo::Input::none, ProgramInfo::Target::water};
        i.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        return i;
    }
    case program_cube_terrain: {
        ProgramInfo i{"terrain.vert", "terrain.frag", ProgramInfo::Input::none, ProgramInfo::Target::water};
        i.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        return i;
    }
    case program_cube_sky_alpha:
    case program_cube_sky_mask:
    case program_cube_sky_add: {
        ProgramInfo i{"sky_mesh.vert", "sky_mesh.frag", ProgramInfo::Input::sky, ProgramInfo::Target::water};
        i.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        i.blend = p == program_cube_sky_alpha  ? ProgramInfo::Blend::alpha
                  : p == program_cube_sky_mask ? ProgramInfo::Blend::no_colour
                                               : ProgramInfo::Blend::add_alpha;
        return i;
    }
    case program_mesh_opaque:
    case program_mesh_alpha_test:
    case program_effect_opaque:
    case program_effect_alpha_test: {
        const char* frag = p == program_mesh_opaque       ? "mesh.frag"
                           : p == program_mesh_alpha_test ? "mesh_alpha_test.frag"
                           : p == program_effect_opaque   ? "effect.frag"
                                                          : "effect_alpha_test.frag";
        ProgramInfo i{"mesh.vert", frag, ProgramInfo::Input::mesh};
        i.opaque_pass = true;
        i.normals = p == program_mesh_opaque || p == program_mesh_alpha_test;
        return i;
    }
    case program_world_map_land:
    case program_world_map_object: {
        ProgramInfo i{"mesh.vert", p == program_world_map_land ? "world_map_land.frag" : "world_map_object.frag",
                      ProgramInfo::Input::mesh};
        i.opaque_pass = true;
        i.normals = true;
        return i;
    }
    case program_mesh_blend: return {"mesh.vert", "mesh_blend.frag", ProgramInfo::Input::mesh};
    case program_effect_blend: return {"mesh.vert", "effect_blend.frag", ProgramInfo::Input::mesh};
    case program_particle: return {"particle.vert", "particle.frag", ProgramInfo::Input::none};
    case program_skin_decal: return {"skin_decal.vert", "skin_decal.frag", ProgramInfo::Input::none};
    case program_tonemap: return post("tonemap.frag", k_ldr_format);
    case program_refraction:
        return {"mesh.vert", "refraction.frag", ProgramInfo::Input::mesh, ProgramInfo::Target::refraction};
    case program_is_refraction:
        return {"tonemap.vert", "is_refraction.frag", ProgramInfo::Input::none, ProgramInfo::Target::hdr};
    case program_effect_sky: {
        ProgramInfo i{"mesh.vert", "effect_blend.frag", ProgramInfo::Input::mesh};
        i.opaque_pass = true;
        return i;
    }
    case program_sky_alpha:
    case program_sky_mask:
    case program_sky_add: {
        ProgramInfo i{"sky_mesh.vert", "sky_mesh.frag", ProgramInfo::Input::sky};
        i.opaque_pass = true;
        i.blend = p == program_sky_alpha  ? ProgramInfo::Blend::alpha
                  : p == program_sky_mask ? ProgramInfo::Blend::no_colour
                                          : ProgramInfo::Blend::add_alpha;
        return i;
    }
    case program_precipitation: {
        ProgramInfo i{"precipitation.vert", "precipitation.frag", ProgramInfo::Input::none};
        i.blend = ProgramInfo::Blend::add_alpha;
        return i;
    }
    case program_ao_minify: return post("ao_minify.frag", k_depth_z_format);
    case program_ao_raw: return post("ao_raw.frag", k_ao_format);
    case program_ao_blur: return post("ao_blur.frag", k_ao_format);
    case program_lens_down: return post("lens_down.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_lens_streak: return post("lens_streak.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_lens_blur: return post("lens_blur.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_ssr_trace: return post("ssr_trace.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_ssr_blur: return post("ssr_blur.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_snow_sss: {
        ProgramInfo i = post("snow_sss.frag", VK_FORMAT_UNDEFINED);
        i.target = ProgramInfo::Target::scene_colour;
        return i;
    }
    case program_composite: {
        ProgramInfo i = post("composite.frag", VK_FORMAT_UNDEFINED);
        i.target = ProgramInfo::Target::scene_colour;
        i.blend = ProgramInfo::Blend::one_src_alpha;
        return i;
    }
    case program_vl_apply: return post("vl_apply.frag", k_vl_format);
    case program_vl_blur: return post("vl_blur.frag", k_vl_format);
    case program_vl_composite: {
        ProgramInfo i = post("vl_blur.frag", VK_FORMAT_UNDEFINED);
        i.target = ProgramInfo::Target::hdr; // the scene's format
        i.blend = ProgramInfo::Blend::add;
        return i;
    }
    case program_hdr_down4: return post("hdr_down4.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_hdr_lum4: return post("hdr_lum4.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_hdr_lum16: return post("hdr_lum16.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_hdr_adapt: return post("hdr_adapt.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_bloom_bright: return post("bloom_bright.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_bloom_blur: return post("bloom_blur.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_radial_blur: return post("radial_blur.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_world_map_blur: return post("world_map_blur.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_blur_mix: return post("blur_mix.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_ldr_down4: return post("hdr_down4.frag", k_ldr_format);
    case program_dof_blur: return post("dof_blur.frag", k_ldr_format);
    case program_local_map: {
        ProgramInfo i = post("local_map.frag", VK_FORMAT_UNDEFINED);
        i.target = ProgramInfo::Target::output;
        return i;
    }
    case program_dof:
    case program_double_vision: {
        ProgramInfo i = post(p == program_dof ? "dof.frag" : "double_vision.frag", VK_FORMAT_UNDEFINED);
        i.target = ProgramInfo::Target::output;
        return i;
    }
    case program_decal: {
        ProgramInfo i{"decal.vert", "decal.frag", ProgramInfo::Input::none, ProgramInfo::Target::scene_colour};
        i.blend = ProgramInfo::Blend::alpha;
        return i;
    }
    case program_screen_blood: {
        ProgramInfo i = post("screen_blood.frag", VK_FORMAT_UNDEFINED);
        i.target = ProgramInfo::Target::output;
        i.blend = ProgramInfo::Blend::alpha;
        return i;
    }
    case program_shadow_opaque:
        return {"shadow.vert", nullptr, ProgramInfo::Input::mesh, ProgramInfo::Target::shadow};
    case program_shadow_land:
        return {"shadow_land.vert", nullptr, ProgramInfo::Input::none, ProgramInfo::Target::shadow};
    case program_shadow_land_far:
        return {"shadow_land_far.vert", nullptr, ProgramInfo::Input::none, ProgramInfo::Target::shadow};
    case program_shadow_alpha_test:
        return {"shadow.vert", "shadow.frag", ProgramInfo::Input::mesh, ProgramInfo::Target::shadow};
    case program_portal_depth: return {"portal.vert", "portal.frag", ProgramInfo::Input::none};
    case program_portal_copy: return {"tonemap.vert", "portal_copy.frag", ProgramInfo::Input::none};
    case program_portal_save: return post("portal_copy.frag", VK_FORMAT_R16G16B16A16_SFLOAT);
    case program_portal_stamp: {
        ProgramInfo i = post("portal_stamp.frag", VK_FORMAT_UNDEFINED);
        i.target = ProgramInfo::Target::output;
        i.blend = ProgramInfo::Blend::alpha;
        return i;
    }
    default: return {"", "", ProgramInfo::Input::none};
    }
}

bool is_image_space(Program p) { return p >= program_sky_alpha; }

bool has_variants(Program p) { return p == program_mesh_opaque || p == program_mesh_alpha_test; }

// Technique bits the lighting shader branches on.
constexpr uint32_t k_tech_soft = 1u << 10, k_tech_rim = 1u << 11, k_tech_back = 1u << 12;
constexpr uint32_t k_tech_projected_uv = 1u << 15, k_tech_snow = 1u << 21;
constexpr uint32_t k_tech_features = 0x00FFFFFFu;

} // namespace

MeshVariantSpec mesh_variant_spec(MeshVariant v) {
    // Shader types: 0 default, 1 environment map, 2 glow map, 12 tree, 13 and 15 LOD objects; 14 the sparkle pass.
    constexpr uint32_t plain_types = (1u << 0) | (1u << 2) | (1u << 13) | (1u << 15);
    constexpr uint32_t lit_types = plain_types | (1u << 1) | (1u << 12);
    // Lighting materials are never effects or refraction surfaces: those have their own programs.
    constexpr uint32_t general_flags = ~(k_material_effect_bit | k_material_refraction);
    constexpr uint32_t flags = general_flags & ~k_material_billboard;
    constexpr uint32_t lit = k_tech_features & ~(k_tech_projected_uv | k_tech_snow);
    switch (v) {
    case mesh_variant_plain: return {plain_types, lit & ~(k_tech_soft | k_tech_rim | k_tech_back), flags};
    case mesh_variant_lit: return {lit_types, lit, flags};
    case mesh_variant_projected: return {lit_types, k_tech_features, flags};
    case mesh_variant_general: return {~(1u << 14), ~0u, general_flags};
    default: return {~0u, ~0u, ~0u};
    }
}

MeshVariant mesh_variant(uint32_t technique, uint32_t flags) {
    const uint32_t type = technique >> 24;
    for (uint32_t v = 0; v < k_mesh_variant_count; ++v) {
        const MeshVariantSpec s = mesh_variant_spec(static_cast<MeshVariant>(v));
        if (type < 32 && ((s.types >> type) & 1u) && (technique & k_tech_features & ~s.technique) == 0 &&
            (flags & ~s.flags) == 0) {
            return static_cast<MeshVariant>(v);
        }
    }
    return mesh_variant_full;
}

Pipelines::Pipelines(Device& device, JobSystem& jobs, VkDescriptorSetLayout bindless_layout,
                     const std::filesystem::path& shader_dir, const std::filesystem::path& cache_file,
                     PipelineFormats formats)
    : m_device(device), m_cache_file(cache_file) {
    const auto start = std::chrono::steady_clock::now();
    VkDevice dev = device.device();

    VkPushConstantRange push{VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &bindless_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    vk_check(vkCreatePipelineLayout(dev, &layout_info, nullptr, &m_layout));

    std::vector<std::byte> cache_data;
    std::error_code ec;
    if (!cache_file.empty() && std::filesystem::exists(cache_file, ec)) {
        try {
            cache_data = read_file(cache_file);
        } catch (const std::exception&) {
            cache_data.clear();
        }
    }
    VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cache_info.initialDataSize = cache_data.size();
    cache_info.pInitialData = cache_data.empty() ? nullptr : cache_data.data();
    if (vkCreatePipelineCache(dev, &cache_info, nullptr, &m_cache) != VK_SUCCESS) {
        cache_info.initialDataSize = 0;
        cache_info.pInitialData = nullptr;
        vk_check(vkCreatePipelineCache(dev, &cache_info, nullptr, &m_cache));
    }

    // Shader modules, shared by all variants.
    std::map<std::string, VkShaderModule> modules;
    auto module = [&](const char* name) {
        auto it = modules.find(name);
        if (it != modules.end()) {
            return it->second;
        }
        const auto path = shader_dir / (std::string(name) + ".spv");
        const auto code = read_file(path);
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size();
        info.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m;
        vk_check(vkCreateShaderModule(dev, &info, nullptr, &m));
        modules.emplace(name, m);
        return m;
    };
    auto built = [&](int p) { return formats.image_space || !is_image_space(static_cast<Program>(p)); };
    for (int p = 0; p < k_program_count; ++p) {
        if (!built(p)) {
            continue;
        }
        auto info = program_info(static_cast<Program>(p));
        module(info.vert);
        if (info.frag) {
            module(info.frag);
        }
    }

    const bool wire_supported = [&] {
        VkPhysicalDeviceFeatures f;
        vkGetPhysicalDeviceFeatures(device.physical(), &f);
        return f.fillModeNonSolid == VK_TRUE;
    }();

    struct Desc {
        Program program;
        bool wire;
        uint32_t src;
        uint32_t dst;
        VkPipeline* out;
        MeshVariant variant = mesh_variant_full;
    };
    std::vector<Desc> descs;
    for (int p = 0; p < k_program_count; ++p) {
        const auto prog = static_cast<Program>(p);
        if (!built(p)) {
            continue;
        }
        if (is_image_space(prog)) {
            descs.push_back({prog, false, 0, 0, &m_basic[p][0]}); // no wireframe view of these
        } else if (is_blend_program(prog)) {
            m_blend[p].assign(k_blend_factor_count * k_blend_factor_count, VK_NULL_HANDLE);
            for (uint32_t s = 0; s < k_blend_factor_count; ++s) {
                for (uint32_t d = 0; d < k_blend_factor_count; ++d) {
                    descs.push_back({prog, false, s, d, &m_blend[p][s * k_blend_factor_count + d]});
                }
            }
            descs.push_back({prog, true, 6, 7, &m_basic[p][1]});
        } else {
            descs.push_back({prog, false, 0, 0, &m_basic[p][0]});
            descs.push_back({prog, true, 0, 0, &m_basic[p][1]});
            if (has_variants(prog)) {
                for (uint32_t v = 0; v < mesh_variant_full; ++v) {
                    descs.push_back({prog, false, 0, 0, &m_variants[p][v], static_cast<MeshVariant>(v)});
                }
            }
        }
    }

    auto build = [&, formats](const Desc& d) {
        const auto info = program_info(d.program);
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = modules.at(info.vert);
        stages[0].pName = "main";
        if (info.frag) {
            stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = modules.at(info.frag);
            stages[1].pName = "main";
        }
        // k_spec_types, k_spec_technique, k_spec_flags (shaders/common.glsl); the full variant keeps the defaults.
        const MeshVariantSpec spec = mesh_variant_spec(d.variant);
        const VkSpecializationMapEntry entries[3] = {
            {0, offsetof(MeshVariantSpec, types), sizeof(uint32_t)},
            {1, offsetof(MeshVariantSpec, technique), sizeof(uint32_t)},
            {2, offsetof(MeshVariantSpec, flags), sizeof(uint32_t)},
        };
        const VkSpecializationInfo specialization{3, entries, sizeof(spec), &spec};
        if (d.variant != mesh_variant_full) {
            stages[0].pSpecializationInfo = &specialization;
            stages[1].pSpecializationInfo = &specialization;
        }

        VkVertexInputBindingDescription binding{0, 0, VK_VERTEX_INPUT_RATE_VERTEX};
        std::vector<VkVertexInputAttributeDescription> attrs;
        switch (info.input) {
        case ProgramInfo::Input::none:
            break;
        case ProgramInfo::Input::mesh:
            // osm::Vertex: the game's own packing (docs/formats/osm.md).
            binding.stride = 36;
            attrs = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 16},
                     {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 24},      {3, 0, VK_FORMAT_R8G8B8A8_UNORM, 28},
                     {4, 0, VK_FORMAT_R8G8B8A8_UNORM, 32}};
            break;
        case ProgramInfo::Input::sky:
            binding.stride = 24; // render::SkyVertex
            attrs = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
                     {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 20}};
            break;
        }
        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (info.input != ProgramInfo::Input::none) {
            vertex_input.vertexBindingDescriptionCount = 1;
            vertex_input.pVertexBindingDescriptions = &binding;
            vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
            vertex_input.pVertexAttributeDescriptions = attrs.data();
        }

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = (d.wire && wire_supported) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        rs.depthClampEnable = d.program == program_portal_depth ? VK_TRUE : VK_FALSE;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        const bool scene_target = info.target == ProgramInfo::Target::scene;
        const bool scene_colour = info.target == ProgramInfo::Target::scene_colour;
        ms.rasterizationSamples = scene_target || scene_colour ? formats.samples : VK_SAMPLE_COUNT_1_BIT;
        // The composite's fog and the decals' box follow each sample's depth.
        if ((d.program == program_composite || d.program == program_decal) && formats.samples != VK_SAMPLE_COUNT_1_BIT) {
            ms.sampleShadingEnable = VK_TRUE;
            ms.minSampleShading = 1.0f;
        }
        // Alpha-tested foliage: coverage from alpha (docs/RENDERING.md), the test itself when single-sampled.
        ms.alphaToCoverageEnable =
            scene_target && formats.samples != VK_SAMPLE_COUNT_1_BIT &&
                    (d.program == program_mesh_alpha_test || d.program == program_effect_alpha_test ||
                     d.program == program_impostor || d.program == program_grass)
                ? VK_TRUE
                : VK_FALSE;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        const bool shadow_target = info.target == ProgramInfo::Target::shadow;
        // Reverse-Z, except the shadow maps: the game's standard Z, LESS_EQUAL, cleared to 1. The doorway's depth replaces
        // whatever is there (with the test off it would not be written).
        ds.depthCompareOp = shadow_target                     ? VK_COMPARE_OP_LESS_OR_EQUAL
                            : d.program == program_portal_depth ? VK_COMPARE_OP_ALWAYS
                                                                : VK_COMPARE_OP_GREATER_OR_EQUAL;

        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                             VK_COLOR_COMPONENT_A_BIT;
        if (d.program == program_skin_decal) { // DESTCOLOR, INVSRCALPHA
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.colorBlendOp = VK_BLEND_OP_ADD;
            att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (is_blend_program(d.program)) {
            const uint32_t src = d.src;
            const uint32_t dst = d.dst;
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = ni_blend_factor(src);
            att.dstColorBlendFactor = ni_blend_factor(dst);
            att.colorBlendOp = VK_BLEND_OP_ADD;
            att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.alphaBlendOp = VK_BLEND_OP_ADD;
            // Destination alpha stays 1 so DEST_ALPHA factors behave like an opaque framebuffer.
            att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        }
        constexpr VkColorComponentFlags rgb =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        auto blend = [&](VkBlendFactor src, VkBlendFactor dst, VkBlendOp op) {
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = src;
            att.dstColorBlendFactor = dst;
            att.colorBlendOp = op;
            att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.alphaBlendOp = VK_BLEND_OP_ADD;
            att.colorWriteMask = rgb;
        };
        switch (info.blend) {
        case ProgramInfo::Blend::none: break;
        case ProgramInfo::Blend::alpha:
            blend(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD);
            break;
        case ProgramInfo::Blend::add_alpha:
            blend(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD);
            break;
        case ProgramInfo::Blend::add: blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD); break;
        case ProgramInfo::Blend::one_src_alpha:
            blend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_OP_ADD);
            break;
        case ProgramInfo::Blend::no_colour: att.colorWriteMask = 0; break;
        }
        if (d.program == program_portal_depth) {
            att.colorWriteMask = 0;
        }
        // The opaque pass has a second attachment, the normals; programs that don't shade surfaces leave it alone.
        const bool two_attachments = info.opaque_pass && formats.normal != VK_FORMAT_UNDEFINED;
        VkPipelineColorBlendAttachmentState atts[2] = {att, {}};
        atts[1].colorWriteMask = info.normals ? (rgb | VK_COLOR_COMPONENT_A_BIT) : 0;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = shadow_target ? 0 : (two_attachments ? 2 : 1);
        cb.pAttachments = atts;

        const VkDynamicState dyn[] = {
            VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,           VK_DYNAMIC_STATE_CULL_MODE,
            VK_DYNAMIC_STATE_FRONT_FACE,        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS,
        };
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = static_cast<uint32_t>(std::size(dyn));
        dy.pDynamicStates = dyn;

        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        VkPipelineFragmentShadingRateStateCreateInfoKHR coarse{
            VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR};
        coarse.fragmentSize = {2, 1};
        coarse.combinerOps[0] = coarse.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
        if (m_device.coarse_foliage() && d.program == program_mesh_alpha_test) {
            rendering.pNext = &coarse;
        }
        const VkFormat scene_formats[2] = {formats.color, formats.normal};
        rendering.colorAttachmentCount = shadow_target ? 0 : (two_attachments ? 2 : 1);
        rendering.pColorAttachmentFormats = info.target == ProgramInfo::Target::output       ? &formats.output
                                            : info.target == ProgramInfo::Target::refraction ? &formats.refraction
                                            : info.target == ProgramInfo::Target::post ||
                                                      info.target == ProgramInfo::Target::water
                                                ? &info.format
                                                                                             : scene_formats;
        rendering.depthAttachmentFormat = shadow_target ? formats.shadow
                                          : scene_target || info.target == ProgramInfo::Target::refraction ||
                                                    info.target == ProgramInfo::Target::water
                                              ? formats.depth
                                              : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.pNext = &rendering;
        pi.stageCount = info.frag ? 2 : 1;
        pi.pStages = stages;
        pi.pVertexInputState = &vertex_input;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &dy;
        pi.layout = m_layout;
        if (device.pipeline_stats()) {
            pi.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        }
        vk_check(vkCreateGraphicsPipelines(dev, m_cache, 1, &pi, nullptr, d.out));
    };

    if (formats.image_space) {
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VolumePushConstants)};
        VkDescriptorSetLayoutBinding storage[2]{};
        for (uint32_t b = 0; b < 2; ++b) {
            storage[b].binding = b;
            storage[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            storage[b].descriptorCount = 1;
            storage[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 2;
        sl.pBindings = storage;
        vk_check(vkCreateDescriptorSetLayout(dev, &sl, nullptr, &m_volume_set_layout));
        const VkDescriptorSetLayout sets[2] = {bindless_layout, m_volume_set_layout};
        VkPipelineLayoutCreateInfo cl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        cl.setLayoutCount = 2;
        cl.pSetLayouts = sets;
        cl.pushConstantRangeCount = 1;
        cl.pPushConstantRanges = &range;
        vk_check(vkCreatePipelineLayout(dev, &cl, nullptr, &m_volume_layout));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module("vl_volume.comp");
        ci.stage.pName = "main";
        ci.layout = m_volume_layout;
        vk_check(vkCreateComputePipelines(dev, m_cache, 1, &ci, nullptr, &m_volume));

        VkPushConstantRange skin_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPushConstants)};
        VkPipelineLayoutCreateInfo sk{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        sk.pushConstantRangeCount = 1;
        sk.pPushConstantRanges = &skin_range;
        vk_check(vkCreatePipelineLayout(dev, &sk, nullptr, &m_skin_layout));
        ci.stage.module = module("skin.comp");
        ci.layout = m_skin_layout;
        vk_check(vkCreateComputePipelines(dev, m_cache, 1, &ci, nullptr, &m_skin));

        VkPushConstantRange decal_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinDecalPushConstants)};
        sk.pPushConstantRanges = &decal_range;
        vk_check(vkCreatePipelineLayout(dev, &sk, nullptr, &m_skin_decal_layout));
        ci.stage.module = module("skin_decal.comp");
        ci.layout = m_skin_decal_layout;
        vk_check(vkCreateComputePipelines(dev, m_cache, 1, &ci, nullptr, &m_skin_decal));
    }

    // Startup only: fan the variants out over the workers.
    for (const auto& d : descs) {
        jobs.submit(0.0f, [&build, d] { build(d); });
    }
    jobs.wait_idle();
    for (const auto& d : descs) {
        if (*d.out == VK_NULL_HANDLE) {
            throw Error("pipeline creation failed");
        }
    }
    m_count = static_cast<uint32_t>(descs.size());
    for (int p = 0; p < k_program_count; ++p) {
        if (has_variants(static_cast<Program>(p))) {
            m_variants[p][mesh_variant_full] = m_basic[p][0];
        }
    }
    if (device.pipeline_stats()) {
        log_statistics(dev);
    }
    for (auto& [name, m] : modules) {
        vkDestroyShaderModule(dev, m, nullptr);
    }
    m_creation_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    log::info("pipelines: {} created in {:.0f} ms (cache {} KiB)", m_count, m_creation_ms, cache_data.size() >> 10);
}

Pipelines::~Pipelines() {
    VkDevice dev = m_device.device();
    if (!m_cache_file.empty()) {
        size_t size = 0;
        if (vkGetPipelineCacheData(dev, m_cache, &size, nullptr) == VK_SUCCESS && size > 0) {
            std::vector<std::byte> data(size);
            if (vkGetPipelineCacheData(dev, m_cache, &size, data.data()) == VK_SUCCESS) {
                try {
                    write_file(m_cache_file, std::span(data.data(), size));
                } catch (const std::exception& e) {
                    log::warn("pipelines: cannot save cache: {}", e.what());
                }
            }
        }
    }
    for (auto& per : m_basic) {
        for (auto p : per) {
            if (p) {
                vkDestroyPipeline(dev, p, nullptr);
            }
        }
    }
    for (auto& v : m_blend) {
        for (auto p : v) {
            if (p) {
                vkDestroyPipeline(dev, p, nullptr);
            }
        }
    }
    for (auto& v : m_variants) {
        for (uint32_t i = 0; i < mesh_variant_full; ++i) { // the full one is m_basic's
            if (v[i]) {
                vkDestroyPipeline(dev, v[i], nullptr);
            }
        }
    }
    if (m_skin) {
        vkDestroyPipeline(dev, m_skin, nullptr);
        vkDestroyPipelineLayout(dev, m_skin_layout, nullptr);
    }
    if (m_volume) {
        vkDestroyPipeline(dev, m_volume, nullptr);
        vkDestroyPipelineLayout(dev, m_volume_layout, nullptr);
        vkDestroyDescriptorSetLayout(dev, m_volume_set_layout, nullptr);
    }
    vkDestroyPipelineCache(dev, m_cache, nullptr);
    vkDestroyPipelineLayout(dev, m_layout, nullptr);
}

// SPUD_PIPELINE_STATS=1 (Device::pipeline_stats): the driver's statistics of each program's solid pipeline
// and of every lighting variant.
void Pipelines::log_statistics(VkDevice dev) const {
    auto properties = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR"));
    auto statistics = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR"));
    if (!properties || !statistics) {
        return;
    }
    std::vector<std::pair<std::string, VkPipeline>> list;
    for (int p = 0; p < k_program_count; ++p) {
        const auto prog = static_cast<Program>(p);
        if (!m_basic[p][0] && !is_blend_program(prog)) {
            continue;
        }
        const std::string name = program_info(prog).frag ? program_info(prog).frag : program_info(prog).vert;
        const bool blend = is_blend_program(prog);
        list.emplace_back(name, blend ? m_blend[p][6 * k_blend_factor_count + 7] : m_basic[p][0]);
        if (has_variants(prog)) {
            for (uint32_t v = 0; v < mesh_variant_full; ++v) {
                list.emplace_back(std::format("{} variant {}", name, v), m_variants[p][v]);
            }
        }
    }
    for (const auto& [name, pipeline] : list) {
        VkPipelineInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
        info.pipeline = pipeline;
        uint32_t count = 0;
        properties(dev, &info, &count, nullptr);
        VkPipelineExecutablePropertiesKHR blank{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
        std::vector<VkPipelineExecutablePropertiesKHR> execs(count, blank);
        properties(dev, &info, &count, execs.data());
        for (uint32_t e = 0; e < count; ++e) {
            VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
            ei.pipeline = pipeline;
            ei.executableIndex = e;
            uint32_t n = 0;
            statistics(dev, &ei, &n, nullptr);
            VkPipelineExecutableStatisticKHR empty{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR};
            std::vector<VkPipelineExecutableStatisticKHR> stats(n, empty);
            statistics(dev, &ei, &n, stats.data());
            std::string line;
            for (const auto& s : stats) {
                const auto& v = s.value;
                std::string value;
                switch (s.format) {
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: value = std::to_string(v.b32); break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: value = std::to_string(v.i64); break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: value = std::to_string(v.u64); break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: value = std::format("{}", v.f64); break;
                default: break;
                }
                line += std::format(" {}={}", s.name, value);
            }
            log::info("pipeline stats: {} {}:{}", name, execs[e].name, line);
        }
    }
}

VkPipeline Pipelines::get(Program program, bool wireframe, uint32_t src, uint32_t dst,
                          MeshVariant variant) const {
    if (!wireframe && variant < mesh_variant_full && has_variants(program)) {
        return m_variants[program][variant];
    }
    if (is_blend_program(program) && !wireframe) {
        src = std::min(src, k_blend_factor_count - 1);
        dst = std::min(dst, k_blend_factor_count - 1);
        return m_blend[program][src * k_blend_factor_count + dst];
    }
    return m_basic[program][wireframe ? 1 : 0];
}

} // namespace spud::render
