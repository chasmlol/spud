#include "engine/weather.hpp"

#include "core/units.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <span>

namespace spud {

namespace {

constexpr float k_byte_to_unit = 0.0039215689f;        // the engine's 1/255
constexpr float k_framebuffer_range_rcp = 1.0f / 1.2f; // g_FramebufferRangeRcp (FogNearColor.w, FogColor.w, VParams)
constexpr float k_pi = std::numbers::pi_v<float>;
constexpr uint32_t k_default_image_space = 0x167;
// Depth-bias emulation of the sky passes: the game's D24 depth 1 - k * 2^-24 as
// spud's reverse-Z depth k * 2^-24.
constexpr float k_mask_depth = 3.0f / 16777216.0f;
constexpr float k_disc_depth = 9.0f / 16777216.0f;
// [Weather] INI settings the sky reads, at the user's values (settings dump; exe defaults equal).
constexpr float k_sun_base_size = 425.0f; // fSunBaseSize:Weather
constexpr float k_sun_boost = 1.0f;       // fSunBoost:Weather
// [Display] dynamic depth of field, user's values.
constexpr float k_dof_center_weight = 0.6f;        // fDOFCenterWeight
constexpr float k_dof_center_weight_ext = 2.0f;    // fDDOFFocusCenterweightExt
constexpr float k_dof_focus_delay = 0.5f;          // fDDOFFocusDelay (ms / 1000)
constexpr float k_dof_focus_duration = 1.0f;       // fDDOFFocusDuration
constexpr float k_dof_angle_threshold = 2.0f;      // fDDOFAngleThreshold (degrees)
constexpr float k_dof_position_threshold = 5.0f;   // fDDOFPositionThreshold (units)
// Volumetric lighting defaults for a missing VOLI (the [Display] INI values).
constexpr osa::Volumetric k_default_volumetric{0, 2.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.3f, 300.0f, 15.0f, 0.3f, 0.83f,
                                               0.85f, 40.0f};
// Default cloud colours for weathers without layers, rgb.
constexpr uint8_t k_default_cloud[osa::k_times][3] = {{0x46, 0x4B, 0x5B}, {0x9B, 0x9B, 0x9B}, {0x53, 0x59, 0x6C},
                                                      {0x0F, 0x14, 0x1C}};
constexpr uint8_t k_default_cloud_speed = 51;

enum Time : int { sunrise = 0, day = 1, sunset = 2, night = 3 };

struct Keys {
    int k = day;
    int n = day;
    float w = 1.0f;
};

struct Times {
    float sunrise_begin, sunrise_end, sunset_begin, sunset_end; // hours, as stored (10-minute units / 6)
};

// Key time k with weight w and neighbour n with 1 - w (6.3.1): colours, ambient and image spaces.
Keys time_keys(const Times& c, float extension, float t) {
    const float srb = std::max(c.sunrise_begin - extension, 0.0f);
    const float sse = std::min(c.sunset_end + extension, 23.99f);
    Keys out;
    if (t > srb && t < c.sunrise_end) {
        const float h = (c.sunrise_end - srb) * 0.5f;
        const float m = srb + h;
        out.k = sunrise;
        out.n = t >= m ? day : night;
        out.w = 1.0f - std::abs(t - m) / h;
    } else if (t >= c.sunrise_end && t <= c.sunset_begin) {
        out = {day, day, 1.0f};
    } else if (t > c.sunset_begin && t < sse) {
        const float h = (sse - c.sunset_begin) * 0.5f;
        const float m = c.sunset_begin + h;
        out.k = sunset;
        out.n = t >= m ? night : day;
        out.w = 1.0f - std::abs(t - m) / h;
    } else {
        out = {night, night, 1.0f};
    }
    return out;
}

Times climate_times(const osa::Climate& c) {
    return {c.timing[0] * 0.16666667f, c.timing[1] * 0.16666667f, c.timing[2] * 0.16666667f,
            c.timing[3] * 0.16666667f};
}

float smoothstep01(float t) { return t * t * (3.0f - 2.0f * t); }

glm::vec4 rgba(const glm::vec3& c, float a) { return glm::vec4(c, a); }

} // namespace

// The four weights of: (current k, current n, outgoing k, outgoing n).
struct Sky::Weights {
    const osa::Weather* weather[4] = {};
    int time[4] = {};
    float weight[4] = {};
    Keys keys;
    float transition = 1.0f;

    template <typename F>
    void each(F&& f) const {
        for (int i = 0; i < 4; ++i) {
            if (weather[i] && weight[i] > 0.0f) {
                f(*weather[i], time[i], weight[i], i >= 2);
            }
        }
    }
};

Sky::Sky(const osa::Atmosphere& atmosphere, TextureResolver textures)
    : m_atm(&atmosphere), m_textures(std::move(textures)) {}

std::vector<render::SkyVertex> Sky::mesh_vertices() const {
    std::vector<render::SkyVertex> v(m_atm->sky_vertices.size());
    static_assert(sizeof(render::SkyVertex) == sizeof(osa::SkyVertex));
    std::memcpy(v.data(), m_atm->sky_vertices.data(), v.size() * sizeof(render::SkyVertex));
    // The sun's and moons' quad (Sun::Init, Moon::Init): +-1 in local XY, uv 0..1, vertex colour (1, 0, 0, 1).
    const float corners[4][4] = {{-1, -1, 0, 1}, {1, -1, 1, 1}, {1, 1, 1, 0}, {-1, 1, 0, 0}};
    for (const auto& c : corners) {
        v.push_back({{c[0], c[1], 0.0f}, {c[2], c[3]}, {255, 0, 0, 255}});
    }
    return v;
}

std::vector<uint32_t> Sky::mesh_indices() const {
    std::vector<uint32_t> i = m_atm->sky_indices;
    const auto q = static_cast<uint32_t>(m_atm->sky_vertices.size());
    for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) {
        i.push_back(q + k);
    }
    return i;
}

void evaluate_interior(const osw::CellLighting& l, float sunlight_scale, render::Environment& env,
                       const osw::CellLighting* to, float w) {
    const osw::CellLighting& b = to ? *to : l;
    auto mixf = [&](float x, float y) { return x + (y - x) * w; };
    auto colour = [&](const uint8_t* x, const uint8_t* y) {
        return glm::mix(glm::vec3(x[0], x[1], x[2]), glm::vec3(y[0], y[1], y[2]), w) * k_byte_to_unit;
    };
    // Sun::Update in mode 1: the light's rotation Rz(xy) Ry(z) from the integer degrees (converted as unsigned);
    // DirLightDirection = -(cos a cos b, -sin a cos b, sin b).
    const float a = float(uint32_t(l.rotation_xy)) * 0.017453292f;
    const float c = float(uint32_t(l.rotation_z)) * 0.017453292f;
    env.sun_dir = glm::vec3(-std::cos(a) * std::cos(c), std::sin(a) * std::cos(c), -std::sin(c));
    // DirLightColor: the directional colour x the image space's Sunlight Scale; the XCLL fade is never read.
    const glm::vec3 directional = colour(l.directional, b.directional);
    env.dir_light_color = directional * sunlight_scale;
    env.sun_color = directional;
    // Directional ambient as for weathers (6.3.5), from the six DALC colours.
    glm::vec3 d[6];
    for (int i = 0; i < 6; ++i) {
        d[i] = colour(l.directional_ambient[i], b.directional_ambient[i]);
    }
    for (int ch = 0; ch < 3; ++ch) {
        const float sum = d[0][ch] + d[1][ch] + d[2][ch] + d[3][ch] + d[4][ch] + d[5][ch];
        env.dir_ambient[ch] = glm::vec4((d[1][ch] - d[0][ch]) * 0.5f, (d[3][ch] - d[2][ch]) * 0.5f,
                                        (d[5][ch] - d[4][ch]) * 0.5f, sum / 6.0f);
    }
    const float fresnel = mixf(l.fresnel_power, b.fresnel_power);
    env.amb_spec = glm::vec4(colour(l.specular, b.specular), fresnel > 1.0f ? fresnel : 1.0f);
    // Fog (mode 1): far outside (0, 163840] becomes 163840, near outside
    // (0, far] becomes 0.17 far; power and max as the cell has them.
    float far = mixf(l.fog_far, b.fog_far);
    if (far <= 0.0f || far > 163840.0f) {
        far = 163840.0f;
    }
    float near = mixf(l.fog_near, b.fog_near);
    if (near <= 0.0f || near > far) {
        near = 0.17f * far;
    }
    const float power = mixf(l.fog_power, b.fog_power);
    const float max = mixf(l.fog_max, b.fog_max);
    env.fog_param = far > near ? glm::vec4(near / (far - near), 1.0f / (far - near), power, max)
                               : glm::vec4(5000000.0f, 0.1f, 1.0f, 0.0f);
    env.fog_near_color = glm::vec4(colour(l.fog_near_color, b.fog_near_color), k_framebuffer_range_rcp);
    env.fog_far_color = glm::vec4(colour(l.fog_far_color, b.fog_far_color), 0.0f);
    env.fog_color = env.fog_near_color;
    env.fog_start = units::to_metres(near);
    env.fog_end = units::to_metres(far);
    env.fog_power = power;
    env.fog_max = max;
    // The far plane: the fog clip distance when set, for the projection fog reads.
    env.far_units = l.fog_clip > 0.0f ? std::min(l.fog_clip, 353840.0f) : 353840.0f;
    // Light fade: the game squares these into g_LightFadeStartSq / EndSq.
    auto fade = [&](float x, float y) {
        return x > 0.0f && y > 0.0f ? mixf(x, y) : (w < 1.0f ? x : y) > 0.0f ? (w < 1.0f ? x : y) : -1.0f;
    };
    env.light_fade_begin = fade(l.light_fade_begin, b.light_fade_begin);
    env.light_fade_end = fade(l.light_fade_end, b.light_fade_end);
    // Effect lighting is the directional colour in modes 1 and 2 (the game sets Sky = Sky).
    env.effect_light = directional * sunlight_scale;
    env.ibl = glm::vec4(0.0f);
    env.output_clamp = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
    env.frame_params = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    env.sky_zenith = env.sky_horizon = env.sky_ground = glm::vec3(0.0f);
    env.skyrim_inputs = true;
}

