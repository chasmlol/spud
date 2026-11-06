#include "importer/convert/world_import.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"
#include "importer/legacy/nif.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace spud::importer {

namespace {

using legacy::EsmDatabase;
using legacy::Record;
using legacy::record_type;
using legacy::RecordEntry;
using legacy::RecordType;

constexpr RecordType k_wrld = record_type("WRLD");
constexpr RecordType k_cell = record_type("CELL");
constexpr RecordType k_land = record_type("LAND");
constexpr RecordType k_refr = record_type("REFR");
constexpr RecordType k_achr = record_type("ACHR");
constexpr RecordType k_navm = record_type("NAVM");
constexpr RecordType k_ltex = record_type("LTEX");
constexpr RecordType k_txst = record_type("TXST");
constexpr RecordType k_ligh = record_type("LIGH");
constexpr RecordType k_armo = record_type("ARMO");

constexpr RecordType k_edid = record_type("EDID");
constexpr RecordType k_full = record_type("FULL");
constexpr RecordType k_wnam = record_type("WNAM");
constexpr RecordType k_pnam = record_type("PNAM");
constexpr RecordType k_dnam = record_type("DNAM");
constexpr RecordType k_nam2 = record_type("NAM2");
constexpr RecordType k_data = record_type("DATA");
constexpr RecordType k_xclc = record_type("XCLC");
constexpr RecordType k_xclw = record_type("XCLW");
constexpr RecordType k_xcwt = record_type("XCWT");
constexpr RecordType k_xwcn = record_type("XWCN");
constexpr RecordType k_xprm = record_type("XPRM");
constexpr RecordType k_xtri = record_type("XTRI");
constexpr RecordType k_nam0 = record_type("NAM0");
constexpr RecordType k_nam1 = record_type("NAM1");
constexpr RecordType k_vhgt = record_type("VHGT");
constexpr RecordType k_vnml = record_type("VNML");
constexpr RecordType k_vclr = record_type("VCLR");
constexpr RecordType k_btxt = record_type("BTXT");
constexpr RecordType k_atxt = record_type("ATXT");
constexpr RecordType k_vtxt = record_type("VTXT");
constexpr RecordType k_name = record_type("NAME");
constexpr RecordType k_xscl = record_type("XSCL");
constexpr RecordType k_xesp = record_type("XESP");
constexpr RecordType k_modl = record_type("MODL");
constexpr RecordType k_mod2 = record_type("MOD2");
constexpr RecordType k_mod4 = record_type("MOD4");
constexpr RecordType k_tnam = record_type("TNAM");
constexpr RecordType k_snam = record_type("SNAM");
constexpr RecordType k_inam = record_type("INAM");
constexpr RecordType k_gnam = record_type("GNAM");
constexpr RecordType k_gras = record_type("GRAS");
constexpr RecordType k_tx00 = record_type("TX00");
constexpr RecordType k_tx01 = record_type("TX01");
constexpr RecordType k_fnam = record_type("FNAM");
constexpr RecordType k_xrds = record_type("XRDS");
constexpr RecordType k_xlig = record_type("XLIG");
constexpr RecordType k_xemi = record_type("XEMI");
constexpr RecordType k_regn = record_type("REGN");
constexpr RecordType k_rdat = record_type("RDAT");
constexpr RecordType k_rdwt = record_type("RDWT");
constexpr RecordType k_stat = record_type("STAT");
constexpr RecordType k_mato = record_type("MATO");
constexpr RecordType k_tree = record_type("TREE");
constexpr RecordType k_cnam = record_type("CNAM");
constexpr RecordType k_xtel = record_type("XTEL");
constexpr RecordType k_xrmr = record_type("XRMR");
constexpr RecordType k_xpod = record_type("XPOD");
constexpr RecordType k_xord = record_type("XORD");
constexpr RecordType k_xmbr = record_type("XMBR");
constexpr uint32_t k_plane_marker = 0x00000017;  // STAT PlaneMarker: occlusion planes (XPRM type 3) and boxes (1)
constexpr uint32_t k_room_marker = 0x0000001F;   // STAT RoomMarker
constexpr uint32_t k_portal_marker = 0x00000020; // STAT PortalMarker
constexpr RecordType k_xcll = record_type("XCLL");
constexpr RecordType k_xccm = record_type("XCCM");
constexpr RecordType k_ltmp = record_type("LTMP");
constexpr RecordType k_xcim = record_type("XCIM");
constexpr RecordType k_lgtm = record_type("LGTM");
constexpr RecordType k_dalc = record_type("DALC");
constexpr RecordType k_imgs = record_type("IMGS");
constexpr RecordType k_hnam = record_type("HNAM");
constexpr RecordType k_enam = record_type("ENAM");

// XCLL inherit flags: the fields taken from the cell's lighting template.
constexpr uint32_t k_inherit_ambient = 0x001;
constexpr uint32_t k_inherit_directional = 0x002;
constexpr uint32_t k_inherit_fog_color = 0x004;
constexpr uint32_t k_inherit_fog_near = 0x008;
constexpr uint32_t k_inherit_fog_far = 0x010;
constexpr uint32_t k_inherit_rotation = 0x020;
constexpr uint32_t k_inherit_fade = 0x040;
constexpr uint32_t k_inherit_clip = 0x080;
constexpr uint32_t k_inherit_fog_power = 0x100;
constexpr uint32_t k_inherit_fog_max = 0x200;
constexpr uint32_t k_inherit_light_fade = 0x400;
constexpr RecordType k_door = record_type("DOOR");
constexpr RecordType k_obnd = record_type("OBND");

// Base types placed with REFR that draw their MODL (ARMO: MOD2, the male world model).
constexpr RecordType k_renderable_types[] = {
    record_type("STAT"), record_type("MSTT"), record_type("SCOL"), record_type("TREE"), record_type("FLOR"),
    record_type("FURN"), record_type("ACTI"), record_type("TACT"), record_type("DOOR"), record_type("CONT"),
    record_type("LIGH"), record_type("MISC"), record_type("BOOK"), record_type("INGR"), record_type("ALCH"),
    record_type("KEYM"), record_type("SLGM"), record_type("SCRL"), record_type("APPA"), record_type("AMMO"),
    record_type("WEAP"), record_type("ARMO"), record_type("NOTE"),
};

constexpr const char* k_default_land_texture = "textures/landscape/dirt02.dds";
constexpr const char* k_default_land_normal = "textures/landscape/dirt02_n.dds";
constexpr uint32_t k_player_ref = 0x00000014; // hardcoded, always enabled, not stored in any plugin

enum ParentUse : uint16_t {
    parent_use_land = 0x01,
    parent_use_lod = 0x02,
    parent_use_map = 0x04,
    parent_use_water = 0x08,
    parent_use_climate = 0x10,
};

enum CellData : uint16_t {
    cell_data_interior = 0x01,
    cell_data_has_water = 0x02,
};

// ---------------------------------------------------------------------------------------------
// World records

struct WorldInfo {
    const RecordEntry* entry = nullptr;
    std::string edid;
    std::string name;
    uint32_t parent = 0;
    uint16_t parent_use = 0;
    bool has_dnam = false;
    float land_height = 0.0f; // Skyrim units
    float water_height = 0.0f;
    uint32_t water = 0; // WATR
    uint8_t data_flags = 0; // DATA
};

WorldInfo read_world(const EsmDatabase& db, const RecordEntry& e) {
    auto r = db.read(e);
    WorldInfo w;
    w.entry = &e;
    w.edid = r.edid();
    if (auto s = r.find(k_data); s && !s->data.empty()) {
        w.data_flags = s->as<uint8_t>();
    }
    if (!r.plugin().localized) {
        w.name = r.string(k_full); // localized plugins store a string-table ID instead
    }
    w.parent = r.form(k_wnam);
    if (auto s = r.find(k_pnam); s && s->data.size() >= 2) {
        w.parent_use = s->as<uint16_t>();
    }
    if (auto s = r.find(k_dnam); s && s->data.size() >= 8) {
        auto br = s->reader();
        w.land_height = br.read<float>();
        w.water_height = br.read<float>();
        w.has_dnam = true;
    }
    w.water = r.form(k_nam2);
    return w;
}

std::optional<WorldInfo> world_by_form(const EsmDatabase& db, uint32_t form_id) {
    auto e = db.find(form_id);
    if (!e || e->deleted() || e->type != k_wrld) {
        return std::nullopt;
    }
    return read_world(db, *e);
}

// ---------------------------------------------------------------------------------------------
// Cells, land, references

struct CellCoord {
    int32_t x;
    int32_t y;
    bool operator==(const CellCoord&) const = default;
};

struct CellCoordHash {
    size_t operator()(const CellCoord& c) const {
        return std::hash<uint64_t>()((uint64_t(uint32_t(c.x)) << 32) | uint32_t(c.y));
    }
};

constexpr int k_quad_vertex_count = osw::k_quad_verts * osw::k_quad_verts;
constexpr uint32_t k_default_ltex = 0xFFFFFFFFu; // the engine's default land texture (an ATXT with FormID 0)

// The game's loaded land data: per quadrant a base LTEX, six layer LTEX
// slots and a weight byte per vertex and slot.
struct PendingLand {
    osw::Land land{};
    uint32_t base[4] = {0, 0, 0, 0};  // LTEX FormID per quadrant, 0 = none (default texture)
    uint32_t tex[4][6] = {};          // layer slots: LTEX FormID, 0 = empty, k_default_ltex
    uint8_t weight[4][k_quad_vertex_count][6] = {};
    bool flat = false; // no VHGT: texture-only LAND (DATA lacks the height-map bit)
};

struct PendingRef {
    uint32_t form_id;
    uint32_t base;
    uint32_t record_flags;
    uint32_t enable_parent;
    bool enable_opposite;
    bool persistent;
    float position[3]; // Skyrim units
    float rotation[3]; // radians
    float scale;
    uint32_t prim_type = 0;  // XPRM: 1 box, 2 sphere, 3 plane, 4 line; 0 none
    float prim_bounds[3] = {0, 0, 0}; // XPRM bounds: half extents, Skyrim units
    uint32_t collision_layer = 0;     // XTRI: the collision layer + 1, 0 none
    // Light references.
    float radius_offset; // XRDS
    float fov_offset = 0.0f; // XLIG
    float depth_bias = 1.0f; // XLIG, 1 without one
    float fade_offset;   // XLIG fade offset
    uint32_t emittance;  // XEMI
    // Load doors (XTEL): the destination door and where the player arrives beside it (units, radians).
    uint32_t teleport_door = 0;
    float teleport_position[3] = {0, 0, 0};
    float teleport_rotation[3] = {0, 0, 0};
    uint32_t teleport_flags = 0;
    // Room and portal markers, occlusion planes (XORD), XMBR.
    uint32_t room_flags = 0;     // XRMR
    uint32_t room_template = 0;  // LNAM after XRMR (bit 15)
    uint32_t room_image_space = 0; // the subrecord after that (bit 14)
    std::vector<uint32_t> room_links; // XLRM
    uint32_t portal_rooms[2] = {0, 0}; // XPOD
    uint32_t occlusion_links[4] = {0, 0, 0, 0}; // XORD
    uint32_t multibound = 0;     // XMBR
};

struct PendingCell {
    const RecordEntry* entry = nullptr; // nullptr for synthetic cells
    CellCoord coord{};
    uint16_t world = 0; // which source world: 0 the imported one, else 1 + the merged child world's index
    bool world_persistent = false; // the world-level persistent cell: refs are redistributed by position
    bool header_ok = false;
    bool has_water = false;
    bool no_water_sentinel = false;
    float water_height = 0.0f; // Skyrim units
    uint32_t water = 0;        // WATR of the cell's water (XCWT, else the world's)
    std::vector<osw::CurrentQuad> current; // XWCN/XWCU, in the game's element format
    const RecordEntry* land = nullptr;
    std::unique_ptr<PendingLand> decoded;
    std::vector<PendingRef> refs;
    std::map<std::string, size_t> skipped;
    std::map<std::string, size_t> warnings;
    std::string error;
};

template <typename Fn>
void parallel_for(size_t count, Fn&& fn) {
    std::atomic<size_t> next{0};
    auto threads = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 32);
    std::vector<std::jthread> pool;
    for (size_t t = 0; t < std::min(threads, count); ++t) {
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < count;) {
                fn(i);
            }
        });
    }
}

// XCLW holds a few sentinels instead of heights. FLT_MAX (by far the most common) means "use the world
// default". ±2^31 and ~2^32 also occur (Tamriel's persistent cell, a few dozen exterior cells): integer
// "no water" markers written as floats. No real water surface is anywhere near a billion units.
constexpr float k_water_default_threshold = 3.0e38f;
constexpr float k_water_none_threshold = 1.0e9f;

void read_cell_header(const Record& r, PendingCell& c, const WorldInfo& water_world) {
    if (auto s = r.find(k_xclc); s && s->data.size() >= 8) {
        auto br = s->reader();
        c.coord.x = br.read<int32_t>();
        c.coord.y = br.read<int32_t>();
    }
    uint16_t data = 0;
    if (auto s = r.find(k_data); s && !s->data.empty()) {
        data = s->data.size() >= 2 ? s->as<uint16_t>() : s->as<uint8_t>();
    }
    c.has_water = (data & cell_data_has_water) != 0;
    c.water_height = water_world.water_height;
    c.water = water_world.water;
    if (auto s = r.find(k_xcwt); s && s->data.size() >= 4) {
        if (const uint32_t w = r.resolve(s->as<uint32_t>())) {
            c.water = w;
        }
    }
    // XWCN gives the number of 16-byte quads; the game reads the next subrecord as their floats, whatever its
    // type, and drops the data if every float is below 0.001.
    const auto subs = r.subrecords();
    for (size_t i = 0; i + 1 < subs.size(); ++i) {
        if (subs[i].type != k_xwcn || subs[i].data.size() < 4) {
            continue;
        }
        uint32_t quads = 0;
        std::memcpy(&quads, subs[i].data.data(), 4);
        const auto& u = subs[i + 1].data;
        if (quads == 0 || u.size() < size_t(quads) * 16) {
            break;
        }
        c.current.resize(quads);
        std::memcpy(c.current.data(), u.data(), size_t(quads) * 16);
        bool any = false;
        for (const auto& q : c.current) {
            for (float v : q.v) {
                any = any || std::fabs(v) >= 0.001f;
            }
        }
        if (!any) {
            c.current.clear();
        }
        break;
    }
    if (auto s = r.find(k_xclw); s && s->data.size() >= 4) {
        float h = s->as<float>();
        if (!std::isfinite(h) || h >= k_water_default_threshold) {
            // default
        } else if (std::fabs(h) >= k_water_none_threshold) {
            c.has_water = false;
            c.no_water_sentinel = true;
        } else {
            c.water_height = h;
        }
    }
}

