// Water: the game's order around its deferred composite. Before the reflections trace, the stencil
// technique puts the surface's depth and view cosine in place; after the composite, the surfaces shade into the water's
// own single-sampled target, blended over its last image (ISWaterBlend); the main pass takes that target into the
// scene through the surface's own geometry, so its edges get the scene's samples.

#include "render/renderer.hpp"

#include "core/trace.hpp"
#include "core/units.hpp"

#include <cstring>

namespace spud::render {

namespace {

constexpr auto k_color_out = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
constexpr auto k_fragment = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr auto k_transfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
constexpr auto k_depth_tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
constexpr uint32_t k_grid = 32; // quads per side of a cell's water
constexpr uint32_t k_cube_size = 512;
constexpr uint32_t k_cube_faces_per_frame = 2;
constexpr float k_cube_faces_per_second = 120.0f; // two a frame at the game's 60

void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 src, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access,
             uint32_t layers = 1) {
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
    b.subresourceRange = {aspect, 0, 1, 0, layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

void Renderer::create_water_targets(VkExtent2D extent) {
    using B = Bindless;
    m_water_mask = make_post_target(VK_FORMAT_R8_UNORM, extent, B::k_water_mask_slot, Lifetime::frame, "water mask");
    for (uint32_t i = 0; i < 2; ++i) {
        m_water_views[0].history[i] = make_post_target(VK_FORMAT_R16G16B16A16_SFLOAT, extent, B::k_water_history_slot + i,
                                                       Lifetime::simulation, "water history");
        m_water_views[1].history[i] = make_post_target(VK_FORMAT_R16G16B16A16_SFLOAT, extent, B::k_water_door_history_slot + i,
                                                       Lifetime::simulation, "water history");
    }
    for (uint32_t i = 0; i < 2; ++i) {
        m_water_ripples[i] = make_post_target(VK_FORMAT_R16G16B16A16_SFLOAT, {512, 512}, B::k_water_ripple_slot + i,
                                              Lifetime::simulation, "water ripples");
    }
    m_water_ripples_ready = false;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = k_depth_format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vk_check(m_device->create_image(info, ai, Lifetime::frame, "water depth", m_water_depth.image, m_water_depth.alloc));
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = m_water_depth.image;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v.format = k_depth_format;
    v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &m_water_depth.view));
    m_bindless->write(B::k_water_depth_slot, m_water_depth.view);
    // The cube's faces keep their size whatever the window's is, so they can't share the depth above.
    info.extent = {k_cube_size, k_cube_size, 1};
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    vk_check(m_device->create_image(info, ai, Lifetime::frame, "water cube depth", m_water_cube_depth.image,
                                    m_water_cube_depth.alloc));
    v.image = m_water_cube_depth.image;
    vk_check(vkCreateImageView(m_device->device(), &v, nullptr, &m_water_cube_depth.view));

    if (!m_water_flow.image) {
        VkImageCreateInfo f{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        f.imageType = VK_IMAGE_TYPE_2D;
        f.format = VK_FORMAT_R8G8B8A8_UNORM;
        f.extent = {704, 704, 1};
        f.mipLevels = 1;
        f.arrayLayers = 1;
        f.samples = VK_SAMPLE_COUNT_1_BIT;
        f.tiling = VK_IMAGE_TILING_OPTIMAL;
        f.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        vk_check(m_device->create_image(f, ai, Lifetime::cache, "water flow", m_water_flow.image, m_water_flow.alloc));
        VkImageViewCreateInfo fv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        fv.image = m_water_flow.image;
        fv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        fv.format = f.format;
        fv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_check(vkCreateImageView(m_device->device(), &fv, nullptr, &m_water_flow.view));
        m_bindless->write(B::k_water_flow_slot, m_water_flow.view);
    }
    const uint32_t cube_slots[2] = {B::k_water_cube_slot, B::k_water_door_cube_slot};
    for (uint32_t v = 0; v < 2; ++v) {
        WaterView& wv = m_water_views[v];
        if (wv.cube) {
            continue;
        }
        VkImageCreateInfo c{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        c.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        c.imageType = VK_IMAGE_TYPE_2D;
        c.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        c.extent = {512, 512, 1};
        c.mipLevels = 1;
        c.arrayLayers = 6;
        c.samples = VK_SAMPLE_COUNT_1_BIT;
        c.tiling = VK_IMAGE_TILING_OPTIMAL;
        c.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        vk_check(m_device->create_image(c, ai, Lifetime::cache, "water reflection", wv.cube, wv.cube_alloc));
        VkImageViewCreateInfo cv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        cv.image = wv.cube;
        cv.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        cv.format = c.format;
        cv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        vk_check(vkCreateImageView(m_device->device(), &cv, nullptr, &wv.cube_view));
        wv.cube_slot = cube_slots[v];
        m_bindless->write(wv.cube_slot, wv.cube_view);
        for (uint32_t f = 0; f < 6; ++f) {
            cv.viewType = VK_IMAGE_VIEW_TYPE_2D;
            cv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
            vk_check(vkCreateImageView(m_device->device(), &cv, nullptr, &wv.cube_faces[f]));
        }
    }
}

void Renderer::destroy_water_targets() {
    VkDevice dev = m_device->device();
    for (PostTarget* t : {&m_water_mask, &m_water_views[0].history[0], &m_water_views[0].history[1], &m_water_views[1].history[0], &m_water_views[1].history[1], &m_water_ripples[0], &m_water_ripples[1]}) {
        if (t->view) {
            vkDestroyImageView(dev, t->view, nullptr);
            vmaDestroyImage(m_device->allocator(), t->image, t->alloc);
            *t = PostTarget{};
        }
    }
    for (Target* t : {&m_water_depth, &m_water_cube_depth}) {
        if (t->view) {
            vkDestroyImageView(dev, t->view, nullptr);
            vmaDestroyImage(m_device->allocator(), t->image, t->alloc);
            *t = Target{};
        }
    }
    m_water_views[0].have_last = m_water_views[1].have_last = false;
}

VkDeviceAddress Renderer::write_water_data(Frame& frame, const View& view, const SpaceView& space, const RenderSettings& rs,
                                           bool second_view) {
    using B = Bindless;
    const WaterFrame& wf = *space.water;
    const Environment& env = space.environment;
    constexpr float k = units::k_skyrim_unit_m;
    const VkDeviceSize materials_bytes = wf.materials.size_bytes(), surfaces_bytes = wf.surfaces.size_bytes();
    const VkDeviceSize materials = arena_alloc(frame, materials_bytes, 16);
    const VkDeviceSize surfaces = arena_alloc(frame, surfaces_bytes, 16);
    const VkDeviceSize placed = arena_alloc(frame, std::max<VkDeviceSize>(wf.placed.size_bytes(), 16), 16);
    const VkDeviceSize offset = arena_alloc(frame, sizeof(GpuWater), 16);
    std::memcpy(frame.arena.as<std::byte>() + placed, wf.placed.data(), wf.placed.size_bytes());
    std::memcpy(frame.arena.as<std::byte>() + materials, wf.materials.data(), materials_bytes);
    std::memcpy(frame.arena.as<std::byte>() + surfaces, wf.surfaces.data(), surfaces_bytes);

    WaterView& wv = water_view();
    const uint32_t current = second_view ? wv.current : (wv.current ^= 1u);
    GpuWater w{};
    // The last frame's clip position of a point given relative to this frame's camera.
    const glm::mat4 moved = glm::translate(glm::mat4(1.0f), view.position - wv.last_camera);
    w.previous_view_proj = wv.last_view_proj * moved;
    const float tan_y = std::tan(view.fov_y * 0.5f);
    w.camera_z = glm::vec4(view.near_plane / k, k, tan_y * view.aspect, tan_y);
    const float extent_x = float(m_target_extent.width), extent_y = float(m_target_extent.height);
    w.screen = glm::vec4(1.0f / extent_x, 1.0f / extent_y, 250.0f / (env.far_units - env.near_units), 0.0f);
    const glm::vec3 right(view.rotation[0][0], view.rotation[1][0], view.rotation[2][0]);
    const glm::vec3 up(view.rotation[0][1], view.rotation[1][1], view.rotation[2][1]);
    const glm::vec3 forward(-view.rotation[0][2], -view.rotation[1][2], -view.rotation[2][2]);
    w.camera_forward = glm::vec4(forward, env.near_units);
    w.sun_dir_range = glm::vec4(wf.sun_dir, 1.0f / 1.2f);
    w.sun_color = wf.sun_color;
    w.eye = glm::vec4(view.position / k, 0.0f);
    w.params = glm::vec4(!rs.hold_effect_state && wf.clock_running && wv.have_last ? 1.0f : 0.0f, 0.0f,
                         second_view ? 1.0f : 0.0f, 0.0f);
    w.default_normal = glm::vec4(right.z, up.z, forward.z, 0.0f);
    w.quad_rect = wf.quad_rect;
    w.ripple = glm::vec4(wf.ripple_shift, 10.24f / 2048.0f, float(wf.ripple_stamps));
    std::copy(wf.ripple_segments.begin(), wf.ripple_segments.end(), w.ripple_segments);
    std::copy(wf.light_positions.begin(), wf.light_positions.end(), w.light_positions);
    std::copy(wf.light_colors.begin(), wf.light_colors.end(), w.light_colors);
    w.light_count = glm::vec4(float(wf.light_count));
    w.slots = glm::uvec4(B::k_snow_source_slot, B::k_depth_slot, wv.cube_slot, B::k_water_ripple_slot);
    w.slots2 = glm::uvec4(B::k_ssr_slot, B::k_ssr_raw_slot, B::k_water_flow_slot, wf.flow_normals);
    w.slots3 = glm::uvec4(wv.history[current ^ 1u].slot, wv.history[current].slot,
                          B::k_water_mask_slot, 0);
    w.materials = frame.arena.address + materials;
    w.instances = frame.arena.address + surfaces;
    w.placed = frame.arena.address + placed;
    std::memcpy(frame.arena.as<std::byte>() + offset, &w, sizeof(w));
    if (!second_view) {
        wv.last_view_proj = view.view_proj;
        wv.last_camera = view.position;
        wv.have_last = true;
    }
    return frame.arena.address + offset;
}

void Renderer::draw_water_surfaces(VkCommandBuffer cmd, PushConstants& pc, Program near, Program flow, Program far,
                                   const WaterFrame& water, bool wire, const Pipelines* pipelines) {
    const Pipelines& set = pipelines ? *pipelines : *m_pipelines;
    const uint32_t count = uint32_t(water.surfaces.size());
    auto draw = [&](Program program, uint32_t first, uint32_t last, uint32_t grid) {
        if (first >= last) {
            return;
        }
        pc.mode = grid;
        vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, set.get(program, wire));
        vkCmdDraw(cmd, grid * grid * 6, last - first, 0, first);
        ++m_stats.draw_calls;
    };
    draw(near, 0, water.flow_first, k_grid);
    draw(flow, water.flow_first, water.far_first, k_grid);
    draw(far, water.far_first, water.quad_first, 1);
    draw(water.quad_flow ? flow : near, water.quad_first, count, k_grid / 2);
    if (!water.placed.empty()) {
        pc.mode = 0;
        vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, set.get(near, wire));
        vkCmdDraw(cmd, uint32_t(water.placed.size()), 1, 0, 0);
        ++m_stats.draw_calls;
    }
    pc.mode = 0;
    vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
}

// The wading ripples' steps: shift and stamp, then the simulation, each into the other target.
void Renderer::record_water_ripples(VkCommandBuffer cmd, PushConstants& pc, const WaterFrame& water) {
    using B = Bindless;
    auto pass = [&](uint32_t mode) {
        const uint32_t source = m_water_ripple_current;
        m_water_ripple_current ^= 1u;
        post_pass(cmd, program_water_ripple, m_water_ripples[m_water_ripple_current], pc, mode, B::k_water_ripple_slot + source);
    };
    if (!m_water_ripples_ready || water.ripple_clear) {
        pass(0);
        pass(0);
        m_water_ripples_ready = true;
    }
    for (uint32_t i = 0; i < water.ripple_steps; ++i) {
        pass(1);
        pass(2);
    }
    pc.mode = 0;
}

VkDeviceAddress Renderer::write_particle_data(Frame& frame, const ParticleFrame& particles) {
    const VkDeviceSize array = arena_alloc(frame, particles.particles.size_bytes(), 16);
    const VkDeviceSize systems = arena_alloc(frame, particles.systems.size_bytes(), 16);
    const VkDeviceSize offset = arena_alloc(frame, sizeof(GpuParticles), 16);
    std::memcpy(frame.arena.as<std::byte>() + array, particles.particles.data(), particles.particles.size_bytes());
    std::memcpy(frame.arena.as<std::byte>() + systems, particles.systems.data(), particles.systems.size_bytes());
    const GpuParticles g{frame.arena.address + array, frame.arena.address + systems};
    std::memcpy(frame.arena.as<std::byte>() + offset, &g, sizeof(g));
    return frame.arena.address + offset;
}

// One instanced quad draw a system, in the main pass after the blended meshes' place.
void Renderer::record_particles(VkCommandBuffer cmd, PushConstants& pc, const ParticleFrame& particles, bool flip) {
    DrawState s;
    s.two_sided = true;
    s.z_write = false;
    apply_state(cmd, s, flip);
    vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
    for (const ParticleDraw& d : particles.draws) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->get(program_particle, false, d.blend_src, d.blend_dst));
        vkCmdDraw(cmd, 6, d.count, 0, d.first);
        ++m_stats.draw_calls;
    }
}

