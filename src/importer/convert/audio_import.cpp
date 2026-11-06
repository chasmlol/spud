#include "importer/convert/audio_import.hpp"

#include "core/binary.hpp"
#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"
#include "formats/osv.hpp"
#include "importer/convert/audio_codec.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace spud::importer {

namespace {

using legacy::EsmDatabase;
using legacy::Record;
using legacy::record_type;
using legacy::RecordEntry;
using legacy::RecordType;

constexpr RecordType k_snct = record_type("SNCT");
constexpr RecordType k_sopm = record_type("SOPM");
constexpr RecordType k_sndr = record_type("SNDR");
constexpr RecordType k_soun = record_type("SOUN");
constexpr RecordType k_revb = record_type("REVB");
constexpr RecordType k_aspc = record_type("ASPC");
constexpr RecordType k_musc = record_type("MUSC");
constexpr RecordType k_must = record_type("MUST");
constexpr RecordType k_fstp = record_type("FSTP");
constexpr RecordType k_fsts = record_type("FSTS");
constexpr RecordType k_ipct = record_type("IPCT");
constexpr RecordType k_ipds = record_type("IPDS");
constexpr RecordType k_matt = record_type("MATT");
constexpr RecordType k_regn = record_type("REGN");
constexpr RecordType k_wthr = record_type("WTHR");
constexpr RecordType k_wrld = record_type("WRLD");
constexpr RecordType k_lctn = record_type("LCTN");
constexpr RecordType k_cell = record_type("CELL");
constexpr RecordType k_refr = record_type("REFR");
constexpr RecordType k_acti = record_type("ACTI");
constexpr RecordType k_tact = record_type("TACT");
constexpr RecordType k_mstt = record_type("MSTT");
constexpr RecordType k_ligh = record_type("LIGH");
constexpr RecordType k_door = record_type("DOOR");
constexpr RecordType k_watr = record_type("WATR");
constexpr RecordType k_eczn = record_type("ECZN");
constexpr RecordType k_dobj = record_type("DOBJ");

constexpr RecordType k_full = record_type("FULL");
constexpr RecordType k_fnam = record_type("FNAM");
constexpr RecordType k_pnam = record_type("PNAM");
constexpr RecordType k_vnam = record_type("VNAM");
constexpr RecordType k_unam = record_type("UNAM");
constexpr RecordType k_nam1 = record_type("NAM1");
constexpr RecordType k_mnam = record_type("MNAM");
constexpr RecordType k_onam = record_type("ONAM");
constexpr RecordType k_anam = record_type("ANAM");
constexpr RecordType k_bnam = record_type("BNAM");
constexpr RecordType k_cnam = record_type("CNAM");
constexpr RecordType k_gnam = record_type("GNAM");
constexpr RecordType k_snam = record_type("SNAM");
constexpr RecordType k_lnam = record_type("LNAM");
constexpr RecordType k_ctda = record_type("CTDA");
constexpr RecordType k_sdsc = record_type("SDSC");
constexpr RecordType k_data = record_type("DATA");
constexpr RecordType k_obnd = record_type("OBND");
constexpr RecordType k_rdat = record_type("RDAT");
constexpr RecordType k_wtnm = record_type("WNAM");
constexpr RecordType k_tnam = record_type("TNAM");
constexpr RecordType k_fltv = record_type("FLTV");
constexpr RecordType k_dnam = record_type("DNAM");
constexpr RecordType k_hnam = record_type("HNAM");
constexpr RecordType k_xcnt = record_type("XCNT");
constexpr RecordType k_rdmo = record_type("RDMO");
constexpr RecordType k_rdsa = record_type("RDSA");
constexpr RecordType k_znam = record_type("ZNAM");
constexpr RecordType k_xlcn = record_type("XLCN");
constexpr RecordType k_xclc = record_type("XCLC");
constexpr RecordType k_xcas = record_type("XCAS");
constexpr RecordType k_xcmo = record_type("XCMO");
constexpr RecordType k_xclr = record_type("XCLR");
constexpr RecordType k_name = record_type("NAME");
constexpr RecordType k_xscl = record_type("XSCL");
constexpr RecordType k_xesp = record_type("XESP");
constexpr RecordType k_xprm = record_type("XPRM");
constexpr RecordType k_rpld = record_type("RPLD");
constexpr RecordType k_nam2 = record_type("NAM2");
constexpr RecordType k_xcwt = record_type("XCWT");
constexpr RecordType k_xclw = record_type("XCLW");
constexpr RecordType k_xezn = record_type("XEZN");

constexpr uint32_t k_player_ref = 0x00000014;
constexpr uint32_t k_none = osd::k_none;

// Effective INI defaults: the value compiled into the game (each setting's object
// audio.md), then Skyrim_Default.ini, which the launcher copies to the user's Skyrim.ini.
struct IniDefault {
    const char* name;
    float value;
};
constexpr IniDefault k_ini_defaults[] = {
    {"fASFadeInTime:Audio", 3.0f},
    {"fASFadeOutTime:Audio", 8.0f},
    {"fRegionLoopFadeInTime:Audio", 6.0f},
    {"fRegionLoopFadeOutTime:Audio", 8.0f},
    {"fReverbTransitionTime:Audio", 0.5f},
    {"fRegionRandomSoundPlacementBase:Audio", 100.0f},
    {"fRegionSoundPlacementRandomOffset:Audio", 650.0f},
    {"fRegionSoundPlacementZOffset:Audio", 256.0f},
    {"uRegionSoundsAllowedPerUpdate:Audio", 1.0f},
    {"fMusicDuckingSeconds:Audio", 1.0f},
    {"fMusicUnDuckingSeconds:Audio", 4.0f},
    {"fMusicFinaleCrossFadeTimeIn:Audio", 3.0f},
    {"fMusicFinaleCrossFadeTimeOut:Audio", 6.0f},
    {"fMenuModeFadeInTime:Audio", 0.5f},
    {"fMenuModeFadeOutTime:Audio", 0.5f},
    {"fNonDialogVoiceDucking:Audio", 9.0f},
    {"fNonDialogVoiceDuckingFadeIn:Audio", 1.0f},
    {"fNonDialogVoiceDuckingFadeOut:Audio", 1.5f},
    {"fNonHighlightSpeechAtten:Audio", 12.0f},
    {"fMaxHighlightRadius:Audio", 250.0f},
    {"fWaterAudioFadeInSeconds:Audio", 3.0f},
    {"fWaterAudioFadeOutSeconds:Audio", 5.0f},
    {"uWaterAudioAmbientAttenuation:Audio", 2200.0f},
    {"uWaterAudioAxisSamplePoints:Audio", 8.0f},
    {"uWaterAudioSamplePointsToUse:Audio", 8.0f},
    {"uAudioThreadSleepTimeGameMode:Audio", 33.0f},
    {"uAudioThreadSleepTimeMenuMode:Audio", 1.0f},
    {"fDefaultMasterVolume:Audio", 1.0f},
    {"fAudioMasterVolume:AudioMenu", 1.0f},
    {"fCollisionSoundHeavyThreshold:Audio", 160.0f},
    {"iCollisionSoundTimeDelta:Audio", 150.0f},
    {"fMinSoundVel:Audio", 60.0f},
    {"fHardLandingDamageThreshold:Audio", 500.0f},
    {"uMainMenuMusicAttnmB:General", 1200.0f},
    {"uMainMenuMusicFadeTimeMS:General", 1500.0f},
    {"uGridsToLoad:General", 5.0f},
    {"bPrecipitation:Weather", 1.0f},
    // The sound cache: setting objects,,
    {"bEnableAudioCache:Audio", 1.0f},
    {"uMaxSizeForCachedSound:Audio", 262144.0f},
    {"uiInitialCacheSize:Audio", 1048576.0f},
};

// Condition functions whose first or second parameter is a form (xEdit's wbCTDAFunctions), for the ones
// audio records use. Other parameters are stored raw.
struct FormParams {
    uint16_t function;
    bool param1;
    bool param2;
};
constexpr FormParams k_form_params[] = {
    {69, true, false},  // GetIsRace
    {71, true, false},  // GetInFaction
    {72, true, false},  // GetIsID
    {74, true, false},  // GetGlobalValue
    {359, true, false}, // GetInCurrentLoc
    {448, true, false}, // HasPerk
    {560, true, false}, // HasKeyword
    {707, true, false}, // GetCombatTargetHasKeyword
};

struct Quat {
    float x, y, z, w;
};

Quat mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Same convention as the world import (osw::Ref): R = Rx(-x) * Ry(-y) * Rz(-z).
Quat euler_to_quat(const float rot[3]) {
    auto axis = [](int i, float angle) {
        float s = std::sin(angle * 0.5f);
        Quat q{0.0f, 0.0f, 0.0f, std::cos(angle * 0.5f)};
        (i == 0 ? q.x : i == 1 ? q.y : q.z) = s;
        return q;
    };
    return mul(mul(axis(0, -rot[0]), axis(1, -rot[1])), axis(2, -rot[2]));
}

std::string strip_data_prefix(std::string p) {
    if (p.starts_with("data/")) {
        p.erase(0, 5);
    }
    return p;
}

class Builder {
  public:
    Builder(const EsmDatabase& db, const legacy::DataSource& data) : m_db(db), m_data(data) {}

