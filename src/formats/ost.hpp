#pragma once

// .ost — spud saved game. Spec: docs/formats/ost.md
//
// A save holds no content. The load order it was made on names the content (PLUG), and each section is one system's
// change layer on top of it: what is different from the records, keyed by FormID. Loading is the content first, then
// those layers over it.

#include "core/binary.hpp"
#include "formats/container.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud::ost {

inline constexpr container::FourCC k_kind = container::fourcc("SAVE");
inline constexpr uint32_t k_version = 3;

// Sections, one per system; a reader skips those it doesn't know.
inline constexpr container::FourCC k_clock = container::fourcc("CLOK");
inline constexpr container::FourCC k_world = container::fourcc("WSIM");
inline constexpr container::FourCC k_player = container::fourcc("PLYR");
inline constexpr container::FourCC k_game = container::fourcc("GAME");
inline constexpr container::FourCC k_actors = container::fourcc("ACTR");
inline constexpr container::FourCC k_sky = container::fourcc("SKY_");
inline constexpr container::FourCC k_papyrus = container::fourcc("PVM_");
inline constexpr container::FourCC k_quests = container::fourcc("QUST");
inline constexpr container::FourCC k_ui = container::fourcc("UI__");
inline constexpr container::FourCC k_seen = container::fourcc("SEEN");
inline constexpr container::FourCC k_destruction = container::fourcc("DEST");
inline constexpr container::FourCC k_npc_gear = container::fourcc("NGER");

enum class Kind : uint32_t { manual, autosave, quicksave, exit };

struct Header {
    Kind kind = Kind::manual;
    uint32_t number = 0;      // the next manual save's number when this one was made; a manual save's own
    uint32_t character = 0;   // which playthrough: fixed when the game starts
    uint32_t level = 1;
    float days_passed = 0;
    float reserved0 = 0;
    double play_seconds = 0;
    int64_t saved_at = 0;     // seconds since 1970
    uint64_t load_order = 0;  // the plugins' hashes together
    uint32_t thumb_width = 0, thumb_height = 0; // THMB: RGBA8
    uint32_t modded = 0;      // 1: plugins beyond the official ones; a character is the pair (character, modded)
    uint32_t reserved1 = 0;
};
static_assert(sizeof(Header) == 64);

struct Plugin {
    std::string name;
    uint64_t hash = 0;
};

struct Save {
    Header header;
    std::string player_name, location, race, space;
    std::vector<Plugin> plugins;
    std::vector<uint8_t> thumbnail;
    std::vector<std::pair<container::FourCC, std::vector<std::byte>>> sections;

    const std::vector<std::byte>* section(container::FourCC id) const;
};

std::vector<std::byte> write(const Save& save);
// Everything, or (`sections` false) what the save list shows.
Save read(std::span<const std::byte> file, bool sections = true); // throws ParseError

// What the sections are written with: length-prefixed strings, and sets and maps in FormID order, so the same state
// is the same bytes.
void put(BinaryWriter& w, std::string_view s);
std::string get_string(BinaryReader& r);
template <class T>
void put_set(BinaryWriter& w, const std::unordered_set<T>& set) {
    std::vector<T> sorted(set.begin(), set.end());
    std::sort(sorted.begin(), sorted.end());
    w.write<uint32_t>(uint32_t(sorted.size()));
    w.write_span(std::span<const T>(sorted));
}
template <class T>
void get_set(BinaryReader& r, std::unordered_set<T>& set) {
    set.clear();
    const auto n = r.read<uint32_t>();
    for (uint32_t i = 0; i < n; ++i) {
        set.insert(r.read<T>());
    }
}
template <class K, class V>
std::vector<std::pair<K, V>> sorted(const std::unordered_map<K, V>& map) {
    std::vector<std::pair<K, V>> out(map.begin(), map.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

} // namespace spud::ost
