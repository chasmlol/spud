#include "ui/player/movie.hpp"

#include <cctype>

namespace spud::ui::player {

namespace {

bool equal_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

MovieDef::MovieDef(osu::Movie movie, std::string url) : m_movie(std::move(movie)), m_url(std::move(url)) {
    for (uint32_t i = 0; i < m_movie.characters.size(); ++i) {
        m_by_id[m_movie.characters[i].id] = i;
    }
    for (const auto& e : m_movie.exports) {
        m_exports[std::string(m_movie.string(e.name))] = e.character;
        m_export_names.emplace(e.character, std::string(m_movie.string(e.name)));
    }
}

uint32_t MovieDef::character_by_id(uint16_t id) const {
    auto it = m_by_id.find(id);
    return it == m_by_id.end() ? osu::k_none : it->second;
}

uint32_t MovieDef::export_named(std::string_view name) const {
    auto it = m_exports.find(std::string(name));
    return it == m_exports.end() ? osu::k_none : it->second;
}

int MovieDef::label_frame(uint32_t sprite, std::string_view label) const {
    if (sprite >= m_movie.sprites.size()) {
        return -1;
    }
    const auto& s = m_movie.sprites[sprite];
    for (uint32_t i = 0; i < s.label_count; ++i) {
        const auto& l = m_movie.labels[s.first_label + i];
        // Frame labels compare without regard to ASCII case in AVM1.
        if (equal_ignore_case(m_movie.string(l.name), label)) {
            return static_cast<int>(l.frame);
        }
    }
    return -1;
}

std::string_view MovieDef::export_name_of(uint32_t character) const {
    auto it = m_export_names.find(character);
    return it == m_export_names.end() ? std::string_view() : std::string_view(it->second);
}

uint32_t MovieDef::sprite_of(uint32_t character) const {
    if (character >= m_movie.characters.size() || m_movie.characters[character].kind != osu::CharKind::sprite) {
        return osu::k_none;
    }
    return m_movie.characters[character].index;
}

} // namespace spud::ui::player
