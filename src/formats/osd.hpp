#pragma once

// .osd — spud sound database. Spec: docs/formats/osd.md
//
// Every audio record of the load order (as of game start), compiled for the runtime: categories, output
// models, sound descriptors, reverbs, acoustic spaces, music, footsteps, impacts, region and weather sounds,
// the audio-relevant data of cells, worldspaces and locations, and every placed reference that emits sound.
// Records keep their load-order FormIDs; cross-references between records are indices into the arrays below
// (k_none when absent). Distances are metres, positions are world space (Z up).

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace spud::osd {

inline constexpr container::FourCC k_kind = container::fourcc("SNDB");
inline constexpr uint32_t k_version = 1;
inline constexpr uint32_t k_none = 0xFFFFFFFFu;

// --- sound categories (SNCT) ---------------------------------------------------------------------

enum CategoryFlag : uint32_t {
    category_mute_when_submerged = 1u << 0,
    category_on_menu = 1u << 1, // "Should Appear on Menu": has an Audio menu slider
};

struct Category {
    uint32_t form_id;
    uint32_t edid;            // string
    uint32_t parent;          // index, or k_none
    uint32_t flags;           // CategoryFlag
    float static_volume;      // VNAM / 65535
    float default_menu_value; // UNAM / 65535 (1.0 when absent)
};
static_assert(sizeof(Category) == 24);

// --- sound output models (SOPM) ------------------------------------------------------------------

enum OutputFlag : uint32_t {
    output_attenuates_with_distance = 1u << 0,
    output_allows_rumble = 1u << 1,
};

enum class OutputType : uint32_t { uses_hrtf = 0, defined_speaker_output = 1 };

struct OutputModel {
    uint32_t form_id;
    uint32_t edid;
    uint32_t flags; // OutputFlag
    OutputType type;
    float reverb_send;       // 0..1
    float min_distance;      // metres
    float max_distance;      // metres
    uint8_t curve[5];        // attenuation curve, percent gain at min + {0, 1/4, 1/2, 3/4, 1} * (max - min)
    uint8_t has_attenuation; // ANAM present
    uint8_t pad[2];
    uint8_t speakers[3][8]; // ONAM percent levels: rows mono, stereo left, stereo right;
                            // columns FL FR C LFE RL RR BL BR
};
static_assert(sizeof(OutputModel) == 60);

// --- conditions (CTDA), kept as data for the condition evaluator ---------------------------------

struct Condition {
    uint8_t op; // CTDA byte 0: comparison in the top 3 bits, flags (OR, use global, ...) below
    uint8_t pad[3];
    uint32_t comparison; // float bits, or a resolved GLOB FormID when the "use global" flag is set
    uint16_t function;   // condition function index
    uint16_t pad2;
    uint32_t param1; // resolved FormID when the parameter is a form, raw otherwise
    uint32_t param2;
    uint32_t run_on;
    uint32_t reference; // resolved FormID
    int32_t param3;
};
static_assert(sizeof(Condition) == 32);

// --- sound descriptors (SNDR) and markers (SOUN) -------------------------------------------------

enum DescriptorFlag : uint32_t {
    // The game's flag word: LNAM, or legacy FNAM converted.
    descriptor_loop = 0x0800,
    descriptor_envelope_fast = 0x1000,
    descriptor_envelope_slow = 0x2000,
};

struct Descriptor {
    uint32_t form_id;
    uint32_t edid;
    uint32_t category;      // index, or k_none
    uint32_t output_model;  // index, or k_none
    uint32_t alternate_for; // index of the descriptor this one replaces (SNAM), or k_none
    uint32_t first_file;    // files[first_file, + file_count): cache paths (string offsets)
    uint32_t file_count;
    uint32_t flags;             // DescriptorFlag plus the raw LNAM bytes (rumble in the top byte)
    int8_t frequency_shift;     // percent
    uint8_t frequency_variance; // percent
    uint8_t priority;
    uint8_t db_variance;         // dB
    uint16_t static_attenuation; // hundredths of a dB
    uint16_t reserved;
    uint32_t first_condition; // conditions[first_condition, + condition_count)
    uint32_t condition_count;
};
static_assert(sizeof(Descriptor) == 48);

struct SoundMarker {
    uint32_t form_id;
    uint32_t edid;
    uint32_t descriptor; // index, or k_none
};
static_assert(sizeof(SoundMarker) == 12);

// --- reverb (REVB) and acoustic spaces (ASPC) ----------------------------------------------------

struct Reverb { // REVB DATA as stored; units are converted by the engine
    uint32_t form_id;
    uint32_t edid;
    uint16_t decay_time_ms;
    uint16_t hf_reference_hz;
    int8_t room_filter; // x100 = mB
    int8_t room_hf_filter;
    int8_t reflections;
    int8_t reverb_amp;
    uint8_t decay_hf_ratio; // /100
    uint8_t reflect_delay;  // x1.2 = ms
    uint8_t reverb_delay;   // ms
    uint8_t diffusion;      // percent
    uint8_t density;        // percent
    uint8_t pad[3];
};
static_assert(sizeof(Reverb) == 24);

struct AcousticSpace {
    uint32_t form_id;
    uint32_t edid;
    uint32_t ambient;    // descriptor index, or k_none
    uint32_t region;     // REGN FormID whose sounds play here (interiors), or 0
    uint32_t reverb;     // index, or k_none
    float bounds_min[3]; // OBND, metres
    float bounds_max[3];
};
static_assert(sizeof(AcousticSpace) == 44);

// --- music (MUSC, MUST) --------------------------------------------------------------------------

enum MusicTypeFlag : uint32_t {
    music_plays_one_selection = 0x01,
    music_abrupt_transition = 0x02,
    music_cycle_tracks = 0x04,
    music_maintain_track_order = 0x08,
    music_ducks_current_track = 0x20,
    music_doesnt_queue = 0x40,
};

struct MusicType {
    uint32_t form_id;
    uint32_t edid;
    uint32_t flags; // MusicTypeFlag
    uint16_t priority;
    uint16_t ducking;     // hundredths of a dB
    float fade_duration;  // seconds (WNAM)
    uint32_t first_track; // track_lists[first_track, + track_count): MusicTrack indices (k_none for NULL)
    uint32_t track_count;
};
static_assert(sizeof(MusicType) == 28);

enum class TrackType : uint32_t { palette = 0, single = 1, silent = 2 };

struct MusicTrack {
    uint32_t form_id;
    uint32_t edid;
    TrackType type;
    float duration;   // FLTV, seconds
    float fade_out;   // DNAM, seconds
    uint32_t file;    // string: cache path, or k_no_string
    uint32_t finale;  // string: cache path, or k_no_string
    float loop_begin; // LNAM, seconds
    float loop_end;
    uint32_t loop_count;
    uint32_t has_loop;
    uint32_t first_cue; // cue_points[first_cue, + cue_count), seconds
    uint32_t cue_count;
    uint32_t first_condition;
    uint32_t condition_count;
    uint32_t first_sub; // palette sub-tracks: track_lists[first_sub, + sub_count)
    uint32_t sub_count;
};
static_assert(sizeof(MusicTrack) == 68);

// --- footsteps and impacts (FSTP, FSTS, IPCT, IPDS, MATT) ----------------------------------------

struct Footstep {
    uint32_t form_id;
    uint32_t edid;
    uint32_t impact_set; // index, or k_none
    uint32_t tag;        // string ("FootLeft", ...)
};
static_assert(sizeof(Footstep) == 16);

enum FootstepGait : uint32_t { gait_walk = 0, gait_run, gait_sprint, gait_sneak, gait_swim, gait_count };

struct FootstepSet {
    uint32_t form_id;
    uint32_t edid;
    uint32_t first[gait_count]; // footstep_lists[first[g], + count[g]): Footstep indices
    uint32_t count[gait_count];
};
static_assert(sizeof(FootstepSet) == 48);

struct Impact {
    uint32_t form_id;
    uint32_t edid;
    float effect_duration;
    uint32_t orientation;
    float angle_threshold;
    float placement_radius; // metres
    uint32_t sound_level;   // 0 loud, 1 normal, 2 silent, 3 very loud
    uint8_t flags;
    uint8_t result;
    uint16_t pad;
    uint32_t sound1; // descriptor index, or k_none
    uint32_t sound2;
};
static_assert(sizeof(Impact) == 40);

struct ImpactSet {
    uint32_t form_id;
    uint32_t edid;
    uint32_t first; // impact_pairs[first, + count)
    uint32_t count;
};
static_assert(sizeof(ImpactSet) == 16);

struct ImpactPair {
    uint32_t material; // MATT FormID
    uint32_t impact;   // index, or k_none
};
static_assert(sizeof(ImpactPair) == 8);

struct Material {
    uint32_t form_id;
    uint32_t edid;
    uint32_t parent; // index, or k_none
    uint32_t flags;
    uint32_t havok_impact_set; // index, or k_none
    uint32_t name;             // MNAM string
};
static_assert(sizeof(Material) == 24);

// --- regions and weather -------------------------------------------------------------------------

enum RegionSoundFlag : uint32_t {
    region_sound_pleasant = 1u << 0,
    region_sound_cloudy = 1u << 1,
    region_sound_rainy = 1u << 2,
    region_sound_snowy = 1u << 3,
};

struct RegionSound {
    uint32_t descriptor; // index, or k_none
    uint32_t flags;      // RegionSoundFlag
    float chance;
};
static_assert(sizeof(RegionSound) == 12);

struct Region {
    uint32_t form_id;
    uint32_t edid;
    uint32_t world;         // WRLD FormID
    uint32_t music;         // MusicType index (RDMO), or k_none
    uint8_t sound_priority; // RDAT of the sound entry
    uint8_t sound_override;
    uint8_t has_sounds;
    uint8_t deleted;
    uint32_t first_sound; // region_sounds[first_sound, + sound_count)
    uint32_t sound_count;
    uint32_t first_area; // region_areas[first_area, + area_count), in record order
    uint32_t area_count;
};
static_assert(sizeof(Region) == 36);

struct RegionArea {       // RPLI + RPLD: a polygon in the region's worldspace
    uint32_t first_point; // region_points[first_point, + point_count)
    uint32_t point_count;
};
static_assert(sizeof(RegionArea) == 8);

enum class WeatherSoundType : uint32_t { normal = 0, precipitation = 1, wind = 2, thunder = 3 };

struct WeatherSound {
    uint32_t descriptor; // index, or k_none
    WeatherSoundType type;
};
static_assert(sizeof(WeatherSound) == 8);

struct Weather {
    uint32_t form_id;
    uint32_t edid;
    uint8_t data[20];     // WTHR DATA as stored (wind, fades, thunder, classification flags, ...)
    uint32_t first_sound; // weather_sounds[first_sound, + sound_count)
    uint32_t sound_count;
};
static_assert(sizeof(Weather) == 36);

// --- where things are: worldspaces, locations, cells and placed emitters --------------------------

struct World {
    uint32_t form_id;
    uint32_t edid;
    uint32_t parent;   // WRLD FormID, or 0
    uint32_t music;    // MusicType index (ZNAM), or k_none
    uint32_t location; // LCTN FormID, or 0
    uint32_t water;    // Water index (NAM2), or k_none
};
static_assert(sizeof(World) == 24);

struct Water { // WATR: its "open sound" is the water ambience
    uint32_t form_id;
    uint32_t edid;
    uint32_t sound; // descriptor index (SNAM), or k_none
    uint32_t flags; // FNAM
};
static_assert(sizeof(Water) == 16);

struct Location {
    uint32_t form_id;
    uint32_t edid;
    uint32_t parent; // LCTN FormID, or 0
    uint32_t music;  // MusicType index (NAM1), or k_none
};
static_assert(sizeof(Location) == 16);

enum CellFlag : uint32_t { // CELL DATA flags as stored
    cell_interior = 0x0001,
    cell_has_water = 0x0002,
    cell_show_sky = 0x0080,
};

struct Cell {
    uint32_t form_id;
    uint32_t edid;
    uint32_t world;          // WRLD FormID, 0 for interiors
    int32_t x, y;            // exterior grid
    uint32_t flags;          // CellFlag
    uint32_t acoustic_space; // index (XCAS), or k_none
    uint32_t music;          // MusicType index (XCMO), or k_none
    uint32_t location;       // LCTN FormID (XLCN), or 0
    uint32_t first_region;   // cell_regions[first_region, + region_count): Region indices (XCLR)
    uint32_t region_count;
    uint32_t first_emitter; // emitters[first_emitter, + emitter_count)
    uint32_t emitter_count;
    uint32_t water;     // Water index (XCWT), or k_none (the worldspace's)
    float water_height; // XCLW, metres
    uint32_t has_water_height;
};
static_assert(sizeof(Cell) == 64);

enum class EmitterKind : uint32_t {
    sound_marker = 0,   // SOUN reference
    acoustic_space = 1, // ASPC reference (a volume)
    looping_object = 2, // ACTI / TACT / MSTT looping sound
    light = 3,          // LIGH sound
    door_loop = 4,      // DOOR loop sound
};

enum EmitterFlag : uint32_t {
    emitter_initially_disabled = 1u << 0,
};

struct Emitter {
    uint32_t ref_form_id;
    uint32_t base_form_id;
    EmitterKind kind;
    uint32_t flags;          // EmitterFlag
    uint32_t descriptor;     // index, or k_none
    uint32_t acoustic_space; // index (acoustic_space kind), or k_none
    float position[3];       // metres
    float rotation[4];       // quaternion xyzw (same convention as osw::Ref)
    float scale;
    float primitive_bounds[3]; // XPRM bounds (metres, as stored): half extents of acoustic space boxes
    uint32_t primitive_type;   // XPRM type: 0 none, 1 box, 2 sphere, 3 portal box, 4 line
    float line_start[3];       // line sound markers: where the sound starts and the segment it slides along
    float line_end[3];
};
static_assert(sizeof(Emitter) == 96);

enum PrimitiveType : uint32_t {
    primitive_none = 0,
    primitive_box = 1,
    primitive_sphere = 2,
    primitive_line = 4
};

struct DefaultObject { // DOBJ entries the audio rules use ("RVBT", "UWLS", "MDSC", "BTMS", ...)
    char signature[4];
    uint32_t form_id;
};
static_assert(sizeof(DefaultObject) == 8);

// --- INI and game settings that drive the audio rules ---------------------------------------------

struct Setting {
    uint32_t name; // string, "fReverbTransitionTime:Audio"
    float value;   // effective value (exe default overridden by Skyrim_Default.ini)
};
static_assert(sizeof(Setting) == 8);

struct SoundDb {
    std::vector<Category> categories;
    std::vector<OutputModel> output_models;
    std::vector<Condition> conditions;
    std::vector<Descriptor> descriptors;
    std::vector<uint32_t> files; // string offsets
    std::vector<SoundMarker> markers;
    std::vector<Reverb> reverbs;
    std::vector<AcousticSpace> acoustic_spaces;
    std::vector<MusicType> music_types;
    std::vector<MusicTrack> music_tracks;
    std::vector<uint32_t> track_lists;
    std::vector<float> cue_points;
    std::vector<Footstep> footsteps;
    std::vector<FootstepSet> footstep_sets;
    std::vector<uint32_t> footstep_lists;
    std::vector<Impact> impacts;
    std::vector<ImpactSet> impact_sets;
    std::vector<ImpactPair> impact_pairs;
    std::vector<Material> materials;
    std::vector<Region> regions;
    std::vector<RegionSound> region_sounds;
    std::vector<RegionArea> region_areas;
    std::vector<float> region_points; // x, y pairs, metres
    std::vector<Water> waters;
    std::vector<DefaultObject> default_objects;
    std::vector<Weather> weathers;
    std::vector<WeatherSound> weather_sounds;
    std::vector<World> worlds;
    std::vector<Location> locations;
    std::vector<Cell> cells;
    std::vector<uint32_t> cell_regions;
    std::vector<Emitter> emitters;
    std::vector<Setting> settings;
    std::vector<char> strings;

    std::string_view str(uint32_t offset) const;
};

std::vector<std::byte> write(const SoundDb& db);
SoundDb read(std::span<const std::byte> file);

} // namespace spud::osd