// Interior lighting. XCLL and LGTM DATA share the field
// layout up to the light fade distances; XCLL adds the inherit flags.
void read_lighting_fields(std::span<const std::byte> d, osw::CellLighting& l) {
    auto bytes = [&](size_t at, uint8_t* out) {
        if (d.size() >= at + 4) {
            std::memcpy(out, d.data() + at, 4);
        }
    };
    auto value = [&](size_t at, auto& out) {
        if (d.size() >= at + sizeof(out)) {
            std::memcpy(&out, d.data() + at, sizeof(out));
        }
    };
    bytes(0, l.ambient);
    bytes(4, l.directional);
    bytes(8, l.fog_near_color);
    value(12, l.fog_near);
    value(16, l.fog_far);
    value(20, l.rotation_xy);
    value(24, l.rotation_z);
    value(28, l.directional_fade);
    value(32, l.fog_clip);
    value(36, l.fog_power);
    for (int i = 0; i < 6; ++i) {
        bytes(40 + 4 * i, l.directional_ambient[i]);
    }
    bytes(64, l.specular);
    value(68, l.fresnel_power);
    bytes(72, l.fog_far_color);
    value(76, l.fog_max);
    value(80, l.light_fade_begin);
    value(84, l.light_fade_end);
}

// The game: DATA as XCLL's first 92 bytes, whose directional-ambient slot is never
// read; DALC is the template's only directional ambient.
std::optional<osw::CellLighting> read_template(const EsmDatabase& db, uint32_t form) {
    auto t = form ? db.get(form) : std::nullopt;
    if (!t || t->type() != k_lgtm) {
        return std::nullopt;
    }
    const auto* data = t->find(k_data);
    if (!data) {
        return std::nullopt;
    }
    osw::CellLighting l{};
    read_lighting_fields(data->data, l);
    std::memset(l.directional_ambient, 255, sizeof l.directional_ambient);
    std::memset(l.specular, 0, sizeof l.specular);
    l.fresnel_power = 1.0f;
    if (auto s = t->find(k_dalc); s && s->data.size() >= 32) {
        for (int i = 0; i < 6; ++i) {
            std::memcpy(l.directional_ambient[i], s->data.data() + 4 * i, 4);
        }
        std::memcpy(l.specular, s->data.data() + 24, 4);
        std::memcpy(&l.fresnel_power, s->data.data() + 28, 4);
    }
    return l;
}

// The Sunlight Scale of an image space (0: DefaultImageSpace 0x167), which scales DirLightColor
float sunlight_scale(const EsmDatabase& db, uint32_t image_space) {
    for (uint32_t ims : {image_space, 0x167u}) {
        auto r = ims ? db.get(ims) : std::nullopt;
        if (!r || r->type() != k_imgs) {
            continue;
        }
        float scale = 1.0f;
        if (auto s = r->find(k_hnam); s && s->data.size() >= 28) {
            std::memcpy(&scale, s->data.data() + 24, 4);
        } else if (auto e = r->find(k_enam); e && e->data.size() >= 24) {
            std::memcpy(&scale, e->data.data() + 20, 4);
        }
        return scale;
    }
    return 1.0f;
}

osw::CellLighting read_cell_lighting(const EsmDatabase& db, const Record& cell) {
    // The interior's lighting block before XCLL is read (the DATA handler of the game
    // creates it): black colours, white directional ambient, specular 0, fresnel 1, fog power and max 1, fade 1,
    // inherit flags 0x79F (everything but the rotation and the fade from the template).
    osw::CellLighting own{};
    std::memset(own.directional_ambient, 255, sizeof own.directional_ambient);
    own.fresnel_power = 1.0f;
    own.fog_power = 1.0f;
    own.fog_max = 1.0f;
    own.directional_fade = 1.0f;
    own.inherit = 0x79F;
    if (const auto* xcll = cell.find(k_xcll)) {
        read_lighting_fields(xcll->data, own);
        if (xcll->data.size() >= 92) {
            std::memcpy(&own.inherit, xcll->data.data() + 88, 4);
        }
        // A fog far colour of 0 takes the near colour.
        if (!(own.fog_far_color[0] | own.fog_far_color[1] | own.fog_far_color[2])) {
            std::memcpy(own.fog_far_color, own.fog_near_color, 3);
        }
    }
    if (auto s = cell.find(k_ltmp); s && s->data.size() >= 4) {
        own.lighting_template = cell.resolve(s->as<uint32_t>());
    }
    if (auto s = cell.find(k_xcim); s && s->data.size() >= 4) {
        own.image_space = cell.resolve(s->as<uint32_t>());
    }
    const auto template_lighting = read_template(db, own.lighting_template);
    osw::CellLighting tmpl = template_lighting.value_or(osw::CellLighting{});
    const bool have_template = template_lighting.has_value();
    // The cell's image space: XCIM, else DefaultImageSpace.
    own.sunlight_scale = sunlight_scale(db, own.image_space);
    tmpl.sunlight_scale = own.sunlight_scale;
    if (!have_template) {
        return own;
    }
    osw::CellLighting l = own;
    const uint32_t f = own.inherit;
    auto take = [&](uint32_t bit, auto&& fn) {
        if (f & bit) {
            fn();
        }
    };
    take(k_inherit_ambient, [&] {
        std::memcpy(l.ambient, tmpl.ambient, 4);
        std::memcpy(l.directional_ambient, tmpl.directional_ambient, sizeof l.directional_ambient);
        std::memcpy(l.specular, tmpl.specular, 4);
        l.fresnel_power = tmpl.fresnel_power;
    });
    take(k_inherit_directional, [&] { std::memcpy(l.directional, tmpl.directional, 4); });
    take(k_inherit_fog_color, [&] {
        std::memcpy(l.fog_near_color, tmpl.fog_near_color, 4);
        std::memcpy(l.fog_far_color, tmpl.fog_far_color, 4);
    });
    take(k_inherit_fog_near, [&] { l.fog_near = tmpl.fog_near; });
    take(k_inherit_fog_far, [&] { l.fog_far = tmpl.fog_far; });
    take(k_inherit_rotation, [&] {
        l.rotation_xy = tmpl.rotation_xy;
        l.rotation_z = tmpl.rotation_z;
    });
    take(k_inherit_fade, [&] { l.directional_fade = tmpl.directional_fade; });
    take(k_inherit_clip, [&] { l.fog_clip = tmpl.fog_clip; });
    take(k_inherit_fog_power, [&] { l.fog_power = tmpl.fog_power; });
    take(k_inherit_fog_max, [&] { l.fog_max = tmpl.fog_max; });
    take(k_inherit_light_fade, [&] {
        l.light_fade_begin = tmpl.light_fade_begin;
        l.light_fade_end = tmpl.light_fade_end;
    });
    return l;
}

void decode_land(const Record& r, float default_height_units, PendingLand& out,
                 std::map<std::string, size_t>& warn) {
    auto& land = out.land;
    constexpr int n = osw::k_land_verts;
    if (auto s = r.find(k_vhgt); s && s->data.size() >= 4 + n * n) {
        // float32 running sums, each row from the previous row's first value;
        // the height is 8 x the sum truncated toward zero.
        auto br = s->reader();
        float row_start = br.read<float>();
        auto deltas = reinterpret_cast<const int8_t*>(s->data.data() + 4);
        auto height = [](float sum) { return units::to_metres(float(int32_t(sum) * 8)); };
        for (int row = 0; row < n; ++row) {
            row_start += deltas[row * n];
            float v = row_start;
            land.heights[row * n] = height(v);
            for (int col = 1; col < n; ++col) {
                v += deltas[row * n + col];
                land.heights[row * n + col] = height(v);
            }
        }
    } else {
        // The world's DNAM default land height is what the engine uses for cells without height data.
        std::fill(std::begin(land.heights), std::end(land.heights), units::to_metres(default_height_units));
        out.flat = true;
    }
    if (auto s = r.find(k_vnml); s && s->data.size() >= sizeof land.normals) {
        std::memcpy(land.normals, s->data.data(), sizeof land.normals);
    } else {
        for (int i = 0; i < n * n; ++i) {
            land.normals[i * 3 + 0] = 0;
            land.normals[i * 3 + 1] = 0;
            land.normals[i * 3 + 2] = 127;
        }
    }
    if (auto s = r.find(k_vclr); s && s->data.size() >= sizeof land.colors) {
        std::memcpy(land.colors, s->data.data(), sizeof land.colors);
    } else {
        std::fill(std::begin(land.colors), std::end(land.colors), uint8_t(255));
    }

    // Texture layers only with DATA flag 4. BTXT sets a quadrant's base; ATXT selects a layer slot (clamped to 5)
    // whose weights follow in VTXT: trunc(opacity x 255), low byte, only for slots 0..4.
    uint32_t data_flags = 0;
    if (auto s = r.find(k_data); s && s->data.size() >= 4) {
        data_flags = s->as<uint32_t>();
    }
    if (!(data_flags & 4)) {
        return;
    }
    auto subs = r.subrecords();
    for (size_t i = 0; i < subs.size(); ++i) {
        const auto& s = subs[i];
        if (s.type == k_btxt && s.data.size() >= 6) {
            auto br = s.reader();
            auto ltex = r.resolve(br.read<uint32_t>());
            auto quadrant = br.read<uint8_t>();
            if (quadrant < 4) {
                out.base[quadrant] = ltex;
            }
        } else if (s.type == k_atxt && s.data.size() >= 8) {
            auto br = s.reader();
            const uint32_t raw = br.read<uint32_t>();
            auto quadrant = br.read<uint8_t>();
            br.skip(1);
            auto layer = br.read<uint16_t>();
            if (layer > 5) {
                layer = 5;
                ++warn["ATXT layer above 5"];
            }
            if (quadrant < 4) {
                out.tex[quadrant][layer] = raw ? r.resolve(raw) : k_default_ltex;
            } else {
                ++warn["ATXT with bad quadrant"];
            }
            if (i + 1 < subs.size() && subs[i + 1].type == k_vtxt) {
                auto vr = subs[++i].reader();
                while (vr.remaining() >= 8) {
                    auto position = vr.read<uint16_t>();
                    vr.skip(2);
                    auto opacity = vr.read<float>();
                    if (quadrant < 4 && position < k_quad_vertex_count && layer < 5) {
                        uint8_t w = 0;
                        if (opacity > 0.0f) {
                            const float x = opacity * 255.0f;
                            w = static_cast<uint8_t>(std::abs(x) < 2147483648.0f ? int32_t(x) : INT32_MIN);
                        }
                        out.weight[quadrant][position][layer] = w;
                    }
                }
            }
        }
    }
}

// One quadrant as the game builds it: empty slots are closed up, then on a copy
// repeated layers merge and layers equal to the base drop out. The material binds the closed-up list while the
// weights follow the merged one (an engine quirk kept on purpose).
struct BuiltQuadrant {
    uint32_t base;       // LTEX, 0 = default
    uint32_t bound[5];   // texture slots 1..5: LTEX, 0 = none
    uint8_t weight[5][k_quad_vertex_count];
};

template <typename HasTextureSet>
BuiltQuadrant build_quadrant(PendingLand& pl, int q, HasTextureSet has_ts) {
    // 1. Close up empty slots (the game moves slots 0..4 only).
    uint32_t (&tex)[6] = pl.tex[q];
    auto& w = pl.weight[q];
    auto remove = [&](int slot) {
        if (slot >= 5) {
            return;
        }
        for (int k = slot; k < 5; ++k) {
            tex[k] = tex[k + 1];
        }
        tex[5] = 0;
        for (int v = 0; v < k_quad_vertex_count; ++v) {
            for (int k = slot; k < 5; ++k) {
                w[v][k] = w[v][k + 1];
            }
            w[v][5] = 0;
        }
    };
    int n = 6;
    for (int slot = 0; slot < n; ++slot) {
        while (slot < n && tex[slot] == 0) {
            remove(slot);
            --n;
            if (tex[slot] != 0) {
                break;
            }
        }
    }
    // 2. Merge on a copy.
    uint32_t a[6];
    std::memcpy(a, tex, sizeof a);
    std::vector<std::array<int, 6>> wc(k_quad_vertex_count);
    for (int v = 0; v < k_quad_vertex_count; ++v) {
        for (int k = 0; k < 6; ++k) {
            wc[v][k] = w[v][k];
        }
    }
    auto shift = [&](int from) {
        for (int k = from; k < 5; ++k) {
            a[k] = a[k + 1];
        }
        a[5] = 0;
        for (auto& row : wc) {
            for (int k = from; k < 5; ++k) {
                row[k] = row[k + 1];
            }
            row[5] = 0;
        }
    };
    const uint32_t base = pl.base[q];
    const bool base_ts = base != 0 && has_ts(base);
    for (int i = 0; i < 6; ++i) {
        if (a[i] == 0 || !has_ts(a[i])) {
            continue;
        }
        for (int j = i + 1; i < 5 && j < 6; ++j) {
            if (a[j] != 0 && a[j] == a[i]) {
                for (auto& row : wc) {
                    row[i] = (row[i] + row[j]) & 0xFF;
                }
                shift(j);
                --j;
            }
        }
        if (base_ts && a[i] == base) {
            shift(i);
            --i;
        }
    }
    BuiltQuadrant out{};
    out.base = base != 0 && has_ts(base) ? base : 0;
    for (int k = 0; k < 5; ++k) {
        out.bound[k] = tex[k] != 0 && has_ts(tex[k]) ? tex[k] : 0;
        for (int v = 0; v < k_quad_vertex_count; ++v) {
            out.weight[k][v] = static_cast<uint8_t>(wc[v][k]);
        }
    }
    return out;
}