    AudioRecordsImport run(const std::filesystem::path& game_dir) {
        categories();
        output_models();
        descriptors();
        markers();
        reverbs();
        acoustic_spaces();
        music();
        materials_and_impacts();
        footsteps();
        regions();
        weathers();
        waters();
        locations_and_worlds();
        cells();
        default_objects();
        settings(game_dir);
        m_out.db.strings.assign(reinterpret_cast<const char*>(m_strings.bytes().data()),
                                reinterpret_cast<const char*>(m_strings.bytes().data()) +
                                    m_strings.bytes().size());
        m_out.files.assign(m_files.begin(), m_files.end());
        return std::move(m_out);
    }

  private:
    uint32_t str(std::string_view s) { return s.empty() ? container::k_no_string : m_strings.add(s); }

    template <typename Fn> void each(RecordType type, Fn&& fn) {
        for (auto e : m_db.of_type(type)) {
            try {
                fn(m_db.read(*e));
            } catch (const std::exception& ex) {
                log::warn("audio: {} {:08X}: {}", legacy::type_name(type), e->form_id, ex.what());
            }
        }
    }

    static uint32_t lookup(const std::unordered_map<uint32_t, uint32_t>& map, uint32_t form_id) {
        if (form_id == 0) {
            return k_none;
        }
        auto it = map.find(form_id);
        return it == map.end() ? k_none : it->second;
    }

    // A record's audio file reference resolved like the game: the extension the
    // record names is ignored and .wav, .fuz, .xwm are tried in that order. Returns the cache path, or
    // k_no_string when no candidate exists.
    uint32_t audio_file(std::string_view root, std::string_view record_path, const char* kind) {
        auto p = path::normalize(record_path);
        if (p.empty()) {
            return container::k_no_string;
        }
        p = strip_data_prefix(p);
        if (!p.starts_with(root)) {
            p = path::with_root(p, root);
        }
        for (const char* ext : {".wav", ".fuz", ".xwm"}) {
            auto candidate = path::replace_extension(p, ext);
            if (m_data.exists(candidate)) {
                m_files.insert(candidate);
                return str(audio_cache_path(candidate));
            }
        }
        ++m_out.missing[kind];
        log::debug("audio: {} file not found: {}", kind, p);
        return container::k_no_string;
    }

    uint32_t conditions(const Record& r) {
        const uint32_t first = static_cast<uint32_t>(m_out.db.conditions.size());
        for (const auto& s : r.subrecords()) {
            if (s.type != k_ctda || s.data.size() < 32) {
                continue;
            }
            auto br = s.reader();
            osd::Condition c{};
            c.op = br.read<uint8_t>();
            br.skip(3);
            c.comparison = br.read<uint32_t>();
            c.function = br.read<uint16_t>();
            br.skip(2);
            c.param1 = br.read<uint32_t>();
            c.param2 = br.read<uint32_t>();
            c.run_on = br.read<uint32_t>();
            c.reference = br.read<uint32_t>();
            c.param3 = br.read<int32_t>();
            if (c.op & 0x04) { // "use global": the comparison value is a GLOB
                c.comparison = r.resolve(c.comparison);
            }
            for (const auto& fp : k_form_params) {
                if (fp.function == c.function) {
                    if (fp.param1 && c.param1) {
                        c.param1 = r.resolve(c.param1);
                    }
                    if (fp.param2 && c.param2) {
                        c.param2 = r.resolve(c.param2);
                    }
                }
            }
            if (c.reference) {
                c.reference = r.resolve(c.reference);
            }
            m_out.db.conditions.push_back(c);
        }
        return first;
    }

    void categories() {
        std::vector<uint32_t> parents;
        each(k_snct, [&](const Record& r) {
            osd::Category c{};
            c.form_id = r.form_id();
            c.edid = str(r.edid());
            c.flags = r.has(k_fnam) ? r.find(k_fnam)->as<uint32_t>() : 0;
            c.static_volume = r.has(k_vnam) ? r.find(k_vnam)->as<uint16_t>() / 65535.0f : 1.0f;
            c.default_menu_value = r.has(k_unam) ? r.find(k_unam)->as<uint16_t>() / 65535.0f : 1.0f;
            m_category.emplace(c.form_id, static_cast<uint32_t>(m_out.db.categories.size()));
            m_out.db.categories.push_back(c);
            parents.push_back(r.form(k_pnam));
        });
        for (size_t i = 0; i < parents.size(); ++i) {
            m_out.db.categories[i].parent = lookup(m_category, parents[i]);
        }
        m_out.counts["SNCT"] = m_out.db.categories.size();
    }