namespace {
constexpr uint32_t k_default_weather = 0x15E; // DefaultWeather
constexpr float k_cell_units = 4096.0f;

// the bounding box, then a crossing test.
bool polygon_contains(const float* pts, uint32_t n, float x, float y) {
    if (n < 3) {
        return false;
    }
    float minx = pts[0], maxx = pts[0], miny = pts[1], maxy = pts[1];
    for (uint32_t i = 1; i < n; ++i) {
        minx = std::min(minx, pts[2 * i]);
        maxx = std::max(maxx, pts[2 * i]);
        miny = std::min(miny, pts[2 * i + 1]);
        maxy = std::max(maxy, pts[2 * i + 1]);
    }
    if (x < minx || x > maxx || y < miny || y > maxy) {
        return false;
    }
    bool inside = false;
    for (uint32_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = pts[2 * i], yi = pts[2 * i + 1], xj = pts[2 * j], yj = pts[2 * j + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
            inside = !inside;
        }
    }
    return inside;
}
} // namespace

void Sky::set_world(std::string_view world_edid) {
    const uint32_t world = m_atm->world_index(world_edid);
    if (world != m_world) {
        m_world = world;
        m_region_cell_known = false; // a new world's first cell is an enter event
    }
    m_climate = m_atm->climate(m_atm->climate_of_world(world_edid));
}

uint32_t Sky::weather() const { return m_current ? m_current->form_id : 0; }
uint32_t Sky::outgoing_weather() const { return m_outgoing ? m_outgoing->form_id : 0; }

uint32_t Sky::next_random() {
    // xorshift32: the game draws from its global the game; spud seeds its own stream so runs repeat.
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return m_rng;
}

void Sky::start_transition(const osa::Weather* next, bool instant, bool accelerate) {
    if (!next) {
        return;
    }
    m_outgoing = instant ? nullptr : m_current;
    if (m_outgoing == next) {
        m_outgoing = nullptr;
    }
    m_current = next;
    m_change_hour = m_hour;
    m_transition = (instant || !m_outgoing) ? 1.0f : 0.0f;
    m_accelerated = accelerate;
    m_transition_start = 0.0f;
    if (instant) {
        m_reset_adaptation = true; // Sky::Update flag 0x100: the eye adaptation jumps
    }
    // Wind direction on a weather change (Sky::Update): DATA[17] * 360/255 plus the smallest of four uniform
    // offsets in [-r, r], r = DATA[18] * 90/255.
    const float r = next->data[osa::data_wind_direction_range] * 90.0f / 255.0f;
    float o = 1e9f;
    for (int i = 0; i < 4; ++i) {
        const float u = (next_random() * 2.3283064e-10f) * 2.0f * r - r;
        if (std::abs(u) < std::abs(o)) {
            o = u;
        }
    }
    float dir = glm::radians(next->data[osa::data_wind_direction] * 360.0f / 255.0f + o);
    dir = std::fmod(dir, 2.0f * k_pi);
    m_wind_direction = dir < 0.0f ? dir + 2.0f * k_pi : dir;
    // New precipitation geometry; the current one becomes the outgoing (Precipitation::Update).
    m_precip[1] = instant ? PrecipState{} : m_precip[0];
    m_precip[0] = new_precip(m_atm->precipitation(next->precipitation));
}

Sky::PrecipState Sky::new_precip(const osa::Precipitation* spgd) {
    PrecipState p;
    p.spgd = spgd;
    if (spgd) {
        p.seed = next_random();
        for (auto& off : p.instance_offset) {
            for (int c = 0; c < 3; ++c) {
                off[c] = (next_random() * 2.3283064e-10f) * spgd->box_size;
            }
        }
    }
    return p;
}

void Sky::apply_particles(uint32_t form_id, float fade_in) {
    // one effect per form, and an active one does nothing.
    if (std::ranges::any_of(m_script_precip, [form_id](const ScriptPrecip& s) { return s.form == form_id && s.on; })) {
        return;
    }
    const osa::Precipitation* spgd = m_atm ? m_atm->precipitation(form_id) : nullptr;
    if (!spgd) {
        return;
    }
    std::erase_if(m_script_precip, [form_id](const ScriptPrecip& s) { return s.form == form_id; });
    m_script_precip.push_back({form_id, new_precip(spgd), 0.0f, fade_in, true});
}

void Sky::remove_particles(uint32_t form_id, float fade_out) {
    for (ScriptPrecip& s : m_script_precip) {
        if (s.form == form_id) {
            s.on = false;
            s.fade = fade_out;
        }
    }
}

// spud's callers are scripted checks, which pin the weather like an override
// (Weather.ForceActive(true)): crossing into another region doesn't replace it, the climate's period does.
void Sky::force_weather(uint32_t form_id) {
    const osa::Weather* w = form_id ? m_atm->weather(form_id) : nullptr;
    if (w) {
        m_override = w;
        m_default = nullptr;
        m_player_region = nullptr;
        start_transition(w, true, false);
    }
}

void Sky::hold_weather(uint32_t form_id) {
    if (const osa::Weather* w = m_atm->weather(form_id); w && !m_held) {
        m_held = true;
        m_held_current = m_current;
        m_held_override = m_override;
        m_held_default = m_default;
        m_held_region = m_player_region;
        m_override = w;
        start_transition(w, true, false);
    }
}

void Sky::release_weather() {
    if (m_held) {
        m_held = false;
        m_override = m_held_override;
        m_default = m_held_default;
        m_player_region = m_held_region;
        if (m_held_current) {
            start_transition(m_held_current, true, false);
        }
    }
}

void Sky::set_interior(bool interior, int mode, uint32_t weather) {
    if (m_interior && !interior) {
        m_region_cell_known = false; // back outside: the cell is entered
        // What a view through the door showed is what stands outside.
        if (m_outdoor_weather && m_outdoor_weather != m_current) {
            start_transition(m_outdoor_weather, true, false);
        }
        m_outdoor_weather = nullptr;
    }
    m_interior = interior;
    m_mode = mode;
    const osa::Weather* w = weather && mode != 1 ? m_atm->weather(weather) : nullptr;
    // Through a load door: the new sky stands at once.
    if (w && w != m_interior_weather && w != m_current) {
        if (!m_outdoor_weather) {
            m_outdoor_weather = m_current;
        }
        start_transition(w, true, false);
    }
    m_interior_weather = w;
}

bool Sky::evaluate_as(render::Environment& env, int mode, const osa::Weather* weather) {
    if (!weather || weather == m_current) {
        return evaluate(env, mode);
    }
    // Another space's sky: the frame's own weather, and what it leaves for the frame, stay.
    const osa::Weather* const current = m_current;
    const osa::Weather* const outgoing = m_outgoing;
    const glm::vec3 multiplier = m_water_multiplier;
    const bool reset = m_reset_adaptation;
    m_current = weather;
    m_outgoing = nullptr;
    const bool lit = evaluate(env, mode);
    m_current = current;
    m_outgoing = outgoing;
    m_water_multiplier = multiplier;
    m_reset_adaptation = reset;
    return lit;
}

bool Sky::evaluate_outdoors(render::Environment& env) { return evaluate_as(env, 3, m_outdoor_weather); }

bool Sky::evaluate_interior(render::Environment& env, int mode, uint32_t weather) {
    return evaluate_as(env, mode, weather ? m_atm->weather(weather) : nullptr);
}

// The game with override set: the next update starts the transition. Accelerating speeds up the one
// already running.
void Sky::set_weather(uint32_t form_id, bool accelerate) {
    const osa::Weather* w = m_atm->weather(form_id);
    if (!w) {
        return;
    }
    if (!m_current) {
        force_weather(form_id);
        return;
    }
    m_override = w;
    if (accelerate) {
        m_accelerated = m_outgoing != nullptr;
        m_transition_start = m_outgoing ? m_transition : 0.0f;
    }
}