void read_ref(const Record& r, bool persistent, PendingRef& out) {
    out.depth_bias = 1.0f;
    out = {};
    out.form_id = r.form_id();
    out.base = r.form(k_name);
    out.record_flags = r.flags();
    out.persistent = persistent || (r.flags() & legacy::record_persistent);
    out.scale = 1.0f;
    if (auto s = r.find(k_data); s && s->data.size() >= 24) {
        auto br = s->reader();
        for (auto& v : out.position) {
            v = br.read<float>();
        }
        for (auto& v : out.rotation) {
            v = br.read<float>();
        }
    }
    if (auto s = r.find(k_xscl); s && s->data.size() >= 4) {
        out.scale = s->as<float>();
    }
    if (auto s = r.find(k_xprm); s && s->data.size() >= 32) {
        auto br = s->reader();
        for (auto& v : out.prim_bounds) {
            v = std::fabs(br.read<float>());
        }
        br.skip(16); // colour, unknown
        out.prim_type = br.read<uint32_t>();
    }
    if (auto s = r.find(k_xtri); s && s->data.size() >= 4) {
        out.collision_layer = s->as<uint32_t>() + 1; // the layer number, +1 so 0 means none
    }
    if (auto s = r.find(k_xrds); s && s->data.size() >= 4) {
        out.radius_offset = s->as<float>();
    }
    if (auto s = r.find(k_xlig); s && s->data.size() >= 8) {
        auto br = s->reader();
        out.fov_offset = br.read<float>();
        out.fade_offset = br.read<float>();
        if (s->data.size() >= 16) {
            br.read<float>(); // end-distance cap
            out.depth_bias = br.read<float>();
        }
    }
    if (auto s = r.find(k_xemi); s && s->data.size() >= 4) {
        out.emittance = r.resolve(s->as<uint32_t>());
    }
    if (auto s = r.find(k_xesp); s && s->data.size() >= 5) {
        auto br = s->reader();
        out.enable_parent = r.resolve(br.read<uint32_t>());
        out.enable_opposite = (br.read<uint8_t>() & 0x01) != 0;
    }
    // XRMR is followed, without their names checked, by LNAM (bit 15), the image space (bit 14) and one XLRM per
    // linked room.
    const auto subs = r.subrecords();
    for (size_t i = 0; i < subs.size(); ++i) {
        const auto& s = subs[i];
        if (s.type == k_xrmr && s.data.size() >= 4) {
            uint32_t v;
            std::memcpy(&v, s.data.data(), 4);
            out.room_flags = v;
            size_t at = i + 1;
            auto next_form = [&]() -> uint32_t {
                if (at < subs.size() && subs[at].data.size() >= 4) {
                    uint32_t f;
                    std::memcpy(&f, subs[at].data.data(), 4);
                    ++at;
                    return r.resolve(f);
                }
                return 0;
            };
            if (v & 0x8000) {
                out.room_template = next_form();
            }
            if (v & 0x4000) {
                out.room_image_space = next_form();
            }
            for (uint32_t n = v & 0x3FFF; n > 0 && at < subs.size(); --n) {
                if (subs[at].type == k_xrmr) {
                    ++at; // a repeated XRMR inside the run is skipped
                }
                if (const uint32_t room = next_form()) {
                    out.room_links.push_back(room);
                }
            }
            break;
        }
    }
    if (auto s = r.find(k_xpod); s && s->data.size() >= 8) {
        auto br = s->reader();
        out.portal_rooms[0] = r.resolve(br.read<uint32_t>());
        out.portal_rooms[1] = r.resolve(br.read<uint32_t>());
    }
    if (auto s = r.find(k_xord); s && s->data.size() >= 16) {
        auto br = s->reader();
        for (auto& v : out.occlusion_links) {
            v = r.resolve(br.read<uint32_t>());
        }
    }
    if (auto s = r.find(k_xmbr); s && s->data.size() >= 4) {
        out.multibound = r.resolve(s->as<uint32_t>());
    }
    if (auto s = r.find(k_xtel); s && s->data.size() >= 28) {
        auto br = s->reader();
        out.teleport_door = r.resolve(br.read<uint32_t>());
        for (auto& v : out.teleport_position) {
            v = br.read<float>();
        }
        for (auto& v : out.teleport_rotation) {
            v = br.read<float>();
        }
        out.teleport_flags = s->data.size() >= 32 ? br.read<uint32_t>() : 0;
    }
}

// A cell's LAND and placed references (REFR), counting what is not exported.
void read_cell_children(const EsmDatabase& db, PendingCell& c) {
    for (auto child : db.children(c.entry->form_id)) {
        if (child->type == k_land) {
            // A few cells carry a second LAND added by a DLC (e.g. Dawnguard's Volkihar
            // exterior); the last-loaded plugin's terrain is the one the game shows.
            if (!c.land || child->plugin > c.land->plugin) {
                c.land = child;
            }
            continue;
        }
        if (child->type == k_navm) {
            continue;
        }
        if (child->type == k_achr) {
            ++c.skipped["actor (ACHR)"];
            continue;
        }
        if (child->type != k_refr) {
            ++c.skipped["placed " + legacy::type_name(child->type)];
            continue;
        }
        PendingRef ref;
        read_ref(db.read(*child), child->group == legacy::group_cell_persistent, ref);
        c.refs.push_back(ref);
    }
}

// ---------------------------------------------------------------------------------------------
// Base objects

struct BaseInfo {
    std::string mesh; // normalised "meshes/..." path; empty when not rendered
    std::string skip; // reason when not rendered
    RecordType type = 0;
    uint32_t record_flags = 0;
    bool can_carry = false; // LIGH DATA flag 0x2
};

// Base types whose models load as fade type 2, items (form types 23, 26,
// 27, 30, 32, 41, 42, 45, 46, 51, 52).
constexpr RecordType k_item_types[] = {
    record_type("SCRL"), record_type("ARMO"), record_type("BOOK"), record_type("INGR"), record_type("MISC"),
    record_type("WEAP"), record_type("AMMO"), record_type("KEYM"), record_type("ALCH"), record_type("HAZD"),
    record_type("SLGM"),
};

// The category of a reference's fade node.
// STATs flagged Never Fades (0x4) never fade (metric 0); else references visible when distant
// (ref or base flag 0x8000) get fade type 6; else "Is Full LOD" references (0x10000) other than lights type 10;
// otherwise the base's type, items or objects. Bases with record flag 0x40 and TREEs would never fade too, but
// only with bEnableStippleFade:Display off (1 in the image and in the user's settings), so here they fade like
// the rest. Trees whose root is a leaf-animation node compute their metric anyway, as objects.
uint32_t fade_flags(const BaseInfo& base, uint32_t ref_flags, bool leaf_anim_tree) {
    if (base.type == k_tree && leaf_anim_tree) {
        return 0;
    }
    if (base.type == k_stat && (base.record_flags & 0x4)) {
        return osw::ref_fade_never;
    }
    if ((ref_flags & 0x8000) || (base.record_flags & 0x8000)) {
        return osw::ref_fade_visible_when_distant;
    }
    if ((ref_flags & 0x10000) && base.type != k_ligh) {
        return osw::ref_fade_sky_cell;
    }
    const bool item =
        std::find(std::begin(k_item_types), std::end(k_item_types), base.type) != std::end(k_item_types);
    return item || (base.type == k_ligh && base.can_carry) ? osw::ref_fade_items : 0;
}

// Editor markers. The primary rule is the base record's "Is Marker" flag (0x00800000), which the
// Creation Kit sets on every engine marker: XMarker, XMarkerHeading, door/map/north/COC/travel markers,
// furniture sit/lay markers, spawners, room/portal/collision markers. About 85 unflagged bases still
// point at editor-only meshes (link markers, guard markers, "AnimInteractionMarker", invisible
// activators and chairs), so a mesh is also treated as a marker when its file name starts with
// "marker", it lives in a "markers/" folder (except blackplane01.nif, real geometry used to hide the
// void), or its path says "invisible". Other furniture "...marker.nif" meshes are left alone: some
// carry visible geometry (e.g. the civil-war map markers), and the NIF importer should drop
// EditorMarker nodes anyway.
bool is_marker_mesh(std::string_view mesh) {
    auto rel = mesh.substr(std::min(mesh.size(), std::string_view("meshes/").size()));
    auto slash = rel.rfind('/');
    auto file = slash == std::string_view::npos ? rel : rel.substr(slash + 1);
    if (file.starts_with("marker")) {
        return true;
    }
    if ((rel.starts_with("markers/") || rel.find("/markers/") != std::string_view::npos) &&
        file != "blackplane01.nif") {
        return true;
    }
    return rel.find("invisible") != std::string_view::npos;
}

BaseInfo classify_base(const EsmDatabase& db, uint32_t form_id) {
    if (form_id == 0) {
        return {{}, "no base object"};
    }
    auto e = db.find(form_id);
    if (!e) {
        return {{}, "missing base object"};
    }
    if (e->deleted()) {
        return {{}, "deleted base object"};
    }
    if (std::find(std::begin(k_renderable_types), std::end(k_renderable_types), e->type) ==
        std::end(k_renderable_types)) {
        return {{}, "base type " + legacy::type_name(e->type)};
    }
    if (e->flags & legacy::record_is_marker) {
        return {{}, "marker (base flag)"};
    }
    auto r = db.read(*e);
    std::string_view model;
    if (e->type == k_armo) {
        model = r.string(k_mod2);
        if (model.empty()) {
            model = r.string(k_mod4);
        }
    } else {
        model = r.string(k_modl);
    }
    auto mesh = path::with_root(path::normalize(model), "meshes");
    if (model.empty() || mesh == "meshes/") {
        return {{}, e->type == k_ligh ? "light without model" : "no model"};
    }
    if (is_marker_mesh(mesh)) {
        return {{}, "marker (model)"};
    }
    BaseInfo info{mesh, {}, e->type, e->flags};
    if (e->type == k_ligh) {
        // DATA: time, radius, colour, then the flags.
        if (auto s = r.find(k_data); s && s->data.size() >= 16) {
            uint32_t flags = 0;
            std::memcpy(&flags, s->data.data() + 12, 4);
            info.can_carry = (flags & 0x2) != 0;
        }
    }
    return info;
}

// ---------------------------------------------------------------------------------------------
// Visibility at game start

class EnableState {
public:
    explicit EnableState(const EsmDatabase& db) : m_db(db) {}

    void add(const PendingRef& r) {
        m_refs.emplace(r.form_id, Info{r.record_flags, r.enable_parent, r.enable_opposite});
    }

    bool enabled(uint32_t form_id) {
        if (auto it = m_memo.find(form_id); it != m_memo.end()) {
            return it->second;
        }
        bool result = compute(form_id, 0);
        m_memo[form_id] = result;
        return result;
    }

    size_t missing_parents = 0;
    size_t cycles = 0;

private:
    struct Info {
        uint32_t flags;
        uint32_t parent;
        bool opposite;
    };

    std::optional<Info> lookup(uint32_t form_id) {
        if (auto it = m_refs.find(form_id); it != m_refs.end()) {
            return it->second;
        }
        // Enable parents may live anywhere: another world, an interior, an actor.
        auto rec = m_db.get(form_id);
        if (!rec) {
            return std::nullopt;
        }
        Info info{rec->flags(), 0, false};
        if (auto s = rec->find(k_xesp); s && s->data.size() >= 5) {
            auto br = s->reader();
            info.parent = rec->resolve(br.read<uint32_t>());
            info.opposite = (br.read<uint8_t>() & 0x01) != 0;
        }
        m_refs.emplace(form_id, info);
        return info;
    }

    bool compute(uint32_t form_id, int depth) {
        if (auto it = m_memo.find(form_id); it != m_memo.end()) {
            return it->second;
        }
        if (form_id == k_player_ref) {
            return true;
        }
        if (depth > 64) {
            ++cycles;
            return true;
        }
        auto info = lookup(form_id);
        if (!info) {
            ++missing_parents;
            return true;
        }
        bool result;
        if (info->parent != 0) {
            bool parent = compute(info->parent, depth + 1);
            result = info->opposite ? !parent : parent;
        } else {
            result = (info->flags & legacy::record_initially_disabled) == 0;
        }
        m_memo[form_id] = result;
        return result;
    }

    const EsmDatabase& m_db;
    std::unordered_map<uint32_t, Info> m_refs;
    std::unordered_map<uint32_t, bool> m_memo;
};

// ---------------------------------------------------------------------------------------------
// Math

struct Quat {
    float x, y, z, w;
};

Quat mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// R = Rx(-x) * Ry(-y) * Rz(-z), column vectors, counter-clockwise right-handed rotations: Z applies
// first. Our best reading of Gamebryo's convention; source_euler keeps the raw angles for checking.
Quat euler_to_quat(const float rot[3]);

// ---------------------------------------------------------------------------------------------
// Water currents: the game's element list per cell. Elements are
// 3 quads (types 0 uniform, 1 circle) or 4 (types 2 capsule, 3 half-disc), the type an int in quad 2's w.

constexpr uint32_t k_current_marker = 0x00000061; // STAT CellWaterCurrentMarker
constexpr uint32_t k_collision_marker = 0x00000021; // STAT CollisionMarker

int current_type(const osw::CurrentQuad& q2) {
    int32_t type = 0;
    std::memcpy(&type, &q2.v[3], 4);
    return type;
}

// Index of the first uniform element's first quad, or -1.
int first_uniform(const std::vector<osw::CurrentQuad>& list) {
    for (size_t i = 0; i + 2 < list.size();) {
        const int type = current_type(list[i + 2]);
        if (type == 0) {
            return int(i);
        }
        i += (type == 2 || type == 3) ? 4 : 3;
    }
    return -1;
}

void append_uniform(std::vector<osw::CurrentQuad>& list, const float lin[3], const float ang[3]) {
    osw::CurrentQuad q0{{lin[0], lin[1], lin[2], 0.0f}};
    osw::CurrentQuad q1{{ang[0], ang[1], ang[2], 0.0f}};
    osw::CurrentQuad q2{{0.0f, 0.0f, 0.0f, 0.0f}}; // type 0
    list.insert(list.end(), {q0, q1, q2});
}

