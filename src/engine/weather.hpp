#pragma once

// The sky: game time, the climate's weather choice and transitions, exterior lighting (
// section 6) and everything the renderer draws of the sky and its image space. One object per
// session, like the game's Sky singleton.

#include "engine/game_clock.hpp"
#include "formats/osa.hpp"
#include "formats/osw.hpp"
#include "render/view.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace spud {

class Sky {
public:
    // Resolves a texture cache path to a bindless slot (TextureManager::acquire); 0 when absent.
    using TextureResolver = std::function<uint32_t(std::string_view path)>;

    Sky(const osa::Atmosphere& atmosphere, TextureResolver textures);

    const osa::Atmosphere& atmosphere() const { return *m_atm; }
    // The sky mesh the renderer draws from (Renderer::set_sky_mesh): the imported sky models and a unit quad for
    // the sun and moons.
    std::vector<render::SkyVertex> mesh_vertices() const;
    std::vector<uint32_t> mesh_indices() const;

    void set_world(std::string_view world_edid); // its climate; "" or an unknown world: no sky
    bool has_climate() const { return m_climate != nullptr; }

    // Console fw: the weather at once, no transition. 0 = the climate's first weather.
    void force_weather(uint32_t form_id);
    // ShaderParticleGeometry.Apply and Remove: particles of a script's, over the weather's, one per form.
    void apply_particles(uint32_t form_id, float fade_in);
    void remove_particles(uint32_t form_id, float fade_out);
    uint32_t find_weather(int type) const; // Weather.FindWeather: of 0 pleasant, 1 cloudy, 2 rainy, 3 snow
    // The world map: a weather held at once, and what was in force put back as it was.
    void hold_weather(uint32_t form_id);
    void release_weather();
    // In an interior no cell changes the weather region; a Show Sky interior's own region gives `weather`, 0 for none.
    // `mode`: the game's, 1 no sky, 2 Show Sky, 3 fully exterior.
    void set_interior(bool interior, int mode, uint32_t weather);
    // An exterior seen through a door: its own weather, whatever a Show Sky interior around the camera has.
    bool evaluate_outdoors(render::Environment& env);
    // A Show Sky interior: its own weather, also while it is seen through a door from outside.
    bool evaluate_interior(render::Environment& env, int mode, uint32_t weather);
    // Console sw: a transition from the current weather; `accelerate` as the weather change
    // script functions do.
    void set_weather(uint32_t form_id, bool accelerate = false);
    uint32_t weather() const;          // current weather FormID, 0 = none
    // The game's pick on arriving under a region (XCCM): weighted, the current one kept with its chance. 0: unknown.
    uint32_t pick_for_region(uint32_t region_form);
    uint32_t region() const { return m_region ? m_region->form_id : 0; } // the region the weather came from
    uint32_t outgoing_weather() const; // 0 = none
    float transition() const { return m_transition; }

    // The sky reads the game's clock and keeps no time of its own.
    void set_clock(const GameClock& clock);
    float hour() const { return m_hour; }

    // Checks: the cloud layers as they stand `seconds` of still weather after the game started counting.
    void set_cloud_clock(float seconds);

    // An image-space modifier (IMAD) by EditorID at `strength`, from now (ImageSpaceModifierInstanceForm).
    bool apply_modifier(std::string_view edid, float strength = 1.0f, float fade_in = 0.0f);
    // nothing if the modifier already runs.
    bool apply_modifier_unique(std::string_view edid, float strength = 1.0f);
    // Its instances go, over `fade_out` seconds; every Apply made one.
    void remove_modifier(std::string_view edid, float fade_out = 0.0f);
    void release_override() { m_override = nullptr; } // Weather.ReleaseOverride
    void clear_modifiers() { m_modifiers.clear(); }
    // DefaultObject 170 (IMID, ISMTween in Skyrim.esm) from the count's 0 to 1 until its 1 to 0.
    void set_ui_blur(bool on);
    // The world map is up: g_HDRWorldMapMode's boosts (fGlobalMap*:Display), no volumetric lighting.
    void set_world_map(bool on) { m_world_map = on; }
    void set_fog_off(bool off) { m_fog_off = off; } // the console's tfog
    size_t modifier_count() const { return m_modifiers.size(); }

    // Advances the frame: `dt` real seconds (0 while paused or in scripted runs, so their images are reproducible),
    // `frame_dt` the frame's length even then, which the focus weight eases with;
    // the camera's world position (game units) and orientation for the dynamic depth of field's focus weight.
    void update(const GameClock& clock, float dt, float frame_dt, const glm::vec3& camera_units, float yaw, float pitch);

    // Fills env's lighting inputs and its sky frame for the current state. False without a climate or a weather
    // (env is left as it was).
    bool evaluate(render::Environment& env, int mode = 3) const;

    // The image space of a place with no sky (DefaultImageSpace 0x167): interiors until their cells carry one.
    void evaluate_indoors(render::Environment& env) const;
    // An interior's image space: `cell`, or a room's in its place, by `weight` from the first to the second.
    void set_cell_image_space(render::Environment& env, uint32_t cell, uint32_t from_room, uint32_t to_room,
                              float weight) const;

    // The STAT bases the current or outgoing weather shows (WTHR TNAM).
    std::span<const uint32_t> sky_statics(bool outgoing) const;

    // The current weather's sky model (aurora,...) and its fade; "" when there is none.
    std::string sky_model() const;
    float sky_model_fade() const;

