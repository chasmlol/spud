#include "formats/osa.hpp"

#include <tuple>
#include <algorithm>
#include <cctype>

namespace spud::osa {

namespace {

constexpr auto k_sett = container::fourcc("SETT");
constexpr auto k_wthr = container::fourcc("WTHR");
constexpr auto k_clmt = container::fourcc("CLMT");
constexpr auto k_wlst = container::fourcc("WLST");
constexpr auto k_wrld = container::fourcc("WRLD");
constexpr auto k_imgs = container::fourcc("IMGS");
constexpr auto k_voli = container::fourcc("VOLI");
constexpr auto k_spgd = container::fourcc("SPGD");
constexpr auto k_imad = container::fourcc("IMAD");
constexpr auto k_keys = container::fourcc("KEYS");
constexpr auto k_stat = container::fourcc("STAT");
constexpr auto k_regn = container::fourcc("REGN");
constexpr auto k_rara = container::fourcc("RARA");
constexpr auto k_rpts = container::fourcc("RPTS");
constexpr auto k_rcel = container::fourcc("RCEL");
constexpr auto k_rlst = container::fourcc("RLST");
constexpr auto k_skym = container::fourcc("SKYM");
constexpr auto k_skys = container::fourcc("SKYS");
constexpr auto k_skyv = container::fourcc("SKYV");
constexpr auto k_skyi = container::fourcc("SKYI");
constexpr auto k_watr = container::fourcc("WATR");
constexpr auto k_strs = container::fourcc("STRS");

bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

template <typename T>
const T* find_form(const std::vector<T>& v, uint32_t form_id) {
    for (const auto& x : v) {
        if (x.form_id == form_id) {
            return &x;
        }
    }
    return nullptr;
}

} // namespace

const Weather* Atmosphere::weather(uint32_t form_id) const { return find_form(weathers, form_id); }

const Weather* Atmosphere::weather(std::string_view edid) const {
    for (size_t i = 0; i < weathers.size(); ++i) {
        if (iequal(weather_edids[i], edid)) {
            return &weathers[i];
        }
    }
    return nullptr;
}

const Climate* Atmosphere::climate(uint32_t form_id) const { return find_form(climates, form_id); }
const ImageSpace* Atmosphere::image_space(uint32_t form_id) const { return find_form(image_spaces, form_id); }
const Volumetric* Atmosphere::volumetric(uint32_t form_id) const { return find_form(volumetrics, form_id); }
const Precipitation* Atmosphere::precipitation(uint32_t form_id) const { return find_form(precipitations, form_id); }

const Modifier* Atmosphere::modifier(std::string_view edid) const {
    for (size_t i = 0; i < modifiers.size(); ++i) {
        if (iequal(modifier_edids[i], edid)) {
            return &modifiers[i];
        }
    }
    return nullptr;
}

const Water* Atmosphere::water(uint32_t form_id) const {
    auto it = std::lower_bound(waters.begin(), waters.end(), form_id,
                               [](const Water& w, uint32_t id) { return w.form_id < id; });
    return it != waters.end() && it->form_id == form_id ? &*it : nullptr;
}

const SkyModel* Atmosphere::sky_model(std::string_view path) const {
    for (const auto& m : sky_models) {
        if (iequal(string(m.path), path)) {
            return &m;
        }
    }
    return nullptr;
}

uint32_t Atmosphere::world_index(std::string_view world_edid) const {
    for (size_t i = 0; i < world_climates.size(); ++i) {
        if (iequal(world_climates[i].first, world_edid)) {
            return static_cast<uint32_t>(i);
        }
    }
    return ~0u;
}

const RegionCell* Atmosphere::region_cell(uint32_t world, int32_t x, int32_t y) const {
    const auto key = std::tuple(world, x, y);
    auto it = std::lower_bound(region_cells.begin(), region_cells.end(), key, [](const RegionCell& c, const auto& k) {
        return std::tuple(c.world, c.x, c.y) < k;
    });
    return it != region_cells.end() && std::tuple(it->world, it->x, it->y) == key ? &*it : nullptr;
}

uint32_t Atmosphere::climate_of_world(std::string_view world_edid) const {
    for (const auto& [edid, climate] : world_climates) {
        if (iequal(edid, world_edid)) {
            return climate;
        }
    }
    return 0;
}

std::string_view Atmosphere::string(uint32_t offset) const {
    return container::StringTable(string_data).get(offset);
}

uint32_t Atmosphere::intern(std::string_view s) {
    auto it = m_interned.find(std::string(s));
    if (it != m_interned.end()) {
        return it->second;
    }
    const auto offset = static_cast<uint32_t>(string_data.size());
    for (char c : s) {
        string_data.push_back(static_cast<std::byte>(c));
    }
    string_data.push_back(std::byte{0});
    m_interned.emplace(std::string(s), offset);
    return offset;
}

std::vector<std::byte> write(const Atmosphere& a) {
    // EditorIDs join the paths in one table; the in-memory offsets of paths stay valid because they come first.
    Atmosphere copy = a;
    std::vector<Weather> weathers = a.weathers;
    for (size_t i = 0; i < weathers.size(); ++i) {
        weathers[i].edid = copy.intern(a.weather_edids[i]);
    }
    std::vector<Climate> climates = a.climates;
    for (size_t i = 0; i < climates.size(); ++i) {
        climates[i].edid = copy.intern(a.climate_edids[i]);
    }
    std::vector<Modifier> modifiers = a.modifiers;
    for (size_t i = 0; i < modifiers.size(); ++i) {
        modifiers[i].edid = copy.intern(a.modifier_edids[i]);
    }
    std::vector<WorldClimate> worlds;
    for (const auto& [edid, climate] : a.world_climates) {
        worlds.push_back({copy.intern(edid), climate});
    }
    container::Writer w(k_kind, k_version);
    w.add_struct(k_sett, a.settings);
    w.add_array(k_wthr, weathers);
    w.add_array(k_clmt, climates);
    w.add_array(k_wlst, a.climate_weathers);
    w.add_array(k_wrld, worlds);
    w.add_array(k_imgs, a.image_spaces);
    w.add_array(k_voli, a.volumetrics);
    w.add_array(k_spgd, a.precipitations);
    w.add_array(k_imad, modifiers);
    w.add_array(k_keys, a.keys);
    w.add_array(k_stat, a.sky_statics);
    w.add_array(k_regn, a.regions);
    w.add_array(k_rara, a.region_areas);
    w.add_array(k_rpts, a.region_points);
    w.add_array(k_rcel, a.region_cells);
    w.add_array(k_rlst, a.region_lists);
    w.add_array(k_skym, a.sky_models);
    w.add_array(k_skys, a.sky_shapes);
    w.add_array(k_skyv, a.sky_vertices);
    w.add_array(k_skyi, a.sky_indices);
    w.add_array(k_watr, a.waters);
    w.add(k_strs, copy.string_data);
    return w.finish();
}

Atmosphere read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() != k_version) {
        throw ParseError("unsupported .osa version " + std::to_string(r.version()));
    }
    Atmosphere a;
    const auto strs = r.require(k_strs);
    a.string_data.assign(strs.begin(), strs.end());
    a.settings = r.read_struct<SkySettings>(k_sett);
    a.weathers = r.read_array<Weather>(k_wthr);
    for (const auto& w : a.weathers) {
        a.weather_edids.emplace_back(a.string(w.edid));
    }
    a.climates = r.read_array<Climate>(k_clmt);
    for (const auto& c : a.climates) {
        a.climate_edids.emplace_back(a.string(c.edid));
    }
    a.climate_weathers = r.read_array<ClimateWeather>(k_wlst);
    for (const auto& wc : r.read_array<WorldClimate>(k_wrld)) {
        a.world_climates.emplace_back(std::string(a.string(wc.world_edid)), wc.climate);
    }
    a.image_spaces = r.read_array<ImageSpace>(k_imgs);
    a.volumetrics = r.read_array<Volumetric>(k_voli);
    a.precipitations = r.read_array<Precipitation>(k_spgd);
    a.modifiers = r.read_array<Modifier>(k_imad);
    for (const auto& m : a.modifiers) {
        a.modifier_edids.emplace_back(a.string(m.edid));
    }
    a.keys = r.read_array<float>(k_keys);
    a.sky_statics = r.read_array<uint32_t>(k_stat);
    a.regions = r.read_array<Region>(k_regn);
    a.region_areas = r.read_array<RegionArea>(k_rara);
    a.region_points = r.read_array<float>(k_rpts);
    a.region_cells = r.read_array<RegionCell>(k_rcel);
    a.region_lists = r.read_array<uint32_t>(k_rlst);
    a.sky_models = r.read_array<SkyModel>(k_skym);
    a.sky_shapes = r.read_array<SkyShape>(k_skys);
    a.sky_vertices = r.read_array<SkyVertex>(k_skyv);
    a.sky_indices = r.read_array<uint32_t>(k_skyi);
    a.waters = r.read_array<Water>(k_watr);
    return a;
}

} // namespace spud::osa