    void output_models() {
        each(k_sopm, [&](const Record& r) {
            osd::OutputModel m{};
            m.form_id = r.form_id();
            m.edid = str(r.edid());
            // The game. NAM1: flags, then the reverb send in byte 3. Older records
            // carry FNAM instead: the same flags, with the reverb send forced to 50 %.
            if (auto s = r.find(k_nam1); s && s->data.size() >= 4) {
                auto br = s->reader();
                m.flags = br.read<uint8_t>();
                br.skip(2);
                m.reverb_send = br.read<uint8_t>() / 100.0f;
            } else if (auto f = r.find(k_fnam); f && f->data.size() >= 4) {
                m.flags = f->reader().read<uint8_t>();
                m.reverb_send = 0.5f;
            }
            if (auto s = r.find(k_mnam); s && s->data.size() >= 4) {
                m.type = static_cast<osd::OutputType>(s->as<uint32_t>());
            }
            // Speaker levels: the object's defaults (mono FL FR 100, RL RR BL BR 50; stereo left
            // and right to their side at 100, their rear at 50), then ONAM (all three rows) or the older SNAM
            // (8 bytes: the mono row; 16 bytes: the stereo rows).
            static constexpr uint8_t k_default_speakers[3][8] = {
                {100, 100, 0, 0, 50, 50, 50, 50}, {100, 0, 0, 0, 50, 0, 50, 0}, {0, 100, 0, 0, 0, 50, 0, 50}};
            std::memcpy(m.speakers, k_default_speakers, sizeof(k_default_speakers));
            if (auto s = r.find(k_onam); s && s->data.size() >= 24) {
                std::memcpy(m.speakers, s->data.data(), 24);
            } else if (auto o = r.find(k_snam); o && (o->data.size() == 8 || o->data.size() == 16)) {
                std::memcpy(o->data.size() == 8 ? m.speakers[0] : m.speakers[1], o->data.data(),
                            o->data.size());
            }
            if (auto s = r.find(k_anam); s && s->data.size() >= 17) {
                auto br = s->reader();
                br.skip(4);
                m.min_distance = units::to_metres(br.read<float>());
                m.max_distance = units::to_metres(br.read<float>());
                for (auto& v : m.curve) {
                    v = br.read<uint8_t>();
                }
                m.has_attenuation = 1;
            }
            m_output.emplace(m.form_id, static_cast<uint32_t>(m_out.db.output_models.size()));
            m_out.db.output_models.push_back(m);
        });
        m_out.counts["SOPM"] = m_out.db.output_models.size();
    }

    void descriptors() {
        std::vector<uint32_t> alternates;
        each(k_sndr, [&](const Record& r) {
            osd::Descriptor d{};
            d.form_id = r.form_id();
            d.edid = str(r.edid());
            d.category = lookup(m_category, r.form(k_gnam));
            d.output_model = lookup(m_output, r.form(k_onam));
            d.first_file = static_cast<uint32_t>(m_out.db.files.size());
            for (const auto& s : r.subrecords()) {
                if (s.type == k_anam) {
                    auto f = audio_file("sound", s.zstring(), "SNDR");
                    if (f != container::k_no_string) {
                        m_out.db.files.push_back(f);
                    }
                }
            }
            d.file_count = static_cast<uint32_t>(m_out.db.files.size()) - d.first_file;
            // Flags: the game. LNAM is the flag word; the older FNAM maps
            // 0x10 -> loop (0x800), 0x2000000 -> envelope fast (0x1000), 0x4000000 -> envelope slow (0x2000).
            if (auto s = r.find(k_lnam); s && s->data.size() >= 4) {
                d.flags = s->as<uint32_t>();
            } else if (auto f = r.find(k_fnam); f && f->data.size() >= 4) {
                uint32_t v = f->as<uint32_t>();
                if (v & 0x10) {
                    v = (v ^ 0x10) | 0x800;
                }
                if (v & 0x2000000) {
                    v = (v ^ 0x2000000) | 0x1000;
                }
                if (v & 0x4000000) {
                    v = (v ^ 0x4000000) | 0x2000;
                }
                d.flags = v;
            }
            if (auto s = r.find(k_bnam); s && s->data.size() >= 6) {
                auto br = s->reader();
                d.frequency_shift = br.read<int8_t>();
                d.frequency_variance = br.read<uint8_t>();
                d.priority = br.read<uint8_t>();
                d.db_variance = br.read<uint8_t>();
                d.static_attenuation = br.read<uint16_t>();
            }
            d.first_condition = conditions(r);
            d.condition_count = static_cast<uint32_t>(m_out.db.conditions.size()) - d.first_condition;
            m_descriptor.emplace(d.form_id, static_cast<uint32_t>(m_out.db.descriptors.size()));
            m_out.db.descriptors.push_back(d);
            alternates.push_back(r.form(k_snam));
        });
        for (size_t i = 0; i < alternates.size(); ++i) {
            m_out.db.descriptors[i].alternate_for = lookup(m_descriptor, alternates[i]);
        }
        m_out.counts["SNDR"] = m_out.db.descriptors.size();
    }

    void markers() {
        each(k_soun, [&](const Record& r) {
            osd::SoundMarker m{r.form_id(), str(r.edid()), lookup(m_descriptor, r.form(k_sdsc))};
            m_marker.emplace(m.form_id, static_cast<uint32_t>(m_out.db.markers.size()));
            m_out.db.markers.push_back(m);
        });
        m_out.counts["SOUN"] = m_out.db.markers.size();
    }

    // A sound field that may name a SNDR or (older form versions) a SOUN.
    uint32_t sound_or_marker(uint32_t form_id) {
        if (auto d = lookup(m_descriptor, form_id); d != k_none) {
            return d;
        }
        if (auto m = lookup(m_marker, form_id); m != k_none) {
            return m_out.db.markers[m].descriptor;
        }
        return k_none;
    }

    void reverbs() {
        each(k_revb, [&](const Record& r) {
            osd::Reverb v{};
            v.form_id = r.form_id();
            v.edid = str(r.edid());
            if (auto s = r.find(k_data); s && s->data.size() >= 14) {
                auto br = s->reader();
                v.decay_time_ms = br.read<uint16_t>();
                v.hf_reference_hz = br.read<uint16_t>();
                v.room_filter = br.read<int8_t>();
                v.room_hf_filter = br.read<int8_t>();
                v.reflections = br.read<int8_t>();
                v.reverb_amp = br.read<int8_t>();
                v.decay_hf_ratio = br.read<uint8_t>();
                v.reflect_delay = br.read<uint8_t>();
                v.reverb_delay = br.read<uint8_t>();
                v.diffusion = br.read<uint8_t>();
                v.density = br.read<uint8_t>();
            }
            m_reverb.emplace(v.form_id, static_cast<uint32_t>(m_out.db.reverbs.size()));
            m_out.db.reverbs.push_back(v);
        });
        m_out.counts["REVB"] = m_out.db.reverbs.size();
    }

