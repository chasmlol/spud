#include "formats/osd.hpp"

namespace spud::osd {

namespace {

using container::fourcc;

constexpr auto k_catg = fourcc("CATG");
constexpr auto k_omdl = fourcc("OMDL");
constexpr auto k_cond = fourcc("COND");
constexpr auto k_desc = fourcc("DESC");
constexpr auto k_fils = fourcc("FILS");
constexpr auto k_soun = fourcc("SOUN");
constexpr auto k_revb = fourcc("REVB");
constexpr auto k_aspc = fourcc("ASPC");
constexpr auto k_musc = fourcc("MUSC");
constexpr auto k_must = fourcc("MUST");
constexpr auto k_trkl = fourcc("TRKL");
constexpr auto k_cues = fourcc("CUES");
constexpr auto k_fstp = fourcc("FSTP");
constexpr auto k_fsts = fourcc("FSTS");
constexpr auto k_fstl = fourcc("FSTL");
constexpr auto k_ipct = fourcc("IPCT");
constexpr auto k_ipds = fourcc("IPDS");
constexpr auto k_ippr = fourcc("IPPR");
constexpr auto k_matt = fourcc("MATT");
constexpr auto k_regn = fourcc("REGN");
constexpr auto k_rsnd = fourcc("RSND");
constexpr auto k_rara = fourcc("RARA");
constexpr auto k_rpts = fourcc("RPTS");
constexpr auto k_watr = fourcc("WATR");
constexpr auto k_dobj = fourcc("DOBJ");
constexpr auto k_wthr = fourcc("WTHR");
constexpr auto k_wsnd = fourcc("WSND");
constexpr auto k_wrld = fourcc("WRLD");
constexpr auto k_lctn = fourcc("LCTN");
constexpr auto k_cell = fourcc("CELL");
constexpr auto k_creg = fourcc("CREG");
constexpr auto k_emit = fourcc("EMIT");
constexpr auto k_sett = fourcc("SETT");
constexpr auto k_strs = fourcc("STRS");

template <typename T>
void read_optional(const container::Reader& r, container::FourCC id, std::vector<T>& out) {
    if (r.find(id)) {
        out = r.read_array<T>(id);
    }
}

} // namespace

std::string_view SoundDb::str(uint32_t offset) const {
    if (offset == container::k_no_string || offset >= strings.size()) {
        return {};
    }
    return std::string_view(strings.data() + offset);
}

std::vector<std::byte> write(const SoundDb& db) {
    container::Writer w(k_kind, k_version);
    w.add_array(k_catg, db.categories);
    w.add_array(k_omdl, db.output_models);
    w.add_array(k_cond, db.conditions);
    w.add_array(k_desc, db.descriptors);
    w.add_array(k_fils, db.files);
    w.add_array(k_soun, db.markers);
    w.add_array(k_revb, db.reverbs);
    w.add_array(k_aspc, db.acoustic_spaces);
    w.add_array(k_musc, db.music_types);
    w.add_array(k_must, db.music_tracks);
    w.add_array(k_trkl, db.track_lists);
    w.add_array(k_cues, db.cue_points);
    w.add_array(k_fstp, db.footsteps);
    w.add_array(k_fsts, db.footstep_sets);
    w.add_array(k_fstl, db.footstep_lists);
    w.add_array(k_ipct, db.impacts);
    w.add_array(k_ipds, db.impact_sets);
    w.add_array(k_ippr, db.impact_pairs);
    w.add_array(k_matt, db.materials);
    w.add_array(k_regn, db.regions);
    w.add_array(k_rsnd, db.region_sounds);
    w.add_array(k_rara, db.region_areas);
    w.add_array(k_rpts, db.region_points);
    w.add_array(k_watr, db.waters);
    w.add_array(k_dobj, db.default_objects);
    w.add_array(k_wthr, db.weathers);
    w.add_array(k_wsnd, db.weather_sounds);
    w.add_array(k_wrld, db.worlds);
    w.add_array(k_lctn, db.locations);
    w.add_array(k_cell, db.cells);
    w.add_array(k_creg, db.cell_regions);
    w.add_array(k_emit, db.emitters);
    w.add_array(k_sett, db.settings);
    w.add_array(k_strs, db.strings);
    return w.finish();
}

SoundDb read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind);
    if (r.version() != k_version) {
        throw ParseError("unsupported .osd version " + std::to_string(r.version()));
    }
    SoundDb db;
    read_optional(r, k_catg, db.categories);
    read_optional(r, k_omdl, db.output_models);
    read_optional(r, k_cond, db.conditions);
    read_optional(r, k_desc, db.descriptors);
    read_optional(r, k_fils, db.files);
    read_optional(r, k_soun, db.markers);
    read_optional(r, k_revb, db.reverbs);
    read_optional(r, k_aspc, db.acoustic_spaces);
    read_optional(r, k_musc, db.music_types);
    read_optional(r, k_must, db.music_tracks);
    read_optional(r, k_trkl, db.track_lists);
    read_optional(r, k_cues, db.cue_points);
    read_optional(r, k_fstp, db.footsteps);
    read_optional(r, k_fsts, db.footstep_sets);
    read_optional(r, k_fstl, db.footstep_lists);
    read_optional(r, k_ipct, db.impacts);
    read_optional(r, k_ipds, db.impact_sets);
    read_optional(r, k_ippr, db.impact_pairs);
    read_optional(r, k_matt, db.materials);
    read_optional(r, k_regn, db.regions);
    read_optional(r, k_rsnd, db.region_sounds);
    read_optional(r, k_rara, db.region_areas);
    read_optional(r, k_rpts, db.region_points);
    read_optional(r, k_watr, db.waters);
    read_optional(r, k_dobj, db.default_objects);
    read_optional(r, k_wthr, db.weathers);
    read_optional(r, k_wsnd, db.weather_sounds);
    read_optional(r, k_wrld, db.worlds);
    read_optional(r, k_lctn, db.locations);
    read_optional(r, k_cell, db.cells);
    read_optional(r, k_creg, db.cell_regions);
    read_optional(r, k_emit, db.emitters);
    read_optional(r, k_sett, db.settings);
    read_optional(r, k_strs, db.strings);
    if (db.strings.empty() || db.strings.back() != '\0') {
        db.strings.push_back('\0');
    }
    return db;
}

} // namespace spud::osd