void Sky::set_cloud_clock(float seconds) {
    if (!m_current) {
        return;
    }
    const float e = m_atm->settings.cloud_speed_max;
    for (int i = 0; i < osa::k_cloud_layers; ++i) {
        const int layer = static_cast<uint32_t>(i) < m_current->cloud_layer_count ? i : 0;
        m_cloud_uv[i][0] = (m_current->clouds[layer].speed_x * 2.0f * e / 254.0f - e) * seconds * 0.1f;
        m_cloud_uv[i][1] = (m_current->clouds[layer].speed_y * 2.0f * e / 254.0f - e) * seconds * 0.1f;
    }
}

void Sky::set_clock(const GameClock& clock) {
    m_hour = clock.hour();
    m_days = clock.whole_days();
    m_day_wraps = int(clock.midnights());
}

bool Sky::apply_modifier(std::string_view edid, float strength, float fade_in) {
    const osa::Modifier* m = m_atm->modifier(edid);
    if (!m) {
        return false;
    }
    m_modifiers.push_back({m, m_modifier_clock, strength, fade_in});
    return true;
}

void Sky::set_ui_blur(bool on) {
    if (on == m_ui_blur) {
        return;
    }
    m_ui_blur = on;
    if (on) {
        apply_modifier("ISMTween");
    } else if (const osa::Modifier* m = m_atm->modifier("ISMTween")) {
        std::erase_if(m_modifiers, [&](const ModifierInstance& i) { return i.modifier == m; });
    }
}

bool Sky::apply_modifier_unique(std::string_view edid, float strength) {
    const osa::Modifier* m = m_atm->modifier(edid);
    return m && (std::ranges::any_of(m_modifiers, [m](const auto& running) { return running.modifier == m; }) ||
                 apply_modifier(edid, strength));
}

void Sky::remove_modifier(std::string_view edid, float fade_out) {
    const osa::Modifier* m = m_atm->modifier(edid);
    if (fade_out <= 0.0f) {
        std::erase_if(m_modifiers, [m](const ModifierInstance& i) { return i.modifier == m; });
        return;
    }
    for (ModifierInstance& i : m_modifiers) {
        if (i.modifier == m && i.fade_start < 0.0f) {
            i.fade_start = m_modifier_clock;
            i.fade_out = fade_out;
        }
    }
}

float Sky::modifier_fade(const ModifierInstance& m) const {
    float fade = 1.0f;
    if (m.fade_in > 0.0f) {
        fade = std::min(fade, (m_modifier_clock - m.start) / m.fade_in);
    }
    if (m.fade_start >= 0.0f) {
        fade = std::min(fade, 1.0f - (m_modifier_clock - m.fade_start) / m.fade_out);
    }
    return std::clamp(fade, 0.0f, 1.0f);
}

// The game over a span of climate_weathers.
uint32_t Sky::find_weather(int type) const {
    // the player's region's list, with no going on to the climate's if it has one.: of the list's
    // weathers with that flag, the one with the lowest chance.
    const uint32_t first = m_player_region ? m_player_region->first_weather : m_climate ? m_climate->first_weather : 0;
    const uint32_t count = m_player_region ? m_player_region->weather_count : m_climate ? m_climate->weather_count : 0;
    uint32_t found = 0;
    uint32_t lowest = 0xFFFFFFFFu;
    for (uint32_t i = 0; type >= 0 && type < 8 && i < count; ++i) {
        const auto& cw = m_atm->climate_weathers[first + i];
        const osa::Weather* w = m_atm->weather(cw.weather);
        if (w && (w->data[osa::data_flags] & (1u << type)) && uint32_t(cw.chance) < lowest) {
            found = cw.weather;
            lowest = uint32_t(cw.chance);
        }
    }
    return found;
}

const osa::Weather* Sky::pick_weather(uint32_t first, uint32_t count) {
    int64_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        total += std::max(m_atm->climate_weathers[first + i].chance, 0);
    }
    if (total <= 0) {
        return nullptr;
    }
    int64_t r = static_cast<int64_t>(next_random() % static_cast<uint32_t>(total));
    for (uint32_t i = 0; i < count; ++i) {
        const auto& cw = m_atm->climate_weathers[first + i];
        r -= std::max(cw.chance, 0);
        if (r < 0) {
            return m_atm->weather(cw.weather);
        }
    }
    return nullptr;
}

bool Sky::lists_weather(uint32_t first, uint32_t count, const osa::Weather* w) const {
    for (uint32_t i = 0; w && i < count; ++i) {
        if (m_atm->climate_weathers[first + i].weather == w->form_id) {
            return true;
        }
    }
    return false;
}

// The climate's own choice (Sky), kept until the period ends; DefaultWeather 0x15E when the list has nothing.
const osa::Weather* Sky::default_weather() {
    if (!m_default) {
        m_default = pick_weather(m_climate->first_weather, m_climate->weather_count);
    }
    if (!m_default) {
        m_default = m_atm->weather(k_default_weather);
    }
    return m_default;
}

// The game's cell enter event: of the cell's regions containing the position, the weather region
// is the first, until one overrides where it doesn't, or matches it in that and has the higher priority.
void Sky::enter_cell(const glm::vec3& position) {
    const int32_t x = static_cast<int32_t>(std::floor(position.x / k_cell_units));
    const int32_t y = static_cast<int32_t>(std::floor(position.y / k_cell_units));
    if (m_region_cell_known && x == m_region_cell_x && y == m_region_cell_y) {
        return;
    }
    m_region_cell_known = true;
    m_region_cell_x = x;
    m_region_cell_y = y;
    m_player_region = nullptr;
    const osa::RegionCell* cell = m_atm->region_cell(m_world, x, y);
    for (uint32_t k = 0; cell && k < cell->count; ++k) {
        const osa::Region& g = m_atm->regions[m_atm->region_lists[cell->first + k]];
        bool contains = false;
        for (uint32_t i = 0; i < g.area_count && !contains; ++i) {
            const osa::RegionArea& area = m_atm->region_areas[g.first_area + i];
            contains = polygon_contains(&m_atm->region_points[2 * size_t(area.first_point)], area.point_count, position.x,
                                        position.y);
        }
        if (!contains) {
            continue;
        }
        const osa::Region* now = m_player_region;
        if (!now || (g.override_others && !now->override_others) ||
            (g.override_others == now->override_others && g.priority > now->priority)) {
            m_player_region = &g;
        }
    }
}

uint32_t Sky::pick_for_region(uint32_t region_form) {
    for (const osa::Region& g : m_atm->regions) {
        if (g.form_id != region_form) {
            continue;
        }
        const osa::Weather* next = pick_weather(g.first_weather, g.weather_count);
        if (next && m_climate && m_current != next && lists_weather(g.first_weather, g.weather_count, m_current)) {
            const float period = 22.0f * (255.0f - m_climate->timing[4]) / 255.0f + 1.0f;
            float elapsed = m_hour - m_change_hour;
            elapsed += elapsed < 0.0f ? 24.0f : 0.0f;
            if (next_random() * 2.3283064e-10f > elapsed / period) {
                next = m_current;
            }
        }
        return next ? next->form_id : 0;
    }
    return 0;
}

void Sky::choose_weather() {
    const float period = 22.0f * (255.0f - m_climate->timing[4]) / 255.0f + 1.0f;
    float elapsed = m_hour - m_change_hour;
    if (elapsed < 0.0f) {
        elapsed += 24.0f;
    }
    bool expired = false;
    if (m_override && elapsed > period) {
        m_override = nullptr;
        expired = true;
    }
    // A weather both lists share survives a change of list with the chance 1 - elapsed / period.
    auto stays = [&] { return next_random() * 2.3283064e-10f > elapsed / period; };
    const osa::Weather* next = nullptr;
    if (m_override) {
        next = m_override;
    } else if (m_region != m_player_region || expired) {
        m_region = m_player_region;
        if (m_region) {
            next = pick_weather(m_region->first_weather, m_region->weather_count);
            if (m_current != next && lists_weather(m_region->first_weather, m_region->weather_count, m_current) &&
                stays()) {
                next = m_current;
            }
        } else {
            if (m_current != m_default && lists_weather(m_climate->first_weather, m_climate->weather_count, m_current) &&
                stays()) {
                m_default = m_current;
            }
            next = default_weather();
        }
    } else if (elapsed > period) {
        if (m_region) {
            next = pick_weather(m_region->first_weather, m_region->weather_count);
        } else {
            m_default = nullptr;
            next = default_weather();
        }
    } else if (!m_region) {
        next = default_weather();
    }
    if (!m_current && !next) {
        next = default_weather();
    }
    if (next && next != m_current && !m_outgoing) {
        start_transition(next, false, false);
    }
}