    void acoustic_spaces() {
        each(k_aspc, [&](const Record& r) {
            osd::AcousticSpace a{};
            a.form_id = r.form_id();
            a.edid = str(r.edid());
            a.ambient = lookup(m_descriptor, r.form(k_snam));
            a.region = r.form(k_rdat);
            a.reverb = lookup(m_reverb, r.form(k_bnam));
            if (auto s = r.find(k_obnd); s && s->data.size() >= 12) {
                auto br = s->reader();
                for (auto& v : a.bounds_min) {
                    v = units::to_metres(br.read<int16_t>());
                }
                for (auto& v : a.bounds_max) {
                    v = units::to_metres(br.read<int16_t>());
                }
            }
            m_aspc.emplace(a.form_id, static_cast<uint32_t>(m_out.db.acoustic_spaces.size()));
            m_out.db.acoustic_spaces.push_back(a);
        });
        m_out.counts["ASPC"] = m_out.db.acoustic_spaces.size();
    }

    void music() {
        // Tracks first (types and palettes reference them), then their cross-references.
        std::vector<std::vector<uint32_t>> palette_forms;
        each(k_must, [&](const Record& r) {
            osd::MusicTrack t{};
            t.form_id = r.form_id();
            t.edid = str(r.edid());
            const uint32_t cnam = r.has(k_cnam) ? r.find(k_cnam)->as<uint32_t>() : 0;
            t.type = cnam == 0x23F678C3u   ? osd::TrackType::palette
                     : cnam == 0xA1A9C4D5u ? osd::TrackType::silent
                                           : osd::TrackType::single;
            if (auto s = r.find(k_fltv); s && s->data.size() >= 4) {
                t.duration = s->as<float>();
            }
            if (auto s = r.find(k_dnam); s && s->data.size() >= 4) {
                t.fade_out = s->as<float>();
            }
            t.file = r.has(k_anam) ? audio_file("music", r.string(k_anam), "MUST") : container::k_no_string;
            t.finale =
                r.has(k_bnam) ? audio_file("music", r.string(k_bnam), "MUST finale") : container::k_no_string;
            if (auto s = r.find(k_lnam); s && s->data.size() >= 12) {
                auto br = s->reader();
                t.loop_begin = br.read<float>();
                t.loop_end = br.read<float>();
                t.loop_count = br.read<uint32_t>();
                t.has_loop = 1;
            }
            t.first_cue = static_cast<uint32_t>(m_out.db.cue_points.size());
            if (auto s = r.find(k_fnam)) {
                auto br = s->reader();
                for (size_t i = 0; i < s->data.size() / 4; ++i) {
                    m_out.db.cue_points.push_back(br.read<float>());
                }
            }
            t.cue_count = static_cast<uint32_t>(m_out.db.cue_points.size()) - t.first_cue;
            t.first_condition = conditions(r);
            t.condition_count = static_cast<uint32_t>(m_out.db.conditions.size()) - t.first_condition;
            std::vector<uint32_t> subs;
            if (auto s = r.find(k_snam)) {
                auto br = s->reader();
                for (size_t i = 0; i < s->data.size() / 4; ++i) {
                    subs.push_back(r.resolve(br.read<uint32_t>()));
                }
            }
            palette_forms.push_back(std::move(subs));
            m_track.emplace(t.form_id, static_cast<uint32_t>(m_out.db.music_tracks.size()));
            m_out.db.music_tracks.push_back(t);
        });
        for (size_t i = 0; i < palette_forms.size(); ++i) {
            auto& t = m_out.db.music_tracks[i];
            t.first_sub = static_cast<uint32_t>(m_out.db.track_lists.size());
            for (auto f : palette_forms[i]) {
                m_out.db.track_lists.push_back(lookup(m_track, f));
            }
            t.sub_count = static_cast<uint32_t>(m_out.db.track_lists.size()) - t.first_sub;
        }
        each(k_musc, [&](const Record& r) {
            osd::MusicType m{};
            m.form_id = r.form_id();
            m.edid = str(r.edid());
            m.flags = r.has(k_fnam) ? r.find(k_fnam)->as<uint32_t>() : 0;
            if (auto s = r.find(k_pnam); s && s->data.size() >= 4) {
                auto br = s->reader();
                m.priority = br.read<uint16_t>();
                m.ducking = br.read<uint16_t>();
            }
            if (auto s = r.find(k_wtnm); s && s->data.size() >= 4) {
                m.fade_duration = s->as<float>();
            }
            m.first_track = static_cast<uint32_t>(m_out.db.track_lists.size());
            if (auto s = r.find(k_tnam)) {
                auto br = s->reader();
                for (size_t i = 0; i < s->data.size() / 4; ++i) {
                    m_out.db.track_lists.push_back(lookup(m_track, r.resolve(br.read<uint32_t>())));
                }
            }
            m.track_count = static_cast<uint32_t>(m_out.db.track_lists.size()) - m.first_track;
            m_music.emplace(m.form_id, static_cast<uint32_t>(m_out.db.music_types.size()));
            m_out.db.music_types.push_back(m);
        });
        m_out.counts["MUST"] = m_out.db.music_tracks.size();
        m_out.counts["MUSC"] = m_out.db.music_types.size();
    }

    void materials_and_impacts() {
        std::vector<uint32_t> parents, havok_sets;
        each(k_matt, [&](const Record& r) {
            osd::Material m{};
            m.form_id = r.form_id();
            m.edid = str(r.edid());
            m.flags = r.has(k_fnam) ? r.find(k_fnam)->as<uint32_t>() : 0;
            m.name = str(r.string(k_mnam));
            m_material.emplace(m.form_id, static_cast<uint32_t>(m_out.db.materials.size()));
            m_out.db.materials.push_back(m);
            parents.push_back(r.form(k_pnam));
            havok_sets.push_back(r.form(k_hnam));
        });
        each(k_ipct, [&](const Record& r) {
            osd::Impact i{};
            i.form_id = r.form_id();
            i.edid = str(r.edid());
            if (auto s = r.find(k_data); s && s->data.size() >= 16) {
                auto br = s->reader();
                i.effect_duration = br.read<float>();
                i.orientation = br.read<uint32_t>();
                i.angle_threshold = br.read<float>();
                i.placement_radius = units::to_metres(br.read<float>());
                if (s->data.size() >= 24) {
                    i.sound_level = br.read<uint32_t>();
                    i.flags = br.read<uint8_t>();
                    i.result = br.read<uint8_t>();
                }
            }
            i.sound1 = sound_or_marker(r.form(k_snam));
            i.sound2 = sound_or_marker(r.form(k_nam1));
            m_impact.emplace(i.form_id, static_cast<uint32_t>(m_out.db.impacts.size()));
            m_out.db.impacts.push_back(i);
        });
        each(k_ipds, [&](const Record& r) {
            osd::ImpactSet set{};
            set.form_id = r.form_id();
            set.edid = str(r.edid());
            set.first = static_cast<uint32_t>(m_out.db.impact_pairs.size());
            for (const auto& s : r.subrecords()) {
                if (s.type == k_pnam && s.data.size() >= 8) {
                    auto br = s.reader();
                    const auto material = r.resolve(br.read<uint32_t>());
                    const auto impact = r.resolve(br.read<uint32_t>());
                    m_out.db.impact_pairs.push_back({material, lookup(m_impact, impact)});
                }
            }
            set.count = static_cast<uint32_t>(m_out.db.impact_pairs.size()) - set.first;
            m_impact_set.emplace(set.form_id, static_cast<uint32_t>(m_out.db.impact_sets.size()));
            m_out.db.impact_sets.push_back(set);
        });
        for (size_t i = 0; i < parents.size(); ++i) {
            m_out.db.materials[i].parent = lookup(m_material, parents[i]);
            m_out.db.materials[i].havok_impact_set = lookup(m_impact_set, havok_sets[i]);
        }
        m_out.counts["MATT"] = m_out.db.materials.size();
        m_out.counts["IPCT"] = m_out.db.impacts.size();
        m_out.counts["IPDS"] = m_out.db.impact_sets.size();
    }

