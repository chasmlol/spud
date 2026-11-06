#include "formats/osg.hpp"

#include <cstring>
#include <format>

namespace spud::osg {

namespace {

constexpr auto k_mark = container::fourcc("MARK");
constexpr auto k_wrld = container::fourcc("WRLD");
constexpr auto k_zone = container::fourcc("ZONE");
constexpr auto k_locn = container::fourcc("LOCN");
constexpr auto k_cell = container::fourcc("CELL");
constexpr auto k_kywd = container::fourcc("KYWD");
constexpr auto k_door = container::fourcc("DOOR");
constexpr auto k_lref = container::fourcc("LREF");
constexpr auto k_wnam = container::fourcc("WNAM");
constexpr auto k_cnam = container::fourcc("CNAM");
constexpr auto k_cedi = container::fourcc("CEDI");
constexpr auto k_regn = container::fourcc("REGN");
constexpr auto k_rpts = container::fourcc("RPTS");
constexpr auto k_strs = container::fourcc("STRS");

void validate(const World& w) {
    for (const auto& m : w.markers) {
        w.string(m.name);
    }
    for (const auto& s : w.worldspaces) {
        w.string(s.edid);
        w.string(s.space);
    }
    for (const auto& l : w.locations) {
        w.string(l.name);
        if (uint64_t{l.first_keyword} + l.keyword_count > w.keywords.size()) {
            throw ParseError(std::format(".osg location {:08X} keywords out of range", l.form_id));
        }
    }
}

} // namespace

uint32_t World::add_string(std::string_view s) {
    std::string key(s);
    if (auto it = m_string_index.find(key); it != m_string_index.end()) {
        return it->second;
    }
    auto offset = static_cast<uint32_t>(m_strings.size());
    m_strings.insert(m_strings.end(), s.begin(), s.end());
    m_strings.push_back('\0');
    m_string_index.emplace(std::move(key), offset);
    return offset;
}

std::string_view World::string(uint32_t offset) const {
    if (offset == k_none) {
        return {};
    }
    return container::StringTable(std::as_bytes(std::span<const char>(m_strings))).get(offset);
}

void World::set_string_data(std::vector<char> data) {
    m_strings = std::move(data);
    m_string_index.clear();
}

std::vector<std::byte> write(const World& w) {
    validate(w);
    container::Writer out(k_kind, k_version);
    out.add_array(k_mark, w.markers);
    out.add_array(k_wrld, w.worldspaces);
    out.add_array(k_zone, w.zones);
    out.add_array(k_locn, w.locations);
    out.add_array(k_cell, w.cells);
    out.add_array(k_kywd, w.keywords);
    out.add_array(k_door, w.doors);
    out.add_array(k_lref, w.location_refs);
    out.add_array(k_wnam, w.world_names);
    out.add_array(k_cnam, w.cell_names);
    out.add_array(k_cedi, w.cell_edids);
    out.add_array(k_regn, w.map_regions);
    out.add_array(k_rpts, w.region_points);
    out.add(k_strs, std::as_bytes(std::span<const char>(w.string_data())));
    return out.finish();
}

World read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() != k_version) {
        throw ParseError("unsupported .osg version " + std::to_string(r.version()));
    }
    World w;
    w.markers = r.read_array<MapMarker>(k_mark);
    w.worldspaces = r.read_array<Worldspace>(k_wrld);
    w.zones = r.read_array<EncounterZone>(k_zone);
    w.locations = r.read_array<Location>(k_locn);
    w.cells = r.read_array<Cell>(k_cell);
    w.keywords = r.read_array<uint32_t>(k_kywd);
    w.doors = r.read_array<Door>(k_door);
    w.location_refs = r.read_array<LocationRef>(k_lref);
    if (r.find(k_regn)) {
        w.world_names = r.read_array<uint32_t>(k_wnam);
        w.cell_names = r.read_array<uint32_t>(k_cnam);
        w.cell_edids = r.read_array<uint32_t>(k_cedi);
        w.map_regions = r.read_array<MapRegion>(k_regn);
        w.region_points = r.read_array<float>(k_rpts);
    }
    auto strings = r.require(k_strs);
    std::vector<char> data(strings.size());
    std::memcpy(data.data(), strings.data(), strings.size());
    w.set_string_data(std::move(data));
    validate(w);
    return w;
}

} // namespace spud::osg