void Sky::update(const GameClock& clock, float dt, float frame_dt, const glm::vec3& camera, float yaw, float pitch) {
    // A jump of the clock (a scripted `set animation_time`, a stall) is not elapsed play: nothing advances through it.
    dt = dt > 1.0f ? 0.0f : std::max(dt, 0.0f);
    const auto& gs = m_atm->settings;
    set_clock(clock);
    if (!m_climate) {
        return;
    }
    if (!m_interior) {
        enter_cell(camera);
    }
    // Sky::Update: mode 1 (an interior without Show Sky) leaves the weather as it is.
    if (!m_interior_weather && m_mode != 1) {
        choose_weather();
    }
    if (!m_current) {
        return;
    }
    // Transition: T = elapsed / D, D = DATA[3]/255 * (fWeatherTransMax - fWeatherTransMin) + fWeatherTransMin hours.
    if (m_outgoing && m_mode != 1) {
        float elapsed = m_hour - m_change_hour;
        if (elapsed < 0.0f) {
            elapsed += 24.0f;
        }
        const float d = m_current->data[osa::data_trans_delta] / 255.0f * (gs.trans_max - gs.trans_min) + gs.trans_min;
        float t = elapsed / d;
        if (m_accelerated) {
            t = m_transition_start + (t - m_transition_start) * (gs.trans_accel + 1.0f);
        }
        m_transition = t;
        if (m_transition > 1.0f) {
            m_transition = 1.0f;
            m_outgoing = nullptr;
            m_precip[1] = PrecipState{};
        }
    }

    // Clouds (Clouds::Update): UV += speed * dt * 0.1 per layer, speed = byte * 2E/254 - E, blended by T.
    const float e = gs.cloud_speed_max;
    auto speed = [&](const osa::Weather& w, int i, int axis) {
        const int layer = (static_cast<uint32_t>(i) < w.cloud_layer_count) ? i : 0;
        const uint8_t b = w.cloud_layer_count == 0 ? k_default_cloud_speed
                                                   : (axis == 0 ? w.clouds[layer].speed_x : w.clouds[layer].speed_y);
        return b * 2.0f * e / 254.0f - e;
    };
    for (int i = 0; i < osa::k_cloud_layers; ++i) {
        for (int axis = 0; axis < 2; ++axis) {
            float s = speed(*m_current, i, axis);
            if (m_outgoing) {
                s = m_transition * s + (1.0f - m_transition) * speed(*m_outgoing, i, axis);
            }
            m_cloud_uv[i][axis] += s * dt * 0.1f;
        }
    }

    // Lightning: each thunder entry tries every 30 ms with probability
    // 1 / (frequency x entries) while its intensity is above 1e-4; the flash decays linearly over
    // fWeatherFlashDuration.
    m_flash_age += dt;
    m_flash = std::max(0.0f, 1.0f - m_flash_age / gs.flash_duration);
    m_thunder_clock += dt;
    while (m_thunder_clock >= 0.03f) {
        m_thunder_clock -= 0.03f;
        const osa::Weather& w = *m_current;
        const uint32_t freq = w.data[osa::data_lightning_frequency];
        const float begin = w.data[osa::data_lightning_fade_in] / 255.0f;
        const float intensity = m_transition >= begin ? (m_transition - begin) / std::max(1.0f - begin, 1e-6f) : 0.0f;
        for (uint32_t n = 0; n < w.thunder_count && freq > 0; ++n) {
            if (next_random() % (freq * w.thunder_count) == 0 && intensity > 1e-4f) {
                m_flash_age = 0.0f;
                m_flash = 1.0f;
            }
        }
    }

    // Precipitation emitters.
    const float wind_speed =
        m_current->data[osa::data_wind_speed] / 255.0f * m_transition +
        (m_outgoing ? m_outgoing->data[osa::data_wind_speed] / 255.0f * (1.0f - m_transition) : 0.0f);
    const glm::vec3 wind = gs.precip_wind_mult * wind_speed *
                           glm::vec3(std::cos(m_wind_direction), std::sin(m_wind_direction), 0.0f);
    const auto drift = [&](PrecipState& p) {
        if (!p.spgd) {
            return;
        }
        const float b = p.spgd->box_size;
        const glm::vec3 v = wind + glm::vec3(0.0f, 0.0f, -p.spgd->gravity);
        for (int c = 0; c < 3; ++c) {
            p.scroll[c] = std::fmod(p.scroll[c] + v[c] * dt + b, b);
        }
        p.rotation = std::fmod(p.rotation + glm::radians(p.spgd->rotation_velocity) * dt, 2.0f * k_pi);
    };
    for (auto& p : m_precip) {
        drift(p);
    }
    for (ScriptPrecip& s : m_script_precip) {
        drift(s.state);
        const float step = s.fade > 0.0f ? frame_dt / s.fade : 1.0f;
        s.amount = std::clamp(s.amount + (s.on ? step : -step), 0.0f, 1.0f);
    }
    std::erase_if(m_script_precip, [](const ScriptPrecip& s) { return !s.on && s.amount <= 0.0f; });
    m_precip_camera = camera;
    // Volumetric lighting's noise drift: the game's random walk at its expected rate, so it repeats.
    {
        const auto w = weights();
        osa::Volumetric v{};
        float* acc = &v.intensity;
        w.each([&](const osa::Weather& wt, int time, float weight, bool) {
            const osa::Volumetric* src = m_atm->volumetric(wt.volumetric[time]);
            const float* s = &(src ? *src : k_default_volumetric).intensity;
            for (int i = 0; i < 12; ++i) {
                acc[i] += s[i] * weight;
            }
        });
        const float k = 0.5f * dt * v.density_size;
        m_vl_wind += glm::vec3(-std::sin(m_wind_direction) * v.density_wind_speed * k,
                               std::cos(m_wind_direction) * v.density_wind_speed * k, v.density_falling_speed * k);
    }

    // Dynamic depth of field: the focus weight drops to fDOFCenterWeight when the camera turns or moves, and eases
    // back to the exterior weight after fDDOFFocusDelay over fDDOFFocusDuration.
    if (!m_dof_init) {
        m_dof_init = true;
        m_dof_pos = camera;
        m_dof_yaw = yaw;
        m_dof_pitch = pitch;
    }
    const float turned = glm::degrees(std::max(std::abs(yaw - m_dof_yaw), std::abs(pitch - m_dof_pitch)));
    if (turned >= k_dof_angle_threshold || glm::length(camera - m_dof_pos) >= k_dof_position_threshold) {
        m_dof_still = 0.0f;
        m_dof_pos = camera;
        m_dof_yaw = yaw;
        m_dof_pitch = pitch;
    } else {
        m_dof_still += frame_dt;
    }
    const float ease = std::clamp((m_dof_still - k_dof_focus_delay) / k_dof_focus_duration, 0.0f, 1.0f);
    m_dof_weight = k_dof_center_weight + (k_dof_center_weight_ext - k_dof_center_weight) * std::sin(ease * k_pi * 0.5f);

    // Image-space modifiers: the clock advances; finished animatable ones are dropped.
    m_modifier_clock += dt;
    std::erase_if(m_modifiers, [&](const ModifierInstance& m) {
        return (m.modifier->animatable && m_modifier_clock - m.start > m.modifier->duration) ||
               (m.fade_start >= 0.0f && m_modifier_clock - m.fade_start >= m.fade_out);
    });
}

Sky::Weights Sky::weights() const {
    Weights w;
    if (!m_climate || !m_current) {
        return w;
    }
    w.keys = time_keys(climate_times(*m_climate), m_atm->settings.daytime_color_extension, m_hour);
    w.transition = m_transition;
    const float t = m_outgoing ? m_transition : 1.0f;
    w.weather[0] = w.weather[1] = m_current;
    w.weather[2] = w.weather[3] = m_outgoing;
    w.time[0] = w.time[2] = w.keys.k;
    w.time[1] = w.time[3] = w.keys.n;
    w.weight[0] = w.keys.w * t;
    w.weight[1] = (w.keys.k != w.keys.n) ? (1.0f - w.keys.w) * t : 0.0f;
    if (w.keys.k == w.keys.n) {
        w.weight[0] = t;
    }
    if (m_outgoing) {
        w.weight[2] = w.keys.k == w.keys.n ? (1.0f - t) : w.keys.w * (1.0f - t);
        w.weight[3] = w.keys.k != w.keys.n ? (1.0f - w.keys.w) * (1.0f - t) : 0.0f;
    }
    return w;
}