    void footsteps() {
        each(k_fstp, [&](const Record& r) {
            osd::Footstep f{};
            f.form_id = r.form_id();
            f.edid = str(r.edid());
            f.impact_set = lookup(m_impact_set, r.form(k_data));
            f.tag = str(r.string(k_anam));
            m_footstep.emplace(f.form_id, static_cast<uint32_t>(m_out.db.footsteps.size()));
            m_out.db.footsteps.push_back(f);
        });
        each(k_fsts, [&](const Record& r) {
            osd::FootstepSet set{};
            set.form_id = r.form_id();
            set.edid = str(r.edid());
            uint32_t counts[osd::gait_count] = {};
            if (auto s = r.find(k_xcnt); s && s->data.size() >= 20) {
                auto br = s->reader();
                for (auto& c : counts) {
                    c = br.read<uint32_t>();
                }
            }
            // XCNT counts are in the order walk, run, sprint, sneak, swim, but DATA holds the lists in the
            // reverse order: swim, sneak, sprint, run, walk (xEdit; confirmed by the tags of
            // DefaultFootstepSet's footsteps).
            if (auto s = r.find(k_data)) {
                auto br = s->reader();
                for (int g = osd::gait_count - 1; g >= 0; --g) {
                    set.first[g] = static_cast<uint32_t>(m_out.db.footstep_lists.size());
                    for (uint32_t i = 0; i < counts[g] && br.tell() + 4 <= br.size(); ++i) {
                        m_out.db.footstep_lists.push_back(lookup(m_footstep, r.resolve(br.read<uint32_t>())));
                    }
                    set.count[g] = static_cast<uint32_t>(m_out.db.footstep_lists.size()) - set.first[g];
                }
            }
            m_out.db.footstep_sets.push_back(set);
        });
        m_out.counts["FSTP"] = m_out.db.footsteps.size();
        m_out.counts["FSTS"] = m_out.db.footstep_sets.size();
    }

    void regions() {
        each(k_regn, [&](const Record& r) {
            osd::Region g{};
            g.form_id = r.form_id();
            g.edid = str(r.edid());
            g.world = r.form(k_wtnm);
            g.music = k_none;
            g.first_sound = static_cast<uint32_t>(m_out.db.region_sounds.size());
            g.first_area = static_cast<uint32_t>(m_out.db.region_areas.size());
            uint32_t entry_type = 0;
            for (const auto& s : r.subrecords()) {
                if (s.type == k_rpld) {
                    auto br = s.reader();
                    osd::RegionArea area{static_cast<uint32_t>(m_out.db.region_points.size() / 2),
                                         static_cast<uint32_t>(s.data.size() / 8)};
                    for (uint32_t i = 0; i < area.point_count; ++i) {
                        m_out.db.region_points.push_back(units::to_metres(br.read<float>()));
                        m_out.db.region_points.push_back(units::to_metres(br.read<float>()));
                    }
                    m_out.db.region_areas.push_back(area);
                } else if (s.type == k_rdat && s.data.size() >= 6) {
                    auto br = s.reader();
                    entry_type = br.read<uint32_t>();
                    if (entry_type == 7) {
                        g.sound_override = br.read<uint8_t>();
                        g.sound_priority = br.read<uint8_t>();
                        g.has_sounds = 1;
                    }
                } else if (entry_type == 7 && s.type == k_rdmo && s.data.size() >= 4) {
                    g.music = lookup(m_music, r.resolve(s.as<uint32_t>()));
                } else if (entry_type == 7 && s.type == k_rdsa) {
                    auto br = s.reader();
                    for (size_t i = 0; i < s.data.size() / 12; ++i) {
                        osd::RegionSound rs{};
                        rs.descriptor = sound_or_marker(r.resolve(br.read<uint32_t>()));
                        rs.flags = br.read<uint32_t>();
                        rs.chance = br.read<float>();
                        m_out.db.region_sounds.push_back(rs);
                    }
                }
            }
            g.sound_count = static_cast<uint32_t>(m_out.db.region_sounds.size()) - g.first_sound;
            g.area_count = static_cast<uint32_t>(m_out.db.region_areas.size()) - g.first_area;
            m_region.emplace(g.form_id, static_cast<uint32_t>(m_out.db.regions.size()));
            m_out.db.regions.push_back(g);
        });
        m_out.counts["REGN"] = m_out.db.regions.size();
    }

    void weathers() {
        each(k_wthr, [&](const Record& r) {
            osd::Weather w{};
            w.form_id = r.form_id();
            w.edid = str(r.edid());
            if (auto s = r.find(k_data)) {
                std::memcpy(w.data, s->data.data(), std::min<size_t>(s->data.size(), sizeof(w.data)));
            }
            w.first_sound = static_cast<uint32_t>(m_out.db.weather_sounds.size());
            for (const auto& s : r.subrecords()) {
                if (s.type == k_snam && s.data.size() >= 8) {
                    auto br = s.reader();
                    osd::WeatherSound ws{};
                    ws.descriptor = sound_or_marker(r.resolve(br.read<uint32_t>()));
                    ws.type = static_cast<osd::WeatherSoundType>(br.read<uint32_t>());
                    m_out.db.weather_sounds.push_back(ws);
                }
            }
            w.sound_count = static_cast<uint32_t>(m_out.db.weather_sounds.size()) - w.first_sound;
            m_out.db.weathers.push_back(w);
        });
        m_out.counts["WTHR"] = m_out.db.weathers.size();
    }