// A CellWaterCurrentMarker: its forward (+Y) axis times its scale is the cell's uniform current, Havok m/s.
void apply_current_marker(std::vector<osw::CurrentQuad>& list, const float rot[3], float scale);

// The cell water's WATR velocities fill a still-zero uniform element, or make one if the cell has no list.
void apply_water_current(std::vector<osw::CurrentQuad>& list, const float lin[3], const float ang[3]) {
    const bool lin_set = lin[0] != 0 || lin[1] != 0 || lin[2] != 0;
    const bool ang_set = ang[0] != 0 || ang[1] != 0 || ang[2] != 0;
    if (!lin_set && !ang_set) {
        return;
    }
    if (list.empty()) {
        append_uniform(list, lin, ang);
        return;
    }
    const int u = first_uniform(list);
    if (u < 0) {
        return;
    }
    auto& q0 = list[size_t(u)].v;
    auto& q1 = list[size_t(u) + 1].v;
    if (q0[0] == 0 && q0[1] == 0 && q0[2] == 0) {
        q0[0] = lin[0], q0[1] = lin[1], q0[2] = lin[2];
    }
    if (q1[0] == 0 && q1[1] == 0 && q1[2] == 0) {
        q1[0] = ang[0], q1[1] = ang[1], q1[2] = ang[2];
    }
}

Quat euler_to_quat(const float rot[3]) {
    auto axis = [](int i, float angle) {
        float s = std::sin(angle * 0.5f);
        Quat q{0.0f, 0.0f, 0.0f, std::cos(angle * 0.5f)};
        (i == 0 ? q.x : i == 1 ? q.y : q.z) = s;
        return q;
    };
    return mul(mul(axis(0, -rot[0]), axis(1, -rot[1])), axis(2, -rot[2]));
}

void apply_current_marker(std::vector<osw::CurrentQuad>& list, const float rot[3], float scale) {
    const Quat q = euler_to_quat(rot);
    // R (0, 1, 0): the second column of the rotation.
    const float v[3] = {scale * 2.0f * (q.x * q.y - q.w * q.z), scale * (1.0f - 2.0f * (q.x * q.x + q.z * q.z)),
                        scale * 2.0f * (q.y * q.z + q.w * q.x)};
    if (v[0] == 0 && v[1] == 0 && v[2] == 0) {
        return;
    }
    const int u = first_uniform(list);
    if (u >= 0) {
        auto& q0 = list[size_t(u)].v;
        q0[0] = v[0], q0[1] = v[1], q0[2] = v[2];
    } else {
        const float zero[3] = {0, 0, 0};
        append_uniform(list, v, zero);
    }
}

void rotate(const Quat& q, const float v[3], float out[3]) {
    const Quat p{v[0], v[1], v[2], 0.0f};
    const Quat r = mul(mul(q, p), Quat{-q.x, -q.y, -q.z, q.w});
    out[0] = r.x;
    out[1] = r.y;
    out[2] = r.z;
}

// ---------------------------------------------------------------------------------------------
// Placed lights

enum LighFlag : uint32_t {
    ligh_negative = 0x4,
    ligh_flicker = 0x8,
    ligh_off_by_default = 0x20,
    ligh_pulse = 0x80,
    ligh_pulse_slow = 0x100,
    ligh_shadow_spot = 0x400,
    ligh_shadow_hemisphere = 0x800,
    ligh_shadow_omni = 0x1000,
    ligh_portal_strict = 0x2000,
};

enum LightRefFlag : uint32_t {
    ref_turn_off_fire = 0x80,
    ref_no_water = 0x100,
    ref_never_fades = 0x10000,
    ref_no_land = 0x20000,
};

struct LightBase {
    bool valid = false;
    uint32_t radius = 0;    // DATA, units
    uint8_t color[3] = {};  // DATA
    uint32_t flags = 0;     // DATA
    float falloff = 1.0f;   // DATA, shadow-casting lights: 0 becomes 1
    float fov = 90.0f;      // DATA, degrees: 0 becomes 90
    float near_clip = 0.0f; // DATA, units
    float frequency = 0.0f; // DATA, clamped to [0, 1] at load
    float intensity_amplitude = 0.0f;
    float movement_amplitude = 0.0f; // units
    float fade = 1.0f;               // FNAM (constructor default 1)
    std::string model;               // normalised "meshes/..." path, or empty
};

LightBase read_light_base(const EsmDatabase& db, uint32_t form_id) {
    LightBase b;
    auto rec = db.get(form_id);
    if (!rec || rec->type() != k_ligh) {
        return b;
    }
    if (auto s = rec->find(k_data); s && s->data.size() >= 40) {
        auto br = s->reader();
        br.read<int32_t>(); // time
        b.radius = br.read<uint32_t>();
        for (auto& c : b.color) {
            c = br.read<uint8_t>();
        }
        br.read<uint8_t>();
        b.flags = br.read<uint32_t>();
        b.falloff = br.read<float>(); // falloff exponent, FOV, near clip: shadow-casting lights only
        b.fov = br.read<float>();
        b.near_clip = br.read<float>();
        b.falloff = b.falloff != 0.0f ? b.falloff : 1.0f;
        b.fov = b.fov != 0.0f ? b.fov : 90.0f;
        b.frequency = std::clamp(br.read<float>(), 0.0f, 1.0f);
        b.intensity_amplitude = br.read<float>();
        b.movement_amplitude = br.read<float>();
        b.valid = true;
    }
    if (auto s = rec->find(k_fnam); s && s->data.size() >= 4) {
        b.fade = s->as<float>();
    }
    if (auto model = rec->string(k_modl); !model.empty()) {
        b.model = path::with_root(path::normalize(model), "meshes");
    }
    return b;
}

