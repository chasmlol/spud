// Shadow maps: the sun's cascades drawn from the casters the engine culled into each, before
// the scene pass, which evaluates the game's shadow mask from them (shaders/shadows.glsl). Kept out of renderer.cpp
// so the main frame changes in a few lines.

#include "render/renderer.hpp"

#include "core/trace.hpp"
#include "render/shadows.hpp"

#include <algorithm>
#include <cstring>

namespace spud::render {

namespace {

constexpr VkDeviceSize k_cmd_stride = sizeof(VkDrawIndexedIndirectCommand);
constexpr uint32_t k_god_ray_map_size = 512; // target 3 in every capture with a sun

enum ShadowPass : uint32_t { // PushConstants::mode, SHADOW_PASS_* in shaders/common.glsl
    shadow_pass_sun,
    shadow_pass_spot,
    shadow_pass_paraboloid,
};

void layers_barrier(VkCommandBuffer cmd, VkImage image, uint32_t layers, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 src, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

void Renderer::set_shadow_resolution(uint32_t resolution) {
    if (resolution == m_shadow_resolution) {
        return;
    }
    wait_idle();
    destroy_shadow_maps();
    m_shadow_resolution = resolution;
    create_shadow_maps();
}

void Renderer::create_shadow_maps() {
    if (m_shadow_resolution == 0) {
        m_shadow_resolution = ShadowSettings{}.resolution;
    }
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_D16_UNORM;
    info.extent = {m_shadow_resolution, m_shadow_resolution, 1};
    info.mipLevels = 1;
    info.arrayLayers = k_max_cascades;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    // Cleared and redrawn every frame they are used: nothing in them outlives the frame.
    vk_check(m_device->create_image(info, ai, Lifetime::frame, "sun shadow cascades", m_sun_shadow.image,
                                    m_sun_shadow.alloc));
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = m_sun_shadow.image;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    v.format = VK_FORMAT_D16_UNORM;
    v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, k_max_cascades};
    vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &m_sun_shadow.view));
    for (uint32_t i = 0; i < k_max_cascades; ++i) {
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
        vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &m_sun_shadow_layers[i]));
    }
    m_bindless->write(Bindless::k_sun_shadow_slot, m_sun_shadow.view);

    info.extent = {k_god_ray_map_size, k_god_ray_map_size, 1};
    info.arrayLayers = k_max_cascades;
    vk_check(m_device->create_image(info, ai, Lifetime::frame, "god-ray map", m_god_ray_map.image, m_god_ray_map.alloc));
    v.image = m_god_ray_map.image;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, k_max_cascades};
    vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &m_god_ray_map.view));
    for (uint32_t i = 0; i < k_max_cascades; ++i) {
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
        vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &m_god_ray_layers[i]));
    }
    m_bindless->write(Bindless::k_god_ray_slot, m_god_ray_map.view);

    info.extent = {m_shadow_resolution, m_shadow_resolution, 1};
    info.arrayLayers = k_max_shadow_lights;
    vk_check(m_device->create_image(info, ai, Lifetime::frame, "shadow light maps", m_light_shadow.image,
                                    m_light_shadow.alloc));
    v.image = m_light_shadow.image;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, k_max_shadow_lights};
    vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &m_light_shadow.view));
    for (uint32_t i = 0; i < k_max_shadow_lights; ++i) {
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
        vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &m_light_shadow_layers[i]));
    }
    m_bindless->write(Bindless::k_light_shadow_slot, m_light_shadow.view);
}