    void locations_and_worlds() {
        each(k_lctn, [&](const Record& r) {
            m_out.db.locations.push_back(
                {r.form_id(), str(r.edid()), r.form(k_pnam), lookup(m_music, r.form(k_nam1))});
        });
        each(k_wrld, [&](const Record& r) {
            m_out.db.worlds.push_back({r.form_id(), str(r.edid()), r.form(k_wtnm),
                                       lookup(m_music, r.form(k_znam)), r.form(k_xlcn),
                                       lookup(m_water, r.form(k_nam2))});
        });
        m_out.counts["LCTN"] = m_out.db.locations.size();
        m_out.counts["WRLD"] = m_out.db.worlds.size();
    }

    // Visibility at game start (initially disabled, or an enable parent that is), as in the world import.
    bool enabled(uint32_t form_id, int depth = 0) {
        if (form_id == k_player_ref || depth > 64) {
            return true;
        }
        if (auto it = m_enabled.find(form_id); it != m_enabled.end()) {
            return it->second;
        }
        bool result = true;
        if (auto rec = m_db.get(form_id)) {
            result = (rec->flags() & legacy::record_initially_disabled) == 0;
            if (auto s = rec->find(k_xesp); s && s->data.size() >= 5) {
                auto br = s->reader();
                const auto parent = rec->resolve(br.read<uint32_t>());
                const bool opposite = (br.read<uint8_t>() & 0x01) != 0;
                if (parent) {
                    const bool p = enabled(parent, depth + 1);
                    result = opposite ? !p : p;
                }
            }
        }
        m_enabled[form_id] = result;
        return result;
    }

    // The emitter a placed reference makes, if its base object sounds.
    std::optional<osd::Emitter> emitter_for(const Record& ref) {
        const auto base_form = ref.form(k_name);
        const auto* base = m_db.find(base_form);
        if (!base || base->deleted()) {
            return std::nullopt;
        }
        osd::Emitter em{};
        em.ref_form_id = ref.form_id();
        em.base_form_id = base_form;
        em.descriptor = k_none;
        em.acoustic_space = k_none;
        if (base->type == k_soun) {
            em.kind = osd::EmitterKind::sound_marker;
            if (auto m = lookup(m_marker, base_form); m != k_none) {
                em.descriptor = m_out.db.markers[m].descriptor;
            }
        } else if (base->type == k_aspc) {
            em.kind = osd::EmitterKind::acoustic_space;
            em.acoustic_space = lookup(m_aspc, base_form);
        } else if (base->type == k_acti || base->type == k_mstt || base->type == k_ligh ||
                   base->type == k_door) {
            // The game handles SOUN, ACTI, DOOR, LIGH and MSTT; not
            // TACT.
            auto b = m_db.read(*base);
            const auto field = base->type == k_door ? k_bnam : k_snam;
            em.descriptor = lookup(m_descriptor, b.form(field));
            if (em.descriptor == k_none) {
                return std::nullopt;
            }
            em.kind = base->type == k_ligh   ? osd::EmitterKind::light
                      : base->type == k_door ? osd::EmitterKind::door_loop
                                             : osd::EmitterKind::looping_object;
        } else {
            return std::nullopt;
        }
        float pos[3] = {}, rot[3] = {};
        if (auto s = ref.find(k_data); s && s->data.size() >= 24) {
            auto br = s->reader();
            for (auto& v : pos) {
                v = br.read<float>();
            }
            for (auto& v : rot) {
                v = br.read<float>();
            }
        }
        for (int i = 0; i < 3; ++i) {
            em.position[i] = units::to_metres(pos[i]);
        }
        const auto q = euler_to_quat(rot);
        em.rotation[0] = q.x;
        em.rotation[1] = q.y;
        em.rotation[2] = q.z;
        em.rotation[3] = q.w;
        em.scale = ref.has(k_xscl) ? ref.find(k_xscl)->as<float>() : 1.0f;
        if (auto s = ref.find(k_xprm); s && s->data.size() >= 32) {
            auto br = s->reader();
            for (auto& v : em.primitive_bounds) {
                v = units::to_metres(br.read<float>());
            }
            br.skip(16); // colour, unknown
            em.primitive_type = br.read<uint32_t>();
            if (em.kind == osd::EmitterKind::sound_marker && em.primitive_type == osd::primitive_line) {
                line_endpoints(pos, rot, em);
            }
        }
        em.flags = enabled(em.ref_form_id) ? 0 : osd::emitter_initially_disabled;
        return em;
    }

