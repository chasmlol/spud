#include "importer/convert/atmosphere_import.hpp"

#include "core/log.hpp"
#include "core/path.hpp"
#include "importer/legacy/nif.hpp"

#include <tuple>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <functional>
#include <map>

#include <glm/glm.hpp>

namespace spud::importer {

using namespace spud::legacy;

namespace {

constexpr RecordType k_gmst = record_type("GMST");
constexpr RecordType k_watr = record_type("WATR");
constexpr RecordType k_nnam = record_type("NNAM");
constexpr RecordType k_nam2 = record_type("NAM2");
constexpr RecordType k_nam3 = record_type("NAM3");
constexpr RecordType k_nam4 = record_type("NAM4");
constexpr RecordType k_inam = record_type("INAM");
constexpr RecordType k_glob = record_type("GLOB");
constexpr RecordType k_wrld = record_type("WRLD");
constexpr RecordType k_regn = record_type("REGN");
constexpr RecordType k_rdat = record_type("RDAT");
constexpr RecordType k_rdwt = record_type("RDWT");
constexpr RecordType k_rpld = record_type("RPLD");
constexpr RecordType k_cell = record_type("CELL");
constexpr RecordType k_xclc = record_type("XCLC");
constexpr RecordType k_xclr = record_type("XCLR");
constexpr RecordType k_clmt = record_type("CLMT");
constexpr RecordType k_wthr = record_type("WTHR");
constexpr RecordType k_imgs = record_type("IMGS");
constexpr RecordType k_imad = record_type("IMAD");
constexpr RecordType k_voli = record_type("VOLI");
constexpr RecordType k_spgd = record_type("SPGD");
constexpr RecordType k_data = record_type("DATA");
constexpr RecordType k_fltv = record_type("FLTV");
constexpr RecordType k_cnam = record_type("CNAM");
constexpr RecordType k_dnam = record_type("DNAM");
constexpr RecordType k_anam = record_type("ANAM");
constexpr RecordType k_bnam = record_type("BNAM");
constexpr RecordType k_pnam = record_type("PNAM");
constexpr RecordType k_wnam = record_type("WNAM");
constexpr RecordType k_tnam = record_type("TNAM");
constexpr RecordType k_wlst = record_type("WLST");
constexpr RecordType k_nam0 = record_type("NAM0");
constexpr RecordType k_nam1 = record_type("NAM1");
constexpr RecordType k_fnam = record_type("FNAM");
constexpr RecordType k_gnam = record_type("GNAM");
constexpr RecordType k_dalc = record_type("DALC");
constexpr RecordType k_imsp = record_type("IMSP");
constexpr RecordType k_hnam = record_type("HNAM");
constexpr RecordType k_enam = record_type("ENAM");
constexpr RecordType k_lnam = record_type("LNAM");
constexpr RecordType k_mnam = record_type("MNAM");
constexpr RecordType k_qnam = record_type("QNAM");
constexpr RecordType k_rnam = record_type("RNAM");
constexpr RecordType k_jnam = record_type("JNAM");
constexpr RecordType k_snam = record_type("SNAM");
constexpr RecordType k_modl = record_type("MODL");
constexpr RecordType k_icon = record_type("ICON");

constexpr uint32_t k_default_image_space = 0x167; // DefaultImageSpace (Skyrim.esm), used for a missing IMSP entry
constexpr uint32_t k_time_scale_global = 0x3A;    // TimeScale (Skyrim.esm)
constexpr uint16_t k_parent_use_climate = 0x10;   // WRLD PNAM
constexpr uint32_t k_thunder_sound = 3;           // WTHR SNAM type of the thunder sounds

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string rooted(std::string_view raw, std::string_view root) {
    auto p = path::normalize(raw);
    return p.empty() ? p : path::with_root(p, root);
}

template <typename T>
T read_at(const Subrecord& s, size_t offset, T fallback) {
    if (s.data.size() < offset + sizeof(T)) {
        return fallback;
    }
    T v;
    std::memcpy(&v, s.data.data() + offset, sizeof(T));
    return v;
}

float field_float(const Record& rec, RecordType type, float fallback) {
    const Subrecord* s = rec.find(type);
    return s ? read_at<float>(*s, 0, fallback) : fallback;
}

// The weather's cloud texture subrecord of layer i: chr(0x30 + i) + "0TX".
RecordType cloud_texture_type(int i) {
    return static_cast<uint32_t>(0x30 + i) | (uint32_t('0') << 8) | (uint32_t('T') << 16) | (uint32_t('X') << 24);
}

// Sky meshes: every shape with a BSSkyShaderProperty, its node transforms baked into the vertices.
void import_sky_model(const DataSource& data, std::string_view nif_path, osa::Atmosphere& a,
                      std::set<std::string>& textures) {
    if (a.sky_model(nif_path)) {
        return;
    }
    auto bytes = data.read(nif_path);
    if (!bytes) {
        log::warn("atmosphere: sky model {} is missing", nif_path);
        return;
    }
    NifFile nif;
    try {
        nif = read_nif(*bytes);
    } catch (const std::exception& e) {
        log::warn("atmosphere: {}: {}", nif_path, e.what());
        return;
    }
    osa::SkyModel model{a.intern(nif_path), static_cast<uint32_t>(a.sky_shapes.size()), 0};
    // Cloud layers are numbered in the order their properties load, which is
    // block order.
    std::map<NifRef, uint32_t> cloud_layer;
    for (size_t b = 0; b < nif.blocks.size(); ++b) {
        if (const auto* p = nif.get<BSSkyShaderProperty>(static_cast<NifRef>(b));
            p && p->sky_object_type == osa::sky_clouds) {
            const auto next = static_cast<uint32_t>(cloud_layer.size());
            cloud_layer.emplace(static_cast<NifRef>(b), next);
        }
    }
    struct Xf {
        glm::mat3 r{1.0f};
        glm::vec3 t{0.0f};
        float s = 1.0f;
    };
    std::function<void(NifRef, const Xf&, bool)> walk = [&](NifRef ref, const Xf& parent, bool hidden) {
        if (const auto* node = nif.get<NiNode>(ref)) {
            Xf w{parent.r * node->rotation, parent.r * (parent.s * node->translation) + parent.t,
                 parent.s * node->scale};
            for (NifRef c : node->children) {
                walk(c, w, hidden || node->hidden());
            }
            return;
        }
        const auto* shape = nif.get<BSTriShape>(ref);
        if (!shape) {
            return;
        }
        const auto* prop = nif.get<BSSkyShaderProperty>(shape->shader_property);
        const auto& g = shape->geometry;
        if (!prop || g.positions.empty() || g.triangles.empty()) {
            return;
        }
        Xf w{parent.r * shape->rotation, parent.r * (parent.s * shape->translation) + parent.t,
             parent.s * shape->scale};
        const auto base = static_cast<uint32_t>(a.sky_vertices.size());
        for (size_t v = 0; v < g.positions.size(); ++v) {
            const glm::vec3 p = w.r * (w.s * g.positions[v]) + w.t;
            osa::SkyVertex sv{};
            sv.position[0] = p.x;
            sv.position[1] = p.y;
            sv.position[2] = p.z;
            if (v < g.uvs.size()) {
                sv.uv[0] = g.uvs[v].x;
                sv.uv[1] = g.uvs[v].y;
            }
            const glm::u8vec4 c = v < g.colors.size() ? g.colors[v] : glm::u8vec4(255);
            for (int i = 0; i < 4; ++i) {
                sv.color[i] = c[i];
            }
            a.sky_vertices.push_back(sv);
        }
        osa::SkyShape s{};
        s.name = a.intern(shape->name);
        s.type = prop->sky_object_type;
        auto it = cloud_layer.find(shape->shader_property);
        s.layer = it != cloud_layer.end() ? it->second : 0;
        const auto tex = rooted(prop->source_texture, "textures");
        s.texture = tex.empty() ? container::k_no_string : a.intern(tex);
        if (!tex.empty()) {
            textures.insert(tex);
        }
        s.first_index = static_cast<uint32_t>(a.sky_indices.size());
        for (const auto& t : g.triangles) {
            for (uint16_t i : t) {
                a.sky_indices.push_back(base + i);
            }
        }
        s.index_count = static_cast<uint32_t>(a.sky_indices.size()) - s.first_index;
        s.hidden = (hidden || shape->hidden()) ? 1u : 0u;
        a.sky_shapes.push_back(s);
    };
    for (NifRef root : nif.roots) {
        walk(root, Xf{}, false);
    }
    model.shape_count = static_cast<uint32_t>(a.sky_shapes.size()) - model.first_shape;
    a.sky_models.push_back(model);
}

// A curve subrecord of `stride`-float keys appended to the key array.
osa::Curve read_curve(const Subrecord* s, int stride, osa::Atmosphere& a) {
    if (!s) {
        return {};
    }
    const size_t count = s->data.size() / (sizeof(float) * stride);
    osa::Curve c{static_cast<uint32_t>(a.keys.size()), static_cast<uint32_t>(count)};
    for (size_t i = 0; i < count * stride; ++i) {
        a.keys.push_back(read_at<float>(*s, i * 4, 0.0f));
    }
    return c;
}

} // namespace

AtmosphereImport import_atmosphere(const EsmDatabase& db, const DataSource& data) {
    AtmosphereImport out;
    osa::Atmosphere& a = out.atmosphere;
    a.intern(""); // offset 0 is the empty string

    // Game settings (GMST EDID -> DATA); the engine defaults stay when no plugin sets one. i-prefixed settings are
    // integers.
    auto& gs = a.settings;
    const std::map<std::string, float*> settings = {
        {"fdaytimecolorextension", &gs.daytime_color_extension},
        {"fsunxextreme", &gs.sun_x_extreme},
        {"fsunyextreme", &gs.sun_y_extreme},
        {"fsunzextreme", &gs.sun_z_extreme},
        {"fsundirxextreme", &gs.sun_dir_x_extreme},
        {"fsunalphatranstime", &gs.sun_alpha_trans_time},
        {"fweathercloudspeedmax", &gs.cloud_speed_max},
        {"fweathertransmin", &gs.trans_min},
        {"fweathertransmax", &gs.trans_max},
        {"fweathertransaccel", &gs.trans_accel},
        {"fweatherflashduration", &gs.flash_duration},
        {"fweatherflashdirectional", &gs.flash_directional},
        {"fweatherflashambient", &gs.flash_ambient},
        {"fprecipwindmult", &gs.precip_wind_mult},
        {"fsunminimumglarescale", &gs.sun_minimum_glare_scale},
        {"fsunreduceglarespeed", &gs.sun_reduce_glare_speed},
        {"fmasseranglefadestart", &gs.moon_fade_start[0]},
        {"fmasseranglefadeend", &gs.moon_fade_end[0]},
        {"fmasserangleshadowearlyfade", &gs.moon_shadow_early_fade[0]},
        {"fmasserspeed", &gs.moon_speed[0]},
        {"fmasserzoffset", &gs.moon_z_offset[0]},
        {"imassersize", &gs.moon_size[0]},
        {"fsecundaanglefadestart", &gs.moon_fade_start[1]},
        {"fsecundaanglefadeend", &gs.moon_fade_end[1]},
        {"fsecundaangleshadowearlyfade", &gs.moon_shadow_early_fade[1]},
        {"fsecundaspeed", &gs.moon_speed[1]},
        {"fsecundazoffset", &gs.moon_z_offset[1]},
        {"isecundasize", &gs.moon_size[1]},
        {"fstarsrotatedays", &gs.stars_rotate_days},
        {"fstarsrotatexaxis", &gs.stars_rotate_axis[0]},
        {"fstarsrotateyaxis", &gs.stars_rotate_axis[1]},
        {"fstarsrotatezaxis", &gs.stars_rotate_axis[2]},
        {"faurorafadeinstart", &gs.aurora_fade_in_start},
        {"fautorafadein", &gs.aurora_fade_in},
        {"faurorafadeoutstart", &gs.aurora_fade_out_start},
        {"fautorafadeout", &gs.aurora_fade_out},
    };
    for (auto e : db.of_type(k_gmst)) {
        if (e->deleted()) {
            continue;
        }
        auto rec = db.read(*e);
        const std::string name = lower(rec.edid());
        auto it = settings.find(name);
        if (it == settings.end()) {
            continue;
        }
        if (const auto* d = rec.find(k_data); d && d->data.size() >= 4) {
            if (name[0] == 'i') {
                *it->second = static_cast<float>(read_at<int32_t>(*d, 0, 0));
            } else {
                std::memcpy(it->second, d->data.data(), 4);
            }
        }
    }
    auto global_value = [&](uint32_t form_id) -> std::optional<float> {
        auto rec = db.get(form_id);
        if (!rec || rec->type() != k_glob) {
            return std::nullopt;
        }
        if (const auto* s = rec->find(k_fltv)) {
            return read_at<float>(*s, 0, 0.0f);
        }
        return std::nullopt;
    };
    if (auto ts = global_value(k_time_scale_global)) {
        gs.time_scale = *ts;
    }

    // Image spaces (HNAM, or the old 56-byte ENAM; CNAM, TNAM, DNAM).
    for (auto e : db.of_type(k_imgs)) {
        if (e->deleted()) {
            continue;
        }
        auto rec = db.read(*e);
        osa::ImageSpace is{};
        is.form_id = rec.form_id();
        if (const auto* s = rec.find(k_hnam)) {
            for (int i = 0; i < 9; ++i) {
                is.hdr[i] = read_at<float>(*s, i * 4, 0.0f);
            }
        } else if (const auto* s = rec.find(k_enam)) {
            // Old form: eye adapt, blur radius, threshold, scale, receive threshold, sunlight scale (float 5 lands
            // at the same data offset as HNAM's), sky scale; then cinematic and tint.
            const int map[7] = {0, 1, 2, 3, 4, 6, 7};
            for (int i = 0; i < 7; ++i) {
                is.hdr[map[i]] = read_at<float>(*s, i * 4, 0.0f);
            }
            for (int i = 0; i < 3; ++i) {
                is.cinematic[i] = read_at<float>(*s, 28 + i * 4, 0.0f);
            }
            for (int i = 0; i < 4; ++i) {
                is.tint[i] = read_at<float>(*s, 40 + i * 4, 0.0f);
            }
        }
        if (const auto* s = rec.find(k_cnam)) {
            for (int i = 0; i < 3; ++i) {
                is.cinematic[i] = read_at<float>(*s, i * 4, 0.0f);
            }
        }
        if (const auto* s = rec.find(k_tnam)) {
            for (int i = 0; i < 4; ++i) {
                is.tint[i] = read_at<float>(*s, i * 4, 0.0f);
            }
        }
        if (const auto* s = rec.find(k_dnam)) {
            for (int i = 0; i < 4; ++i) {
                is.dof[i] = read_at<float>(*s, i * 4, 0.0f);
            }
        }
        a.image_spaces.push_back(is);
    }

    // Volumetric lighting (VOLI: CNAM..NNAM, one float each).
    for (auto e : db.of_type(k_voli)) {
        if (e->deleted()) {
            continue;
        }
        auto rec = db.read(*e);
        osa::Volumetric v{};
        v.form_id = rec.form_id();
        float* f = &v.intensity;
        for (int i = 0; i < 12; ++i) {
            const char sig[5] = {char('C' + i), 'N', 'A', 'M', 0};
            f[i] = field_float(rec, parse_type(sig), 0.0f);
        }
        a.volumetrics.push_back(v);
    }

    // Shader particle geometry (precipitation).
    for (auto e : db.of_type(k_spgd)) {
        if (e->deleted()) {
            continue;
        }
        auto rec = db.read(*e);
        const auto* d = rec.find(k_data);
        if (!d || d->data.size() < 48) {
            continue; // the 40-byte DATA forms (DustParticles, FogParticles) have no box or density
        }
        osa::Precipitation p{};
        p.form_id = rec.form_id();
        p.gravity = read_at<float>(*d, 0x00, 0.0f);
        p.rotation_velocity = read_at<float>(*d, 0x04, 0.0f);
        p.size_x = read_at<float>(*d, 0x08, 0.0f);
        p.size_y = read_at<float>(*d, 0x0C, 0.0f);
        p.offset_min = read_at<float>(*d, 0x10, 0.0f);
        p.offset_max = read_at<float>(*d, 0x14, 0.0f);
        p.rotation_range = read_at<float>(*d, 0x18, 0.0f);
        p.subtextures_x = read_at<uint32_t>(*d, 0x1C, 1);
        p.subtextures_y = read_at<uint32_t>(*d, 0x20, 1);
        p.type = read_at<uint32_t>(*d, 0x24, 0);
        p.box_size = static_cast<float>(read_at<uint32_t>(*d, 0x28, 4096));
        p.density = read_at<float>(*d, 0x2C, 1.0f);
        // ICON is relative to textures\; an empty one falls back to FXSmokeSoftSub.
        std::string tex = rooted(rec.string(k_icon), "textures");
        if (tex.empty()) {
            tex = "textures/effects/fxsmokesoftsub.dds";
        }
        p.texture = a.intern(tex);
        out.textures.insert(tex);
        a.precipitations.push_back(p);
    }

    // Image-space modifiers: DNAM, the xIAD multiply/add curves, the single curves.
    for (auto e : db.of_type(k_imad)) {
        if (e->deleted()) {
            continue;
        }
        auto rec = db.read(*e);
        osa::Modifier m{};
        m.form_id = rec.form_id();
        if (const auto* d = rec.find(k_dnam)) {
            m.animatable = read_at<uint32_t>(*d, 0, 0);
            m.duration = read_at<float>(*d, 4, 0.0f);
            m.radial_use_target = read_at<uint32_t>(*d, 200, 0);
            m.radial_center[0] = read_at<float>(*d, 204, 0.0f);
            m.radial_center[1] = read_at<float>(*d, 208, 0.0f);
            m.dof_use_target = read_at<uint8_t>(*d, 224, 0);
            m.dof_flags = read_at<uint8_t>(*d, 225, 0);
        }
        for (const auto& s : rec.subrecords()) {
            if ((s.type >> 8) == (uint32_t('I') | (uint32_t('A') << 8) | (uint32_t('D') << 16))) {
                const uint32_t b = s.type & 0xFF;
                const uint32_t field = b & 0x3F;
                if (field < osa::k_curve_fields) {
                    ((b >> 6) & 3) == 0 ? m.mult[field] = read_curve(&s, 2, a) : m.add[field] = read_curve(&s, 2, a);
                }
            }
        }
        const std::pair<const char*, osa::ModifierCurve> singles[] = {
            {"BNAM", osa::curve_blur_radius},      {"VNAM", osa::curve_double_vision},
            {"RNAM", osa::curve_radial_strength},  {"SNAM", osa::curve_radial_ramp_up},
            {"UNAM", osa::curve_radial_start},     {"NAM1", osa::curve_radial_ramp_down},
            {"NAM2", osa::curve_radial_down_start}, {"WNAM", osa::curve_dof_strength},
            {"XNAM", osa::curve_dof_distance},     {"YNAM", osa::curve_dof_range},
            {"NAM4", osa::curve_motion_blur},
        };
        for (const auto& [sig, curve] : singles) {
            m.single[curve] = read_curve(rec.find(parse_type(sig)), 2, a);
        }
        m.tint = read_curve(rec.find(k_tnam), 5, a);
        m.fade = read_curve(rec.find(parse_type("NAM3")), 5, a);
        a.modifiers.push_back(m);
        a.modifier_edids.emplace_back(rec.edid());
    }

    // Weathers.
    for (auto e : db.of_type(k_wthr)) {
        if (e->deleted()) {
            continue;
        }
        auto rec = db.read(*e);
        osa::Weather w{};
        w.form_id = rec.form_id();
        w.sky_model = container::k_no_string;
        if (const auto* s = rec.find(k_nam0)) {
            const size_t n = std::min(s->data.size(), sizeof w.colours);
            std::memcpy(w.colours, s->data.data(), n);
            // An all-zero Effect Lighting entry takes that time's Sunlight.
            for (int t = 0; t < osa::k_times; ++t) {
                auto& c = w.colours[osa::colour_effect_lighting][t];
                if (c[0] == 0 && c[1] == 0 && c[2] == 0) {
                    std::memcpy(c, w.colours[osa::colour_sunlight][t], 4);
                }
            }
            if (s->data.size() <= 0xC0) {
                // a short colour table copies Fog Near into Fog Far.
                std::memcpy(w.colours[osa::colour_fog_far], w.colours[osa::colour_fog_near], sizeof w.colours[0]);
            }
        }
        if (const auto* s = rec.find(k_fnam)) {
            // 32 bytes; the old 24-byte form has no max values (left at 1).
            w.fog[6] = w.fog[7] = 1.0f;
            std::memcpy(w.fog, s->data.data(), std::min<size_t>(s->data.size(), sizeof w.fog));
        }
        if (const auto* s = rec.find(k_data)) {
            std::memcpy(w.data, s->data.data(), std::min<size_t>(s->data.size(), 19));
        }
        int t = 0;
        for (const auto& s : rec.subrecords()) {
            if (s.type == k_dalc && t < osa::k_times) {
                // A shorter subrecord keeps the defaults (specular 0, fresnel 1).
                w.ambient[t].fresnel_power = 1.0f;
                std::memcpy(&w.ambient[t], s.data.data(), std::min(s.data.size(), sizeof w.ambient[t]));
                ++t;
            } else if (s.type == k_tnam && s.data.size() >= 4) {
                if (w.sky_statics_count == 0) {
                    w.sky_statics_first = static_cast<uint32_t>(a.sky_statics.size());
                }
                a.sky_statics.push_back(rec.resolve(read_at<uint32_t>(s, 0, 0)));
                ++w.sky_statics_count;
            } else if (s.type == k_snam && s.data.size() >= 8 && read_at<uint32_t>(s, 4, 0) == k_thunder_sound) {
                ++w.thunder_count;
            }
        }
        for (int k = 0; k < osa::k_times; ++k) {
            w.image_spaces[k] = k_default_image_space;
        }
        if (const auto* s = rec.find(k_imsp)) {
            for (int k = 0; k < osa::k_times && size_t(k + 1) * 4 <= s->data.size(); ++k) {
                const uint32_t local = read_at<uint32_t>(*s, k * 4, 0);
                if (local) {
                    w.image_spaces[k] = rec.resolve(local);
                }
            }
        }
        if (const auto* s = rec.find(k_hnam)) {
            for (int k = 0; k < osa::k_times && size_t(k + 1) * 4 <= s->data.size(); ++k) {
                const uint32_t local = read_at<uint32_t>(*s, k * 4, 0);
                w.volumetric[k] = local ? rec.resolve(local) : 0;
            }
        }
        w.precipitation = rec.form(k_mnam);
        if (auto model = rooted(rec.string(k_modl), "meshes"); !model.empty()) {
            w.sky_model = a.intern(model);
            out.meshes.insert(model);
        }
        // Clouds: textures per layer (old records name the first five with ANAM..ENAM), speeds, colours, alphas.
        const Subrecord* lnam = rec.find(k_lnam);
        w.cloud_layer_count = lnam ? read_at<uint32_t>(*lnam, 0, 0) : 0;
        const Subrecord* nam1 = rec.find(k_nam1);
        w.clouds_disabled = nam1 ? read_at<uint32_t>(*nam1, 0, 0) : 0;
        const std::pair<RecordType, int> old_textures[] = {
            {k_dnam, 0}, {k_cnam, 1}, {k_anam, 2}, {k_bnam, 3}, {k_enam, 4}};
        const Subrecord* qnam = rec.find(k_qnam);
        const Subrecord* rnam = rec.find(k_rnam);
        const Subrecord* pnam = rec.find(k_pnam);
        const Subrecord* jnam = rec.find(k_jnam);
        for (int i = 0; i < osa::k_cloud_layers; ++i) {
            auto& layer = w.clouds[i];
            std::string tex = rooted(rec.string(cloud_texture_type(i)), "textures");
            if (tex.empty()) {
                for (const auto& [sig, l] : old_textures) {
                    if (l == i) {
                        tex = rooted(rec.string(sig), "textures");
                    }
                }
            }
            layer.texture = tex.empty() ? container::k_no_string : a.intern(tex);
            if (tex.empty()) {
                w.clouds_disabled |= 1u << i; // layers without a texture are disabled
            } else {
                out.textures.insert(tex);
            }
            layer.speed_x = qnam ? read_at<uint8_t>(*qnam, i, 127) : 127;
            layer.speed_y = rnam ? read_at<uint8_t>(*rnam, i, 127) : 127;
            for (int k = 0; k < osa::k_times; ++k) {
                if (pnam) {
                    for (int c = 0; c < 4; ++c) {
                        layer.colours[k][c] = read_at<uint8_t>(*pnam, (i * 4 + k) * 4 + c, 0);
                    }
                }
                layer.alphas[k] = jnam ? read_at<float>(*jnam, (i * 4 + k) * 4, 1.0f) : 1.0f;
            }
        }
        a.weathers.push_back(w);
        a.weather_edids.emplace_back(rec.edid());
    }

    // Climates and their weather lists (a chance global's value replaces the chance).
    for (auto e : db.of_type(k_clmt)) {
        if (e->deleted()) {
            continue;
        }
        auto rec = db.read(*e);
        osa::Climate c{};
        c.form_id = rec.form_id();
        if (const auto* s = rec.find(k_tnam)) {
            std::memcpy(c.timing, s->data.data(), std::min<size_t>(s->data.size(), 6));
        }
        auto tex = [&](RecordType sig, std::string_view fallback) {
            std::string p = rooted(rec.string(sig), "textures");
            if (p.empty()) {
                p = fallback;
            }
            out.textures.insert(p);
            return a.intern(p);
        };
        c.sun_texture = tex(k_fnam, "textures/sky/sun.dds");
        c.glare_texture = tex(k_gnam, "textures/sky/sunglare.dds");
        std::string stars = rooted(rec.string(k_modl), "meshes");
        if (stars.empty()) {
            stars = "meshes/sky/stars.nif";
        }
        c.stars_model = a.intern(stars);
        import_sky_model(data, stars, a, out.textures);
        c.first_weather = static_cast<uint32_t>(a.climate_weathers.size());
        if (const auto* s = rec.find(k_wlst)) {
            for (size_t o = 0; o + 12 <= s->data.size(); o += 12) {
                const uint32_t weather = rec.resolve(read_at<uint32_t>(*s, o, 0));
                int32_t chance = read_at<int32_t>(*s, o + 4, 0);
                if (const uint32_t glob = read_at<uint32_t>(*s, o + 8, 0)) {
                    if (auto v = global_value(rec.resolve(glob))) {
                        chance = static_cast<int32_t>(*v);
                    }
                }
                a.climate_weathers.push_back({weather, chance});
            }
        }
        c.weather_count = static_cast<uint32_t>(a.climate_weathers.size()) - c.first_weather;
        a.climates.push_back(c);
        a.climate_edids.emplace_back(rec.edid());
    }

    // Water forms. NNAM repeats, one per noise layer; DNAM is read as floats.
    for (auto e : db.of_type(k_watr)) {
        if (e->deleted()) {
            continue;
        }
        auto rec = db.read(*e);
        osa::Water w{};
        w.form_id = rec.form_id();
        w.edid = a.intern(rec.edid());
        std::fill(std::begin(w.noise), std::end(w.noise), container::k_no_string);
        int noise = 0;
        for (const auto& s : rec.subrecords()) {
            // Skyrim.esm's forms repeat NNAM; Update.esm's and later ones name the layers NAM2, NAM3, NAM4.
            const int layer = s.type == k_nam2 ? 0 : s.type == k_nam3 ? 1 : s.type == k_nam4 ? 2 : s.type == k_nnam ? noise++ : -1;
            if (layer < 0 || layer > 2) {
                continue;
            }
            const auto* text = reinterpret_cast<const char*>(s.data.data());
            std::string_view raw(text, strnlen(text, s.data.size()));
            if (raw.size() > 5 && path::normalize(raw.substr(0, 5)) == "data/") {
                raw.remove_prefix(5); // these paths alone keep the game's "Data\\"
            }
            const std::string tex = rooted(raw, "textures");
            if (!tex.empty()) {
                w.noise[layer] = a.intern(tex);
                out.textures.insert(tex);
            }
        }
        if (const auto* s = rec.find(k_gnam)) {
            for (int i = 0; i < 3; ++i) {
                w.related[i] = rec.resolve(read_at<uint32_t>(*s, i * 4, 0));
            }
        }
        if (const auto* s = rec.find(k_inam)) {
            w.image_space = rec.resolve(read_at<uint32_t>(*s, 0, 0));
        }
        if (const auto* s = rec.find(k_fnam)) {
            w.flags = read_at<uint8_t>(*s, 0, 0);
        }
        if (const auto* s = rec.find(k_anam)) {
            w.opacity = read_at<uint8_t>(*s, 0, 0);
        }
        if (const auto* s = rec.find(k_nam0)) {
            for (int i = 0; i < 3; ++i) {
                w.velocity[i] = read_at<float>(*s, i * 4, 0.0f);
            }
        }
        if (const auto* s = rec.find(k_dnam)) {
            std::memcpy(w.data, s->data.data(), std::min(s->data.size(), sizeof(w.data)));
        }
        a.waters.push_back(w);
    }
    std::sort(a.waters.begin(), a.waters.end(), [](const osa::Water& x, const osa::Water& y) {
        return x.form_id < y.form_id;
    });

    out.textures.insert("textures/water/riverflow.dds");

    // The sky's own meshes (Atmosphere::Init, Clouds::Init) and textures (moons: the game, MoonShadow).
    import_sky_model(data, "meshes/sky/atmosphere.nif", a, out.textures);
    import_sky_model(data, "meshes/sky/clouds.nif", a, out.textures);
    for (const char* moon : {"masser", "secunda"}) {
        for (const char* phase : {"full", "three_wan", "half_wan", "one_wan", "new", "one_wax", "half_wax",
                                  "three_wax"}) {
            out.textures.insert(std::format("textures/sky/{}_{}.dds", moon, phase));
        }
    }
    out.textures.insert("textures/sky/moonshadow.dds");

    // Worldspace -> climate; child worlds that use their parent's climate take it.
    struct WorldInfo {
        std::string edid;
        uint32_t climate = 0;
        uint32_t parent = 0;
        uint16_t parent_flags = 0;
    };
    std::map<uint32_t, WorldInfo> worlds;
    for (auto e : db.of_type(k_wrld)) {
        if (e->deleted()) {
            continue;
        }
        auto rec = db.read(*e);
        WorldInfo info;
        info.edid = lower(rec.edid());
        info.climate = rec.form(k_cnam);
        info.parent = rec.form(k_wnam);
        if (const auto* s = rec.find(k_pnam); s && s->data.size() >= 2) {
            std::memcpy(&info.parent_flags, s->data.data(), 2);
        }
        worlds.emplace(rec.form_id(), std::move(info));
    }
    for (const auto& [id, info] : worlds) {
        uint32_t climate = info.climate;
        const WorldInfo* w = &info;
        for (int depth = 0; depth < 8 && w->parent && (w->parent_flags & k_parent_use_climate); ++depth) {
            auto it = worlds.find(w->parent);
            if (it == worlds.end()) {
                break;
            }
            w = &it->second;
            climate = w->climate;
        }
        a.world_climates.emplace_back(info.edid, climate);
    }
    // Weather regions (REGN RDAT type 3) and the exterior cells that list them.
    std::map<uint32_t, uint32_t> world_index;
    {
        uint32_t i = 0;
        for (const auto& [id, info] : worlds) {
            world_index.emplace(id, i++);
        }
    }
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> region_index; // FormID -> index, world (0 = any)
    for (auto e : db.of_type(k_regn)) {
        if (e->deleted()) {
            continue;
        }
        auto rec = db.read(*e);
        osa::Region g{};
        g.form_id = rec.form_id();
        g.first_weather = static_cast<uint32_t>(a.climate_weathers.size());
        g.first_area = static_cast<uint32_t>(a.region_areas.size());
        const size_t first_point = a.region_points.size();
        uint32_t entry_type = 0;
        for (const auto& s : rec.subrecords()) {
            if (s.type == k_rpld) {
                a.region_areas.push_back({static_cast<uint32_t>(a.region_points.size() / 2),
                                          static_cast<uint32_t>(s.data.size() / 8)});
                for (size_t o = 0; o + 4 <= s.data.size() / 8 * 8; o += 4) {
                    a.region_points.push_back(read_at<float>(s, o, 0.0f));
                }
            } else if (s.type == k_rdat && s.data.size() >= 6) {
                entry_type = read_at<uint32_t>(s, 0, 0);
                if (entry_type == 3) {
                    g.override_others = read_at<uint8_t>(s, 4, 0) != 0;
                    g.priority = read_at<uint8_t>(s, 5, 0);
                }
            } else if (s.type == k_rdwt && entry_type == 3) {
                for (size_t o = 0; o + 12 <= s.data.size(); o += 12) {
                    const uint32_t weather = rec.resolve(read_at<uint32_t>(s, o, 0));
                    int32_t chance = read_at<int32_t>(s, o + 4, 0);
                    if (const uint32_t glob = read_at<uint32_t>(s, o + 8, 0)) {
                        if (auto v = global_value(rec.resolve(glob))) {
                            chance = static_cast<int32_t>(*v);
                        }
                    }
                    a.climate_weathers.push_back({weather, chance});
                }
            }
        }
        g.weather_count = static_cast<uint32_t>(a.climate_weathers.size()) - g.first_weather;
        g.area_count = static_cast<uint32_t>(a.region_areas.size()) - g.first_area;
        if (g.weather_count == 0) {
            a.region_areas.resize(g.first_area);
            a.region_points.resize(first_point);
            continue;
        }
        region_index.emplace(g.form_id, std::pair(static_cast<uint32_t>(a.regions.size()), rec.form(k_wnam)));
        a.regions.push_back(g);
    }
    for (auto e : db.of_type(k_cell)) {
        auto wi = world_index.find(e->parent);
        if (e->deleted() || wi == world_index.end()) {
            continue;
        }
        auto rec = db.read(*e);
        const auto* grid = rec.find(k_xclc);
        const auto* list = rec.find(k_xclr);
        if (!grid || grid->data.size() < 8 || !list) {
            continue;
        }
        osa::RegionCell c{wi->second, read_at<int32_t>(*grid, 0, 0), read_at<int32_t>(*grid, 4, 0),
                          static_cast<uint32_t>(a.region_lists.size()), 0};
        for (size_t o = 0; o + 4 <= list->data.size(); o += 4) {
            auto it = region_index.find(rec.resolve(read_at<uint32_t>(*list, o, 0)));
            if (it != region_index.end() && (it->second.second == 0 || it->second.second == e->parent)) {
                a.region_lists.push_back(it->second.first);
                ++c.count;
            }
        }
        if (c.count) {
            a.region_cells.push_back(c);
        }
    }
    std::sort(a.region_cells.begin(), a.region_cells.end(), [](const osa::RegionCell& l, const osa::RegionCell& r) {
        return std::tuple(l.world, l.x, l.y) < std::tuple(r.world, r.x, r.y);
    });
    log::info("atmosphere: {} weather regions, {} cells in them", a.regions.size(), a.region_cells.size());
    log::info("atmosphere: {} weathers, {} climates, {} worldspaces, {} image spaces, {} modifiers, {} sky shapes",
              a.weathers.size(), a.climates.size(), a.world_climates.size(), a.image_spaces.size(),
              a.modifiers.size(), a.sky_shapes.size());
    return out;
}

} // namespace spud::importer