void Renderer::destroy_shadow_maps() {
    VkDevice dev = m_device->device();
    for (VkImageView& v : m_sun_shadow_layers) {
        if (v) {
            vkDestroyImageView(dev, v, nullptr);
        }
        v = VK_NULL_HANDLE;
    }
    if (m_sun_shadow.view) {
        vkDestroyImageView(dev, m_sun_shadow.view, nullptr);
        vmaDestroyImage(m_device->allocator(), m_sun_shadow.image, m_sun_shadow.alloc);
    }
    m_sun_shadow = Target{};
    for (VkImageView& v : m_god_ray_layers) {
        if (v) {
            vkDestroyImageView(dev, v, nullptr);
        }
        v = VK_NULL_HANDLE;
    }
    if (m_god_ray_map.view) {
        vkDestroyImageView(dev, m_god_ray_map.view, nullptr);
        vmaDestroyImage(m_device->allocator(), m_god_ray_map.image, m_god_ray_map.alloc);
    }
    m_god_ray_map = Target{};
    for (VkImageView& v : m_light_shadow_layers) {
        if (v) {
            vkDestroyImageView(dev, v, nullptr);
        }
        v = VK_NULL_HANDLE;
    }
    if (m_light_shadow.view) {
        vkDestroyImageView(dev, m_light_shadow.view, nullptr);
        vmaDestroyImage(m_device->allocator(), m_light_shadow.image, m_light_shadow.alloc);
    }
    m_light_shadow = Target{};
}

size_t Renderer::shadow_group_count(const SpaceView& space) const {
    size_t groups = 0;
    auto add = [&](const DrawList* list) {
        if (list) {
            for (const DrawItem& item : list->items()) {
                groups += m_meshes->groups(item.mesh, item.variant).size();
            }
        }
    };
    if (space.sun_shadows) {
        for (const DrawList* list : space.shadow_draws) {
            add(list); // the cascade
            add(list); // the god-ray map
        }
    }
    for (size_t i = 0; i < space.shadow_lights.size(); ++i) {
        // A paraboloid's two halves each build the list's runs.
        add(space.shadow_light_draws[i]);
        add(space.shadow_light_draws[i]);
    }
    return groups;
}

void Renderer::build_shadow_runs(Frame& frame, const DrawList& draws, std::vector<Run>& runs,
                                 VkDeviceAddress& draw_instances, uint32_t casters) {
    runs.clear();
    draw_instances = 0;
    const MeshManager& meshes = *m_meshes;
    if (m_group_counts.size() < meshes.group_count()) {
        m_group_counts.resize(meshes.group_count() + 1024, 0);
    }
    // Four buckets by caster state (alpha test, two-sided); within each, one instanced command per draw group.
    constexpr uint32_t k_states = 8;
    std::array<std::vector<uint32_t>, k_states>& buckets = m_shadow_buckets;
    uint32_t instanced = 0;
    for (const DrawItem& item : draws.items()) {
        for (uint32_t g : meshes.groups(item.mesh, item.variant)) {
            const DrawGroup& dg = meshes.group(g);
            if ((dg.shadow & casters) == 0) {
                continue;
            }
            if (m_group_counts[g]++ == 0) {
                // The god-ray map culls nothing.
                buckets[(dg.shadow | (casters == shadow_god_ray ? shadow_two_sided : 0u)) & (k_states - 1)].push_back(g);
            }
            ++instanced;
        }
    }
    if (instanced == 0) {
        return;
    }
    uint32_t commands = 0;
    for (const auto& b : buckets) {
        commands += static_cast<uint32_t>(b.size());
    }
    const VkDeviceSize inst_offset = arena_alloc(frame, VkDeviceSize(instanced) * sizeof(GpuDrawInstance), 16);
    const VkDeviceSize cmd_offset = arena_alloc(frame, VkDeviceSize(commands) * k_cmd_stride, 16);
    auto* inst = reinterpret_cast<GpuDrawInstance*>(frame.arena.as<std::byte>() + inst_offset);
    auto* cmds = reinterpret_cast<VkDrawIndexedIndirectCommand*>(frame.arena.as<std::byte>() + cmd_offset);
    draw_instances = frame.arena.address + inst_offset;
    uint32_t next_instance = 0;
    uint32_t next_cmd = 0;
    for (uint32_t state = 0; state < k_states; ++state) {
        auto& b = buckets[state];
        if (b.empty()) {
            continue;
        }
        const uint32_t first = next_cmd;
        for (uint32_t g : b) {
            const DrawGroup& dg = meshes.group(g);
            const uint32_t count = m_group_counts[g];
            cmds[next_cmd++] = {dg.index_count, count, dg.first_index, dg.vertex_offset, next_instance};
            m_stats.shadow_triangles += uint64_t(dg.index_count / 3) * count;
            m_group_counts[g] = next_instance;
            next_instance += count;
        }
        runs.push_back({state, cmd_offset + VkDeviceSize(first) * k_cmd_stride, next_cmd - first});
    }
    for (const DrawItem& item : draws.items()) {
        for (uint32_t g : meshes.groups(item.mesh, item.variant)) {
            const DrawGroup& dg = meshes.group(g);
            if ((dg.shadow & casters) != 0) {
                inst[m_group_counts[g]++] = {item.instance, dg.material};
            }
        }
    }
    for (auto& b : buckets) {
        for (uint32_t g : b) {
            m_group_counts[g] = 0;
        }
        b.clear();
    }
}