    // One colour (osa colour index) of `weather` at the current hour, blended between its time-of-day keys
    // (6.3.2) and clamped to at most 1; nullopt without a climate or that weather.
    std::optional<glm::vec3> weather_colour(uint32_t weather, int colour) const;
    glm::vec3 water_multiplier() const { return m_water_multiplier; } // the weather's Water Multiplier, this frame
    float sun_glare() const { return m_sun_glare; }                  // the weather's sun glare, 0..1, this frame
    float rain_ripples() const { return m_rain_ripples; }            // the rain's ripples on water, 0..1, this frame
    void set_water_image_space(uint32_t form) { m_water_image_space = form; } // replaces the weather's; 0 none

private:
    struct Weights; // the four (weather, time) weights of the current state

    Weights weights() const;
    void apply_modifiers(render::ImageSpaceData& is) const;
    void enter_cell(const glm::vec3& position);
    void choose_weather();
    const osa::Weather* pick_weather(uint32_t first, uint32_t count);
    bool lists_weather(uint32_t first, uint32_t count, const osa::Weather* w) const;
    const osa::Weather* default_weather();
    void start_transition(const osa::Weather* next, bool instant, bool accelerate);
    uint32_t next_random();

    const osa::Atmosphere* m_atm;
    TextureResolver m_textures;
    mutable glm::vec3 m_water_multiplier{1.0f}; // set by evaluate()
    mutable float m_sun_glare = 0.0f;
    mutable float m_rain_ripples = 0.0f;
    uint32_t m_water_image_space = 0;
    const osa::Climate* m_climate = nullptr;
    const osa::Weather* m_current = nullptr;
    const osa::Weather* m_outgoing = nullptr;
    float m_transition = 1.0f;
    float m_change_hour = 0.0f;     // hour of the last weather change (Sky)
    const osa::Weather* m_default = nullptr;  // the climate's choice (Sky)
    const osa::Weather* m_override = nullptr; // holds until the period ends (Sky)
    const osa::Region* m_region = nullptr;        // the list the weather came from (Sky)
    const osa::Region* m_player_region = nullptr; // set on entering a cell
    // What hold_weather put aside.
    bool m_held = false;
    const osa::Weather *m_held_current = nullptr, *m_held_override = nullptr, *m_held_default = nullptr;
    const osa::Region* m_held_region = nullptr;
    uint32_t m_world = ~0u;
    bool m_interior = false;
    int m_mode = 3;
    bool evaluate_as(render::Environment& env, int mode, const osa::Weather* weather);
    const osa::Weather* m_interior_weather = nullptr;
    const osa::Weather* m_outdoor_weather = nullptr; // what stands outside while a Show Sky interior has its own
    int32_t m_region_cell_x = 0, m_region_cell_y = 0;
    bool m_region_cell_known = false;
    float m_transition_start = 0.0f; // accelerated transitions: T when the acceleration began
    bool m_accelerated = false;
    mutable bool m_reset_adaptation = false; // consumed by the next evaluate
    float m_hour = 12.0f;
    double m_days = 0.0;   // whole days passed
    int m_day_wraps = 0;          // midnights since then: Secunda's orbit counts every hour (Moon::Update)
    float m_wind_direction = 0.0f; // radians, from +X towards +Y
    float m_cloud_uv[osa::k_cloud_layers][2] = {};
    float m_flash = 0.0f;          // lightning
    float m_flash_age = 1e9f;      // seconds since the flash began
    float m_thunder_clock = 0.0f;  // seconds since the last thunder try
    struct PrecipState {
        const osa::Precipitation* spgd = nullptr;
        glm::vec3 scroll{0.0f};
        float rotation = 0.0f;
        glm::vec3 instance_offset[10] = {};
        uint32_t seed = 0;
    };
    PrecipState m_precip[2]; // current, outgoing
    struct ScriptPrecip {
        uint32_t form = 0;
        PrecipState state;
        float amount = 0.0f, fade = 0.0f;
        bool on = true;
    };
    std::vector<ScriptPrecip> m_script_precip;
    PrecipState new_precip(const osa::Precipitation* spgd);
    glm::vec3 m_precip_camera{0.0f};
    glm::vec3 m_vl_wind{0.0f};
    // Dynamic depth of field.
    float m_dof_weight = 2.0f;
    float m_dof_still = 1e9f; // seconds without movement
    glm::vec3 m_dof_pos{0.0f};
    float m_dof_yaw = 0.0f, m_dof_pitch = 0.0f;
    bool m_dof_init = false;
    struct ModifierInstance {
        const osa::Modifier* modifier;
        float start;
        float strength;
        float fade_in = 0.0f;
        float fade_start = -1.0f, fade_out = 0.0f; // fading out from this clock
    };
    float modifier_fade(const ModifierInstance& m) const;
    std::vector<ModifierInstance> m_modifiers;
    float m_modifier_clock = 0.0f;
    bool m_ui_blur = false;
    bool m_world_map = false;
    bool m_fog_off = false;
    uint32_t m_rng = 0x2545F491u;
};

// An interior's lighting, sky mode 1: the cell's resolved
// XCLL in the shader inputs exterior weather fills. Show Sky interiors keep these too; Use Sky Lighting ones take
// the sky's instead (evaluate_sky).
// `to` and `w`: the blend towards a room's lighting template while the camera changes rooms (every value but the
// direction lerps). A light fade of 0 stands for the settings'.
void evaluate_interior(const osw::CellLighting& lighting, float sunlight_scale, render::Environment& env,
                       const osw::CellLighting* to = nullptr, float w = 0.0f);

} // namespace spud
