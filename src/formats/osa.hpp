#pragma once

// .osa — spud atmosphere: the data the sky, the weather and exterior lighting are computed from (climates,
// weathers, image spaces and their modifiers, volumetric lighting, precipitation, the sky's game settings).
// Spec: docs/formats/osa.md. How the engine uses it.

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::osa {

inline constexpr container::FourCC k_kind = container::fourcc("ATMO");
inline constexpr uint32_t k_version = 4;

inline constexpr int k_colour_types = 17; // weather colour types (NAM0), see Weather::colours
inline constexpr int k_times = 4;         // 0 sunrise, 1 day, 2 sunset, 3 night
inline constexpr int k_cloud_layers = 32;
inline constexpr uint32_t k_no_form = 0;

// Weather colour types (WTHR NAM0 order).
enum ColourType : uint32_t {
    colour_sky_upper = 0,
    colour_fog_near = 1,
    colour_ambient = 3,
    colour_sunlight = 4,
    colour_sun = 5,
    colour_stars = 6,
    colour_sky_lower = 7,
    colour_horizon = 8,
    colour_effect_lighting = 9,
    colour_cloud_lod_diffuse = 10,
    colour_cloud_lod_ambient = 11,
    colour_fog_far = 12,
    colour_sky_statics = 13,
    colour_water_multiplier = 14,
    colour_sun_glare = 15,
    colour_moon_glare = 16,
};

// WTHR DATA bytes (19; the old 17-byte form ends before the wind direction).
enum WeatherData : uint32_t {
    data_wind_speed = 0,
    data_trans_delta = 3,
    data_sun_glare = 4,
    data_sun_damage = 5,
    data_precip_fade_in = 6,
    data_precip_fade_out = 7,
    data_lightning_fade_in = 8,
    data_lightning_fade_out = 9,
    data_lightning_frequency = 10,
    data_flags = 11,
    data_lightning_color = 12, // 3 bytes
    data_effect_begin = 15,
    data_effect_end = 16,
    data_wind_direction = 17,
    data_wind_direction_range = 18,
};
enum WeatherFlag : uint8_t {
    weather_pleasant = 0x01,
    weather_cloudy = 0x02,
    weather_rainy = 0x04,
    weather_snow = 0x08,
    weather_aurora_permanent = 0x10, // sky model always visible
    weather_aurora_follows_sun = 0x20,
};

// One time of day's directional ambient (WTHR DALC): six colours +X -X +Y -Y +Z -Z as stored, specular, fresnel.
struct DirectionalAmbient {
    uint8_t colours[6][4]; // rgba8, the record's order
    uint8_t specular[4];
    float fresnel_power;
};
static_assert(sizeof(DirectionalAmbient) == 32);

// One cloud layer of a weather: texture `chr(0x30 + i) + "0TX"`, speeds (QNAM x, RNAM y; 127 = still), colour
// per time of day (PNAM, rgb8) and alpha per time (JNAM).
struct CloudLayer {
    uint32_t texture; // string offset ("textures/sky/....dds"), k_no_string if none
    uint8_t speed_x;
    uint8_t speed_y;
    uint8_t reserved[2];
    uint8_t colours[k_times][4];
    float alphas[k_times];
};
static_assert(sizeof(CloudLayer) == 40);

struct Weather {
    uint32_t form_id;
    uint32_t edid;                               // string offset
    uint8_t colours[k_colour_types][k_times][4]; // NAM0, rgba8, load-time fix-ups applied
    float fog[8];                                // FNAM: day near, day far, night near, night far, day power,
                                                 // night power, day max, night max (units)
    DirectionalAmbient ambient[k_times];         // DALC
    uint32_t image_spaces[k_times];              // IMSP, DefaultImageSpace (0x167) where missing
    uint32_t volumetric[k_times];                // HNAM: VOLI FormIDs, 0 = the INI defaults
    uint8_t data[20];                            // DATA (WeatherData), 19 bytes + padding
    uint32_t precipitation;                      // MNAM: SPGD FormID, 0 = none
    uint32_t sky_model;                          // MODL (aurora / sky effect), string offset or k_no_string
    uint32_t cloud_layer_count;                  // LNAM (layers at or past it use layer 0's colours/speeds)
    uint32_t clouds_disabled;                    // NAM1, with every layer without a texture set too
    uint32_t thunder_count;                      // SNAM entries of type 3 (thunder): lightning tries per entry
    uint32_t sky_statics_first;                  // TNAM STAT FormIDs in Atmosphere::sky_statics
    uint32_t sky_statics_count;
    CloudLayer clouds[k_cloud_layers];
};
static_assert(sizeof(Weather) == 1800);

struct ClimateWeather {
    uint32_t weather; // FormID
    int32_t chance;   // the global's value when WLST names one (resolved at import), else the record's
};

struct Climate {
    uint32_t form_id;
    uint32_t edid;          // string offset
    uint8_t timing[6];      // TNAM: sunrise begin/end, sunset begin/end (10-minute units), volatility, moons
    uint8_t reserved[2];
    uint32_t first_weather; // into the weather-list chunk
    uint32_t weather_count;
    uint32_t sun_texture;   // FNAM, string offset (default textures/sky/sun.dds)
    uint32_t glare_texture; // GNAM (default textures/sky/sunglare.dds)
    uint32_t stars_model;   // MODL (default meshes/sky/stars.nif)
};
static_assert(sizeof(Climate) == 36);

// IMGS: the image-space data the record fills, as floats.
struct ImageSpace {
    uint32_t form_id;
    float hdr[9];       // HNAM: eye adapt speed, bloom blur radius, bloom threshold, bloom scale, receive bloom
                        // threshold, white, sunlight scale, sky scale, eye adapt strength
    float cinematic[3]; // CNAM: saturation, brightness, contrast
    float tint[4];      // TNAM: amount, r, g, b
    float dof[4];       // DNAM: strength, distance, range, flags (a float the engine truncates)
};
static_assert(sizeof(ImageSpace) == 84);

// VOLI, CNAM..NNAM in order.
struct Volumetric {
    uint32_t form_id;
    float intensity, custom_color_contribution, red, green, blue, density_contribution, density_size,
        density_wind_speed, density_falling_speed, phase_contribution, phase_scattering, sampling_range;
};
static_assert(sizeof(Volumetric) == 52);

// SPGD DATA and ICON.
struct Precipitation {
    uint32_t form_id;
    float gravity, rotation_velocity, size_x, size_y, offset_min, offset_max, rotation_range;
    uint32_t subtextures_x, subtextures_y;
    uint32_t type;    // 0 rain, 1 snow
    float box_size;   // a u32 the engine reads as a float
    float density;
    uint32_t texture; // string offset
};
static_assert(sizeof(Precipitation) == 56);

// IMAD: an image-space modifier. Every animated value is a curve of (time, value) keys in Atmosphere::keys; colour
// curves have five floats per key (time, r, g, b, a).
enum ModifierCurve : uint32_t {
    // 0..8: the HDR fields in HNAM order, 0x11..0x13 the cinematic ones (the xIAD subrecord byte & 0x3F); each has
    // a multiply and an add curve.
    curve_hdr_first = 0,
    curve_saturation = 0x11,
    curve_brightness = 0x12,
    curve_contrast = 0x13,
    k_curve_fields = 0x14,
    // Single curves.
    curve_blur_radius = 0,
    curve_double_vision,
    curve_radial_strength,
    curve_radial_ramp_up,
    curve_radial_start,
    curve_radial_ramp_down,
    curve_radial_down_start,
    curve_dof_strength,
    curve_dof_distance,
    curve_dof_range,
    curve_motion_blur,
    k_single_curves,
};
struct Curve {
    uint32_t first; // into Atmosphere::keys (floats)
    uint32_t count; // keys
};
struct Modifier {
    uint32_t form_id;
    uint32_t edid;      // string offset
    uint32_t animatable;
    float duration;
    float radial_center[2];
    uint32_t radial_use_target;
    uint32_t dof_use_target;
    uint32_t dof_flags;
    Curve mult[k_curve_fields];
    Curve add[k_curve_fields];
    Curve single[k_single_curves];
    Curve tint;         // TNAM colour keys
    Curve fade;         // NAM3 colour keys
};
static_assert(sizeof(Modifier) == 36 + 8 * (2 * 0x14 + 11 + 2));

// The sky's meshes (meshes/sky/atmosphere.nif, clouds.nif, each climate's stars model), with every shape's node
// transform baked into its vertices (game units, camera-centred).
struct SkyVertex {
    float position[3];
    float uv[2];
    uint8_t color[4]; // rgba8 vertex colour (the atmosphere's blend weights)
};
static_assert(sizeof(SkyVertex) == 24);
enum SkyObjectType : uint32_t { // BSSkyShaderProperty sky object type
    sky_texture = 0,
    sky_sun_glare = 1,
    sky_sky = 2,
    sky_clouds = 3,
    sky_stars = 5,
    sky_moon_mask = 7,
};
struct SkyShape {
    uint32_t name;    // string offset
    uint32_t type;    // SkyObjectType
    uint32_t layer;   // clouds: the layer index (file order of the model's cloud shapes)
    uint32_t texture; // string offset, k_no_string if none
    uint32_t first_index;
    uint32_t index_count;
    uint32_t hidden;  // the shape or a parent node is app-culled
};
static_assert(sizeof(SkyShape) == 28);
struct SkyModel {
    uint32_t path; // string offset ("meshes/sky/clouds.nif")
    uint32_t first_shape;
    uint32_t shape_count;
};

// A region with a weather list (REGN RDAT type 3). Its weathers sit in Atmosphere::climate_weathers.
struct Region {
    uint32_t form_id;
    uint8_t override_others; // RDAT byte 4
    uint8_t priority;        // RDAT byte 5
    uint8_t pad[2];
    uint32_t first_weather;
    uint32_t weather_count;
    uint32_t first_area; // region_areas[first_area, + area_count)
    uint32_t area_count;
};
static_assert(sizeof(Region) == 24);

struct RegionArea {
    uint32_t first_point; // pairs in Atmosphere::region_points, Skyrim units
    uint32_t point_count;
};

// An exterior cell's XCLR, reduced to the regions with weather lists of its own worldspace, in record order.
struct RegionCell {
    uint32_t world; // index in Atmosphere::world_climates
    int32_t x, y;
    uint32_t first; // region_lists[first, + count): indices in regions
    uint32_t count;
};
static_assert(sizeof(RegionCell) == 20);

struct WorldClimate {
    uint32_t world_edid; // string offset, lowercase
    uint32_t climate;    // FormID, 0 = none
};

// Game settings the sky reads (GMST), with the engine's defaults where no plugin sets them.
struct SkySettings {
    float daytime_color_extension = 0.5f; // fDaytimeColorExtension
    float sun_x_extreme = 400.0f;         // fSunXExtreme
    float sun_y_extreme = 25.0f;          // fSunYExtreme
    float sun_z_extreme = -100.0f;        // fSunZExtreme
    float sun_dir_x_extreme = 400.0f;     // fSunDirXExtreme
    float sun_alpha_trans_time = 2.0f;    // fSunAlphaTransTime
    float cloud_speed_max = 0.1f;         // fWeatherCloudSpeedMax
    float trans_min = 0.01f;              // fWeatherTransMin
    float trans_max = 0.25f;              // fWeatherTransMax
    float trans_accel = 16.0f;            // fWeatherTransAccel
    float flash_duration = 0.25f;         // fWeatherFlashDuration
    float flash_directional = 1.0f;       // fWeatherFlashDirectional
    float flash_ambient = 0.3f;           // fWeatherFlashAmbient
    float precip_wind_mult = 500.0f;      // fPrecipWindMult
    float sun_minimum_glare_scale = 0.25f; // fSunMinimumGlareScale
    float sun_reduce_glare_speed = 4.0f;  // fSunReduceGlareSpeed
    // Moons: Masser, Secunda (fMasserAngleFadeStart, ...End, ...ShadowEarlyFade, fMasserSpeed, fMasserZOffset,
    // iMasserSize).
    float moon_fade_start[2] = {55.0f, 55.0f};
    float moon_fade_end[2] = {45.0f, 45.0f};
    float moon_shadow_early_fade[2] = {0.5f, 0.5f};
    float moon_speed[2] = {0.25f, 0.3f};
    float moon_z_offset[2] = {35.0f, 50.0f};
    float moon_size[2] = {94.0f, 40.0f};
    float stars_rotate_days = 4.0f;            // fStarsRotateDays
    float stars_rotate_axis[3] = {0, 0, 1.0f}; // fStarsRotateXAxis, Y, Z
    float aurora_fade_in_start = 19.5f;        // fAuroraFadeInStart
    float aurora_fade_in = 20.0f;              // fAutoraFadeIn (sic)
    float aurora_fade_out_start = 5.0f;        // fAuroraFadeOutStart
    float aurora_fade_out = 5.5f;              // fAutoraFadeOut (sic)
    float time_scale = 20.0f;                  // GLOB TimeScale (0x3A) as the plugins set it
    float reserved[3] = {};
};
static_assert(sizeof(SkySettings) == 160);

// WATR. `data` is DNAM read as floats, by its record order; the three colours are the rgba8
// words at data_shallow_color and on. Records written before the last fields leave them 0.
enum WaterData : uint32_t {
    water_sun_specular_power = 4,
    water_reflectivity = 5,
    water_fresnel = 6,
    water_fog_near = 8,
    water_fog_far = 9,
    water_shallow_color = 10,
    water_deep_color = 11,
    water_reflection_color = 12,
    water_displacement = 18, // starting size, force, velocity, falloff, dampner
    water_noise_falloff = 24,
    water_noise_wind_direction = 25, // three layers
    water_noise_wind_speed = 28,     // three layers
    water_fog_above_amount = 33,
    water_fog_under = 35, // amount, near, far
    water_refraction_magnitude = 38,
    water_specular_power = 39,
    water_specular_radius = 41,
    water_specular_brightness = 42,
    water_noise_uv_scale = 43,      // three layers
    water_noise_amplitude = 46,     // three layers
    water_reflection_magnitude = 49,
    water_sparkle_magnitude = 50,
    water_sun_specular_magnitude = 51,
    water_depth_control = 52,       // reflections, refraction, normals, specular lighting
    water_sparkle_power = 56,
    water_noise_flowmap_scale = 57,
    water_data_count = 58,
};

struct Water {
    uint32_t form_id;
    uint32_t edid;
    uint32_t noise[3];    // NNAM in record order, cache texture paths
    uint32_t related[3];  // GNAM: daytime, nighttime, underwater FormIDs
    uint32_t image_space; // INAM FormID
    uint32_t flags;       // FNAM
    uint32_t opacity;     // ANAM
    float velocity[3];    // NAM0
    float data[water_data_count];
};
static_assert(sizeof(Water) == 288);

struct Atmosphere {
    SkySettings settings;
    std::vector<Weather> weathers;
    std::vector<std::string> weather_edids; // parallel to weathers
    std::vector<Climate> climates;
    std::vector<std::string> climate_edids;
    std::vector<ClimateWeather> climate_weathers;
    std::vector<std::pair<std::string, uint32_t>> world_climates; // lowercase world EditorID -> climate FormID
    std::vector<ImageSpace> image_spaces;
    std::vector<Volumetric> volumetrics;
    std::vector<Precipitation> precipitations;
    std::vector<Modifier> modifiers;
    std::vector<std::string> modifier_edids; // parallel to modifiers
    std::vector<float> keys;                 // curve keys of every modifier
    std::vector<uint32_t> sky_statics;       // weathers' TNAM STAT FormIDs
    std::vector<Region> regions;
    std::vector<RegionArea> region_areas;
    std::vector<float> region_points;
    std::vector<RegionCell> region_cells; // sorted by world, x, y
    std::vector<uint32_t> region_lists;
    std::vector<SkyModel> sky_models;
    std::vector<SkyShape> sky_shapes;
    std::vector<SkyVertex> sky_vertices;
    std::vector<uint32_t> sky_indices;       // into sky_vertices
    std::vector<Water> waters;               // sorted by FormID

    const Weather* weather(uint32_t form_id) const;
    const Weather* weather(std::string_view edid) const; // case-insensitive
    const Climate* climate(uint32_t form_id) const;
    uint32_t climate_of_world(std::string_view world_edid) const; // 0 if none
    uint32_t world_index(std::string_view world_edid) const; // in world_climates, ~0u when unknown
    const RegionCell* region_cell(uint32_t world, int32_t x, int32_t y) const;
    const ImageSpace* image_space(uint32_t form_id) const;
    const Volumetric* volumetric(uint32_t form_id) const;
    const Precipitation* precipitation(uint32_t form_id) const;
    const Modifier* modifier(std::string_view edid) const; // case-insensitive
    const SkyModel* sky_model(std::string_view path) const;
    const Water* water(uint32_t form_id) const;

    // Strings (texture and model paths, EditorIDs) by offset; "" for k_no_string.
    std::string_view string(uint32_t offset) const;
    uint32_t intern(std::string_view s); // importer: adds a string, returns its offset

    std::vector<std::byte> string_data;

private:
    std::unordered_map<std::string, uint32_t> m_interned;
};

std::vector<std::byte> write(const Atmosphere& atmosphere);
Atmosphere read(std::span<const std::byte> file);

} // namespace spud::osa