// New cells of the flow atlas. The camera moving a cell brings in a row of them at most; a jump, 25.
void Renderer::record_water_flow(VkCommandBuffer cmd, Frame& frame, const WaterFrame& water) {
    if (!m_water_flow_ready) {
        barrier(cmd, m_water_flow.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, k_fragment,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        m_water_flow_ready = true;
    }
    if (water.flow_uploads.empty()) {
        return;
    }
    barrier(cmd, m_water_flow.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, k_fragment, VK_ACCESS_2_NONE, k_transfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (const WaterFlowUpload& u : water.flow_uploads) {
        constexpr VkDeviceSize k_bytes = 64 * 64 * 4;
        const VkDeviceSize offset = arena_alloc(frame, k_bytes, 16);
        std::memcpy(frame.arena.as<std::byte>() + offset, u.texels, k_bytes);
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {int32_t(u.x * 64), int32_t(u.y * 64), 0};
        region.imageExtent = {64, 64, 1};
        vkCmdCopyBufferToImage(cmd, frame.arena.buffer, m_water_flow.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    barrier(cmd, m_water_flow.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, k_transfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, k_fragment,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

VkDeviceSize Renderer::water_arena_need(const FrameInput& input) const {
    VkDeviceSize need = 256 + input.screen_blood.size_bytes();
    for (const SpaceView& space : input.spaces) {
        if (space.water) {
            need += space.water->flow_uploads.size() * (64 * 64 * 4 + 16);
        }
        need += 256 + space.decals.size_bytes();
        if (space.particles) {
            need += 4096 + space.particles->particles.size_bytes() + space.particles->systems.size_bytes();
        }
        if (space.water) {
            need += 4096 * 4 + space.water->materials.size_bytes() + space.water->surfaces.size_bytes() +
                    space.water->placed.size_bytes();
            // The first frames fill every face.
            need += ((space.terrain ? space.terrain->stats().land_cells : 0) * 2 * sizeof(GpuFarNode) +
                     space.lights.size() * sizeof(GpuPointLight) + 4096) * 6;
        }
    }
    return need;
}

// two faces a frame of a cube at the eye, world aligned, not reflected in the plane (
//). Land and sky, without fog. Screen axes are Vulkan's cube-face axes, so a face is a mirrored view.
void Renderer::record_water_cube(VkCommandBuffer cmd, Frame& frame, const SpaceView& space, const FrameInput& input,
                                 VkDeviceAddress post) {
    const VkExtent2D extent{k_cube_size, k_cube_size};
    WaterView& wv = water_view();
    if (!wv.cube_ready) {
        barrier(cmd, wv.cube, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, k_fragment,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 6);
        wv.cube_ready = true;
    }
    static const glm::vec3 k_right[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    static const glm::vec3 k_up[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    static const glm::vec3 k_forward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    SpaceView s = space;
    s.sun_shadows = nullptr;
    s.environment.fog_param.w = 0.0f; // no fog: the faces show the land as it is
    RenderSettings rs = input.settings;
    const FrameStats main_stats = m_stats;
    VkDescriptorSet set = m_bindless->set(m_slot);
    // The game draws its two faces in a 60 Hz frame: by the world's clock, so a faster screen doesn't draw more of them.
    wv.cube_credit = std::min(wv.cube_credit + k_cube_faces_per_second * std::max(input.time - input.previous_time, 0.0f), 6.0f);
    uint32_t faces = wv.cube_warm < 3 ? 6
                     : space.water->underwater || !space.water->cube_needed
                         ? 0
                         : std::min(uint32_t(wv.cube_credit), rs.spread_water_cube ? 1u : k_cube_faces_per_frame);
    wv.cube_credit = wv.cube_warm < 3 ? 0.0f : wv.cube_credit - float(faces);
    ++wv.cube_warm;
    for (uint32_t i = 0; i < faces; ++i) {
        const uint32_t f = wv.cube_next;
        wv.cube_next = (f + 1) % 6;
        View view = *input.view;
        view.fov_y = glm::radians(90.0f);
        view.aspect = 1.0f;
        glm::mat4 r(1.0f);
        for (int c = 0; c < 3; ++c) {
            r[c][0] = k_right[f][c];
            r[c][1] = k_up[f][c];
            r[c][2] = -k_forward[f][c];
        }
        view.rotation = r;
        glm::mat4 p(0.0f);
        p[0][0] = 1.0f;
        p[1][1] = -1.0f;
        p[2][3] = -1.0f;
        p[3][2] = view.near_plane;
        view.projection = p;
        view.view_proj = p * r;
        view.frustum = Frustum::from_view_proj(view.view_proj);

        m_offscreen_terrain.clear();
        if (s.terrain && rs.draw_terrain) {
            s.terrain->collect(view.frustum, view.position, rs.terrain, m_offscreen_terrain);
        }
        const VkDeviceSize frame_offset =
            write_frame_data(frame, view, s, rs, input.time, input.previous_time, extent, 1);
        const VkDeviceAddress lights = write_point_lights(frame, view, s, rs);
        const TerrainFrame land = write_terrain(frame, m_offscreen_terrain);

        VkImageMemoryBarrier2 to_color{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_color.srcStageMask = k_fragment;
        to_color.dstStageMask = k_color_out;
        to_color.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_color.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_color.srcQueueFamilyIndex = to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_color.image = wv.cube;
        to_color.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_color;
        vkCmdPipelineBarrier2(cmd, &dep);
        barrier(cmd, m_water_cube_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, k_depth_tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                k_depth_tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = wv.cube_faces[f];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        // Where no land or sky draws: Sky Lower through the framebuffer's range.
        const glm::vec3 clear = s.environment.sky_ground * s.environment.fog_near_color.w;
        color.clearValue.color = {{clear.r, clear.g, clear.b, 1.0f}};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = m_water_cube_depth.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {0.0f, 0}; // reverse-Z
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport vp{0, 0, float(extent.width), float(extent.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines->layout(), 0, 1, &set, 0, nullptr);
        PushConstants pc{};
        pc.frame = frame.arena.address + frame_offset;
        pc.terrain_quads = s.terrain ? s.terrain->quads_address() : 0;
        pc.land_textures = s.terrain ? s.terrain->land_textures_address() : 0;
        pc.terrain_vertices = s.terrain ? s.terrain->vertex_address() : 0;
        pc.materials = m_meshes->materials_address();
        pc.lights = lights;
        pc.material_anims = m_material_anims_address;
        pc.post = post;
        vkCmdPushConstants(cmd, m_pipelines->layout(), VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(pc), &pc);
        vkCmdSetDepthBias(cmd, 0, 0, 0);
        if (s.terrain) {
            // A mirrored view.
            record_terrain(cmd, *s.terrain, land, pc, *m_pipelines, false, true, program_cube_terrain, program_cube_terrain_far);
        }
        if (s.environment.sky.exterior && s.environment.sky.draw_count > 0) {
            record_sky(cmd, frame, s.environment.sky, pc, false, true);
        }
        vkCmdEndRendering(cmd);
        to_color.srcStageMask = k_color_out;
        to_color.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_color.dstStageMask = k_fragment;
        to_color.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_color.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_color.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    m_stats = main_stats;
}

// The stencil technique (0x5000): the surface's depth over a copy of the scene's, and the view cosine.
void Renderer::record_water_mask(VkCommandBuffer cmd, PushConstants& pc, const WaterFrame& water) {
    const VkExtent2D extent = m_target_extent;
    barrier(cmd, m_depth_copy.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, k_fragment, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, k_transfer,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cmd, m_water_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, k_fragment, VK_ACCESS_2_NONE, k_transfer, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {extent.width, extent.height, 1};
    vkCmdCopyImage(cmd, m_depth_copy.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_water_depth.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cmd, m_depth_copy.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, k_transfer, VK_ACCESS_2_NONE, k_fragment,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cmd, m_water_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, k_transfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, k_depth_tests,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    barrier(cmd, m_water_mask.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, k_fragment, VK_ACCESS_2_NONE, k_color_out,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_water_mask.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_water_depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0, 0, float(extent.width), float(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    DrawState s;
    s.two_sided = true;
    apply_state(cmd, s, false);
    draw_water_surfaces(cmd, pc, program_water_mask, program_water_mask, program_water_mask, water, false);
    vkCmdEndRendering(cmd);

    barrier(cmd, m_water_mask.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, k_color_out, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, k_fragment,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cmd, m_water_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, k_depth_tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            k_fragment, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

// The surfaces into the water's own target, cleared so the blend knows where there was no water last frame.
void Renderer::record_water(VkCommandBuffer cmd, PushConstants& pc, const WaterFrame& water) {
    const VkExtent2D extent = m_target_extent;
    WaterView& wv = water_view();
    PostTarget& out = wv.history[wv.current];
    barrier(cmd, m_water_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, k_fragment, VK_ACCESS_2_NONE, k_depth_tests,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    barrier(cmd, out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            k_fragment, VK_ACCESS_2_NONE, k_color_out, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = out.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_water_depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0, 0, float(extent.width), float(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    DrawState s;
    s.two_sided = true;
    s.z_write = false;
    apply_state(cmd, s, false);
    draw_water_surfaces(cmd, pc, water.underwater ? program_water_under : program_water,
                        water.underwater ? program_water_under_flow : program_water_flow, program_water_lod, water, false);
    vkCmdEndRendering(cmd);
    barrier(cmd, m_water_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, k_depth_tests, VK_ACCESS_2_NONE, k_fragment,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cmd, out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, k_color_out, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, k_fragment,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

} // namespace spud::render
