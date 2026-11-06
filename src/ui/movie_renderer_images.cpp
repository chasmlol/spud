// Engine images for the movie renderer: textures from the cache, read and
// decoded on the renderer's workers and uploaded progressively, and render targets the engine draws.

#include "ui/movie_renderer.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "formats/dds.hpp"

#include <mutex>

namespace spud::ui {

struct MovieRenderer::EngineImages {
    struct Result {
        uint32_t id;
        uint32_t width, height;
        std::vector<std::byte> rgba; // straight alpha (to_straight_rgba)
        std::string error;
    };
    std::mutex mutex;
    std::vector<Result> done;
};

namespace {

// A decoded engine texture as GFx samples it: RGBA8 with straight alpha, filtered as it is and only then
// weighted by its alpha (drawn with engine_image_flags' 4). Measured in the game: GFx's default image between
// an opaque black cell and a magenta one at alpha 128 shows 0x27 over black where premultiplied filtering
// gives 0x15.
bool to_straight_rgba(const dds::Image& image, std::vector<std::byte>& out, std::string& error) {
    const dds::Subresource& top = image.subresource(0, 0);
    const auto* src = reinterpret_cast<const uint8_t*>(image.data.data() + top.offset);
    const size_t texels = size_t(top.width) * top.height;
    out.resize(texels * 4);
    auto* dst = reinterpret_cast<uint8_t*>(out.data());
    for (size_t i = 0; i < texels; ++i) {
        uint8_t r, g, b, a;
        switch (image.format) {
        case dds::Format::rgba8:
            r = src[4 * i], g = src[4 * i + 1], b = src[4 * i + 2], a = src[4 * i + 3];
            break;
        case dds::Format::bgra8:
            b = src[4 * i], g = src[4 * i + 1], r = src[4 * i + 2], a = src[4 * i + 3];
            break;
        case dds::Format::bgrx8:
            b = src[4 * i], g = src[4 * i + 1], r = src[4 * i + 2], a = 255;
            break;
        default:
            error = std::format("format {} (the importer writes interface images as RGBA8)",
                                dds::to_string(image.format));
            return false;
        }
        dst[4 * i] = r;
        dst[4 * i + 1] = g;
        dst[4 * i + 2] = b;
        dst[4 * i + 3] = a;
    }
    return true;
}

} // namespace

void MovieRenderer::set_engine_images(const player::ImageRegistry* images, render::JobSystem* jobs,
                                      std::filesystem::path cache_root) {
    m_images = images;
    m_jobs = jobs;
    m_cache_root = std::move(cache_root);
    if (!m_engine_loaded) {
        m_engine_loaded = std::make_shared<EngineImages>();
    }
}

uint32_t MovieRenderer::external_texture(VkImageView view) {
    const uint32_t slot = uint32_t(m_textures.size());
    m_textures.emplace_back();
    Texture& t = m_textures.back();
    t.view = view; // not ours: t.image stays null, so the destructor leaves it alone
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_texture_layout;
    render::vk_check(vkAllocateDescriptorSets(m_device.device(), &ai, &t.set));
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = t.set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(m_device.device(), 1, &w, 0, nullptr);
    t.ready = true;
    return slot;
}

void MovieRenderer::set_render_target_image(const std::string& name, VkImageView view) {
    if (!view) {
        m_render_targets.erase(name);
        return;
    }
    auto it = m_render_targets.find(name);
    if (it != m_render_targets.end() && m_textures[it->second].view == view) {
        return;
    }
    // A target is registered once, when the engine creates it; its descriptor set lives as long as the
    // renderer.
    m_render_targets[name] = external_texture(view);
}

uint32_t MovieRenderer::engine_image_flags(uint32_t id) const {
    const player::EngineImage* e = m_images ? m_images->get(id) : nullptr;
    return e ? 4u : 0u; // the render targets too: the local map's alpha is the straight alpha of its grey
}

uint32_t MovieRenderer::engine_image_texture(uint32_t id) {
    const player::EngineImage* e = m_images ? m_images->get(id) : nullptr;
    if (!e) {
        return ~0u;
    }
    if (e->source == player::ImageSource::render_target) {
        auto it = m_render_targets.find(e->name);
        return it == m_render_targets.end() ? ~0u : it->second;
    }
    if (id >= m_engine_slots.size()) {
        m_engine_slots.resize(id + 1, ~0u);
        m_engine_requested.resize(id + 1, 0);
    }
    if (m_engine_slots[id] != ~0u) {
        return m_textures[m_engine_slots[id]].ready ? m_engine_slots[id] : ~0u;
    }
    if (!m_engine_requested[id] && m_jobs && m_engine_loaded) {
        // Read and decode on a worker; the frame goes on without it (nothing of it is drawn until it is in).
        m_engine_requested[id] = 1;
        m_jobs->submit(0.0f, [shared = m_engine_loaded, id, file = m_cache_root / e->path] {
            EngineImages::Result r{id, 0, 0, {}, {}};
            try {
                const dds::Image image = dds::parse(read_file(file));
                if (to_straight_rgba(image, r.rgba, r.error)) {
                    r.width = image.width;
                    r.height = image.height;
                }
            } catch (const std::exception& ex) {
                r.error = ex.what();
            }
            if (!r.error.empty()) {
                r.error = file.generic_string() + ": " + r.error;
            }
            std::lock_guard lock(shared->mutex);
            shared->done.push_back(std::move(r));
        });
    }
    return ~0u;
}

void MovieRenderer::set_image_pixels(const std::string& name, const std::string& key, uint32_t width, uint32_t height,
                                     const std::vector<uint8_t>& rgba) {
    auto it = m_pixel_images.find(key);
    if (it == m_pixel_images.end()) {
        const auto* bytes = reinterpret_cast<const std::byte*>(rgba.data());
        it = m_pixel_images.emplace(key, create_texture(width, height, std::vector<std::byte>(bytes, bytes + rgba.size()))).first;
    }
    m_render_targets[name] = it->second;
}

void MovieRenderer::poll_engine_images() {
    if (!m_engine_loaded) {
        return;
    }
    std::vector<EngineImages::Result> done;
    {
        std::lock_guard lock(m_engine_loaded->mutex);
        if (m_engine_loaded->done.empty()) {
            return;
        }
        // A few per frame: interface images are small, and a book names one or two.
        const size_t n = std::min<size_t>(m_engine_loaded->done.size(), 4);
        done.assign(std::make_move_iterator(m_engine_loaded->done.begin()),
                    std::make_move_iterator(m_engine_loaded->done.begin() + ptrdiff_t(n)));
        m_engine_loaded->done.erase(m_engine_loaded->done.begin(),
                                    m_engine_loaded->done.begin() + ptrdiff_t(n));
    }
    for (auto& r : done) {
        if (!r.error.empty() || r.width == 0 || r.height == 0) {
            log::warn("ui: engine image {}", r.error.empty() ? "is empty" : r.error);
            continue;
        }
        if (r.id < m_engine_slots.size()) {
            m_engine_slots[r.id] = create_texture(r.width, r.height, std::move(r.rgba));
        }
    }
}

} // namespace spud::ui