// The weather a region's emittance takes its colour from: the engine picks one of the region's weathers at random,
// weighted by chance, and keeps it; this takes the most likely one (every region used by exterior lights has one).
uint32_t region_weather(const EsmDatabase& db, uint32_t region) {
    auto rec = db.get(region);
    if (!rec || rec->type() != k_regn) {
        return 0;
    }
    uint32_t best = 0;
    uint32_t best_chance = 0;
    bool weather_data = false;
    for (const auto& s : rec->subrecords()) {
        if (s.type == k_rdat) {
            weather_data = s.data.size() >= 4 && s.as<uint32_t>() == 3;
        } else if (s.type == k_rdwt && weather_data) {
            const size_t stride = s.data.size() % 12 == 0 ? 12 : 8;
            for (size_t o = 0; o + 8 <= s.data.size(); o += stride) {
                uint32_t form, chance;
                std::memcpy(&form, s.data.data() + o, 4);
                std::memcpy(&chance, s.data.data() + o + 4, 4);
                if (best == 0 || chance > best_chance) {
                    best = rec->resolve(form);
                    best_chance = chance;
                }
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------------------------
// Directional materials: STAT DNAM and its MATO.

// each DNAM is a whole NIF stream; the multi pass clones the last BSLightingShaderProperty among
// their roots (section 11.2.3). nullopt when there is none (the moss MATOs), which draws no second pass.
std::optional<osw::DirectionalPass> read_mato_pass(const Record& mato) {
    std::optional<osw::DirectionalPass> out;
    for (const auto& s : mato.subrecords()) {
        if (s.type != k_dnam) {
            continue;
        }
        legacy::NifFile nif;
        try {
            nif = legacy::read_nif(s.data);
        } catch (const ParseError& e) {
            log::warn("MATO {:08X}: DNAM: {}", mato.form_id(), e.what());
            continue;
        }
        for (size_t b = 0; b < nif.blocks.size(); ++b) {
            const auto* p = nif.get<legacy::BSLightingShaderProperty>(static_cast<legacy::NifRef>(b));
            if (!p) {
                continue;
            }
            osw::DirectionalPass pass;
            pass.shader_type = p->shader_type;
            pass.flags1 = p->flags1;
            pass.flags2 = p->flags2;
            pass.uv_offset[0] = p->uv_offset.x;
            pass.uv_offset[1] = p->uv_offset.y;
            pass.uv_scale[0] = p->uv_scale.x;
            pass.uv_scale[1] = p->uv_scale.y;
            for (int k = 0; k < 3; ++k) {
                pass.emissive[k] = p->emissive_color[k] * p->emissive_multiple;
                pass.specular[k] = p->specular_color[k] * p->specular_strength;
            }
            pass.alpha = p->alpha;
            pass.glossiness = p->glossiness;
            if (p->shader_type == 14) {
                std::copy_n(p->type_params.begin(), 4, pass.sparkle);
            }
            if (const auto* ts = nif.get<legacy::BSShaderTextureSet>(p->texture_set)) {
                auto texture = [&](size_t slot) {
                    return slot < ts->textures.size() && !ts->textures[slot].empty()
                               ? path::with_root(path::normalize(ts->textures[slot]), "textures")
                               : std::string();
                };
                pass.diffuse = texture(0);
                pass.back_light = texture(7);
            }
            out = std::move(pass);
        }
    }
    return out;
}

std::optional<osw::DirectionalMaterial> read_directional(const EsmDatabase& db, uint32_t base,
                                                          std::vector<osw::DirectionalPass>& passes) {
    auto e = db.find(base);
    if (!e || e->type != k_stat || e->deleted()) {
        return std::nullopt;
    }
    auto rec = db.read(*e);
    const auto* dnam = rec.find(k_dnam);
    if (!dnam || dnam->data.size() < 8) {
        return std::nullopt;
    }
    osw::DirectionalMaterial d{};
    d.pass = osw::k_none;
    d.base_form_id = base;
    std::memcpy(&d.max_angle, dnam->data.data(), 4);
    uint32_t mato_local;
    std::memcpy(&mato_local, dnam->data.data() + 4, 4);
    // The game reads the flags byte only for form version 44 and later.
    if (dnam->data.size() >= 9 && db.form_version(*e) > 43 && (static_cast<uint8_t>(dnam->data[8]) & 1)) {
        d.flags |= osw::directional_base_snow;
    }
    // the class defaults stand for anything a short DATA leaves out.
    float data[13] = {0.4f, 0.4f, 512.0f, 512.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t single_pass = 0, snow = 0;
    if (auto mato = mato_local ? db.get(rec.resolve(mato_local)) : std::nullopt; mato && mato->type() == k_mato) {
        d.mato = mato->form_id();
        if (const auto* s = mato->find(k_data)) {
            const size_t n = std::min<size_t>(s->data.size(), 52);
            std::memcpy(data, s->data.data(), std::min<size_t>(n, 44));
            if (n >= 48) {
                std::memcpy(&single_pass, s->data.data() + 44, 4);
            }
            if (n >= 52) {
                std::memcpy(&snow, s->data.data() + 48, 4);
            }
        }
        d.flags |= single_pass ? osw::directional_projected : osw::directional_multi_pass;
        if (snow) {
            d.flags |= osw::directional_mato_snow;
        }
        d.pass = osw::k_none;
        if (!single_pass) {
            if (auto pass = read_mato_pass(*mato)) {
                d.pass = static_cast<uint32_t>(passes.size());
                passes.push_back(std::move(*pass));
            }
        }
    }
    if (d.flags == 0) {
        return std::nullopt; // a max angle alone changes nothing
    }
    d.falloff_scale = data[0];
    d.falloff_bias = data[1];
    d.noise_uv_scale = data[2];
    d.material_uv_scale = data[3];
    std::copy_n(data + 4, 3, d.projection);
    d.normal_dampener = data[7];
    std::copy_n(data + 8, 3, d.color);
    return d;
}

int32_t grid_of(float units) {
    return static_cast<int32_t>(std::floor(units / units::k_cell_size_units));
}

const RecordEntry* find_world_entry(const EsmDatabase& db, std::string_view edid) {
    auto want = path::to_lower(edid);
    for (auto e : db.of_type(k_wrld)) {
        if (path::to_lower(db.read(*e).edid()) == want) {
            return e;
        }
    }
    return nullptr;
}

// Grid -> LAND of a world's exterior cells, for child worlds that borrow their parent's terrain.
std::unordered_map<CellCoord, const RecordEntry*, CellCoordHash> land_by_grid(const EsmDatabase& db,
                                                                              uint32_t world) {
    std::unordered_map<CellCoord, const RecordEntry*, CellCoordHash> out;
    for (auto c : db.children(world)) {
        if (c->type != k_cell || c->group == legacy::group_world_children) {
            continue;
        }
        const RecordEntry* land = nullptr;
        for (auto child : db.children(c->form_id)) {
            if (child->type == k_land && (!land || child->plugin > land->plugin)) {
                land = child;
            }
        }
        if (!land) {
            continue;
        }
        auto r = db.read(*c);
        if (auto s = r.find(k_xclc); s && s->data.size() >= 8) {
            auto br = s->reader();
            CellCoord coord;
            coord.x = br.read<int32_t>();
            coord.y = br.read<int32_t>();
            out[coord] = land;
        }
    }
    return out;
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

} // namespace

size_t WorldImport::skipped_total() const {
    size_t n = 0;
    for (const auto& [reason, count] : skipped) {
        n += count;
    }
    return n;
}

size_t drop_meshes(WorldImport& import, const std::vector<bool>& keep) {
    auto& world = import.world;
    if (keep.size() != world.meshes.size()) {
        throw Error(std::format("drop_meshes: {} flags for {} meshes", keep.size(), world.meshes.size()));
    }
    std::vector<uint32_t> remap(world.meshes.size(), osw::k_none);
    std::vector<std::string> meshes;
    std::vector<std::string> sources;
    for (size_t i = 0; i < keep.size(); ++i) {
        if (keep[i]) {
            remap[i] = static_cast<uint32_t>(meshes.size());
            meshes.push_back(std::move(world.meshes[i]));
            sources.push_back(std::move(import.source_meshes[i]));
        }
    }
    world.meshes = std::move(meshes);
    import.source_meshes = std::move(sources);
    for (auto& g : world.grasses) {
        g.mesh = g.mesh == osw::k_none ? osw::k_none : remap[g.mesh];
    }

    size_t dropped = 0;
    size_t out = 0;
    for (auto& cell : world.cells) {
        auto first = static_cast<uint32_t>(out);
        for (uint32_t i = cell.first_ref; i < cell.first_ref + cell.ref_count; ++i) {
            auto ref = world.refs[i];
            if (remap[ref.mesh] == osw::k_none) {
                ++dropped;
                continue;
            }
            ref.mesh = remap[ref.mesh];
            world.refs[out++] = ref;
        }
        cell.first_ref = first;
        cell.ref_count = static_cast<uint32_t>(out) - first;
    }
    world.refs.resize(out);
    if (dropped) {
        import.skipped["empty mesh"] += dropped;
    }
    return dropped;
}

std::vector<std::string> list_worldspaces(const legacy::EsmDatabase& db) {
    std::vector<std::string> out;
    for (auto w : db.of_type(k_wrld)) {
        auto cells = db.children(w->form_id);
        bool exterior = std::any_of(cells.begin(), cells.end(), [](const RecordEntry* c) {
            return c->type == k_cell && c->group != legacy::group_world_children;
        });
        if (exterior) {
            out.emplace_back(db.read(*w).edid());
        }
    }
    return out;
}

namespace {

// One source world's cells on its grid (import steps 1-3): decoded, cells at the same coordinates merged, the
// world-level persistent cell's references moved into the cell under them.
struct GatheredWorld {
    WorldInfo info;
    WorldInfo land_world;
    WorldInfo water_world;
    bool borrow_land = false;
    float default_land_units = 0.0f;
    std::vector<PendingCell> exterior;
    std::unordered_map<CellCoord, size_t, CellCoordHash> grid;
    size_t synthetic = 0;
    size_t no_water_sentinels = 0;
    size_t flat_lands = 0;
    double decode_ms = 0.0;
};

GatheredWorld gather_world(const EsmDatabase& db, const WorldInfo& info, uint16_t tag,
                           std::map<std::string, size_t>& skipped) {
    GatheredWorld g;
    g.info = info;
    // Child worlds inherit terrain and water from their parent when PNAM says so.
    std::optional<WorldInfo> parent;
    if (info.parent) {
        parent = world_by_form(db, info.parent);
        if (!parent) {
            log::warn("{}: parent world {:08X} not found", info.edid, info.parent);
        }
    }
    g.land_world = info;
    for (int depth = 0; depth < 8 && (g.land_world.parent_use & parent_use_land) && g.land_world.parent;
         ++depth) {
        auto p = world_by_form(db, g.land_world.parent);
        if (!p) {
            break;
        }
        g.land_world = *p;
    }
    g.borrow_land = g.land_world.entry != info.entry;
    g.water_world = info;
    if ((info.parent_use & parent_use_water) && parent) {
        g.water_world = *parent;
    }
    g.default_land_units =
        (g.borrow_land || !info.has_dnam) && g.land_world.has_dnam ? g.land_world.land_height : info.land_height;

    // 1. Cells of this world.
    std::vector<PendingCell> cells;
    for (auto c : db.children(info.entry->form_id)) {
        if (c->type != k_cell) {
            continue;
        }
        PendingCell pc;
        pc.entry = c;
        pc.world = tag;
        pc.world_persistent = c->group == legacy::group_world_children;
        cells.push_back(std::move(pc));
    }

    std::unordered_map<CellCoord, const RecordEntry*, CellCoordHash> borrowed;
    if (g.borrow_land) {
        borrowed = land_by_grid(db, g.land_world.entry->form_id);
    }

    // 2. Decode cells, land and references in parallel.
    auto t1 = std::chrono::steady_clock::now();
    parallel_for(cells.size(), [&](size_t i) {
        auto& c = cells[i];
        try {
            auto rec = db.read(*c.entry);
            read_cell_header(rec, c, g.water_world);
            c.header_ok = true;
            read_cell_children(db, c);
            if (g.borrow_land && !c.world_persistent) {
                auto it = borrowed.find(c.coord);
                c.land = it == borrowed.end() ? nullptr : it->second;
            }
            if (c.land) {
                c.decoded = std::make_unique<PendingLand>();
                decode_land(db.read(*c.land), g.default_land_units, *c.decoded, c.warnings);
            }
        } catch (const std::exception& e) {
            c.error = e.what();
        }
    });
    g.decode_ms = ms_since(t1);

    // 3. Cell grid: merge duplicates, move world-persistent refs into the cell under them.
    std::vector<PendingRef> persistent_refs;
    for (auto& c : cells) {
        for (const auto& [reason, n] : c.skipped) {
            skipped[reason] += n;
        }
        for (const auto& [what, n] : c.warnings) {
            log::warn("{}: cell {:08X}: {} (x{})", info.edid, c.entry->form_id, what, n);
        }
        if (!c.error.empty()) {
            log::warn("{}: cell {:08X}: {}", info.edid, c.entry->form_id, c.error);
        }
        if (!c.header_ok) {
            continue; // no grid position to put it at
        }
        if (c.world_persistent) {
            persistent_refs.insert(persistent_refs.end(), c.refs.begin(), c.refs.end());
            continue;
        }
        g.no_water_sentinels += c.no_water_sentinel ? 1 : 0;
        g.flat_lands += c.decoded && c.decoded->flat ? 1 : 0;
        if (auto it = g.grid.find(c.coord); it != g.grid.end()) {
            auto& first = g.exterior[it->second];
            log::warn("{}: cells {:08X} and {:08X} both at ({}, {}); merging", info.edid,
                      first.entry->form_id, c.entry->form_id, c.coord.x, c.coord.y);
            first.refs.insert(first.refs.end(), c.refs.begin(), c.refs.end());
            if (!first.decoded && c.decoded) {
                first.decoded = std::move(c.decoded);
            }
            continue;
        }
        g.grid[c.coord] = g.exterior.size();
        g.exterior.push_back(std::move(c));
    }
    cells.clear();

    for (auto& r : persistent_refs) {
        r.persistent = true;
        CellCoord coord{grid_of(r.position[0]), grid_of(r.position[1])};
        auto it = g.grid.find(coord);
        if (it == g.grid.end()) {
            PendingCell c;
            c.coord = coord;
            c.world = tag;
            c.has_water = g.water_world.water != 0;
            c.water_height = g.water_world.water_height;
            c.water = g.water_world.water;
            if (g.borrow_land) {
                if (auto b = borrowed.find(coord); b != borrowed.end()) {
                    c.land = b->second;
                    c.decoded = std::make_unique<PendingLand>();
                    decode_land(db.read(*c.land), g.default_land_units, *c.decoded, c.warnings);
                }
            }
            it = g.grid.emplace(coord, g.exterior.size()).first;
            g.exterior.push_back(std::move(c));
            ++g.synthetic;
        }
        g.exterior[it->second].refs.push_back(r);
    }
    return g;
}

// The world a reference sits in (its cell's WRLD), 0 for interiors and unknown references.
uint32_t world_of_ref(const EsmDatabase& db, uint32_t ref) {
    const RecordEntry* e = db.find(ref);
    if (!e || !e->parent) {
        return 0;
    }
    const RecordEntry* cell = db.find(e->parent);
    return cell && cell->type == k_cell ? cell->parent : 0;
}

std::optional<std::array<float, 3>> ref_position(const EsmDatabase& db, uint32_t ref) {
    auto r = db.get(ref);
    if (!r) {
        return std::nullopt;
    }
    if (auto s = r->find(k_data); s && s->data.size() >= 12) {
        std::array<float, 3> p{};
        std::memcpy(p.data(), s->data.data(), 12);
        return p;
    }
    return std::nullopt;
}

// What the parent shows of a city from outside and the merged world replaces: statics named as low-detail copies
// (WRHouseBanneredMare01LOD, SBluePalace_LOD, MrkCityLandscapeLOD, WHmarket01LOD02).
bool is_stand_in(const EsmDatabase& db, uint32_t base) {
    auto r = db.get(base);
    return r && r->type() == k_stat && r->edid().find("LOD") != std::string_view::npos;
}

// A base's model, the same string for every base that draws the same mesh; else the base itself.
uint64_t model_key(const EsmDatabase& db, uint32_t base) {
    auto r = db.get(base);
    const auto model = r ? r->string(k_modl) : std::string_view();
    return model.empty() ? base : std::hash<std::string>()(path::to_lower(model));
}

// A visible-when-distant STAT's size (OBND extents, units), or nothing for any other base. The parent's copy of a
// city building is such a static, and so is the city's own.
std::optional<std::array<float, 3>> distant_static_size(const EsmDatabase& db, uint32_t base) {
    const RecordEntry* e = db.find(base);
    if (!e || e->type != k_stat || !(e->flags & 0x8000)) {
        return std::nullopt;
    }
    auto r = db.read(*e);
    auto b = r.find(k_obnd);
    if (!b || b->data.size() < 12) {
        return std::nullopt;
    }
    int16_t v[6];
    std::memcpy(v, b->data.data(), 12);
    return std::array<float, 3>{float(v[3] - v[0]), float(v[4] - v[1]), float(v[5] - v[2])};
}

// A merged child's cells into the parent's grid. Where both worlds have a cell the child's header, land (its own,
// or the parent's it borrows) and references win. The parent's stand-ins go from every cell the child spans; of the
// rest it keeps what the child does not duplicate: the same model within a unit of the same place, or the same
// building (a distant static of about the same size within k_twin_units, often a variant of the base).
void merge_child(const EsmDatabase& db, GatheredWorld& into, GatheredWorld&& child, WorldImport& result) {
    struct Key {
        uint64_t model;
        int32_t x, y, z;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return std::hash<uint64_t>()(k.model ^ (uint64_t(uint32_t(k.x)) * 73856093u) ^
                                         (uint64_t(uint32_t(k.y)) * 19349663u) ^ uint32_t(k.z));
        }
    };
    std::unordered_map<uint32_t, uint64_t> models;
    std::unordered_map<uint32_t, bool> stand_ins;
    auto key = [&](const PendingRef& r) {
        auto [it, fresh] = models.try_emplace(r.base, 0);
        if (fresh) {
            it->second = model_key(db, r.base);
        }
        return Key{it->second, int32_t(std::lround(r.position[0])), int32_t(std::lround(r.position[1])),
                   int32_t(std::lround(r.position[2]))};
    };
    size_t shared = 0, parent_kept = 0, parent_duplicates = 0, added = 0, removed_stand_ins = 0, twins = 0;
    constexpr float k_twin_units = 128.0f;
    constexpr float k_twin_size = 0.2f;
    constexpr float k_same_spot_units = 16.0f; // on the very spot the size doesn't matter (RTOrphanage01)
    std::unordered_map<uint32_t, std::optional<std::array<float, 3>>> sizes;
    auto size_of = [&](uint32_t base) -> const std::optional<std::array<float, 3>>& {
        auto [it, fresh] = sizes.try_emplace(base);
        if (fresh) {
            it->second = distant_static_size(db, base);
        }
        return it->second;
    };
    struct Building {
        std::array<float, 3> position;
        std::array<float, 3> size;
    };
    std::vector<Building> buildings;
    std::vector<std::array<float, 3>> statics; // every static of the child, for the same-spot test
    for (const auto& c : child.exterior) {
        for (const auto& r : c.refs) {
            if (const RecordEntry* e = db.find(r.base); e && e->type == k_stat && !(e->flags & legacy::record_is_marker)) {
                statics.push_back({r.position[0], r.position[1], r.position[2]});
            }
            if (const auto& size = size_of(r.base)) {
                buildings.push_back({{r.position[0], r.position[1], r.position[2]}, *size});
            }
        }
    }
    auto twin = [&](const PendingRef& r) {
        const auto& size = size_of(r.base);
        if (!size) {
            return false;
        }
        // The outside of a city wall on the very spot of the inside (WRWallMainGate01EXT): the half with the gate's plinths
        // and paving, which the city's copy doesn't have.
        if (const auto base = db.get(r.base); base && base->edid().ends_with("EXT")) {
            log::info("{}: kept the outside half {} at {:.0f} {:.0f}", into.info.edid, base->edid(), r.position[0], r.position[1]);
            return false;
        }
        for (const auto& p : statics) {
            if (std::hypot(p[0] - r.position[0], p[1] - r.position[1], p[2] - r.position[2]) <= k_same_spot_units) {
                return true;
            }
        }
        for (const auto& b : buildings) {
            const float distance = std::hypot(b.position[0] - r.position[0], b.position[1] - r.position[1],
                                              b.position[2] - r.position[2]);
            if (distance > k_twin_units) {
                continue;
            }
            bool same = true;
            for (int k = 0; k < 3; ++k) {
                same = same && std::fabs(b.size[k] - (*size)[k]) <= k_twin_size * std::max(b.size[k], (*size)[k]);
            }
            if (same) {
                return true;
            }
        }
        return false;
    };
    for (auto& c : child.exterior) {
        auto it = into.grid.find(c.coord);
        if (it == into.grid.end()) {
            into.grid[c.coord] = into.exterior.size();
            into.exterior.push_back(std::move(c));
            ++added;
            continue;
        }
        ++shared;
        PendingCell& p = into.exterior[it->second];
        removed_stand_ins += std::erase_if(p.refs, [&](const PendingRef& r) {
            auto [at, fresh] = stand_ins.try_emplace(r.base, false);
            if (fresh) {
                at->second = is_stand_in(db, r.base);
            }
            return at->second;
        });
        // Child worlds span far more cells than the city: a cell with nothing of its own stays the parent's.
        if (c.refs.empty() && (child.borrow_land || !c.land)) {
            continue;
        }
        std::unordered_set<Key, KeyHash> own;
        for (const auto& r : c.refs) {
            // Rounded positions: a duplicate a fraction of a unit off may round apart, so its neighbours count too.
            const Key k = key(r);
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        own.insert({k.model, k.x + dx, k.y + dy, k.z + dz});
                    }
                }
            }
        }
        for (const auto& r : p.refs) {
            if (own.contains(key(r))) {
                ++parent_duplicates;
                continue;
            }
            if (twin(r)) {
                ++twins;
                continue;
            }
            ++parent_kept;
            ++result.merge_kept[r.base];
            c.refs.push_back(r);
        }
        if (!c.decoded && p.decoded) {
            c.decoded = std::move(p.decoded);
        }
        // A city's world keeps its water far below its streets (Whiterun's at -14000) and uses placed water; the river
        // running past it is the parent's.
        if (p.has_water && (!c.has_water || c.water_height < p.water_height)) {
            c.has_water = true;
            c.water_height = p.water_height;
            c.water = p.water;
            c.current = std::move(p.current);
        }
        p = std::move(c);
    }
    log::info("{}: merged {} ({} cells: {} shared, {} its own); of the parent's references there {} were stand-ins, "
              "{} duplicate the child's, {} are the same buildings, {} kept",
              into.info.edid, child.info.edid, shared + added, shared, added, removed_stand_ins, parent_duplicates,
              twins, parent_kept);
}

} // namespace

namespace {

// Load doors: where a teleport leads.
struct DoorContext {
    const MergedWorlds* merged = nullptr; // child world -> the world it is merged into
    std::string self;                     // this space's lowercase EditorID
};

std::optional<osw::Door> make_door(const EsmDatabase& db, const PendingRef& r, const DoorContext& ctx) {
    const RecordEntry* dest = db.find(r.teleport_door);
    const RecordEntry* base = db.find(r.base);
    if (!dest || dest->deleted() || !base || base->type != k_door || !dest->parent) {
        return std::nullopt;
    }
    const RecordEntry* cell = db.find(dest->parent);
    if (!cell || cell->type != k_cell) {
        return std::nullopt;
    }
    osw::Door d{};
    d.form_id = r.form_id;
    d.base_form_id = r.base;
    d.destination = r.teleport_door;
    d.teleport_flags = r.teleport_flags;
    if (auto s = db.read(*base).find(k_fnam); s && !s->data.empty() && (s->as<uint8_t>() & 0x02)) {
        d.flags |= osw::door_automatic;
    }
    if (cell->parent == 0) {
        d.flags |= osw::door_to_interior;
        d.destination_space = interior_space(db, cell->form_id);
    } else {
        auto w = db.find(cell->parent);
        if (!w || w->type != k_wrld) {
            return std::nullopt;
        }
        d.destination_space = path::to_lower(db.read(*w).edid());
        if (ctx.merged) {
            if (auto it = ctx.merged->find(cell->parent); it != ctx.merged->end()) {
                d.destination_space = it->second;
            }
        }
        if (d.destination_space == ctx.self) {
            d.flags |= osw::door_same_space;
        }
    }
    for (int k = 0; k < 3; ++k) {
        d.position[k] = units::to_metres(r.position[k]);
        d.arrival_position[k] = units::to_metres(r.teleport_position[k]);
        d.arrival_rotation[k] = r.teleport_rotation[k];
    }
    const Quat q = euler_to_quat(r.rotation);
    d.rotation[0] = q.x, d.rotation[1] = q.y, d.rotation[2] = q.z, d.rotation[3] = q.w;
    d.scale = r.scale;
    // OBND: the base's bounds, six int16 (min xyz, max xyz) in units.
    auto bounds = [&](uint32_t form, float* lo, float* hi) {
        auto b = db.get(form);
        if (!b) {
            return;
        }
        if (auto s = b->find(k_obnd); s && s->data.size() >= 12) {
            int16_t v[6];
            std::memcpy(v, s->data.data(), 12);
            for (int k = 0; k < 3; ++k) {
                lo[k] = units::to_metres(float(v[k]));
                hi[k] = units::to_metres(float(v[3 + k]));
            }
        }
    };
    bounds(r.base, d.bounds_min, d.bounds_max);
    // The destination door: its frame, and its own teleport (which arrives in front of this door).
    PendingRef other;
    read_ref(db.read(*dest), false, other);
    for (int k = 0; k < 3; ++k) {
        d.dest_position[k] = units::to_metres(other.position[k]);
    }
    const Quat dq = euler_to_quat(other.rotation);
    d.dest_rotation[0] = dq.x, d.dest_rotation[1] = dq.y, d.dest_rotation[2] = dq.z, d.dest_rotation[3] = dq.w;
    d.dest_scale = other.scale;
    bounds(other.base, d.dest_bounds_min, d.dest_bounds_max);
    if (other.teleport_door == r.form_id) {
        d.flags |= osw::door_has_marker;
        for (int k = 0; k < 3; ++k) {
            d.marker_position[k] = units::to_metres(other.teleport_position[k]);
        }
    }
    return d;
}

// Steps 4-6: base objects, land textures and materials, then the cells, terrain, references, lights and load doors
// of `exterior` (sorted by (y, x)) into result.world.
void assemble(const EsmDatabase& db, const std::string& edid, std::vector<PendingCell>& exterior,
              const LightAttachFn& attach, const LeafAnimFn& leaf_anim, const DoorContext& doors,
              WorldImport& result) {
    auto& world = result.world;
    // Rooms, portals and occluders, before the references that
    // name a room (XMBR).
    std::unordered_map<uint32_t, uint32_t> room_index, occluder_index;
    {
        auto quat = [](const float rot[3], float* out) {
            const Quat q = euler_to_quat(rot);
            out[0] = q.x, out[1] = q.y, out[2] = q.z, out[3] = q.w;
        };
        std::vector<const PendingRef*> rooms, portals, occluders;
        for (const auto& c : exterior) {
            for (const auto& r : c.refs) {
                if (r.base == k_room_marker && !(r.record_flags & 0x4000)) {
                    rooms.push_back(&r);
                } else if (r.base == k_portal_marker) {
                    portals.push_back(&r);
                } else if (r.base == k_plane_marker && (r.prim_type == 1 || r.prim_type == 3)) {
                    occluders.push_back(&r);
                }
            }
        }
        auto by_form = [](const PendingRef* a, const PendingRef* b) { return a->form_id < b->form_id; };
        std::sort(rooms.begin(), rooms.end(), by_form);
        std::sort(portals.begin(), portals.end(), by_form);
        std::sort(occluders.begin(), occluders.end(), by_form);
        for (const PendingRef* r : rooms) {
            room_index.emplace(r->form_id, static_cast<uint32_t>(room_index.size()));
        }
        for (const PendingRef* r : occluders) {
            occluder_index.emplace(r->form_id, static_cast<uint32_t>(occluder_index.size()));
        }
        auto room_of = [&](uint32_t form) {
            auto it = room_index.find(form);
            return it == room_index.end() ? osw::k_none : it->second;
        };
        for (const PendingRef* r : rooms) {
            osw::Room room{};
            room.form_id = r->form_id;
            const bool oriented = std::fabs(r->rotation[0]) >= 0.01f || std::fabs(r->rotation[1]) >= 0.01f ||
                                  std::fabs(r->rotation[2]) >= 0.01f;
            room.flags = oriented ? osw::room_oriented : 0;
            const float none[3] = {0, 0, 0};
            quat(oriented ? r->rotation : none, room.rotation);
            for (int k = 0; k < 3; ++k) {
                room.centre[k] = units::to_metres(r->position[k]);
                room.half_extents[k] = units::to_metres(r->prim_bounds[k]);
            }
            room.lighting_template = r->room_template;
            room.image_space = r->room_image_space;
            room.first_link = static_cast<uint32_t>(world.room_links.size());
            for (uint32_t l : r->room_links) {
                if (const uint32_t i = room_of(l); i != osw::k_none) {
                    world.room_links.push_back(i);
                }
            }
            room.link_count = static_cast<uint32_t>(world.room_links.size()) - room.first_link;
            room.lighting = osw::k_none;
            // A room's template replaces the colours, fog and light fade; the direction and clip stay the cell's
            // Exteriors have no portal graph to use it here.
            if (world.flags & osw::world_interior) {
                auto own = read_template(db, r->room_template);
                if (!own && r->room_image_space) {
                    own = world.lighting;
                }
                if (auto l = own) {
                    l->rotation_xy = world.lighting.rotation_xy;
                    l->rotation_z = world.lighting.rotation_z;
                    l->fog_clip = world.lighting.fog_clip;
                    l->lighting_template = r->room_template;
                    l->image_space = r->room_image_space ? r->room_image_space : world.lighting.image_space;
                    l->sunlight_scale = r->room_image_space ? sunlight_scale(db, r->room_image_space)
                                                            : world.lighting.sunlight_scale;
                    room.lighting = static_cast<uint32_t>(world.room_lighting.size());
                    world.room_lighting.push_back(*l);
                }
            }
            world.rooms.push_back(room);
        }
        for (const PendingRef* r : portals) {
            osw::Portal p{};
            p.form_id = r->form_id;
            p.rooms[0] = room_of(r->portal_rooms[0]);
            p.rooms[1] = room_of(r->portal_rooms[1]);
            for (int k = 0; k < 3; ++k) {
                p.centre[k] = units::to_metres(r->position[k]);
            }
            quat(r->rotation, p.rotation);
            p.half_width = units::to_metres(r->prim_bounds[0]);
            p.half_height = units::to_metres(r->prim_bounds[2]);
            world.portals.push_back(p);
        }
        for (const PendingRef* r : occluders) {
            osw::Occluder o{};
            o.form_id = r->form_id;
            o.type = static_cast<osw::OccluderType>(r->prim_type);
            for (int k = 0; k < 3; ++k) {
                o.centre[k] = units::to_metres(r->position[k]);
                o.half_extents[k] = units::to_metres(r->prim_bounds[k]);
            }
            quat(r->rotation, o.rotation);
            for (int k = 0; k < 4; ++k) {
                auto it = occluder_index.find(r->occlusion_links[k]);
                o.links[k] = it == occluder_index.end() ? osw::k_none : it->second;
            }
            world.occluders.push_back(o);
        }
    }
    // 4. Classify base objects once each, resolve visibility.
    auto t2 = std::chrono::steady_clock::now();
    std::unordered_map<uint32_t, BaseInfo> bases;
    EnableState enable(db);
    for (const auto& c : exterior) {
        for (const auto& r : c.refs) {
            enable.add(r);
            if (!bases.contains(r.base)) {
                bases.emplace(r.base, classify_base(db, r.base));
            }
        }
    }
    std::set<std::string> mesh_set;
    for (const auto& [form, b] : bases) {
        if (!b.mesh.empty()) {
            mesh_set.insert(b.mesh);
        }
    }
    result.source_meshes.assign(mesh_set.begin(), mesh_set.end());
    std::unordered_map<std::string, uint32_t> mesh_index;
    for (size_t i = 0; i < result.source_meshes.size(); ++i) {
        mesh_index[result.source_meshes[i]] = static_cast<uint32_t>(i);
        world.meshes.push_back(path::replace_extension(result.source_meshes[i], ".osm"));
    }

    // 5. Land textures used by this world's terrain. Layers whose LTEX is null or unresolvable use a
    //    synthetic entry (FormID 0) for the default texture; base textures use k_no_texture instead.
    std::set<uint32_t> ltex_ids;
    for (const auto& c : exterior) {
        if (!c.decoded) {
            continue;
        }
        for (int q = 0; q < 4; ++q) {
            if (c.decoded->base[q]) {
                ltex_ids.insert(c.decoded->base[q]);
            }
            for (uint32_t t : c.decoded->tex[q]) {
                if (t != 0 && t != k_default_ltex) {
                    ltex_ids.insert(t);
                }
            }
        }
    }
    std::unordered_map<uint32_t, uint16_t> ltex_index;
    std::unordered_map<uint32_t, uint32_t> grass_index;
    for (auto id : ltex_ids) {
        if (id == 0) {
            continue;
        }
        auto ltex = db.get(id);
        if (!ltex || ltex->type() != k_ltex) {
            log::warn("{}: land texture {:08X} missing; using the default", edid, id);
            continue;
        }
        osw::LandTexture t;
        t.form_id = id;
        if (auto s = ltex->find(k_snam); s && !s->data.empty()) {
            t.specular_exponent = s->as<uint8_t>();
        }
        if (auto s = ltex->find(k_inam); s && s->data.size() >= 4 && s->as<uint32_t>() != 0) {
            t.flags |= osw::land_texture_snow;
        }
        if (auto txst = db.get(ltex->form(k_tnam)); txst && txst->type() == k_txst) {
            if (auto d = txst->string(k_tx00); !d.empty()) {
                t.diffuse = path::with_root(path::normalize(d), "textures");
            }
            if (auto n = txst->string(k_tx01); !n.empty()) {
                t.normal = path::with_root(path::normalize(n), "textures");
            }
        }
        if (t.diffuse.empty()) {
            log::warn("{}: land texture {:08X} has no diffuse; using the default", edid, id);
            t.diffuse = k_default_land_texture;
            t.normal = k_default_land_normal;
        }
        // The grass it grows: GNAM in file order (the game appends at the tail).
        t.first_grass = static_cast<uint32_t>(world.land_grass.size());
        for (const auto& sub : ltex->subrecords()) {
            if (sub.type != k_gnam || sub.data.size() < 4) {
                continue;
            }
            const auto gras = db.get(ltex->resolve(sub.as<uint32_t>()));
            if (!gras || gras->type() != k_gras) {
                continue;
            }
            auto [it, added] = grass_index.emplace(gras->form_id(), static_cast<uint32_t>(world.grasses.size()));
            if (added) {
                osw::Grass g{};
                g.form_id = gras->form_id();
                g.mesh = osw::k_none;
                if (auto d = gras->find(k_data); d && d->data.size() >= 32) {
                    const auto* b = reinterpret_cast<const uint8_t*>(d->data.data());
                    g.density = b[0];
                    g.min_slope = b[1];
                    g.max_slope = b[2];
                    std::memcpy(&g.units_from_water, b + 4, 2);
                    std::memcpy(&g.water_type, b + 8, 4);
                    std::memcpy(&g.position_range, b + 12, 4);
                    std::memcpy(&g.height_range, b + 16, 4);
                    std::memcpy(&g.color_range, b + 20, 4);
                    std::memcpy(&g.wave_period, b + 24, 4);
                    g.flags = b[28];
                }
                if (auto model = gras->string(k_modl); !model.empty()) {
                    const std::string source = path::with_root(path::normalize(model), "meshes");
                    auto [m, new_mesh] = mesh_index.emplace(source, static_cast<uint32_t>(world.meshes.size()));
                    if (new_mesh) {
                        result.source_meshes.push_back(source);
                        world.meshes.push_back(path::replace_extension(source, ".osm"));
                    }
                    g.mesh = m->second;
                }
                world.grasses.push_back(g);
            }
            world.land_grass.push_back(it->second);
            ++t.grass_count;
        }
        ltex_index[id] = static_cast<uint16_t>(world.land_textures.size());
        world.land_textures.push_back(std::move(t));
    }
    if (world.land_textures.size() >= osw::k_no_texture) {
        throw Error(std::format("{}: too many land textures ({})", edid, world.land_textures.size()));
    }
    std::set<std::string> texture_set{world.default_land_texture};
    for (const auto& t : world.land_textures) {
        texture_set.insert(t.diffuse);
        if (!t.normal.empty()) {
            texture_set.insert(t.normal);
        }
    }
    result.textures.assign(texture_set.begin(), texture_set.end());

    // LTEX -> whether it names a texture set (the engine's test before it binds or merges a layer).
    std::unordered_map<uint32_t, bool> ts_cache;
    auto has_texture_set = [&](uint32_t ltex) {
        if (ltex == k_default_ltex) {
            return true;
        }
        auto [it, inserted] = ts_cache.try_emplace(ltex, false);
        if (inserted) {
            auto rec = db.get(ltex);
            if (rec && rec->type() == k_ltex) {
                auto txst = db.get(rec->form(k_tnam));
                it->second = txst && txst->type() == k_txst;
            }
        }
        return it->second;
    };

    // Directional materials, one per base STAT that has one.
    std::unordered_map<uint32_t, uint32_t> directional_index;
    auto directional_of = [&](uint32_t base) {
        auto [it, inserted] = directional_index.try_emplace(base, osw::k_none);
        if (inserted) {
            if (auto d = read_directional(db, base, world.directional_passes)) {
                it->second = static_cast<uint32_t>(world.directional_materials.size());
                world.directional_materials.push_back(*d);
            }
        }
        return it->second;
    };

    // Trees whose leaves move: the game registers a reference whose model's root is a BSTreeNode
    // or BSLeafAnimNode and copies its TREE's CNAM leaf amplitude and frequency (section 12).
    std::unordered_map<uint32_t, uint32_t> tree_index;
    auto tree_of = [&](uint32_t base, const std::string& mesh) {
        auto [it, inserted] = tree_index.try_emplace(base, osw::k_none);
        if (inserted && leaf_anim) {
            auto rec = db.get(base);
            if (rec && rec->type() == k_tree && leaf_anim(mesh)) {
                osw::Tree t{base, 1.0f, 1.0f, 0};
                // CNAM is 48 bytes, leaf amplitude and frequency the last two floats.
                if (const auto* c = rec->find(k_cnam); c && c->data.size() >= 48) {
                    std::memcpy(&t.leaf_amplitude, c->data.data() + 40, 4);
                    std::memcpy(&t.leaf_frequency, c->data.data() + 44, 4);
                }
                it->second = static_cast<uint32_t>(world.trees.size());
                world.trees.push_back(t);
            }
        }
        return it->second;
    };

    // Emittance sources of references (10.4.6): a light's colour, or a region's weather.
    std::unordered_map<uint32_t, uint32_t> emittance_index;
    auto emittance_of = [&](uint32_t source) -> uint32_t {
        if (!source) {
            return osw::k_none;
        }
        if (auto it = emittance_index.find(source); it != emittance_index.end()) {
            return it->second;
        }
        uint32_t index = osw::k_none;
        auto src = db.get(source);
        osw::Emittance e{{1.0f, 1.0f, 1.0f}, 0};
        if (src && src->type() == k_ligh) {
            const LightBase lb = read_light_base(db, source);
            for (int k = 0; k < 3; ++k) {
                e.color[k] = float(lb.color[k]) * 0.0039215689f;
            }
            index = static_cast<uint32_t>(world.emittances.size());
            world.emittances.push_back(e);
        } else if (src && src->type() == k_regn) {
            if (uint32_t w = region_weather(db, source)) {
                e.weather = w;
                index = static_cast<uint32_t>(world.emittances.size());
                world.emittances.push_back(e);
            }
        }
        emittance_index.emplace(source, index);
        return index;
    };

    // Light bases, emittance sources and model attach points, read once each.
    std::unordered_map<uint32_t, LightBase> light_bases;
    std::unordered_map<std::string, std::optional<std::array<float, 3>>> attach_points;
    size_t lights_skipped = 0;

    // 6. Assemble cells, terrain and references.
    struct WaterVelocity {
        float lin[3] = {0, 0, 0};
        float ang[3] = {0, 0, 0};
    };
    std::unordered_map<uint32_t, WaterVelocity> water_velocities;
    for (const auto& c : exterior) {
        if (!c.has_water || c.water == 0 || water_velocities.contains(c.water)) {
            continue;
        }
        WaterVelocity wv;
        if (auto e = db.find(c.water); e && !e->deleted()) {
            auto r = db.read(*e);
            if (auto s = r.find(k_nam0); s && s->data.size() >= 12) {
                std::memcpy(wv.lin, s->data.data(), 12);
            }
            if (auto s = r.find(k_nam1); s && s->data.size() >= 12) {
                std::memcpy(wv.ang, s->data.data(), 12);
            }
        }
        water_velocities.emplace(c.water, wv);
    }
    world.cells.reserve(exterior.size());
    size_t land_count = 0;
    for (const auto& c : exterior) {
        land_count += c.decoded ? 1 : 0;
    }
    world.lands.reserve(land_count);
    for (auto& c : exterior) {
        osw::Cell cell{};
        cell.x = c.coord.x;
        cell.y = c.coord.y;
        cell.form_id = c.entry ? c.entry->form_id : 0;
        cell.flags = c.has_water ? osw::cell_has_water : 0;
        cell.water_height = units::to_metres(c.water_height);
        world.cell_waters.push_back(c.has_water ? c.water : 0);
        cell.land = osw::k_none;
        if (c.decoded) {
            auto& pl = *c.decoded;
            for (int q = 0; q < 4; ++q) {
                auto& quad = pl.land.quadrants[q];
                const BuiltQuadrant b = build_quadrant(pl, q, has_texture_set);
                auto index_of = [&](uint32_t ltex) {
                    auto it = ltex_index.find(ltex);
                    return it != ltex_index.end() ? it->second : osw::k_no_texture;
                };
                quad.base_texture = index_of(b.base);
                quad.first_layer = static_cast<uint32_t>(world.land_layers.size());
                int count = 0;
                for (int k = 0; k < 5; ++k) {
                    bool used = b.bound[k] != 0;
                    for (int v = 0; !used && v < k_quad_vertex_count; ++v) {
                        used = b.weight[k][v] != 0;
                    }
                    if (used) {
                        count = k + 1;
                    }
                }
                quad.layer_count = static_cast<uint16_t>(count);
                for (int k = 0; k < count; ++k) {
                    osw::LandLayer layer{};
                    layer.texture = index_of(b.bound[k]);
                    std::memcpy(layer.opacity, b.weight[k], sizeof layer.opacity);
                    world.land_layers.push_back(layer);
                }
            }
            cell.flags |= osw::cell_has_land;
            cell.land = static_cast<uint32_t>(world.lands.size());
            world.lands.push_back(pl.land);
            c.decoded.reset();
        }

        std::sort(c.refs.begin(), c.refs.end(),
                  [](const PendingRef& a, const PendingRef& b) { return a.form_id < b.form_id; });
        // Water currents: the record's zones, then the current markers placed in the cell (at attach), then the
        // water form's own velocities (when the cell water is built).
        if (c.has_water) {
            for (const auto& r : c.refs) {
                if (r.base == k_current_marker) {
                    apply_current_marker(c.current, r.rotation, r.scale);
                }
            }
            if (auto it = water_velocities.find(c.water); it != water_velocities.end()) {
                apply_water_current(c.current, it->second.lin, it->second.ang);
            }
            if (!c.current.empty()) {
                world.currents.push_back({c.coord.x, c.coord.y, static_cast<uint32_t>(world.current_quads.size()),
                                          static_cast<uint32_t>(c.current.size())});
                world.current_quads.insert(world.current_quads.end(), c.current.begin(), c.current.end());
            }
        }
        cell.first_ref = static_cast<uint32_t>(world.refs.size());
        for (const auto& r : c.refs) {
            if (r.base == k_collision_marker && r.prim_type >= 1 && r.prim_type <= 3) {
                // A CollisionMarker's primitive is a fixed solid body of its own (
                // "Invisible collision"): bounds are half extents, not scaled by the reference; the layer is
                // XTRI's, else TRANSPARENT.
                osw::Primitive prim{};
                prim.cell_x = c.coord.x;
                prim.cell_y = c.coord.y;
                prim.form_id = r.form_id;
                prim.type = static_cast<osw::PrimitiveType>(r.prim_type);
                prim.layer = r.collision_layer ? r.collision_layer - 1 : 3;
                prim.flags = (r.persistent ? osw::ref_persistent : 0) |
                             (enable.enabled(r.form_id) ? 0 : osw::ref_initially_disabled);
                for (int k = 0; k < 3; ++k) {
                    prim.position[k] = units::to_metres(r.position[k]);
                    prim.half_extents[k] = units::to_metres(r.prim_bounds[k]);
                }
                const auto q = euler_to_quat(r.rotation);
                prim.rotation[0] = q.x, prim.rotation[1] = q.y, prim.rotation[2] = q.z, prim.rotation[3] = q.w;
                world.primitives.push_back(prim);
            }
            // A light reference becomes a point light whether or not it has a model.
            if (auto e = db.find(r.base); e && e->type == k_ligh && !e->deleted()) {
                auto [it, inserted] = light_bases.try_emplace(r.base);
                if (inserted) {
                    it->second = read_light_base(db, r.base);
                }
                const LightBase& lb = it->second;
                const bool below_water = c.has_water && r.position[2] < c.water_height;
                if (!lb.valid || (r.record_flags & ref_turn_off_fire) || (lb.flags & ligh_off_by_default) ||
                    below_water) {
                    ++lights_skipped;
                } else {
                    osw::Light l{};
                    l.form_id = r.form_id;
                    l.base_form_id = r.base;
                    l.cell = static_cast<uint32_t>(world.cells.size());
                    float radius = static_cast<float>(lb.radius) + r.radius_offset;
                    if (radius <= 0.0f) {
                        radius = 0.1f;
                    }
                    l.radius = units::to_metres(radius);
                    const float sign = (lb.flags & ligh_negative) ? -1.0f : 1.0f;
                    for (int k = 0; k < 3; ++k) {
                        l.color[k] = sign * (float(lb.color[k]) * 0.0039215689f);
                    }
                    if (r.emittance) {
                        // source colour x the light's own colour, unsigned.
                        auto src = db.get(r.emittance);
                        if (src && src->type() == k_ligh) {
                            const LightBase sb = read_light_base(db, r.emittance);
                            for (int k = 0; k < 3; ++k) {
                                l.color[k] = (float(sb.color[k]) * 0.0039215689f) *
                                             (float(lb.color[k]) * 0.0039215689f);
                            }
                        } else if (src && src->type() == k_regn) {
                            if (uint32_t w = region_weather(db, r.emittance)) {
                                for (int k = 0; k < 3; ++k) {
                                    l.color[k] = float(lb.color[k]) * 0.0039215689f;
                                }
                                l.flags |= osw::light_region_emittance;
                                l.emittance_weather = w;
                            }
                        }
                    }
                    l.dimmer = lb.fade + r.fade_offset;
                    l.frequency = lb.frequency;
                    l.intensity_amplitude = lb.intensity_amplitude;
                    l.movement_amplitude = units::to_metres(lb.movement_amplitude);
                    // Animation phases start at the light's attenuation values: constant 0, linear 0, quadratic
                    // 16 / int(radius)^2 ([bLightAttenuation] defaults); one 2 pi is taken off a larger phase.
                    const int ir = static_cast<int>(radius);
                    l.phase[2] = ir != 0 ? 16.0f / float(ir * ir) : 0.0f;
                    for (float& p : l.phase) {
                        if (p > 6.2831855f) {
                            p -= 6.2831855f;
                        }
                    }
                    l.flags |= (enable.enabled(r.form_id) ? 0 : osw::light_initially_disabled) |
                               ((r.record_flags & ref_never_fades) ? osw::light_never_fades : 0) |
                               ((r.record_flags & ref_no_land) ? osw::light_no_land : 0) |
                               ((r.record_flags & ref_no_water) ? osw::light_no_water : 0) |
                               ((lb.flags & ligh_flicker) ? osw::light_flicker : 0) |
                               ((lb.flags & ligh_portal_strict) ? osw::light_portal_strict : 0) |
                               ((lb.flags & (ligh_pulse | ligh_pulse_slow)) ? osw::light_pulse : 0) |
                               ((lb.flags & (ligh_shadow_spot | ligh_shadow_hemisphere | ligh_shadow_omni))
                                    ? osw::light_shadow
                                    : 0);
                    // Position: the reference transform applied to where the light hangs in the model.
                    float offset[3] = {0.0f, 0.0f, 0.0f};
                    if (!lb.model.empty() && attach) {
                        auto [at, fresh] = attach_points.try_emplace(lb.model);
                        if (fresh) {
                            at->second = attach(lb.model);
                        }
                        if (at->second) {
                            const float local[3] = {(*at->second)[0] * r.scale, (*at->second)[1] * r.scale,
                                                    (*at->second)[2] * r.scale};
                            rotate(euler_to_quat(r.rotation), local, offset);
                        }
                    }
                    for (int k = 0; k < 3; ++k) {
                        l.position[k] = units::to_metres(r.position[k] + offset[k]);
                    }
                    const Quat rq = euler_to_quat(r.rotation);
                    l.rotation[0] = rq.x;
                    l.rotation[1] = rq.y;
                    l.rotation[2] = rq.z;
                    l.rotation[3] = rq.w;
                    l.scale = r.scale;
                    // the shadow light's type goes by its field of view.
                    l.shadow_fov = (lb.flags & ligh_shadow_spot)         ? (lb.fov + r.fov_offset) * 0.017453292f
                                   : (lb.flags & ligh_shadow_hemisphere) ? 3.1415927f
                                                                         : 6.2831855f;
                    l.shadow_falloff = lb.falloff;
                    l.shadow_near = units::to_metres(lb.near_clip);
                    l.shadow_bias = r.depth_bias;
                    world.lights.push_back(l);
                }
            }
            if (r.teleport_door) {
                if (auto d = make_door(db, r, doors)) {
                    d->flags |= enable.enabled(r.form_id) ? 0 : osw::door_initially_disabled;
                    world.doors.push_back(std::move(*d));
                }
            }
            const auto& base = bases.at(r.base);
            if (base.mesh.empty()) {
                ++result.skipped[base.skip];
                continue;
            }
            osw::Ref out{};
            out.emittance = emittance_of(r.emittance);
            out.room_hint = osw::k_none;
            if (r.multibound) {
                if (auto it = room_index.find(r.multibound); it != room_index.end()) {
                    out.room_hint = it->second;
                }
            }
            out.form_id = r.form_id;
            out.base_form_id = r.base;
            out.mesh = mesh_index.at(base.mesh);
            out.flags = (r.persistent ? osw::ref_persistent : 0) |
                        (enable.enabled(r.form_id) ? 0 : osw::ref_initially_disabled);
            out.directional = directional_of(r.base);
            out.tree = tree_of(r.base, base.mesh);
            out.flags |= fade_flags(base, r.record_flags, out.tree != osw::k_none);
            if (base.type == k_stat || base.type == record_type("SCOL") || base.type == record_type("MSTT")) {
                out.flags |= osw::ref_occludable;
            } else if (base.type == k_tree || base.type == record_type("FLOR")) {
                out.flags |= osw::ref_occludable_far;
            }
            for (int k = 0; k < 3; ++k) {
                out.position[k] = units::to_metres(r.position[k]);
                out.source_euler[k] = r.rotation[k];
            }
            out.scale = r.scale;
            auto q = euler_to_quat(r.rotation);
            out.rotation[0] = q.x;
            out.rotation[1] = q.y;
            out.rotation[2] = q.z;
            out.rotation[3] = q.w;
            world.refs.push_back(out);
        }
        cell.ref_count = static_cast<uint32_t>(world.refs.size()) - cell.first_ref;
        world.cells.push_back(cell);
        world.cell_regions.push_back(c.world);
    }

    if (!world.cells.empty()) {
        world.cell_min[0] = world.cell_max[0] = world.cells.front().x;
        world.cell_min[1] = world.cell_max[1] = world.cells.front().y;
        for (const auto& c : world.cells) {
            world.cell_min[0] = std::min(world.cell_min[0], c.x);
            world.cell_min[1] = std::min(world.cell_min[1], c.y);
            world.cell_max[0] = std::max(world.cell_max[0], c.x);
            world.cell_max[1] = std::max(world.cell_max[1], c.y);
        }
    }

    // The multi-pass materials' textures (their normal map is the object's own).
    for (const auto& p : world.directional_passes) {
        for (const auto* t : {&p.diffuse, &p.back_light}) {
            if (!t->empty() && std::find(result.textures.begin(), result.textures.end(), *t) == result.textures.end()) {
                result.textures.push_back(*t);
            }
        }
    }
    log::info("{}: {} lights ({} light references create none), {} directional materials", edid,
              world.lights.size(), lights_skipped, world.directional_materials.size());
    if (enable.missing_parents || enable.cycles) {
        log::warn("{}: {} enable parents not found (treated as enabled), {} enable-parent cycles", edid,
                  enable.missing_parents, enable.cycles);
    }
    if (world.regions.empty()) {
        world.cell_regions.clear();
    }
    std::sort(world.doors.begin(), world.doors.end(),
              [](const osw::Door& a, const osw::Door& b) { return a.form_id < b.form_id; });
}

} // namespace

namespace {

// Whether a worldspace's own location (WRLD XLCN) is a city: its LCTN carries the LocTypeCity keyword.
bool is_city(const EsmDatabase& db, const RecordEntry& world) {
    auto w = db.read(world);
    auto loc = db.get(w.form(record_type("XLCN")));
    if (!loc || loc->type() != record_type("LCTN")) {
        return false;
    }
    auto kwda = loc->find(record_type("KWDA"));
    if (!kwda) {
        return false;
    }
    for (size_t i = 0; i + 4 <= kwda->data.size(); i += 4) {
        uint32_t k;
        std::memcpy(&k, kwda->data.data() + i, 4);
        if (auto kw = db.get(loc->resolve(k)); kw && kw->edid() == "LocTypeCity") {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<MergedChild> merged_children(const legacy::EsmDatabase& db, std::string_view parent_edid) {
    std::vector<MergedChild> out;
    auto parent_entry = find_world_entry(db, parent_edid);
    if (!parent_entry) {
        return out;
    }
    const uint32_t parent = parent_entry->form_id;
    for (auto w : db.of_type(k_wrld)) {
        if (w->deleted()) {
            continue;
        }
        auto info = read_world(db, *w);
        if (info.parent != parent) {
            continue;
        }
        // Load doors between the child and the parent, and how far each one's destination door stands from it.
        size_t doors = 0;
        float nearest = 1e30f;
        for (auto c : db.children(w->form_id)) {
            if (c->type != k_cell) {
                continue;
            }
            for (auto child : db.children(c->form_id)) {
                if (child->type != k_refr || child->deleted()) {
                    continue;
                }
                auto r = db.read(*child);
                auto s = r.find(k_xtel);
                if (!s || s->data.size() < 4) {
                    continue;
                }
                const uint32_t dest = r.resolve(s->as<uint32_t>());
                if (world_of_ref(db, dest) != parent) {
                    continue;
                }
                auto a = ref_position(db, child->form_id);
                auto b = ref_position(db, dest);
                if (!a || !b) {
                    continue;
                }
                const float d = std::hypot((*a)[0] - (*b)[0], (*a)[1] - (*b)[1], (*a)[2] - (*b)[2]);
                nearest = std::min(nearest, d);
                ++doors;
            }
        }
        const bool city = is_city(db, *w);
        if (city || doors > 0) {
            log::debug("{}: child world {} (PNAM {:02X}, {}): {} load doors to it, nearest pair {:.0f} units apart",
                       parent_edid, info.edid, info.parent_use, city ? "city" : "not a city", doors, nearest);
        }
        if (city && doors > 0 && nearest < k_shared_frame_units) {
            out.push_back({info.edid, w->form_id});
        }
    }
    return out;
}

MergedWorlds merged_worlds(const legacy::EsmDatabase& db) {
    std::set<uint32_t> parents;
    for (auto w : db.of_type(k_wrld)) {
        if (!w->deleted()) {
            if (const uint32_t p = read_world(db, *w).parent) {
                parents.insert(p);
            }
        }
    }
    MergedWorlds out;
    for (uint32_t p : parents) {
        if (auto e = db.find(p); e && !e->deleted() && e->type == k_wrld) {
            const std::string edid(db.read(*e).edid());
            for (const auto& c : merged_children(db, edid)) {
                out.emplace(c.form_id, path::to_lower(edid));
            }
        }
    }
    return out;
}

std::string interior_space(const legacy::EsmDatabase& db, uint32_t cell) {
    const RecordEntry* e = db.find(cell);
    std::string edid = e ? std::string(db.read(*e).edid()) : std::string();
    return edid.empty() ? std::format("cell_{:08x}", cell) : path::to_lower(edid);
}

std::vector<uint32_t> list_interiors(const legacy::EsmDatabase& db) {
    std::vector<uint32_t> out;
    for (auto c : db.of_type(k_cell)) {
        if (c->deleted() || c->parent != 0) {
            continue;
        }
        auto r = db.read(*c);
        if (auto s = r.find(k_data); s && !s->data.empty() && (s->as<uint8_t>() & cell_data_interior)) {
            out.push_back(c->form_id);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

WorldImport import_interior(const legacy::EsmDatabase& db, uint32_t cell_form, const LightAttachFn& attach,
                            const LeafAnimFn& leaf_anim, const MergedWorlds* merged) {
    auto t0 = std::chrono::steady_clock::now();
    const RecordEntry* entry = db.find(cell_form);
    if (!entry || entry->type != k_cell) {
        throw Error(std::format("no interior cell {:08X}", cell_form));
    }
    WorldImport result;
    auto& world = result.world;
    auto rec = db.read(*entry);
    world.edid = rec.edid().empty() ? interior_space(db, cell_form) : std::string(rec.edid());
    if (!rec.plugin().localized) {
        world.name = rec.string(k_full);
    }
    world.form_id = cell_form;
    world.cell_size = units::k_cell_size_m;
    world.default_land_texture = k_default_land_texture;
    uint16_t data = 0;
    if (auto s = rec.find(k_data); s && !s->data.empty()) {
        data = s->data.size() >= 2 ? s->as<uint16_t>() : s->as<uint8_t>();
    }
    world.flags = osw::world_interior | ((data & 0x80) ? osw::world_show_sky : 0) |
                  ((data & 0x100) ? osw::world_use_sky_lighting : 0);
    world.lighting = read_cell_lighting(db, rec);
    if (auto s = rec.find(k_xccm); s && s->data.size() >= 4) {
        world.lighting.sky_weather = region_weather(db, rec.resolve(s->as<uint32_t>()));
    }

    // The cell's references, on the exterior grid by position.
    PendingCell source;
    source.entry = entry;
    WorldInfo no_world;
    read_cell_header(rec, source, no_world);
    read_cell_children(db, source);
    for (const auto& [reason, n] : source.skipped) {
        result.skipped[reason] += n;
    }
    if (source.has_water) {
        world.flags |= osw::world_has_water;
        world.default_water_height = units::to_metres(source.water_height);
    }
    std::unordered_map<CellCoord, size_t, CellCoordHash> grid;
    std::vector<PendingCell> cells;
    for (const auto& r : source.refs) {
        const CellCoord coord{grid_of(r.position[0]), grid_of(r.position[1])};
        auto [it, fresh] = grid.try_emplace(coord, cells.size());
        if (fresh) {
            PendingCell c;
            c.entry = entry;
            c.coord = coord;
            c.has_water = source.has_water;
            c.water_height = source.water_height;
            c.water = source.water;
            cells.push_back(std::move(c));
        }
        cells[it->second].refs.push_back(r);
    }
    if (cells.empty()) {
        PendingCell c; // an empty interior still has a cell, so it can be entered
        c.entry = entry;
        cells.push_back(std::move(c));
    }
    std::sort(cells.begin(), cells.end(), [](const PendingCell& a, const PendingCell& b) {
        return a.coord.y != b.coord.y ? a.coord.y < b.coord.y : a.coord.x < b.coord.x;
    });
    MergedWorlds own;
    if (!merged) {
        own = merged_worlds(db);
    }
    assemble(db, world.edid, cells, attach, leaf_anim,
             DoorContext{merged ? merged : &own, interior_space(db, cell_form)}, result);
    log::debug("{}: interior, {} refs, {} lights, {} doors, {:.0f} ms", world.edid, world.refs.size(),
               world.lights.size(), world.doors.size(), ms_since(t0));
    return result;
}

WorldImport import_world(const legacy::EsmDatabase& db, std::string_view world_edid,
                         const LightAttachFn& attach, const LeafAnimFn& leaf_anim, const MergedWorlds* merged_in) {
    auto t0 = std::chrono::steady_clock::now();
    auto world_entry = find_world_entry(db, world_edid);
    if (!world_entry) {
        throw Error(std::format("no worldspace with EditorID '{}'", world_edid));
    }
    auto info = read_world(db, *world_entry);

    WorldImport result;
    auto g = gather_world(db, info, 0, result.skipped);
    const bool borrow_land = g.borrow_land;
    const WorldInfo land_world = g.land_world;
    const WorldInfo water_world = g.water_world;
    const double decode_ms = g.decode_ms;

    auto& world = result.world;
    world.edid = info.edid;
    world.name = info.name;
    if (info.parent) {
        if (auto p = world_by_form(db, info.parent)) {
            world.parent_edid = p->edid;
        }
    }
    world.form_id = info.entry->form_id;
    world.flags = (water_world.water != 0 ? osw::world_has_water : 0) | ((info.data_flags & 0x20) ? osw::world_no_sky : 0);
    world.cell_size = units::k_cell_size_m;
    world.default_land_height = units::to_metres(g.default_land_units);
    world.default_water_height = units::to_metres(water_world.water_height);
    world.default_land_texture = k_default_land_texture;

    // Cities: child worlds that share this world's coordinates are merged into it.
    MergedWorlds own;
    if (!merged_in) {
        own = merged_worlds(db);
    }
    const MergedWorlds& merged = merged_in ? *merged_in : own;
    std::vector<uint32_t> children;
    for (const auto& [child, into] : merged) {
        if (into == path::to_lower(info.edid)) {
            children.push_back(child);
        }
    }
    std::sort(children.begin(), children.end());
    for (uint32_t child : children) {
        const auto cinfo = read_world(db, *db.find(child));
        const auto tag = static_cast<uint16_t>(world.regions.size() + 1);
        world.regions.push_back({cinfo.entry->form_id, cinfo.edid});
        merge_child(db, g, gather_world(db, cinfo, tag, result.skipped), result);
    }

    std::vector<PendingCell> exterior = std::move(g.exterior);
    const size_t synthetic = g.synthetic;
    const size_t no_water_sentinels = g.no_water_sentinels;
    const size_t flat_lands = g.flat_lands;
    std::sort(exterior.begin(), exterior.end(), [](const PendingCell& a, const PendingCell& b) {
        return a.coord.y != b.coord.y ? a.coord.y < b.coord.y : a.coord.x < b.coord.x;
    });

    const auto t2 = std::chrono::steady_clock::now();
    assemble(db, info.edid, exterior, attach, leaf_anim, DoorContext{&merged, path::to_lower(info.edid)}, result);
    log::info("{}: {} cells ({} synthetic, {} with no-water sentinels), {} lands{} ({} without heights), "
              "{} refs, {} meshes; decode {:.0f} ms, assemble {:.0f} ms, total {:.0f} ms",
              info.edid, world.cells.size(), synthetic, no_water_sentinels, world.lands.size(),
              borrow_land ? std::format(" from {}", land_world.edid) : std::string(), flat_lands,
              world.refs.size(), world.meshes.size(), decode_ms, ms_since(t2), ms_since(t0));
    return result;
}

} // namespace spud::importer