void Sky::apply_modifiers(render::ImageSpaceData& is) const {
    // while g_HDRWorldMapMode is on.
    is.world_map = m_world_map;
    if (m_world_map) {
        is.brightness += 0.25f;     // fGlobalMapBrightnessBoost:Display
        is.contrast -= 0.3f;        // fGlobalMapContrastBoost:Display
        is.bloom_threshold += 0.25f; // fGlobalMapBloomThresholdBoost:Display
    }
    // Modifiers.
    if (!m_modifiers.empty()) {
        const auto& keys = m_atm->keys;
        auto eval = [&](const osa::Curve& c, float x, float fallback, int stride = 2, int component = 1) {
            if (c.count == 0) {
                return fallback;
            }
            const float* k = keys.data() + c.first;
            if (x <= k[0]) {
                return k[component];
            }
            for (uint32_t i = 1; i < c.count; ++i) {
                const float* a = k + (i - 1) * stride;
                const float* b = k + i * stride;
                if (x <= b[0]) {
                    const float f = (b[0] - a[0]) > 0.0f ? (x - a[0]) / (b[0] - a[0]) : 1.0f;
                    return a[component] + (b[component] - a[component]) * f;
                }
            }
            return k[(c.count - 1) * stride + component];
        };
        float* d = &is.eye_adapt_speed;
        glm::vec3 tint_sum = is.tint * is.tint_amount;
        float tint_weight = is.tint_amount, tint_max = is.tint_amount;
        glm::vec3 fade_sum(0.0f);
        float fade_weight = 0.0f, fade_max = 0.0f;
        float best_radial = 0.0f, best_dof = 0.0f;
        for (const auto& inst : m_modifiers) {
            const osa::Modifier& m = *inst.modifier;
            const float s = inst.strength * modifier_fade(inst);
            const float x = m.animatable && m.duration > 0.0f
                                ? std::clamp((m_modifier_clock - inst.start) / m.duration, 0.0f, 1.0f)
                                : 0.0f;
            // HDR fields 0..8 and cinematic 0x11..0x13 (data offsets 0x24..0x2C).
            for (uint32_t f = 0; f < osa::k_curve_fields; ++f) {
                const bool hdr = f < 9;
                const bool cine = f >= osa::curve_saturation && f <= osa::curve_contrast;
                if (!hdr && !cine) {
                    continue;
                }
                float& v = d[hdr ? f : 9 + (f - osa::curve_saturation)];
                const float mul = eval(m.mult[f], x, 1.0f), add = eval(m.add[f], x, 0.0f);
                v = v + (v * mul + add - v) * s;
            }
            if (m.tint.count) {
                const float a = std::clamp(eval(m.tint, x, 0.0f, 5, 4) * s, 0.0f, 1.0f);
                tint_sum += a * glm::vec3(eval(m.tint, x, 0.0f, 5, 1), eval(m.tint, x, 0.0f, 5, 2),
                                          eval(m.tint, x, 0.0f, 5, 3));
                tint_weight += a;
                tint_max = std::max(tint_max, a);
            }
            if (m.fade.count) {
                const float a = std::clamp(eval(m.fade, x, 0.0f, 5, 4) * s, 0.0f, 1.0f);
                fade_sum += a * glm::vec3(eval(m.fade, x, 0.0f, 5, 1), eval(m.fade, x, 0.0f, 5, 2),
                                          eval(m.fade, x, 0.0f, 5, 3));
                fade_weight += a;
                fade_max = std::max(fade_max, a);
            }
            is.blur_radius =
                std::max(is.blur_radius, std::clamp(eval(m.single[osa::curve_blur_radius], x, 0.0f) * s, 0.0f, 7.0f));
            is.double_vision = std::max(is.double_vision, eval(m.single[osa::curve_double_vision], x, 0.0f) * s);
            is.motion_blur = std::max(is.motion_blur, eval(m.single[osa::curve_motion_blur], x, 0.0f));
            const float radial = eval(m.single[osa::curve_radial_strength], x, 0.0f) * s;
            if (radial > best_radial) {
                best_radial = radial;
                is.radial_strength = radial;
                is.radial_ramp_up = eval(m.single[osa::curve_radial_ramp_up], x, 0.0f);
                is.radial_start = eval(m.single[osa::curve_radial_start], x, 0.0f);
                is.radial_ramp_down = eval(m.single[osa::curve_radial_ramp_down], x, 0.0f);
                is.radial_down_start = eval(m.single[osa::curve_radial_down_start], x, 0.0f);
                is.radial_center = glm::vec2(m.radial_center[0], m.radial_center[1]);
            }
            const float dof = eval(m.single[osa::curve_dof_strength], x, 0.0f) * s;
            if (dof > best_dof) {
                best_dof = dof;
                is.dof = glm::vec4(dof, eval(m.single[osa::curve_dof_distance], x, 0.0f),
                                   eval(m.single[osa::curve_dof_range], x, 0.0f), float(m.dof_flags));
            }
        }
        if (tint_weight > 0.0f) {
            is.tint = tint_sum / tint_weight;
            is.tint_amount = tint_max;
        }
        if (fade_weight > 0.0f) {
            is.fade = fade_sum / fade_weight;
            is.fade_amount = fade_max;
        }
    }
}

void Sky::evaluate_indoors(render::Environment& env) const {
    render::SkyFrame& sf = env.sky;
    sf.exterior = false;
    sf.draw_count = 0;
    sf.precipitation = {};
    env.fog_param = glm::vec4(5000000.0f, 0.1f, 1.0f, 0.0f); // no fog (6.3.6)
    env.fog_near_color.w = k_framebuffer_range_rcp;
    set_cell_image_space(env, 0, 0, 0, 1.0f);
}

// The game and the room blend in Sky::Update: the cell's XCIM or
// DefaultImageSpace, a room marker's in its place, lerped through a change of room. Sky mode 2 lights the cell like an
// interior, so it comes here too, and draws no volumetric lighting (no ISApplyVolumetricLighting in `cities-bleakfalls2`).
void Sky::set_cell_image_space(render::Environment& env, uint32_t cell, uint32_t from_room, uint32_t to_room,
                               float weight) const {
    render::SkyFrame& sf = env.sky;
    sf.volumetric.enabled = false;
    auto block = [&](uint32_t room) {
        const osa::ImageSpace* src = m_atm->image_space(room ? room : cell);
        return src ? src : m_atm->image_space(k_default_image_space);
    };
    const osa::ImageSpace* from = block(from_room);
    const osa::ImageSpace* to = block(to_room);
    if (!from || !to) {
        sf.image_space_valid = false;
        return;
    }
    float acc[20];
    for (int i = 0; i < 20; ++i) {
        acc[i] = from->hdr[i] + (to->hdr[i] - from->hdr[i]) * weight; // hdr, cinematic, tint, dof: contiguous
    }
    render::ImageSpaceData is;
    float* d = &is.eye_adapt_speed;
    for (int i = 0; i < 13; ++i) {
        d[i] = acc[i];
    }
    is.tint = glm::vec3(acc[13], acc[14], acc[15]);
    is.dof = glm::vec4(acc[16], acc[17], acc[18], acc[19]);
    apply_modifiers(is);
    sf.image_space = is;
    sf.image_space_valid = true;
    sf.sky_scale = k_framebuffer_range_rcp * is.sky_scale;
    sf.dof_center_weight = 1.0f; // fDDOFFocusCenterweightInt
}

std::span<const uint32_t> Sky::sky_statics(bool outgoing) const {
    const osa::Weather* w = outgoing ? m_outgoing : m_current;
    if (!w) {
        return {};
    }
    return std::span(m_atm->sky_statics).subspan(w->sky_statics_first, w->sky_statics_count);
}

std::string Sky::sky_model() const {
    const osa::Weather* w = m_current;
    if (w && w->sky_model == container::k_no_string && m_outgoing) {
        w = m_outgoing;
    }
    if (!w || w->sky_model == container::k_no_string) {
        return {};
    }
    std::string path(m_atm->string(w->sky_model));
    return path.substr(0, path.size() - 3) + "osm"; // the converted model
}

float Sky::sky_model_fade() const {
    const auto& gs = m_atm->settings;
    const bool cur = m_current && m_current->sky_model != container::k_no_string;
    const bool out = m_outgoing && m_outgoing->sky_model != container::k_no_string;
    if (!cur && !out) {
        return 0.0f;
    }
    float transition = 1.0f;
    if (m_outgoing && !(cur && out && m_current->sky_model == m_outgoing->sky_model)) {
        transition = cur ? m_transition : 1.0f - m_transition;
    }
    const osa::Weather* w = cur ? m_current : m_outgoing;
    float time = 1.0f;
    if (!(w->data[osa::data_flags] & osa::weather_aurora_permanent)) {
        const float t = m_hour;
        if (t >= gs.aurora_fade_in || t <= gs.aurora_fade_out_start) {
            time = 1.0f;
        } else if (t >= gs.aurora_fade_in_start) {
            time = (t - gs.aurora_fade_in_start) / (gs.aurora_fade_in - gs.aurora_fade_in_start);
        } else if (t <= gs.aurora_fade_out) {
            time = (gs.aurora_fade_out - t) / (gs.aurora_fade_out - gs.aurora_fade_out_start);
        } else {
            time = 0.0f;
        }
    }
    return time * transition;
}

std::optional<glm::vec3> Sky::weather_colour(uint32_t weather, int colour) const {
    const osa::Weather* w = m_atm->weather(weather);
    if (!m_climate || !w) {
        return std::nullopt;
    }
    const Keys keys = time_keys(climate_times(*m_climate), m_atm->settings.daytime_color_extension, m_hour);
    glm::vec3 c;
    for (int i = 0; i < 3; ++i) {
        c[i] = (float(w->colours[colour][keys.k][i]) * keys.w + float(w->colours[colour][keys.n][i]) * (1.0f - keys.w)) *
               k_byte_to_unit;
    }
    return glm::min(c, glm::vec3(1.0f));
}

