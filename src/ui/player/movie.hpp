#pragma once

// A loaded .osu movie and the lookups the player needs (characters by id, exports by name, labels).

#include "formats/osu.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace spud::ui::player {

class MovieDef {
public:
    MovieDef(osu::Movie movie, std::string url);

    const osu::Movie& data() const { return m_movie; }
    const std::string& url() const { return m_url; } // as movies name it, e.g. "startmenu.swf"
    std::string_view string(uint32_t offset) const { return m_movie.string(offset); }

    // Character index by source id, or osu::k_none.
    uint32_t character_by_id(uint16_t id) const;
    // Character index of an exported name, or osu::k_none.
    uint32_t export_named(std::string_view name) const;
    // 0-based frame of a label in a sprite, or -1.
    int label_frame(uint32_t sprite, std::string_view label) const;
    // The sprite a character index refers to (osu::k_none if it isn't a sprite).
    uint32_t sprite_of(uint32_t character) const;
    // The name a character is exported as (its linkage name), or "".
    std::string_view export_name_of(uint32_t character) const;

private:
    osu::Movie m_movie;
    std::string m_url;
    std::unordered_map<uint16_t, uint32_t> m_by_id;
    std::unordered_map<std::string, uint32_t> m_exports;
    std::unordered_map<uint32_t, std::string> m_export_names;
    std::unordered_map<uint64_t, int> m_labels; // (sprite << 32 | hash) collisions resolved by name scan
};

} // namespace spud::ui::player