void Renderer::record_shadow_runs(VkCommandBuffer cmd, Frame& fr, const std::vector<Run>& runs,
                                  const PushConstants& pc, bool always_fragment) {
    vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
    VkBuffer vb = m_meshes->vertices().buffer().buffer;
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
    vkCmdBindIndexBuffer(cmd, m_meshes->indices().buffer().buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthTestEnable(cmd, VK_TRUE);
    vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
    vkCmdSetDepthBiasEnable(cmd, VK_FALSE);
    VkPipeline bound = VK_NULL_HANDLE;
    for (const Run& run : runs) {
        const Program program = always_fragment || (run.state & shadow_alpha_test) ? program_shadow_alpha_test
                                                                                   : program_shadow_opaque;
        VkPipeline p = m_pipelines->get(program, false);
        if (p != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            bound = p;
        }
        // The game's rasterizer states: back faces culled (front counter-clockwise), none when two-sided.
        vkCmdSetCullMode(cmd, (run.state & shadow_two_sided) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
        vkCmdDrawIndexedIndirect(cmd, fr.arena.buffer, run.offset, run.count, k_cmd_stride);
        ++m_stats.draw_calls;
        ++m_stats.shadow_draw_calls;
    }
}

void Renderer::record_light_shadows(VkCommandBuffer cmd, Frame& fr, const SpaceView& space, VkDeviceSize frame_offset,
                                    const PushConstants& base) {
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(space.shadow_lights.size(), k_max_shadow_lights));
    if (count == 0) {
        if (!m_light_shadow_ready) {
            layers_barrier(cmd, m_light_shadow.image, k_max_shadow_lights, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            m_light_shadow_ready = true;
        }
        return;
    }
    SPUD_TRACE("light shadows");
    layers_barrier(cmd, m_light_shadow.image, k_max_shadow_lights, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_NONE,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    const uint32_t size = m_shadow_resolution;
    for (uint32_t i = 0; i < count; ++i) {
        const ShadowLight& light = space.shadow_lights[i];
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = m_light_shadow_layers[i];
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {size, size}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        // A spot and a hemisphere fill the layer; an omni light's front half takes the top, its back half the bottom.
        const uint32_t passes = light.type == ShadowLightType::omni ? 2 : 1;
        const bool paraboloid = light.type != ShadowLightType::spot;
        vkCmdBeginRendering(cmd, &ri);
        for (uint32_t h = 0; h < passes; ++h) {
            const VkDeviceSize offset = arena_alloc(fr, sizeof(GpuFrame), 256);
            std::memcpy(fr.arena.as<std::byte>() + offset, fr.arena.as<std::byte>() + frame_offset, sizeof(GpuFrame));
            auto* gf = reinterpret_cast<GpuFrame*>(fr.arena.as<std::byte>() + offset);
            gf->view_proj = light.render[h];
            gf->shadow_filter.y = paraboloid ? 1.0f / light.param : 0.0f;
            gf->shadow_filter.z = h == 0 ? 1.0f : -1.0f;
            VkDeviceAddress draw_instances = 0;
            if (space.shadow_light_draws[i]) {
                build_shadow_runs(fr, *space.shadow_light_draws[i], m_runs_shadow, draw_instances);
            } else {
                m_runs_shadow.clear();
            }
            if (m_runs_shadow.empty()) {
                continue;
            }
            const float height = passes == 2 ? float(size) * 0.5f : float(size);
            const VkViewport vp{0, float(h) * height, float(size), height, 0.0f, 1.0f};
            const VkRect2D scissor{{0, int32_t(h * uint32_t(height))}, {size, uint32_t(height)}};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            PushConstants pc = base;
            pc.frame = fr.arena.address + offset;
            pc.draw_instances = draw_instances;
            pc.mode = paraboloid ? shadow_pass_paraboloid : shadow_pass_spot;
            record_shadow_runs(cmd, fr, m_runs_shadow, pc, paraboloid);
        }
        vkCmdEndRendering(cmd);
    }
    layers_barrier(cmd, m_light_shadow.image, k_max_shadow_lights, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_light_shadow_ready = true;
}

void Renderer::record_sun_shadows(VkCommandBuffer cmd, Frame& fr, const SpaceView& space, VkDeviceSize frame_offset,
                                  const PushConstants& base) {
    const SunShadows* sun = space.sun_shadows;
    const uint32_t count = sun ? sun->count : 0;
    if (count == 0) {
        if (!m_sun_shadow_ready) {
            // Never sampled without cascades, but kept in the layout its descriptor names.
            for (VkImage image : {m_sun_shadow.image, m_god_ray_map.image}) {
                layers_barrier(cmd, image, k_max_cascades, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
            m_sun_shadow_ready = true;
        }
        return;
    }
    SPUD_TRACE("sun shadows");
    // A held map's layer stays as it is, ready to sample. It is the last, so what's drawn is the layers before it.
    const uint32_t drawn = sun->drawn();
    const uint32_t layers = sun->hold_last ? drawn : k_max_cascades;
    // The previous frame's lighting read the maps: its reads finish before they are cleared.
    for (VkImage image : {m_sun_shadow.image, m_god_ray_map.image}) {
        layers_barrier(cmd, image, layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_NONE,
                       VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }
    const VkExtent2D extent{m_shadow_resolution, m_shadow_resolution};
    for (uint32_t c = 0; c < drawn; ++c) {
        // This cascade's frame constants: the main view's, projected into the map.
        const VkDeviceSize offset = arena_alloc(fr, sizeof(GpuFrame), 256);
        std::memcpy(fr.arena.as<std::byte>() + offset, fr.arena.as<std::byte>() + frame_offset, sizeof(GpuFrame));
        auto* gf = reinterpret_cast<GpuFrame*>(fr.arena.as<std::byte>() + offset);
        gf->view_proj = sun->cascades[c].clip;

        VkDeviceAddress draw_instances = 0;
        if (space.shadow_draws[c]) {
            build_shadow_runs(fr, *space.shadow_draws[c], m_runs_shadow, draw_instances, shadow_sun);
        } else {
            m_runs_shadow.clear();
        }

        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = m_sun_shadow_layers[c];
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0}; // the game's standard Z
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, extent};
        ri.layerCount = 1;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        if (!m_runs_shadow.empty()) {
            const VkViewport vp{0, 0, float(extent.width), float(extent.height), 0.0f, 1.0f};
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            PushConstants pc = base;
            pc.frame = fr.arena.address + offset;
            pc.draw_instances = draw_instances;
            pc.mode = shadow_pass_sun;
            record_shadow_runs(cmd, fr, m_runs_shadow, pc, false);
        }
        vkCmdEndRendering(cmd);

        // The god-ray map's layer: the same camera, land and what casts no ordinary shadow, nothing culled.
        VkDeviceAddress god_ray_instances = 0;
        if (space.shadow_draws[c]) {
            build_shadow_runs(fr, *space.shadow_draws[c], m_runs_shadow, god_ray_instances, shadow_god_ray);
        } else {
            m_runs_shadow.clear();
        }
        const VkExtent2D small{k_god_ray_map_size, k_god_ray_map_size};
        depth.imageView = m_god_ray_layers[c];
        ri.renderArea = {{0, 0}, small};
        vkCmdBeginRendering(cmd, &ri);
        const TerrainDraws& land = m_god_ray_land[c];
        if (!m_runs_shadow.empty() || land.total() > 0) {
            const VkViewport vp{0, 0, float(small.width), float(small.height), 0.0f, 1.0f};
            const VkRect2D scissor{{0, 0}, small};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            PushConstants pc = base;
            pc.frame = fr.arena.address + offset;
            pc.draw_instances = god_ray_instances;
            pc.mode = shadow_pass_sun;
            if (!m_runs_shadow.empty()) {
                record_shadow_runs(cmd, fr, m_runs_shadow, pc, false);
            }
            if (land.total() > 0) {
                const TerrainFrame frame_land = write_terrain(fr, land);
                vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
                vkCmdSetDepthTestEnable(cmd, VK_TRUE);
                vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
                vkCmdSetDepthBiasEnable(cmd, VK_FALSE);
                vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
                vkCmdBindIndexBuffer(cmd, space.terrain->index_buffer(), 0, VK_INDEX_TYPE_UINT32);
                if (frame_land.near_count > 0) {
                    pc.terrain_cells = frame_land.near;
                    vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_shadow_land, false));
                    vkCmdDrawIndexed(cmd, space.terrain->near_index_count(), frame_land.near_count, 0, 0, 0);
                    ++m_stats.draw_calls;
                }
                if (frame_land.far_count > 0) {
                    pc.terrain_cells = frame_land.far;
                    vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_shadow_land_far, false));
                    vkCmdDrawIndexed(cmd, space.terrain->far_index_count(), frame_land.far_count,
                                     space.terrain->far_first_index(), 0, 0);
                    ++m_stats.draw_calls;
                }
            }
        }
        vkCmdEndRendering(cmd);
    }
    layers_barrier(cmd, m_god_ray_map.image, layers, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkImageLayout layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    VkAccessFlags2 access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (m_shadow_capture_requested && !sun->hold_last) {
        // Checks (screenshot_shadow): the cascades as drawn, copied out.
        layers_barrier(cmd, m_sun_shadow.image, layers, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage,
                       access, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkDeviceSize bytes = VkDeviceSize(m_shadow_resolution) * m_shadow_resolution * 2 * count;
        if (!m_shadow_capture_buffer || m_shadow_capture_buffer.size < bytes) {
            m_device->destroy_buffer(m_shadow_capture_buffer);
            m_shadow_capture_buffer = m_device->create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                              MemoryUsage::readback, Lifetime::transfer,
                                                              "shadow capture");
        }
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, count};
        region.imageExtent = {m_shadow_resolution, m_shadow_resolution, 1};
        vkCmdCopyImageToBuffer(cmd, m_sun_shadow.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_shadow_capture_buffer.buffer, 1, &region);
        m_shadow_capture_requested = false;
        m_shadow_capture_frame = m_frame_number + 1;
        m_shadow_capture_slot = m_slot;
        m_shadow_capture_layers = count;
        layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        access = VK_ACCESS_2_NONE;
    }
    layers_barrier(cmd, m_sun_shadow.image, layers, layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, stage,
                   access, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_sun_shadow_ready = true;
}

bool Renderer::take_shadow_capture(std::vector<uint16_t>& texels, uint32_t& layers, uint32_t& size) {
    if (m_shadow_capture_frame == 0) {
        return false;
    }
    Frame& fr = m_frames[m_shadow_capture_slot];
    if (fr.number == m_shadow_capture_frame) {
        vk_check(vkWaitForFences(m_device->device(), 1, &fr.fence, VK_TRUE, UINT64_MAX));
    }
    vmaInvalidateAllocation(m_device->allocator(), m_shadow_capture_buffer.allocation, 0, VK_WHOLE_SIZE);
    layers = m_shadow_capture_layers;
    size = m_shadow_resolution;
    texels.resize(size_t(size) * size * layers);
    std::memcpy(texels.data(), m_shadow_capture_buffer.as<uint8_t>(), texels.size() * sizeof(uint16_t));
    m_shadow_capture_frame = 0;
    return true;
}

} // namespace spud::render