bool Sky::evaluate(render::Environment& env, int mode) const {
    if (!m_climate || !m_current) {
        return false;
    }
    const auto& gs = m_atm->settings;
    const float t = m_hour;
    const Times times = climate_times(*m_climate);
    const Weights ww = weights();
    const float flash = m_outgoing ? 0.0f : m_flash;
    const glm::vec3 lightning = glm::vec3(m_current->data[osa::data_lightning_color],
                                          m_current->data[osa::data_lightning_color + 1],
                                          m_current->data[osa::data_lightning_color + 2]) *
                                k_byte_to_unit;

    // Colours (6.3.2): the four weights, clamped to at most 1; lightning adds to Sunlight and Ambient, then clamps
    // to [0, the weather's lightning colour].
    auto colour = [&](int type) {
        glm::vec3 c(0.0f);
        ww.each([&](const osa::Weather& w, int time, float weight, bool) {
            for (int i = 0; i < 3; ++i) {
                c[i] += float(w.colours[type][time][i]) * weight * k_byte_to_unit;
            }
        });
        if (flash > 0.0f && (type == osa::colour_sunlight || type == osa::colour_ambient)) {
            c += (type == osa::colour_sunlight ? gs.flash_directional : gs.flash_ambient) * flash;
            return glm::clamp(c, glm::vec3(0.0f), lightning);
        }
        return glm::min(c, glm::vec3(1.0f));
    };
    const glm::vec3 sunlight = colour(osa::colour_sunlight);
    m_water_multiplier = colour(osa::colour_water_multiplier);

    // Sun (6.3.3): raw climate times, a parameter running +1 -> -1 over the day and back over the night.
    const float a_trans = gs.sun_alpha_trans_time;
    const float r_mid = (times.sunrise_begin + times.sunrise_end) * 0.5f;
    const float s_mid = (times.sunset_begin + times.sunset_end) * 0.5f;
    const float start = r_mid - a_trans * 0.5f;
    const float end = s_mid + a_trans * 0.5f;
    float p;
    if (t > start && t < end) {
        p = 1.0f - 2.0f * (t - start) / (end - start);
    } else {
        const float d = t >= end ? t - end : 24.0f - end + t;
        p = 2.0f * d / (24.0f - (end - start)) - 1.0f;
    }
    const float sun_x = p * gs.sun_x_extreme;
    {
        const float rho = gs.sun_dir_x_extreme / gs.sun_x_extreme;
        glm::vec3 dir(rho * sun_x, rho * gs.sun_y_extreme, -gs.sun_z_extreme);
        const float len = glm::length(dir);
        env.sun_dir = len > 1e-6f ? dir / len : glm::vec3(0.0f);
    }
    float sun_alpha = 0.0f;
    if (t > start && t < end) {
        const float up = r_mid + a_trans * 0.5f, down = s_mid - a_trans * 0.5f;
        sun_alpha = t < up ? (t - start) / (up - start) : t > down ? (end - t) / (end - down) : 1.0f;
    }

    // The water's sun: the weather's glare over the sun's fade.
    m_sun_glare = 0.0f;
    ww.each([&](const osa::Weather& w, int, float weight, bool) {
        m_sun_glare += float(w.data[osa::data_sun_glare]) / 255.0f * weight;
    });
    m_sun_glare *= sun_alpha;

    // Image space: the weighted sum of whole IMGS blocks, then the modifiers.
    render::ImageSpaceData is;
    {
        float acc[20] = {};
        ww.each([&](const osa::Weather& w, int time, float weight, bool) {
            // The water the camera is in replaces the weather's.
            const osa::ImageSpace* src = m_water_image_space ? m_atm->image_space(m_water_image_space) : nullptr;
            if (!src) {
                src = m_atm->image_space(w.image_spaces[time]);
            }
            if (!src) {
                src = m_atm->image_space(k_default_image_space);
            }
            if (!src) {
                return;
            }
            const float* f = src->hdr; // hdr[9], cinematic[3], tint[4], dof[4]: 20 contiguous floats
            for (int i = 0; i < 20; ++i) {
                acc[i] += f[i] * weight;
            }
        });
        float* d = &is.eye_adapt_speed;
        for (int i = 0; i < 13; ++i) {
            d[i] = acc[i]; // hdr, cinematic, tint amount
        }
        is.tint = glm::vec3(acc[13], acc[14], acc[15]);
        is.dof = glm::vec4(acc[16], acc[17], acc[18], acc[19]);
    }
    apply_modifiers(is);

    // Sun colour (6.3.4): Sunlight x the image space's Sunlight Scale.
    env.dir_light_color = sunlight * is.sunlight_scale;

    // Directional ambient (6.3.5): rows ((C1 - C0)/2, (C3 - C2)/2, (C5 - C4)/2, mean) per channel.
    {
        glm::vec3 c[6] = {};
        glm::vec3 spec(0.0f);
        float fresnel = 0.0f;
        ww.each([&](const osa::Weather& w, int time, float weight, bool) {
            for (int i = 0; i < 6; ++i) {
                for (int ch = 0; ch < 3; ++ch) {
                    c[i][ch] += float(w.ambient[time].colours[i][ch]) * weight * k_byte_to_unit;
                }
            }
            for (int ch = 0; ch < 3; ++ch) {
                spec[ch] += float(w.ambient[time].specular[ch]) * weight * k_byte_to_unit;
            }
            fresnel += w.ambient[time].fresnel_power * weight;
        });
        for (int ch = 0; ch < 3; ++ch) {
            const float sum = c[0][ch] + c[1][ch] + c[2][ch] + c[3][ch] + c[4][ch] + c[5][ch];
            env.dir_ambient[ch] = glm::vec4((c[1][ch] - c[0][ch]) * 0.5f, (c[3][ch] - c[2][ch]) * 0.5f,
                                            (c[5][ch] - c[4][ch]) * 0.5f, sum / 6.0f);
        }
        env.amb_spec = glm::vec4(spec, fresnel > 1.0f ? fresnel : 1.0f);
    }

    // Fog (6.3.6): a day factor over the extended sunrise and sunset, then each weather's values blended by T.
    {
        const float srb = std::max(times.sunrise_begin - gs.daytime_color_extension, 0.0f);
        const float sse = std::min(times.sunset_end + gs.daytime_color_extension, 23.99f);
        float v;
        if (t <= srb || t >= sse) {
            v = 0.0f;
        } else if (t < times.sunrise_end) {
            v = (t - srb) / (times.sunrise_end - srb);
        } else if (t <= times.sunset_begin) {
            v = 1.0f;
        } else {
            v = (sse - t) / (sse - times.sunset_begin);
        }
        auto blend = [&](int day_index, int night_index) {
            auto one = [&](const osa::Weather& w) { return (1.0f - v) * w.fog[night_index] + v * w.fog[day_index]; };
            return m_outgoing ? one(*m_current) * m_transition + one(*m_outgoing) * (1.0f - m_transition)
                              : one(*m_current);
        };
        const float near = blend(0, 2), far = blend(1, 3), power = blend(4, 5), max = blend(6, 7);
        if ((near == 0.0f && far == 0.0f) || m_fog_off) {
            env.fog_param = glm::vec4(5000000.0f, 0.1f, 1.0f, 0.0f);
        } else {
            env.fog_param = glm::vec4(near / (far - near), 1.0f / (far - near), power, max);
        }
        const glm::vec3 fog_near = colour(osa::colour_fog_near);
        const glm::vec3 fog_far = colour(osa::colour_fog_far);
        env.fog_near_color = glm::vec4(fog_near, k_framebuffer_range_rcp);
        env.fog_far_color = glm::vec4(fog_far, 0.0f);
        env.fog_color = env.fog_near_color;
        // Water still reads these (another workstream): fog distances in metres, sky colours.
        env.fog_start = units::to_metres(near);
        env.fog_end = units::to_metres(std::max(far, near + 1.0f));
        env.fog_power = power;
        env.fog_max = max;
        env.sky_horizon = colour(osa::colour_horizon);
        env.sky_zenith = colour(osa::colour_sky_upper);
        env.sky_ground = colour(osa::colour_sky_lower);
        env.sun_color = sunlight;
    }

    env.ambient_colour = colour(osa::colour_ambient);
    env.wind_speed = m_current->data[osa::data_wind_speed] / 255.0f * m_transition +
                     (m_outgoing ? m_outgoing->data[osa::data_wind_speed] / 255.0f * (1.0f - m_transition) : 0.0f);
    // Effect lighting (DLightColor of effect-lit effect shaders): the Effect Lighting colour x Sunlight Scale.
    env.effect_light = colour(osa::colour_effect_lighting) * is.sunlight_scale;
    env.ibl = glm::vec4(0.0f, sunlight); // IBLParams: x = 0 unless the console's ConfigureIBL changes it
    env.output_clamp = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f); // fLightingOutputColourClampPost{Lit,Env,Spec}:General
    // The renderer's per-frame values in the main pass (cb12[42]), captured in game: the output stage's fog term
    // is weighted 1 and subtracted again with weight 0, so lit surfaces leave the shader unfogged (5.6).
    env.frame_params = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    env.skyrim_inputs = true;

    // ---- The sky frame -------------------------------------------------------------------------------------
    render::SkyFrame& sf = env.sky;
    sf.exterior = true;
    sf.draw_count = 0;
    sf.image_space = is;
    sf.image_space_valid = true;
    sf.reset_adaptation = m_reset_adaptation;
    m_reset_adaptation = false;
    sf.dof_center_weight = m_dof_weight;
    sf.sky_scale = k_framebuffer_range_rcp * is.sky_scale;
    sf.sunlight = sunlight;
    sf.transition = m_outgoing ? m_transition : 1.0f;
    sf.sky_statics = colour(osa::colour_sky_statics);
    auto add = [&](const render::SkyDraw& d) {
        if (sf.draw_count < sf.draws.size()) {
            sf.draws[sf.draw_count++] = d;
        }
    };
    const auto quad_first = static_cast<uint32_t>(m_atm->sky_indices.size());
    auto model_shapes = [&](std::string_view path) -> std::span<const osa::SkyShape> {
        const osa::SkyModel* m = m_atm->sky_model(path);
        if (!m) {
            return {};
        }
        return std::span(m_atm->sky_shapes).subspan(m->first_shape, m->shape_count);
    };
    auto tex = [&](uint32_t s) { return s == container::k_no_string ? 0u : m_textures(m_atm->string(s)); };

    // Stars (Stars::Update): alpha by the extended climate times, rotation about the GMST axis.
    float stars_alpha;
    {
        const float srb = std::max(times.sunrise_begin - gs.daytime_color_extension, 0.0f);
        const float sse = std::min(times.sunset_end + gs.daytime_color_extension, 23.99f);
        const float mr = times.sunrise_end - (times.sunrise_end - srb) * 0.5f;
        const float ms = sse - (sse - times.sunset_begin) * 0.5f;
        if (t > srb && t < mr) {
            stars_alpha = (mr - t) / (mr - srb);
        } else if (t >= mr && t <= ms) {
            stars_alpha = 0.0f;
        } else if (t > ms && t < sse) {
            stars_alpha = (t - ms) / (sse - ms);
        } else {
            stars_alpha = 1.0f;
        }
    }

    // 1. Atmosphere (Atmosphere::Update): Horizon, Sky-Lower, Sky-Upper weighted by the vertex colour.
    for (const auto& s : model_shapes("meshes/sky/atmosphere.nif")) {
        if (s.hidden) {
            continue;
        }
        render::SkyDraw d;
        d.technique = render::sky_tech_dome;
        d.first_index = s.first_index;
        d.index_count = s.index_count;
        d.color[0] = rgba(colour(osa::colour_horizon), 1.0f);
        d.color[1] = rgba(colour(osa::colour_sky_lower), 1.0f);
        d.color[2] = rgba(colour(osa::colour_sky_upper), 1.0f);
        add(d);
    }

    // Moons (Moon::Update): angle = 90 + speed x 60 x hours; Masser from the hour, Secunda from the
    // hours since the session began. Phases by whole days.
    struct MoonDraw {
        glm::mat4 world;
        float alpha;
        float shadow_alpha;
        uint32_t texture;
    };
    MoonDraw moons[2] = {};
    int moon_count = 0;
    const uint8_t moon_bits = m_climate->timing[5];
    const int phase_length = std::max<int>(moon_bits & 0x3F, 1);
    const int phase = static_cast<int>(std::fmod(std::floor(m_days), 8.0 * phase_length)) / phase_length;
    static constexpr const char* k_phases[8] = {"full", "three_wan", "half_wan", "one_wan", "new", "one_wax",
                                                "half_wax", "three_wax"};
    const glm::vec3 moon_glare = colour(osa::colour_moon_glare);
    for (int m = 1; m >= 0; --m) { // Secunda, then Masser
        if (!(moon_bits & (m == 0 ? 0x80 : 0x40))) {
            continue;
        }
        const double hours = m == 0 ? double(m_hour) : double(m_hour) + 24.0 * m_day_wraps;
        const float theta = static_cast<float>(std::fmod(90.0 + gs.moon_speed[m] * 60.0 * hours, 360.0));
        const float fs = gs.moon_fade_start[m], fe = gs.moon_fade_end[m];
        auto fade = [&](float end) {
            if (theta > 180.0f || theta < end) {
                return 0.0f;
            }
            if (theta < fs) {
                return (theta - end) / (fs - end);
            }
            if (theta <= 180.0f - fs) {
                return 1.0f;
            }
            return std::clamp((180.0f - end - theta) / (fs - end), 0.0f, 1.0f);
        };
        const float alpha = fade(fe);
        const float shadow = std::min(stars_alpha, fade(fe - gs.moon_shadow_early_fade[m]));
        if ((alpha <= 0.0f && shadow <= 0.0f) || moon_glare == glm::vec3(0.0f)) {
            continue;
        }
        const float a = glm::radians(-theta), ph = glm::radians(gs.moon_z_offset[m]);
        glm::mat4 root(1.0f);
        const glm::vec3 rows[3] = {{std::cos(ph), std::sin(ph), 0.0f},
                                   {-std::cos(a) * std::sin(ph), std::cos(a) * std::cos(ph), std::sin(a)},
                                   {std::sin(a) * std::sin(ph), -std::sin(a) * std::cos(ph), std::cos(a)}};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                root[c][r] = rows[r][c];
            }
        }
        const glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 512.0f, 0.0f)) *
                                glm::rotate(glm::mat4(1.0f), k_pi * 0.5f, glm::vec3(1, 0, 0)) *
                                glm::scale(glm::mat4(1.0f), glm::vec3(gs.moon_size[m]));
        const std::string path = std::format("textures/sky/{}_{}.dds", m == 0 ? "masser" : "secunda", k_phases[phase]);
        moons[moon_count++] = {root * local, alpha, shadow, m_textures(path)};
    }
    // 2. Moon masks: they hide the stars behind the discs (MoonAndStarsMask writes depth only).
    const uint32_t moon_shadow = m_textures("textures/sky/moonshadow.dds");
    for (int i = 0; i < moon_count; ++i) {
        if (moons[i].shadow_alpha <= 0.0f) {
            continue;
        }
        render::SkyDraw d;
        d.technique = render::sky_tech_mask;
        d.first_index = quad_first;
        d.index_count = 6;
        d.texture0 = moon_shadow;
        d.world = moons[i].world;
        d.depth = k_mask_depth;
        d.color[0] = rgba(colour(osa::colour_sky_upper), moons[i].shadow_alpha);
        add(d);
    }
    // 3. Stars, in reverse file order.
    const glm::vec3 stars_colour = colour(osa::colour_stars);
    if (stars_alpha > 0.0f && stars_colour != glm::vec3(0.0f)) {
        const double d = std::floor(m_days) + m_hour / 24.0;
        const float angle = static_cast<float>(2.0 * std::numbers::pi *
                                               std::fmod(d, double(gs.stars_rotate_days)) / gs.stars_rotate_days);
        const glm::vec3 axis = glm::normalize(
            glm::vec3(gs.stars_rotate_axis[0], gs.stars_rotate_axis[1], gs.stars_rotate_axis[2]));
        // The game (its rows as given in the design; World = M p).
        const float c = std::cos(angle), s = std::sin(angle), tt = 1.0f - c;
        const float x = axis.x, y = axis.y, z = axis.z;
        const glm::vec3 rows[3] = {{x * x * tt + c, x * y * tt + s * z, x * z * tt - s * y},
                                   {x * y * tt - s * z, y * y * tt + c, y * z * tt + s * x},
                                   {x * z * tt + s * y, y * z * tt - s * x, z * z * tt + c}};
        glm::mat4 rot(1.0f);
        for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) {
                rot[k][r] = rows[r][k];
            }
        }
        const auto shapes = model_shapes(m_atm->string(m_climate->stars_model));
        for (size_t i = shapes.size(); i-- > 0;) {
            const auto& sh = shapes[i];
            if (sh.hidden) {
                continue;
            }
            render::SkyDraw dr;
            dr.technique = render::sky_tech_stars;
            dr.first_index = sh.first_index;
            dr.index_count = sh.index_count;
            dr.texture0 = tex(sh.texture);
            dr.world = rot;
            dr.color[0] = rgba(stars_colour, stars_alpha);
            add(dr);
        }
    }
    // 4. Moon discs (Secunda, Masser): Moon Glare, additive.
    for (int i = 0; i < moon_count; ++i) {
        if (moons[i].alpha <= 0.0f) {
            continue;
        }
        render::SkyDraw d;
        d.technique = render::sky_tech_texture;
        d.first_index = quad_first;
        d.index_count = 6;
        d.texture0 = moons[i].texture;
        d.world = moons[i].world;
        d.depth = k_disc_depth;
        d.color[0] = rgba(moon_glare, moons[i].alpha);
        d.moon = 1;
        add(d);
    }
    // this flag copies the sun node's local translate onto the model.
    sf.model_offset = (m_current->data[osa::data_flags] & osa::weather_aurora_follows_sun)
                          ? glm::vec3(sun_x, gs.sun_y_extreme, std::abs(gs.sun_x_extreme) - std::abs(sun_x))
                          : glm::vec3(0.0f);
    // 5. Sun disc (Sun::Update): fSunBoost x Sun at the sun alpha, a camera-facing quad of half-size fSunBaseSize at
    // (x, fSunYExtreme, |fSunXExtreme| - |x|). The game draws it at alpha 0 too: nothing reaches the image.
    if (sun_alpha > 0.0f && mode == 3) { // The game culls the sun in mode 2
        const glm::vec3 pos(sun_x, gs.sun_y_extreme, std::abs(gs.sun_x_extreme) - std::abs(sun_x));
        const glm::vec3 zaxis = -glm::normalize(pos);
        const glm::vec3 up_ref = std::abs(zaxis.z) > 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
        const glm::vec3 xaxis = glm::normalize(glm::cross(up_ref, zaxis));
        const glm::vec3 yaxis = glm::cross(zaxis, xaxis);
        glm::mat4 w(1.0f);
        w[0] = glm::vec4(xaxis * k_sun_base_size, 0.0f);
        w[1] = glm::vec4(yaxis * k_sun_base_size, 0.0f);
        w[2] = glm::vec4(zaxis, 0.0f);
        w[3] = glm::vec4(pos, 1.0f);
        render::SkyDraw d;
        d.technique = render::sky_tech_texture;
        d.first_index = quad_first;
        d.index_count = 6;
        d.texture0 = tex(m_climate->sun_texture);
        d.world = w;
        d.depth = k_disc_depth;
        d.color[0] = rgba(k_sun_boost * colour(osa::colour_sun), sun_alpha);
        add(d);
    }
    // 6. Clouds, by layer: colour and alpha per layer (UpdateColors), the texture cross-fade of a transition.
    {
        auto layer_of = [](const osa::Weather& w, int i) {
            return (w.cloud_layer_count == 0 || static_cast<uint32_t>(i) < w.cloud_layer_count) ? i : 0;
        };
        auto layer_colour = [&](const osa::Weather& w, int i, int time) {
            if (w.cloud_layer_count == 0) {
                return glm::vec3(k_default_cloud[time][0], k_default_cloud[time][1], k_default_cloud[time][2]);
            }
            const auto& c = w.clouds[layer_of(w, i)].colours[time];
            return glm::vec3(c[0], c[1], c[2]);
        };
        auto layer_alpha = [&](const osa::Weather& w, int i, int time) {
            return w.cloud_layer_count == 0 ? 1.0f : w.clouds[layer_of(w, i)].alphas[time];
        };
        auto enabled = [](const osa::Weather* w, int i) {
            return w && i < osa::k_cloud_layers && !(w->clouds_disabled & (1u << i));
        };
        const auto shapes = model_shapes("meshes/sky/clouds.nif");
        std::vector<const osa::SkyShape*> order;
        for (const auto& s : shapes) {
            if (s.type == osa::sky_clouds && !s.hidden) {
                order.push_back(&s);
            }
        }
        std::stable_sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->layer < b->layer; });
        for (const osa::SkyShape* s : order) {
            const int i = static_cast<int>(s->layer);
            const bool cur = enabled(m_current, i);
            const bool out = enabled(m_outgoing, i);
            if (!cur && !out) {
                continue;
            }
            glm::vec3 c(0.0f);
            float a = 0.0f;
            ww.each([&](const osa::Weather& w, int time, float weight, bool outgoing) {
                glm::vec3 lc = layer_colour(w, i, time);
                if (lc == glm::vec3(0.0f)) {
                    const osa::Weather* other = outgoing ? m_current : m_outgoing;
                    if (other) {
                        lc = layer_colour(*other, i, time);
                    }
                }
                c += lc * weight * k_byte_to_unit;
                a += layer_alpha(w, i, time) * weight;
            });
            c = glm::min(c, glm::vec3(1.0f));
            if (i == 0 && flash > 0.0f) {
                c = glm::clamp(c + flash, glm::vec3(0.0f), lightning);
            }
            render::SkyDraw d;
            d.first_index = s->first_index;
            d.index_count = s->index_count;
            d.depth = k_disc_depth;
            d.color[0] = rgba(c, a);
            d.uv_offset = glm::vec4(m_cloud_uv[i][0], m_cloud_uv[i][1], 0.0f, 0.0f);
            const float blend = std::max(m_transition, 1e-5f);
            if (cur && out && m_outgoing) {
                d.technique = render::sky_tech_clouds_lerp;
                d.texture0 = tex(m_outgoing->clouds[i].texture);
                d.texture1 = tex(m_current->clouds[i].texture);
                d.blend = blend;
            } else if (cur && m_outgoing) {
                d.technique = render::sky_tech_clouds_fade;
                d.texture0 = tex(m_current->clouds[i].texture);
                d.blend = blend;
            } else if (out) {
                d.technique = render::sky_tech_clouds_fade;
                d.texture0 = tex(m_outgoing->clouds[i].texture);
                d.blend = 1.0f - blend;
            } else {
                d.technique = render::sky_tech_clouds;
                d.texture0 = tex(m_current->clouds[i].texture);
            }
            add(d);
        }
    }

    // Volumetric lighting: the VOLI records with the four weights,
    // the INI defaults for a missing one; the colour leans from Sunlight to the record's by its contribution.
    {
        osa::Volumetric v{};
        float* acc = &v.intensity;
        ww.each([&](const osa::Weather& w, int time, float weight, bool) {
            const osa::Volumetric* src = m_atm->volumetric(w.volumetric[time]);
            const float* s = &(src ? *src : k_default_volumetric).intensity;
            for (int i = 0; i < 12; ++i) {
                acc[i] += s[i] * weight;
            }
        });
        auto& vl = sf.volumetric;
        vl.enabled = !m_world_map; // none under g_HDRWorldMapMode
        vl.intensity = v.intensity;
        vl.custom_color_contribution = v.custom_color_contribution;
        vl.color = glm::mix(sunlight, glm::vec3(v.red, v.green, v.blue), v.custom_color_contribution);
        vl.density_contribution = v.density_contribution;
        vl.density_size = v.density_size;
        vl.wind_speed = v.density_wind_speed;
        vl.falling_speed = v.density_falling_speed;
        vl.phase_contribution = v.phase_contribution;
        vl.phase_scattering = v.phase_scattering;
        vl.range = v.sampling_range;
        vl.wind_offset = m_vl_wind;
    }

    // Precipitation amounts (Precipitation::Update): incoming rain starts at DATA[6] of the transition, outgoing
    // ends at DATA[7].
    m_rain_ripples = 0.0f;
    // A script's particles draw in the outgoing weather's place, which is empty but for a change of weather.
    const ScriptPrecip* scripted = m_script_precip.empty() ? nullptr : &m_script_precip.back();
    for (int i = 0; i < 2; ++i) {
        auto& out = sf.precipitation[i];
        out = {};
        const bool extra = i == 1 && !m_precip[1].spgd && scripted;
        const PrecipState& ps = extra ? scripted->state : m_precip[i];
        if (!ps.spgd) {
            continue;
        }
        const float density = std::abs(ps.spgd->density) <= 1e-4f ? 1.0f : ps.spgd->density;
        if (extra) {
            out.amount = density * scripted->amount;
        } else if (i == 0) {
            const float a = m_current->data[osa::data_precip_fade_in] * 0.999f / 255.0f;
            out.amount = m_transition > a ? density * (m_transition - a) / (1.0f - a) : 0.0f;
        } else if (m_outgoing) {
            const float b = m_outgoing->data[osa::data_precip_fade_out] * 0.999f / 255.0f + 0.001f;
            out.amount = m_transition < b ? density * (1.0f - m_transition / b) : 0.0f;
        }
        const auto& s = *ps.spgd;
        // Rain's ripples on water (Precipitation::Update): from 0.2 past the rain's start.
        if (s.type == 0 && !extra) {
            const float start = i == 0 ? m_current->data[osa::data_precip_fade_in] * 0.999f / 255.0f
                                       : (m_outgoing ? 1.0f - (m_outgoing->data[osa::data_precip_fade_out] * 0.999f / 255.0f + 0.001f) : 1.0f);
            const float at = i == 0 ? m_transition : 1.0f - m_transition;
            m_rain_ripples = std::max(m_rain_ripples, std::clamp((at - (start + 0.2f)) / (1.0f - (start + 0.2f)), 0.0f, 1.0f));
        }
        out.snow = s.type == 1 ? 1u : 0u;
        out.texture = tex(s.texture);
        out.seed = ps.seed;
        out.box = s.box_size;
        out.gravity = s.gravity;
        out.rotation_velocity = s.rotation_velocity;
        out.size_x = s.size_x;
        out.size_y = s.size_y;
        out.offset_min = s.offset_min;
        out.offset_max = s.offset_max;
        out.rotation_range = s.rotation_range;
        out.subtextures_x = std::max(s.subtextures_x, 1u);
        out.subtextures_y = std::max(s.subtextures_y, 1u);
        const float wind_speed =
            m_current->data[osa::data_wind_speed] / 255.0f * m_transition +
            (m_outgoing ? m_outgoing->data[osa::data_wind_speed] / 255.0f * (1.0f - m_transition) : 0.0f);
        out.wind = gs.precip_wind_mult * wind_speed *
                   glm::vec3(std::cos(m_wind_direction), std::sin(m_wind_direction), 0.0f);
        for (int c = 0; c < 3; ++c) {
            out.scroll[c] = std::fmod(ps.scroll[c] - m_precip_camera[c], s.box_size); // C fmod
        }
        out.rotation = ps.rotation;
        for (int k = 0; k < 10; ++k) {
            out.instance_offset[k] = ps.instance_offset[k];
        }
    }
    // A Show Sky cell without Use Sky Lighting keeps its weather and the weather's geometry, but none of it shows: a storm
    // in full in Bleak Falls Barrow's first hall draws no flake there.
    if (mode != 3) {
        sf.precipitation = {};
    }
    return true;
}

} // namespace spud
