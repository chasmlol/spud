#include "ui/player/images.hpp"

#include <algorithm>
#include <cctype>

namespace spud::ui::player {

namespace {

bool starts_with_scheme(std::string_view url) {
    constexpr std::string_view scheme = "img://";
    if (url.size() < scheme.size()) {
        return false;
    }
    for (size_t i = 0; i < scheme.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(url[i])) != scheme[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view ImageRegistry::name_of(std::string_view url) {
    return starts_with_scheme(url) ? url.substr(6) : std::string_view{};
}

std::string ImageRegistry::cache_texture_path(std::string_view game_path) {
    std::string p(game_path);
    for (char& c : p) {
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (p.starts_with('/')) {
        p.erase(0, 1);
    }
    const size_t slash = p.rfind('/');
    const size_t dot = p.rfind('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        p.erase(dot);
    }
    return p + ".dds";
}

uint32_t ImageRegistry::add(std::string_view name, ImageSource source, std::string_view path) {
    auto it = m_by_name.find(std::string(name));
    if (it != m_by_name.end()) {
        EngineImage& e = m_images[it->second];
        e.source = source;
        e.path = std::string(path);
        ++e.users;
        return it->second;
    }
    const auto id = static_cast<uint32_t>(m_images.size());
    m_images.push_back({std::string(name), source, std::string(path), 1});
    m_by_name.emplace(std::string(name), id);
    return id;
}

uint32_t ImageRegistry::add_texture(std::string_view name, std::string_view cache_path) {
    if (!cache_has(cache_path)) {
        return k_none;
    }
    return add(name, ImageSource::texture, cache_path);
}

uint32_t ImageRegistry::add_render_target(std::string_view name) {
    return add(name, ImageSource::render_target, name);
}

void ImageRegistry::release(std::string_view name) {
    auto it = m_by_name.find(std::string(name));
    if (it != m_by_name.end() && m_images[it->second].users > 0) {
        --m_images[it->second].users;
    }
}

void ImageRegistry::set_default_image(std::string_view cache_path) {
    // Not a name the movies can register or release: GFx makes it for each URL its loader has nothing for.
    m_default = static_cast<uint32_t>(m_images.size());
    EngineImage e{"", ImageSource::texture, std::string(cache_path), 1};
    e.width = k_missing_width;
    e.height = k_missing_height;
    m_images.push_back(std::move(e));
}

uint32_t ImageRegistry::resolve(std::string_view url) const {
    const std::string_view name = starts_with_scheme(url) ? url.substr(6) : url;
    auto it = m_by_name.find(std::string(name));
    return it != m_by_name.end() && m_images[it->second].users > 0 ? it->second : k_none;
}

} // namespace spud::ui::player