    void cells() {
        struct Pending {
            osd::Cell cell;
            std::vector<uint32_t> regions;
            std::vector<osd::Emitter> emitters;
        };
        std::vector<Pending> cells;
        std::map<std::tuple<uint32_t, int32_t, int32_t>, size_t> by_grid;
        std::vector<std::pair<uint32_t, std::vector<osd::Emitter>>> world_persistent; // world -> emitters

        for (auto e : m_db.of_type(k_cell)) {
            try {
                auto r = m_db.read(*e);
                const bool persistent_world_cell = e->group == legacy::group_world_children;
                osd::Cell c{};
                c.form_id = r.form_id();
                c.edid = str(r.edid());
                c.world = e->parent;
                uint16_t data_flags = 0;
                if (auto s = r.find(k_data); s && !s->data.empty()) {
                    data_flags =
                        s->data.size() >= 2 ? s->as<uint16_t>() : static_cast<uint16_t>(s->as<uint8_t>());
                }
                c.flags = data_flags;
                if (data_flags & osd::cell_interior) {
                    c.world = 0;
                }
                c.water = lookup(m_water, r.form(k_xcwt));
                // XCLW like the world import: a huge value means "the worldspace's default", +-1e9 and beyond
                // means no water at all.
                if (auto s = r.find(k_xclw); s && s->data.size() >= 4) {
                    const float h = s->as<float>();
                    if (!std::isfinite(h) || h >= 3.0e38f) {
                        // default water height
                    } else if (std::fabs(h) >= 1.0e9f) {
                        c.flags &= ~osd::cell_has_water;
                    } else {
                        c.water_height = units::to_metres(h);
                        c.has_water_height = 1;
                    }
                }
                if (auto s = r.find(k_xclc); s && s->data.size() >= 8) {
                    auto br = s->reader();
                    c.x = br.read<int32_t>();
                    c.y = br.read<int32_t>();
                }
                c.acoustic_space = lookup(m_aspc, r.form(k_xcas));
                c.music = lookup(m_music, r.form(k_xcmo));
                // The game: the encounter zone's location, else XLCN.
                c.location = r.form(k_xlcn);
                if (auto zone = r.form(k_xezn)) {
                    if (auto z = m_db.get(zone); z && z->type() == k_eczn) {
                        if (auto d = z->find(k_data); d && d->data.size() >= 8) {
                            if (auto loc = z->resolve(BinaryReader(d->data.subspan(4)).read<uint32_t>())) {
                                c.location = loc;
                            }
                        }
                    }
                }
                Pending p{c, {}, {}};
                if (auto s = r.find(k_xclr)) {
                    auto br = s->reader();
                    for (size_t i = 0; i < s->data.size() / 4; ++i) {
                        if (auto g = lookup(m_region, r.resolve(br.read<uint32_t>())); g != k_none) {
                            p.regions.push_back(g);
                        }
                    }
                }
                std::vector<osd::Emitter> emitters;
                for (auto child : m_db.children(e->form_id)) {
                    if (child->type != k_refr) {
                        continue;
                    }
                    if (auto em = emitter_for(m_db.read(*child))) {
                        emitters.push_back(*em);
                    }
                }
                if (persistent_world_cell && !(c.flags & osd::cell_interior)) {
                    world_persistent.emplace_back(c.world, std::move(emitters));
                    continue;
                }
                p.emitters = std::move(emitters);
                if (!(c.flags & osd::cell_interior)) {
                    by_grid[{c.world, c.x, c.y}] = cells.size();
                }
                cells.push_back(std::move(p));
            } catch (const std::exception& ex) {
                log::warn("audio: CELL {:08X}: {}", e->form_id, ex.what());
            }
        }
        // References in a world's persistent cell belong to the exterior cell under them.
        size_t orphaned = 0;
        for (auto& [world, emitters] : world_persistent) {
            for (auto& em : emitters) {
                const auto x =
                    static_cast<int32_t>(std::floor(units::to_skyrim_units(em.position[0]) / 4096.0f));
                const auto y =
                    static_cast<int32_t>(std::floor(units::to_skyrim_units(em.position[1]) / 4096.0f));
                auto it = by_grid.find({world, x, y});
                if (it == by_grid.end()) {
                    ++orphaned;
                    continue;
                }
                cells[it->second].emitters.push_back(em);
            }
        }
        if (orphaned) {
            log::info("audio: {} persistent emitters outside any exterior cell", orphaned);
        }
        for (auto& p : cells) {
            p.cell.first_region = static_cast<uint32_t>(m_out.db.cell_regions.size());
            m_out.db.cell_regions.insert(m_out.db.cell_regions.end(), p.regions.begin(), p.regions.end());
            p.cell.region_count = static_cast<uint32_t>(p.regions.size());
            p.cell.first_emitter = static_cast<uint32_t>(m_out.db.emitters.size());
            m_out.db.emitters.insert(m_out.db.emitters.end(), p.emitters.begin(), p.emitters.end());
            p.cell.emitter_count = static_cast<uint32_t>(p.emitters.size());
            m_out.db.cells.push_back(p.cell);
        }
        m_out.counts["CELL"] = m_out.db.cells.size();
        m_out.counts["emitters"] = m_out.db.emitters.size();
    }

    void waters() {
        each(k_watr, [&](const Record& r) {
            osd::Water w{r.form_id(), str(r.edid()), lookup(m_descriptor, r.form(k_snam)), 0};
            if (auto s = r.find(k_fnam); s && !s->data.empty()) {
                w.flags = s->as<uint8_t>();
            }
            m_water.emplace(w.form_id, static_cast<uint32_t>(m_out.db.waters.size()));
            m_out.db.waters.push_back(w);
        });
        m_out.counts["WATR"] = m_out.db.waters.size();
    }

    void default_objects() {
        // DOBJ DNAM: {signature, FormID} pairs; later plugins override per signature.
        // Every plugin's version applies in load order, entry by entry (the game keeps the
        // earlier entries a later version doesn't list; a mod's DOBJ usually carries only a few).
        std::map<std::string, uint32_t> objects;
        for (auto winner : m_db.of_type(k_dobj))
            for (auto e : m_db.versions(winner->form_id)) {
                if (e->deleted()) {
                    continue;
                }
                auto r = m_db.read(*e);
                if (auto s = r.find(k_dnam)) {
                    auto br = s->reader();
                    for (size_t i = 0; i + 8 <= s->data.size(); i += 8) {
                        auto sig = br.read_string(4);
                        const auto form = br.read<uint32_t>();
                        if (sig.empty()) {
                            continue; // unused slots of later plugins' versions
                        }
                        sig.resize(4, ' ');
                        objects[sig] = form ? r.resolve(form) : 0;
                    }
                }
            }
        for (const auto& [sig, form] : objects) {
            osd::DefaultObject d{};
            std::memcpy(d.signature, sig.data(), 4);
            d.form_id = form;
            m_out.db.default_objects.push_back(d);
        }
        m_out.counts["DOBJ"] = m_out.db.default_objects.size();
    }

