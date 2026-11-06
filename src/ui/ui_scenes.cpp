// The UI 3D scenes among the menus: each menu that shows one names it each frame
// (Menu::ui_scene), and its place in the stack follows the menu's display function: the item menus draw the
// movie and then their slot (InventoryMenu::Display), the Stats, Book and Magic menus their slot
// and then the movie (MagicMenu::Display
//). The Book Menu's pages are drawn off-screen first (from
// UI_RenderOffscreenMenus before the frame).

#include "ui/menu.hpp"
#include "ui/movie_renderer.hpp"
#include "ui/ui.hpp"

#include "render/renderer.hpp"

namespace spud::ui {

void Ui::add_menu_layers(Menu& m, float w, float h, std::vector<MovieLayer>& layers) {
    if (m_frame_scene_count == m_frame_scenes.size()) {
        m_frame_scenes.emplace_back();
    }
    UiScene& scene = m_frame_scenes[m_frame_scene_count];
    scene.clear();
    const bool has_scene = m.ui_scene(scene);
    const int index = has_scene ? int(m_frame_scene_count++) : -1;
    if (has_scene && !m.scene_after_movie()) {
        MovieLayer mark;
        mark.scene = index;
        layers.push_back(mark);
    }
    m.scene_pages(m_frame_pages);
    if (m.player() && m.movie_visible()) {
        m.player()->render(m.draw_list());
        layers.push_back({&m.draw_list(), m.stage_to_pixels(w, h), m.background()});
    }
    if (has_scene && m.scene_after_movie()) {
        MovieLayer mark;
        mark.scene = index;
        layers.push_back(mark);
    }
}

void Ui::add_page_layers(std::vector<MovieLayer>& layers) {
    for (const ScenePage& p : m_frame_pages) {
        if (!p.list || p.page < 0 || p.page >= int(MovieRenderer::k_pages)) {
            continue;
        }
        MovieLayer l;
        l.list = p.list;
        l.stage_to_pixels = p.stage_to_pixels;
        l.page = p.page;
        layers.push_back(l);
    }
    // The paper every page starts from, once its texture is in.
    if (m_paper_texture != ~0u) {
        m_movie_renderer->set_page_paper(m_renderer.textures().view(m_paper_texture));
    } else {
        m_movie_renderer->set_page_paper(VK_NULL_HANDLE);
    }
}

uint32_t Ui::page_texture(int page) {
    if (page < 0 || page >= int(MovieRenderer::k_pages)) {
        return m_renderer.textures().missing();
    }
    // The page targets are made again with the screen targets: register the new views (the old slots' views
    // stay alive until no frame samples them, MovieRenderer::destroy_pages defers).
    if (m_page_generation != m_movie_renderer->page_generation()) {
        for (uint32_t& id : m_page_textures) {
            if (id != ~0u) {
                m_renderer.textures().unregister_view(id);
                id = ~0u;
            }
        }
        m_page_generation = m_movie_renderer->page_generation();
    }
    uint32_t& id = m_page_textures[size_t(page)];
    if (id == ~0u && m_movie_renderer->page_view(uint32_t(page))) {
        id = m_renderer.textures().register_view(m_movie_renderer->page_view(uint32_t(page)));
    }
    return id == ~0u ? m_renderer.textures().missing() : id;
}

bool Ui::read_page(int page, std::vector<uint8_t>& rgba, uint32_t& size) {
    if (!m_movie_renderer || page < 0 || !m_movie_renderer->read_page(uint32_t(page), rgba)) {
        return false;
    }
    size = m_movie_renderer->page_size();
    return true;
}

void Ui::set_page_paper(const std::string& texture) {
    if (texture == m_paper) {
        return;
    }
    if (m_paper_texture != ~0u) {
        m_renderer.textures().release(m_paper_texture);
        m_paper_texture = ~0u;
    }
    m_paper = texture;
    if (!texture.empty()) {
        m_paper_texture = m_renderer.textures().acquire(texture, render::TextureKind::color, 1.0f);
    }
}

void Ui::release_scene_textures() {
    for (uint32_t& id : m_page_textures) {
        if (id != ~0u) {
            m_renderer.textures().unregister_view(id);
            id = ~0u;
        }
    }
    if (m_paper_texture != ~0u) {
        m_renderer.textures().release(m_paper_texture);
        m_paper_texture = ~0u;
        m_paper.clear();
    }
}

} // namespace spud::ui