    // Line sound markers (XPRM type 4): the half-extent vector b rotated by the reference's rotation matrix M
    // (built like the game,: Rx(-x) Ry(-y) Rz(-z)) as the row vector b M; the
    // sound starts at pos + (vx, -vy, vz) and slides toward pos - (vx, -vy, vz)
    static void line_endpoints(const float pos[3], const float rot[3], osd::Emitter& em) {
        const float cx = std::cos(rot[0]), sx = std::sin(rot[0]);
        const float cy = std::cos(rot[1]), sy = std::sin(rot[1]);
        const float cz = std::cos(rot[2]), sz = std::sin(rot[2]);
        const float mx[3][3] = {{1, 0, 0}, {0, cx, sx}, {0, -sx, cx}};
        const float my[3][3] = {{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}};
        const float mz[3][3] = {{cz, sz, 0}, {-sz, cz, 0}, {0, 0, 1}};
        float yz[3][3] = {}, m[3][3] = {};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                for (int k = 0; k < 3; ++k) {
                    yz[i][j] += my[i][k] * mz[k][j];
                }
            }
        }
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                for (int k = 0; k < 3; ++k) {
                    m[i][j] += mx[i][k] * yz[k][j];
                }
            }
        }
        float b[3];
        for (int k = 0; k < 3; ++k) {
            b[k] = units::to_skyrim_units(em.primitive_bounds[k]);
        }
        float v[3] = {};
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) {
                v[j] += b[k] * m[k][j];
            }
        }
        const float w[3] = {v[0], -v[1], v[2]};
        for (int i = 0; i < 3; ++i) {
            em.line_start[i] = units::to_metres(pos[i] + w[i]);
            em.line_end[i] = units::to_metres(pos[i] - w[i]);
        }
    }

    void settings(const std::filesystem::path& game_dir) {
        std::unordered_map<std::string, float> values;
        for (const auto& d : k_ini_defaults) {
            values[path::to_lower(d.name)] = d.value;
        }
        // Skyrim_Default.ini: "[Section]" then "name=value"; settings are named "name:Section".
        std::ifstream ini(game_dir / "Skyrim_Default.ini");
        std::string line, section;
        size_t overridden = 0;
        while (std::getline(ini, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (line.starts_with("[") && line.ends_with("]")) {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const auto key = path::to_lower(line.substr(0, eq) + ":" + section);
            if (auto it = values.find(key); it != values.end()) {
                try {
                    it->second = std::stof(line.substr(eq + 1));
                    ++overridden;
                } catch (const std::exception&) {
                }
            }
        }
        for (const auto& d : k_ini_defaults) {
            m_out.db.settings.push_back({str(d.name), values[path::to_lower(d.name)]});
        }
        log::info("audio: {} INI settings ({} from Skyrim_Default.ini)", m_out.db.settings.size(),
                  overridden);
    }

    const EsmDatabase& m_db;
    const legacy::DataSource& m_data;
    AudioRecordsImport m_out;
    container::StringTableBuilder m_strings;
    std::set<std::string> m_files;
    std::unordered_map<uint32_t, uint32_t> m_category, m_output, m_descriptor, m_marker, m_reverb, m_aspc,
        m_track, m_music, m_material, m_impact, m_impact_set, m_footstep, m_region, m_water;
    std::unordered_map<uint32_t, bool> m_enabled;
};

bool is_wave_pcm16(std::span<const std::byte> file, PcmClip* out) {
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 ||
        std::memcmp(file.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    BinaryReader r(file);
    r.skip(12);
    uint16_t tag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    std::span<const std::byte> data;
    while (r.tell() + 8 <= r.size()) {
        const auto id = r.read_string(4);
        const auto size = r.read<uint32_t>();
        const auto body = r.take(std::min<size_t>(size, r.size() - r.tell()));
        if (id == "fmt " && body.size() >= 16) {
            BinaryReader f(body);
            tag = f.read<uint16_t>();
            channels = f.read<uint16_t>();
            rate = f.read<uint32_t>();
            f.skip(6);
            bits = f.read<uint16_t>();
        } else if (id == "data") {
            data = body;
        }
        if ((size & 1) && r.tell() < r.size()) {
            r.skip(1);
        }
    }
    if (tag != 1 || bits != 16 || channels == 0) {
        return false;
    }
    if (out) {
        out->channels = channels;
        out->sample_rate = rate;
        out->samples.resize(data.size() / 2);
        std::memcpy(out->samples.data(), data.data(), out->samples.size() * 2);
    }
    return true;
}

} // namespace

AudioRecordsImport import_audio_records(const EsmDatabase& db, const legacy::DataSource& data,
                                        const std::filesystem::path& game_dir) {
    return Builder(db, data).run(game_dir);
}

std::string audio_cache_path(std::string_view data_path) {
    if (data_path.ends_with(".xwm") || data_path.ends_with(".fuz")) {
        return path::replace_extension(data_path, ".ogg");
    }
    return std::string(data_path);
}

AudioFilesSummary convert_audio_files(const legacy::DataSource& data, const std::filesystem::path& cache,
                                      unsigned threads, bool force) {
    std::vector<std::string> sources;
    for (const char* root : {"sound/", "music/"}) {
        for (const char* ext : {".wav", ".xwm", ".fuz", ".lip"}) {
            auto list = data.list(root, ext);
            sources.insert(sources.end(), list.begin(), list.end());
        }
    }
    std::sort(sources.begin(), sources.end());
    log::info("audio: {} sound and music files", sources.size());

    std::atomic<size_t> next{0}, converted{0}, copied{0}, skipped{0}, failed{0}, lips{0}, done{0};
    std::atomic<uint64_t> source_bytes{0}, output_bytes{0};
    std::mutex log_mutex;
    std::map<std::string, size_t> wav_formats;
    auto work = [&] {
        for (size_t i; (i = next++) < sources.size();) {
            const auto& src = sources[i];
            const auto out = cache / audio_cache_path(src);
            // Voice lines carry lip-sync data: the .fuz's lip block (or a loose .lip) becomes a .osv track.
            const bool has_lip = src.ends_with(".fuz") || src.ends_with(".lip");
            const auto lip_out = cache / path::replace_extension(src, ".osv");
            const bool want_audio = !src.ends_with(".lip") && (force || !std::filesystem::exists(out));
            const bool want_lip = has_lip && (force || !std::filesystem::exists(lip_out));
            try {
                if (!want_audio && !want_lip) {
                    ++skipped;
                } else if (auto bytes = data.read(src)) {
                    source_bytes += bytes->size();
                    std::span<const std::byte> audio = *bytes;
                    std::span<const std::byte> lip;
                    if (src.ends_with(".fuz")) {
                        auto parts = split_fuz(*bytes);
                        audio = parts.audio;
                        lip = parts.lip;
                    } else if (src.ends_with(".lip")) {
                        lip = *bytes;
                    }
                    if (want_lip && !lip.empty()) {
                        auto osv_bytes = osv::write(decode_lip(lip));
                        write_file(lip_out, osv_bytes);
                        output_bytes += osv_bytes.size();
                        ++lips;
                    }
                    if (!want_audio) {
                        // lip only
                    } else if (src.ends_with(".wav")) {
                        // Generic format: kept as it is. Note any that the runtime can't stream directly.
                        if (!is_wave_pcm16(audio, nullptr)) {
                            std::lock_guard lock(log_mutex);
                            ++wav_formats[src.substr(0, src.find('/', 6))];
                        }
                        write_file(out, audio);
                        output_bytes += audio.size();
                        ++copied;
                    } else {
                        PcmClip clip;
                        uint32_t source_bps = 0;
                        if (is_xwma(audio)) {
                            clip = decode_xwma(audio);
                            source_bps = BinaryReader(audio.subspan(28, 4)).read<uint32_t>();
                        } else if (is_wave_pcm16(audio, &clip)) {
                            source_bps = clip.sample_rate * clip.channels * 2;
                        } else {
                            throw Error("unknown audio payload");
                        }
                        const float quality = std::max(0.4f, vorbis_quality_for(clip.channels, source_bps));
                        auto ogg = encode_vorbis(clip, quality);
                        write_file(out, ogg);
                        output_bytes += ogg.size();
                        ++converted;
                    }
                } else {
                    ++failed;
                }
            } catch (const std::exception& e) {
                std::lock_guard lock(log_mutex);
                log::warn("audio: {}: {}", src, e.what());
                ++failed;
            }
            if (auto n = ++done; n % 5000 == 0) {
                log::info("  audio files: {}/{}", n, sources.size());
            }
        }
    };
    {
        std::vector<std::jthread> pool;
        for (unsigned t = 0; t < std::max(1u, threads); ++t) {
            pool.emplace_back(work);
        }
    }
    for (const auto& [where, n] : wav_formats) {
        log::warn("audio: {} WAV files under {} are not 16-bit PCM", n, where);
    }
    AudioFilesSummary s;
    s.converted = converted;
    s.copied = copied;
    s.skipped = skipped;
    s.failed = failed;
    s.lips = lips;
    s.source_bytes = source_bytes;
    s.output_bytes = output_bytes;
    return s;
}

} // namespace spud::importer
