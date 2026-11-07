// The game UI's side of the application (ui::UiHost): menu actions, the
// console's command line and aliases, and the player's settings.

#include "engine/app_internal.hpp"
#include "ui/menu.hpp"
#include "ui/player/json.hpp"
#include "ui/menus_internal.hpp"
#include "ui/player/player.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "engine/graphics_settings.hpp"
#include "core/path.hpp"
#include "core/platform.hpp"
#include "core/units.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <set>
#include <fstream>
#include <sstream>

namespace spud::engine {

namespace {

constexpr float k_base_sensitivity = 0.0022f; // radians per pixel at sensitivity 1.0

std::vector<std::string> words_of(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> out;
    for (std::string w; in >> w;) {
        out.push_back(w);
    }
    return out;
}

const char* k_console_help = R"(Commands (docs/AUTOMATION.md): cell <x> <y> [+z], teleport <x> <y> <z> <yaw> <pitch>,
  fly ..., world <edid>, set <name> <value>, state -, wait_loaded, screenshot <file>, quit
Skyrim-style aliases:
  cow <world> <x> <y>   centre on cell (x, y) of a worldspace
  coc <cell>            needs exterior cell names in the import cache (not imported yet)
  tfc                   toggle free-fly mouse look
  tm                    toggle the debug overlay and HUD
  fov <degrees>         field of view
  sucsm <m/s>           free camera speed
  getpos                camera position in Skyrim units
  qqq                   quit at once
  clear                 clear the console
Up/Down recall earlier lines, PageUp/PageDown scroll, ~ or Esc closes.)";

} // namespace

// ---- setup -------------------------------------------------------------------------------

void App::create_ui() {
    // Resolution choices: the display's modes when there is one, a common list otherwise.
    std::set<std::pair<int, int>> sizes; // smallest first: Left lowers, Right raises
    if (m_window) {
        int count = 0;
        if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(m_window), &count)) {
            for (int i = 0; i < count; ++i) {
                if (modes[i]->w >= 1024 && modes[i]->h >= 600) {
                    sizes.emplace(modes[i]->w, modes[i]->h);
                }
            }
            SDL_free(modes);
        }
    }
    if (sizes.empty()) {
        sizes = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    }
    unsigned cw = 0, ch = 0;
    if (std::sscanf(m_user.get("graphics.resolution").c_str(), "%ux%u", &cw, &ch) == 2) {
        sizes.emplace(int(cw), int(ch)); // the saved size stays selectable
    }
    std::vector<std::string> values, labels;
    for (const auto& [w, h] : sizes) {
        values.push_back(std::format("{}x{}", w, h));
        labels.push_back(std::format("{} \xC3\x97 {}", w, h));
    }
    m_user.set_choices("graphics.resolution", std::move(values), std::move(labels));

    const char* base = SDL_GetBasePath();
    const std::filesystem::path exe_dir = base ? base : "";
    ui::UiOptions o;
    o.shader_dir = exe_dir / "shaders";
    o.pipeline_cache = platform::local_app_data() / "spud" / "ui_pipeline_cache.bin";
    o.window = m_window;
    o.interface_dir = m_opt.cache / "interface";
    // What a recorded game showed must not reach a player: it ends with a tutorial hint nothing clears.
    const bool scripted = m_opt.headless || m_opt.control_port || !m_opt.start.empty() || !m_opt.run.empty() ||
                          !m_opt.bench.empty();
    o.hud_recording = m_opt.hud_recording.value_or(scripted);
    m_ui = std::make_unique<ui::Ui>(*m_renderer, *this, o);
    load_control_map();
    {
        // The hour stays as the options set it until a game starts: a view straight into a cell keeps its own.
        const ui::Calendar calendar = m_ui->start_calendar();
        const float hour = m_atmosphere ? m_clock.hour() : float(calendar.hour);
        m_clock.start(float(calendar.year), float(calendar.month), float(calendar.day), hour);
    }
    m_backdrop = std::make_unique<Backdrop>(*m_renderer, m_opt.cache);
    m_local_map = std::make_unique<LocalMap>(*m_renderer, *m_ui);
    m_ui_scenes = std::make_unique<UiScenes>(*m_renderer);
    m_map_scene = std::make_unique<WorldMapScene>(*m_renderer, m_opt.cache.string());
    m_focus.attach(m_ui.get());
}

void App::load_user_settings(bool interactive) {
    if (!m_opt.settings.empty()) {
        m_user_file = m_opt.settings;
    } else if (interactive) {
        m_user_file = platform::local_app_data() / "spud" / "settings.ini";
    }
    prepare_graphics_settings();
    if (!m_user_file.empty()) {
        m_user.load(m_user_file);
        log::info("settings: {}", m_user_file.string());
    }
    rebuild_bindings();
}

void App::rebuild_bindings() {
    static constexpr const char* ids[k_bind_count] = {"controls.forward", "controls.back", "controls.left",
                                                      "controls.right",   "controls.up",   "controls.down",
                                                      "controls.fast",    "controls.look"};
    for (uint32_t b = 0; b < k_bind_count; ++b) {
        m_binds[b] = SDL_GetScancodeFromName(m_user.get(ids[b]).c_str());
    }
}

// The Audio menu's levels (docs/AUDIO.md "Settings"): the master volume and every sound category the data puts on
// the menu, in percent under [audio], with the categories' record defaults (UNAM) until the file sets them.
void App::apply_audio_settings() {
    if (!m_audio || !m_audio->available()) {
        return;
    }
    m_audio->set_master_volume(std::clamp(m_user.number("audio.master"), 0.0f, 100.0f) / 100.0f);
    m_audio->set_hrtf(m_user.flag("audio.hrtf"));
    for (const auto& s : m_audio->menu_sliders()) {
        const std::string key = "audio." + s.key;
        m_user.add_hidden(key, std::format("{}", std::lround(s.default_value * 100.0f)));
        m_audio->set_category_volume(s.category, std::clamp(m_user.number(key), 0.0f, 100.0f) / 100.0f);
    }
}

// After init: what the loaded settings change in a running engine. Options given on the command line win.
void App::apply_startup_settings() {
    apply_audio_settings();
    if (m_user_file.empty()) {
        return;
    }
    for (const char* id : {"graphics.fov", "controls.mouse_sensitivity", "controls.invert_y"}) {
        apply_user_setting(id);
    }
    apply_graphics_settings();
    if (m_window && !m_opt.windowed && !m_opt.width && !m_opt.height &&
        m_user.get("graphics.window_mode") != "windowed") {
        apply_window_settings();
    }
}

void App::save_user_settings() {
    if (m_user_file.empty() || !m_user.dirty()) {
        return;
    }
    try {
        m_user.save(m_user_file);
    } catch (const std::exception& e) {
        log::warn("settings: cannot write {}: {}", m_user_file.string(), e.what());
    }
}

// Applies one setting to the running engine. Window and display settings only where there is a window.
void App::apply_user_setting(const std::string& id) {
    const std::string v = m_user.get(id);
    if (id == "graphics.vsync") {
        set_setting("vsync", v);
    } else if (id == "graphics.fov") {
        set_setting("fov", v);
    } else if (id == "graphics.draw_distance") {
        set_setting("draw_distance", v);
    } else if (id == "graphics.window_mode" || id == "graphics.resolution") {
        apply_window_settings();
    } else if (id == "fLODFadeOutMultObjects:LOD") { // the Display page's sliders
        m_render_settings.lod_fade_out_mult_objects = m_user.number(id);
    } else if (id == "fLODFadeOutMultItems:LOD") {
        m_render_settings.lod_fade_out_mult_items = m_user.number(id);
    } else if (id == "fGrassStartFadeDistance:Grass") {
        m_render_settings.grass_fade_start = m_user.number(id);
    } else if (id == "controls.mouse_sensitivity") {
        m_camera.sensitivity = k_base_sensitivity * m_user.number(id);
    } else if (id == "controls.invert_y") {
        m_invert_y = m_user.flag(id);
    } else if (id.starts_with("controls.")) {
        rebuild_bindings();
    } else if (id.starts_with("audio.")) {
        apply_audio_settings();
    }
}

void App::apply_window_settings() {
    if (!m_window) {
        return;
    }
    const std::string mode = m_user.get("graphics.window_mode");
    unsigned w = 1920, h = 1080;
    std::sscanf(m_user.get("graphics.resolution").c_str(), "%ux%u", &w, &h);
    if (mode == "windowed") {
        SDL_SetWindowFullscreen(m_window, false);
        SDL_SetWindowSize(m_window, int(w), int(h));
        SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else if (mode == "borderless") {
        SDL_SetWindowFullscreenMode(m_window, nullptr); // desktop size, no mode switch
        SDL_SetWindowFullscreen(m_window, true);
    } else {
        SDL_DisplayMode m{};
        if (SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(m_window), int(w), int(h), 0.0f, true, &m)) {
            SDL_SetWindowFullscreenMode(m_window, &m);
        }
        SDL_SetWindowFullscreen(m_window, true);
    }
    SDL_SyncWindow(m_window);
    m_renderer->on_window_resized();
}

// ---- UiHost ------------------------------------------------------------------------------

void App::ui_settings_closed() {
    save_user_settings();
    save_control_map();
}

void App::ui_control_changed(const std::string& event, int device, uint32_t code) {
    m_controls_dirty = true;
    if (event.empty()) {
        m_control_changes.clear();
    } else {
        m_control_changes[{event, device}] = code;
    }
    if (!m_rules) {
        return;
    }
    if (m_default_keys.empty()) {
        m_default_keys = m_rules->keys;
    }
    if (event.empty()) { // back to the defaults
        m_rules->keys = m_default_keys;
        return;
    }
    for (osr::KeyBinding& k : m_rules->keys) {
        if (k.context == 0 && k.event == event) {
            if (device == 0) {
                k.keys = {uint16_t(code), 0xFF};
            } else if (device == 1) {
                k.mouse = uint16_t(code);
            } else {
                k.gamepad = code;
            }
        }
    }
}

// Beside the settings file, one change a line: event, device, code.
void App::load_control_map() {
    if (m_user_file.empty() || !m_ui || !m_rules || m_controls_loaded) {
        return;
    }
    m_controls_loaded = true;
    std::ifstream in(m_user_file.parent_path() / "controlmap_custom.txt");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string event;
        int device = 0;
        uint32_t code = 0;
        if (std::getline(fields, event, '\t') && fields >> device >> code) {
            m_ui->remap(event, device, code);
        }
    }
    m_controls_dirty = false;
}

void App::save_control_map() {
    if (m_user_file.empty() || !m_controls_dirty) {
        return;
    }
    std::ofstream out(m_user_file.parent_path() / "controlmap_custom.txt");
    for (const auto& [key, code] : m_control_changes) {
        out << key.first << '\t' << key.second << ' ' << code << '\n';
    }
    m_controls_dirty = false;
}

bool App::ui_has_saves() {
    return can_load(); // as the main menu decides CONTINUE (sendMenuProperties value 1), on spud's own saves
}

ui::PlayerInfo App::ui_player_info() {
    // A new character (level 1, no progress) at the start of a game. Measured in the game (the game scenario
    // calendar.osc): once a game has started, GameDaysPassed is the hour as a fraction of the day (0.33368 at 8:00).
    ui::PlayerInfo p;
    p.calendar = ui::Calendar{m_clock.year(), m_clock.month(), m_clock.day(), m_clock.hour(), m_clock.days_passed()};
    if (m_game) {
        const game::Character& c = m_game->character();
        p.level = c.level();
        p.progress_percent = c.experience() / c.experience_threshold() * 100.0f;
    }
    return p;
}

bool App::ui_player_pose(float position[3], float& yaw) {
    if (!m_space) {
        return false;
    }
    // The compass follows the camera: its position in Skyrim units and its yaw (clockwise from north).
    const glm::vec3 u = m_camera.position / units::k_skyrim_unit_m;
    position[0] = u.x;
    position[1] = u.y;
    position[2] = u.z;
    yaw = m_camera.yaw;
    return true;
}

// the first test that fails.
bool App::ui_can_rest(bool sleeping) {
    if (!m_game || !m_player) {
        return true;
    }
    const game::Settings& s = m_game->settings();
    const auto refuse = [&](const char* what) {
        m_ui->notification(s.text(std::format("sNo{}{}", sleeping ? "Sleep" : "Wait", what)));
        return false;
    };
    if (m_mount.dragon) {
        return refuse("InAir");
    }
    const forms::FormEntry* cell = m_space && m_space->interior() ? m_game->db().find(player_space()) : nullptr;
    if (m_player->status().swimming || (cell && (cell->flags & 0x80000))) { // CELL "Can't Wait"
        return refuse("InCell");
    }
    if (m_game->crime().trespassing()) {
        return refuse("Trespass");
    }
    if (cell && (uint32_t(forms::Form(m_game->db(), *cell).get("DATA") ? forms::Form(m_game->db(), *cell).get("DATA")->integer : 0) & 0x200)) {
        return refuse("WarnToLeave");
    }
    game::Combat& combat = m_game->combat();
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    const glm::vec3 feet = m_player->feet() / units::k_skyrim_unit_m;
    const float radius = s.number(cell ? "fHostileActorInteriorDistance" : "fHostileActorExteriorDistance", cell ? 2000.0f : 3000.0f);
    bool hostile = false;
    for (const ActorSet::Who& a : actors ? actors->who() : std::vector<ActorSet::Who>{}) {
        const game::Agent* agent = m_game->ai().agent(a.ref);
        if (!agent || combat.fighter(a.ref).dead()) {
            continue;
        }
        if (agent->arresting) {
            return refuse("WhileAlarm");
        }
        const glm::vec3 at(agent->position[0], agent->position[1], agent->position[2]);
        const float apart = glm::length(at - feet);
        const game::Fighter& f = combat.fighter(a.ref);
        const bool against = f.combat_target == game::k_player_ref || m_game->combat_ai().should_attack(a.ref, a.npc, game::k_player_ref, game::k_player_base, apart);
        hostile = hostile || (apart < radius && against);
    }
    if (hostile) {
        return refuse("HostileActorsNear");
    }
    if (m_game->magic_target(game::k_player_ref).causing_damage()) {
        return refuse("TakingHealthDamage");
    }
    if (combat.fighter(game::k_player_ref).knock_state != 0) {
        return refuse("Default");
    }
    return true;
}

bool App::ui_player_sneaking() {
    return m_player && m_walk && m_player->status().sneaking && !m_player->status().swimming;
}

void App::ui_pass_time(double hours) { m_clock.pass_hours(float(hours)); }

bool App::ui_loaded_area(float min[3], float max[3]) {
    if (!m_space) {
        return false;
    }
    if (m_local_map_area) {
        for (int i = 0; i < 3; ++i) {
            min[i] = (*m_local_map_area)[size_t(i)];
            max[i] = (*m_local_map_area)[size_t(i) + 3];
        }
        return true;
    }
    // TES's LoadedAreaBound: the attached cells, the uGridsToLoad square around the player's cell. The game takes its
    // bounds from the Havok world's AABB of those cells (x 69.991249), which spud doesn't have: the
    // cells' squares in x and y, their geometry's in z.
    const float k = units::k_skyrim_unit_m;
    const float cell_units = m_space->cell_size() / k;
    int32_t x0, y0, x1, y1;
    LocalMap::attached_cells(m_camera.position / k, cell_units, m_grids_to_load, x0, y0, x1, y1);
    glm::vec3 lo, hi;
    if (!m_space->area_bounds(x0, y0, x1, y1, lo, hi)) {
        lo = hi = m_camera.position;
    }
    min[0] = float(x0) * cell_units;
    min[1] = float(y0) * cell_units;
    min[2] = lo.z / k;
    max[0] = float(x1 + 1) * cell_units;
    max[1] = float(y1 + 1) * cell_units;
    max[2] = hi.z / k;
    return true;
}

ui::MainMenuState App::ui_main_menu_state() {
    // Measured on this install's real main menu: no Anniversary upsell banner, CREATIONS shown with
    // its icon and disabled (no Creation Club connection).
    return {false, true, false};
}

// The game: an editor ID that names a music type adds it; a sound plays in 2D through DOBJ 151
// "Interface Output Model", SOMUIDefault.
uint32_t App::ui_start_sound(const std::string& descriptor) {
    if (!m_audio) {
        return 0;
    }
    if (const auto music = m_audio->find_music_type(descriptor)) {
        m_audio->music_add(*music);
        return 0;
    }
    const auto sound = m_audio->find_descriptor(descriptor);
    if (!sound) {
        log::debug("ui sound {}: no such sound", descriptor);
        return 0;
    }
    return uint32_t(m_audio->play(*sound, std::nullopt, m_audio->find_output_model("SOMUIDefault").value_or(osd::k_none)));
}

void App::ui_play_sound(const std::string& descriptor) { ui_start_sound(descriptor); }

void App::ui_stop_sound(uint32_t id) {
    if (m_audio && id) {
        m_audio->stop(audio::SoundId(id));
    }
}

namespace {

// Journal > System > Settings: what each page position holds (the option
// table). Sliders are normalised to 0..1 over [min, max]; flags are 0/1; raw values go as they are.
struct OptionDef {
    int id;
    const char* setting; // nullptr: not written on PC
    enum Kind { flag, normalised, raw, autosave } kind;
    double min, max;
};

const std::vector<OptionDef>& option_page(const std::string& page) {
    static const std::vector<OptionDef> gameplay = {
        {0, "controls.invert_y", OptionDef::flag, 0, 1},
        {1, "fMouseHeadingSensitivity:Controls", OptionDef::normalised, 0.01, 0.05},
        {2, "bGamePadRumble:Controls", OptionDef::flag, 0, 1},
        {3, "bGamepadEnable:Controls", OptionDef::flag, 0, 1},
        {4, "bAlwaysRunByDefault:Controls", OptionDef::flag, 0, 1},
        {5, nullptr, OptionDef::flag, 0, 1}, // survival mode: only written when its global is 1
        {6, "iDifficulty:GamePlay", OptionDef::raw, 0, 5},
        {7, "bShowFloatingQuestMarkers:GamePlay", OptionDef::flag, 0, 1},
        {8, "bSaveOnRest:SaveGame", OptionDef::flag, 0, 1},
        {9, "bSaveOnWait:SaveGame", OptionDef::flag, 0, 1},
        {10, "bSaveOnTravel:SaveGame", OptionDef::flag, 0, 1},
        {11, "fAutosaveEveryXMins:SaveGame", OptionDef::autosave, 0, 6},
    };
    static const std::vector<OptionDef> display = {
        {13, "fGamma:Display", OptionDef::normalised, 0.6, 1.4},
        {14, "fHUDOpacity:Interface", OptionDef::raw, 0, 1},
        {15, "fLODFadeOutMultActors:LOD", OptionDef::normalised, 5, 15},
        {16, "fLODFadeOutMultItems:LOD", OptionDef::normalised, 1.5, 10},
        {17, "fLODFadeOutMultObjects:LOD", OptionDef::normalised, 5, 30},
        {18, "fGrassStartFadeDistance:Grass", OptionDef::normalised, 400, 7000},
        {19, nullptr, OptionDef::raw, 0, 1},
        {20, nullptr, OptionDef::raw, 0, 1},
        {21, nullptr, OptionDef::raw, 0, 1},
        {22, nullptr, OptionDef::raw, 0, 1},
        {23, "bCrosshairEnabled:Main", OptionDef::flag, 0, 1},
        {24, "subtitles_24", OptionDef::flag, 0, 1},
        {25, "subtitles_25", OptionDef::flag, 0, 1},
        {26, "fDynamicDOFBlurMultiplier:Display", OptionDef::raw, 0, 1},
    };
    static const std::vector<OptionDef> audio = {
        {27, "audio.master", OptionDef::raw, 0, 1},
    };
    static const std::vector<OptionDef> none;
    return page == "RequestGameplayOptions" ? gameplay
           : page == "RequestDisplayOptions" ? display
           : page == "RequestAudioOptions"   ? audio
                                             : none;
}

constexpr double k_autosave_minutes[] = {5, 10, 15, 30, 45, 60};

// The Audio page's row for headphone spatial audio: spud's, after the game's. Not a form ID or a game option's.
constexpr int k_hrtf_option = 120;

// A sound category's volume (0..1) in spud's settings, by its form ID as the game's SkyrimPrefs.ini keeps it,
// when the audio system doesn't know the category (it keeps the levels of those it plays: App::audio_menu_key).
std::string sound_category_key(uint32_t form_id) { return std::format("audio.category.{:08X}", form_id); }

} // namespace

std::optional<std::string> App::audio_menu_key(uint32_t form_id) const {
    if (m_audio && m_audio->available()) {
        for (const auto& s : m_audio->menu_sliders()) {
            if (s.form_id == form_id) {
                return "audio." + s.key;
            }
        }
    }
    return std::nullopt;
}

std::vector<ui::OptionValue> App::ui_options(const std::string& page) {
    std::vector<ui::OptionValue> out;
    for (const OptionDef& d : option_page(page)) {
        ui::OptionValue v;
        v.id = d.id;
        if (!d.setting) {
            v.written = false;
            out.push_back(v);
            continue;
        }
        const std::string s = d.setting;
        // spud's own settings that Skyrim's pages edit: invert Y (on/off) and the master volume (0..100).
        double value = s == "controls.invert_y" ? (m_user.flag(s) ? 1.0 : 0.0)
                       : s == "audio.master"    ? m_user.number(s) / 100.0
                                                : double(m_user.number(s));
        double def = s == "controls.invert_y" ? 0.0 : s == "audio.master" ? 1.0 : 0.0;
        if (s != "controls.invert_y" && s != "audio.master") {
            UserSettings defaults;
            def = defaults.number(s);
        }
        switch (d.kind) {
        case OptionDef::normalised:
            value = std::clamp((value - d.min) / (d.max - d.min), 0.0, 1.0);
            def = std::clamp((def - d.min) / (d.max - d.min), 0.0, 1.0);
            break;
        case OptionDef::autosave: {
            // 5, 10, 15, 30, 45, 60 minutes -> 0..5; 6 when saving on pause is off.
            auto index = [](double minutes) {
                for (int i = 0; i < 6; ++i) {
                    if (minutes <= k_autosave_minutes[i]) {
                        return double(i);
                    }
                }
                return 5.0;
            };
            value = m_user.number("bSaveOnPause:SaveGame") < 0.5 ? 6.0 : index(value);
            def = 2.0;
            break;
        }
        default:
            break;
        }
        v.value = value;
        v.default_value = def;
        out.push_back(v);
    }
    if (page == "RequestAudioOptions" && m_ui) {
        // Then every sound category shown in the menu, in the game's order, as a new entry {text, ID: form ID,
        // value: its volume, defaultVal: UNAM / 65535}. The game
        // keeps the volumes in SkyrimPrefs.ini by form ID; spud keeps them in its own settings.
        for (const ui::SoundCategory& c : m_ui->sound_categories()) {
            ui::OptionValue v;
            v.id = int(c.form_id);
            v.text = c.name;
            v.appended = true;
            if (auto key = audio_menu_key(c.form_id)) {
                v.value = std::clamp(double(m_user.number(*key)) / 100.0, 0.0, 1.0);
            } else {
                const std::string fallback = sound_category_key(c.form_id);
                m_user.add_hidden(fallback, std::format("{}", c.default_value));
                v.value = std::clamp(double(m_user.number(fallback)), 0.0, 1.0);
            }
            v.default_value = c.default_value;
            out.push_back(v);
        }
    }
    if (page == "RequestAudioOptions") {
        ui::OptionValue v;
        v.id = k_hrtf_option;
        v.text = "Headphone Spatial Audio";
        v.appended = true;
        v.checkbox = true;
        v.value = m_user.flag("audio.hrtf") ? 1.0 : 0.0;
        out.push_back(v);
    }
    return out;
}

void App::ui_option_change(int id, double value) {
    if (graphics_row(id)) {
        graphics_option_change(id, value);
        return;
    }
    if (id == k_hrtf_option) {
        m_user.set("audio.hrtf", value == 1.0 ? "on" : "off");
        apply_user_setting("audio.hrtf");
        return;
    }
    if (m_ui) {
        for (const ui::SoundCategory& c : m_ui->sound_categories()) {
            if (int(c.form_id) == id) {
                if (auto key = audio_menu_key(c.form_id)) {
                    m_user.set(*key, std::format("{:.0f}", std::clamp(value, 0.0, 1.0) * 100.0));
                    apply_user_setting(*key);
                    return;
                }
                const std::string key = sound_category_key(c.form_id);
                m_user.add_hidden(key, std::format("{}", c.default_value));
                m_user.set(key, std::format("{}", std::clamp(value, 0.0, 1.0)));
                return;
            }
        }
    }
    for (const char* page : {"RequestGameplayOptions", "RequestDisplayOptions", "RequestAudioOptions"}) {
        for (const OptionDef& d : option_page(page)) {
            if (d.id != id || !d.setting) {
                continue;
            }
            const std::string s = d.setting;
            if (s == "controls.invert_y") {
                m_user.set(s, value == 1.0 ? "on" : "off");
            } else if (s == "audio.master") {
                m_user.set(s, std::format("{:.0f}", std::clamp(value, 0.0, 1.0) * 100.0)); // applied below
            } else if (d.kind == OptionDef::normalised) {
                m_user.set(s, std::format("{}", d.min + std::clamp(value, 0.0, 1.0) * (d.max - d.min)));
            } else if (d.kind == OptionDef::autosave) {
                const int i = int(value);
                m_user.set("bSaveOnPause:SaveGame", i < 6 ? "1" : "0");
                if (i >= 0 && i < 6) {
                    m_user.set(s, std::format("{}", k_autosave_minutes[i]));
                }
            } else if (d.kind == OptionDef::flag) {
                m_user.set(s, value == 1.0 ? "1" : "0");
            } else {
                m_user.set(s, std::format("{}", value));
            }
            apply_user_setting(s);
            if (s == "fMouseHeadingSensitivity:Controls") {
                // spud's mouse look scales with Skyrim's heading sensitivity relative to its 0.0125 default.
                m_user.set("controls.mouse_sensitivity", std::format("{:.2f}", m_user.number(s) / 0.0125));
                apply_user_setting("controls.mouse_sensitivity");
            }
            return;
        }
    }
    log::debug("ui: OptionChange({}, {}) has no setting", id, value);
}

void App::ui_action(const std::string& action) {
    if (action == "new_game") {
        try {
            start_new_game();
        } catch (const std::exception& e) {
            return_to_main_menu();
            throw Error(std::format("New Game needs an imported world ({}). Import your game with skytool import.",
                                    e.what()));
        }
    } else if (action == "take_book") {
        if (m_item_menus && m_item_menus->book_ref()) {
            take_ref(m_game->inspect(m_item_menus->book_ref()));
        }
    } else if (action == "main_menu") {
        return_to_main_menu();
    } else if (action == "quit") {
        m_quit_requested = true;
    } else if (action == "continue") {
        continue_game();
    } else {
        log::warn("ui: unhandled action '{}'", action);
    }
}

namespace {

constexpr uint32_t k_char_gen_quest = 0x0003372B;    // sCharGenQuest:General's default
constexpr uint32_t k_intro_start_marker = 0x0004678E; // MQ101's PlayerStartMarker alias

} // namespace

void App::start_new_game() {
    // The calendar as the load order sets it (8:00, the 17th of Last Seed, 4E 201 in Skyrim.esm).
    const ui::Calendar calendar = m_ui->start_calendar();
    m_clock.start(float(calendar.year), float(calendar.month), float(calendar.day), float(calendar.hour));
    apply_time_of_day(m_clock.hour());
    if (m_world_sim) {
        m_world_sim->new_game();
    }
    m_custom_marker.reset();
    m_play_seconds = 0;
    m_character_id = uint32_t(std::chrono::system_clock::now().time_since_epoch().count());
    m_save_character = m_character_id;
    m_last_save.clear();
    m_saving_disabled = false;
    m_ui->close_all();
    reset_papyrus(); // a second New Game starts the VM and every quest again
    m_character_menus.reset();
    m_game.reset();
    m_item_menus.reset();
    m_dialogue_output.reset();
    m_quests_started = false;
    start_game();
    // Where the player is as the loading screen appears (before the move), for its conditions.
    const std::string from_world = m_space ? m_space->edid() : std::string();
    const auto words = words_of(m_user.get("new_game.position"));
    Pose pose;
    // The opening's script moves the player onto the cart itself. Loading there to begin with saves building the
    // wrong place first.
    const std::optional<std::array<float, 3>> intro_at =
        m_user.flag("new_game.intro") && m_game ? m_game->ref_position(k_intro_start_marker) : std::nullopt;
    if (intro_at) {
        const std::string& quest = m_game->settings().text("sCharGenQuest:General");
        m_intro_quest = quest.empty() ? k_char_gen_quest : uint32_t(std::stoul(quest, nullptr, 16));
    }
    try {
        if (intro_at) {
            pose.position_units = {(*intro_at)[0], (*intro_at)[1], (*intro_at)[2]};
            load_world("tamriel");
        } else {
            if (words.size() != 3) {
                throw Error("needs x y z");
            }
            const std::string world = m_user.get("new_game.world");
            for (int i = 0; i < 3; ++i) {
                const std::string& t = words[size_t(i)];
                const char* b = t.data() + (t[0] == '+' ? 1 : 0);
                if (std::from_chars(b, t.data() + t.size(), pose.position_units[i]).ec != std::errc()) {
                    throw Error("bad number '" + t + "'");
                }
            }
            pose.z_above_terrain = words[2][0] == '+';
            pose.heading_deg = m_user.number("new_game.heading");
            pose.pitch_deg = m_user.number("new_game.pitch");
            load_world(world);
        }
    } catch (const std::exception& e) {
        if (m_space) {
            throw; // the world loaded; the error is elsewhere
        }
        log::error("new game: cannot start at the configured location ({}); trying the default", e.what());
        load_world("tamriel"); // throws if there is no import at all
        pose = Pose{{18432, -47104, 250}, true, 45, 5};
    }
    glm::vec3 p = pose.position_units * units::k_skyrim_unit_m;
    if (pose.z_above_terrain) {
        p.z = terrain_height(p.x, p.y).value_or(0.0f) + pose.position_units.z * units::k_skyrim_unit_m;
    }
    m_camera.position = p;
    m_camera.set_skyrim_angles(pose.heading_deg, pose.pitch_deg);
    m_ui->set_in_world(true);
    // A move to a worldspace position: the loading screen is chosen for the cell it lands in.
    m_loading_destination = {};
    m_loading_destination.worldspace = m_space ? m_space->edid() : std::string();
    m_loading_destination.cell_x = static_cast<int>(std::floor(pose.position_units.x / 4096.0f));
    m_loading_destination.cell_y = static_cast<int>(std::floor(pose.position_units.y / 4096.0f));
    m_loading_destination.from_worldspace = from_world;
    m_loading_destination.new_game = true;
    m_loading_destination_set = true;
    m_ui->open("loading");
    m_loading = true;
    m_loading_frames = 0;
    m_loading_start = Clock::now();
    m_loading_new_game = true;
    log::info("new game: {} at {:.0f} {:.0f} {:.0f}", m_space ? m_space->edid() : "none", pose.position_units.x,
              pose.position_units.y, pose.position_units.z);
}

// `travel`: a move behind the real loading screen, as coc or a door with one: what a tour's sections start with.
void App::travel(const std::string& space, const Pose& pose) {
    if (!m_space || !m_ui) {
        throw Error("travel needs a world");
    }
    const std::string from_world = m_space->edid();
    if (space != "-" && path::to_lower(space) != path::to_lower(from_world)) {
        load_world(space);
    }
    glm::vec3 feet = pose.position_units * units::k_skyrim_unit_m;
    if (pose.z_above_terrain) {
        feet.z += terrain_height(feet.x, feet.y).value_or(0.0f);
    }
    if (m_walk) {
        place_player(feet, pose.heading_deg, pose.pitch_deg);
    } else {
        m_camera.position = feet; // the free camera: the player stays where it is
        m_camera.set_skyrim_angles(pose.heading_deg, pose.pitch_deg);
    }
    if (m_world_sim) {
        int32_t cx = 0, cy = 0;
        const WorldSim::Place to = player_place(cx, cy);
        m_world_sim->player_moved(to, cx, cy);
    }
    m_loading_destination = {};
    m_loading_destination.worldspace = m_space->edid();
    m_loading_destination.cell_x = static_cast<int>(std::floor(pose.position_units.x / 4096.0f));
    m_loading_destination.cell_y = static_cast<int>(std::floor(pose.position_units.y / 4096.0f));
    m_loading_destination.from_worldspace = from_world;
    m_loading_destination_set = true;
    m_ui->open("loading");
    m_loading = true;
    m_loading_frames = 0;
    m_loading_start = Clock::now();
}

ui::LoadingDestination App::ui_loading_destination() {
    if (m_loading_destination_set) {
        m_loading_destination_set = false;
        return m_loading_destination;
    }
    // Shown without a load (a script's "ui open loading"): where the camera is now.
    ui::LoadingDestination d;
    if (m_space) {
        const glm::vec3 u = m_camera.position / units::k_skyrim_unit_m;
        d.worldspace = m_space->edid();
        d.from_worldspace = d.worldspace;
        d.cell_x = static_cast<int>(std::floor(u.x / 4096.0f));
        d.cell_y = static_cast<int>(std::floor(u.y / 4096.0f));
    }
    return d;
}

void App::return_to_main_menu() {
    end_down_view();
    m_ui->close_all();
    m_loading = false;
    m_loading_new_game = false;
    m_space.reset();
    m_ui->set_in_world(false);
    m_ui->open("main_menu");
}

void App::update_menus() {
    if (!m_loading) {
        return;
    }
    if (m_world_fx && m_space) {
        m_world_fx->settle(m_space->world());
    }
    // Opaque until the view has streamed in (like Skyrim's load screen), at most 30 s.
    ++m_loading_frames;
    // Seconds of work, so it waits for the screen to be up, and the first conversation doesn't pay for it.
    if (m_loading_new_game && m_loading_frames == 2) {
        m_loading_new_game = false;
        start_quests();
    }
    // A save's load runs once the screen is showing: its model and smoke are in, or after a second without them.
    if (m_load) {
        m_load_go = m_backdrop->shown() || ms_between(m_loading_start, Clock::now()) > 1000.0;
        return;
    }
    // The game takes seconds over a load, and here a save can be in within a frame: the screen stays long enough to be seen, a
    // choice of spud's, not a value of the game's.
    constexpr double k_least_save_screen_ms = 3000.0;
    if (m_loading_save_screen && ms_between(m_loading_start, Clock::now()) < k_least_save_screen_ms) {
        return;
    }
    m_loading_save_screen = false;
    const bool timed_out = ms_between(m_loading_start, Clock::now()) > 30000.0;
    if (m_loading_frames > 2 && !streaming_busy() && !timed_out && m_load_settle != LoadSettle::none &&
        m_load_settle != LoadSettle::done) {
        // One tick before the screen goes: the location it finds starts quests, over half a second of the first frame otherwise.
        m_load_settle = LoadSettle::tick;
        return;
    }
    if ((m_loading_frames > 2 && !streaming_busy()) || timed_out) {
        m_load_settle = LoadSettle::none;
        m_loading = false;
        m_ui->close("loading");
        m_ui->open("HUD Menu"); // open during play
    }
}

// ---- console -----------------------------------------------------------------------------

bool App::console_alias(const std::vector<std::string>& w) {
    const std::string cmd = path::to_lower(w[0]);
    auto run = [&](const std::string& line) {
        auto c = parse_command(line, "console", static_cast<int>(++m_console_seq));
        if (!c || !m_executor) {
            return;
        }
        m_executor->push_front(std::move(*c), [this](bool ok, const std::string& result) {
            if (!ok || !result.empty()) {
                m_ui->console_print(result, !ok);
            }
        });
    };
    if (cmd == "help" || cmd == "?") {
        m_ui->console_print(k_console_help);
    } else if (cmd == "clear" || cmd == "cls") {
        m_ui->console_clear();
    } else if (cmd == "qqq") {
        m_quit_requested = true;
    } else if (cmd == "autosave") {
        autosave();
    } else if (cmd == "quicksave") {
        quick_save();
    } else if (cmd == "forcesave") {
        manual_save();
    } else if (cmd == "save" && w.size() == 2) {
        start_game();
        begin_save(ost::Kind::manual, w[1], save_record().next_number);
    } else if (cmd == "load" && w.size() == 2) {
        m_load_request = saves_dir() / (w[1] + ".ost");
    } else if (cmd == "tm") {
        m_ui_visible = !m_ui_visible;
        m_ui->console_print(std::format("debug overlay and HUD {}", m_ui_visible ? "shown" : "hidden"));
    } else if (cmd == "tfc") {
        // Skyrim's free camera: fly with the movement keys and look with the mouse, no key held.
        m_free_look = !m_free_look;
        m_ui->console_print(std::format("free-fly mouse look {}", m_free_look ? "on" : "off"));
    } else if (cmd == "fov" && w.size() == 2) {
        run("set fov " + w[1]);
    } else if (cmd == "sucsm" && w.size() == 2) {
        run("set speed " + w[1]);
    } else if (cmd == "cow" && w.size() == 4) {
        // Each run() goes to the front of the queue, so the world loads before the cell is placed on it.
        run("cell " + w[2] + " " + w[3] + " +128");
        run("world " + w[1]);
    } else if (cmd == "coc") {
        m_ui->console_print("coc: exterior cell names are not in the import cache yet (planned importer step, "
                            "docs/UI.md); use cow <world> <x> <y>",
                            true);
    } else if (cmd == "getpos" || cmd == "player.getpos") {
        const glm::vec3 u = m_camera.position / units::k_skyrim_unit_m;
        m_ui->console_print(std::format("{:.0f} {:.0f} {:.0f}  (heading {:.1f}, pitch {:.1f})", u.x, u.y, u.z,
                                        m_camera.skyrim_heading_deg(), m_camera.skyrim_pitch_deg()));
    } else {
        return false;
    }
    return true;
}

void App::ui_console(const std::string& line) {
    const auto w = words_of(line);
    if (w.empty()) {
        return;
    }
    try {
        if (console_alias(w)) {
            return;
        }
        auto c = parse_command(line, "console", static_cast<int>(++m_console_seq));
        if (!c) {
            return;
        }
        if (!m_executor) {
            throw Error("no command executor is running");
        }
        m_executor->push_front(std::move(*c), [this](bool ok, const std::string& result) {
            if (!ok || !result.empty()) {
                m_ui->console_print(result, !ok);
            }
        });
    } catch (const std::exception& e) {
        // "console:12: unknown command 'x'" reads better without the parser's source prefix.
        std::string msg = e.what();
        if (msg.starts_with("console:")) {
            if (const size_t at = msg.find(": "); at != std::string::npos) {
                msg = msg.substr(at + 2);
            }
        }
        m_ui->console_print(msg, true);
    }
}

// ---- automation --------------------------------------------------------------------------

std::string App::ui_command(const std::string& sub, const std::string& args) {
    if (!m_ui) {
        throw Error("ui: the game UI is not available in this mode");
    }
    if (sub == "open") {
        m_ui->open(args);
    } else if (sub == "close") {
        if (args.empty()) {
            m_ui->back();
        } else if (args == "all") {
            m_ui->close_all();
        } else {
            m_ui->close(args);
        }
    } else if (sub == "back") {
        m_ui->back();
    } else if (sub == "activate") {
        m_ui->activate(args);
    } else if (sub == "select") {
        m_ui->select(args);
    } else if (sub == "mouse") {
        // ui mouse <x> <y>: the cursor to that pixel of the render target.
        const auto w = words_of(args);
        float x = 0, y = 0;
        if (w.size() != 2 || std::from_chars(w[0].data(), w[0].data() + w[0].size(), x).ec != std::errc() ||
            std::from_chars(w[1].data(), w[1].data() + w[1].size(), y).ec != std::errc()) {
            throw Error("ui mouse <x> <y>");
        }
        m_ui->mouse(x, y);
    } else if (sub == "key") {
        const auto w = words_of(args);
        if (w.empty() || w.size() > 2) {
            throw Error("ui key <name> [count|down|up]");
        }
        if (w.size() == 2 && (w[1] == "down" || w[1] == "up")) {
            m_ui->key_state(w[0], w[1] == "down"); // held across frames until "up"
            return {};
        }
        int count = 1;
        if (w.size() == 2 && std::from_chars(w[1].data(), w[1].data() + w[1].size(), count).ec != std::errc()) {
            throw Error("ui key: bad count '" + w[1] + "'");
        }
        m_ui->key(w[0], count);
    } else if (sub == "userevent") {
        // ui userevent <event>: a mapped button press as the game hands it to the top menu ("YButton",...).
        if (args.empty()) {
            throw Error("ui userevent <event>");
        }
        m_ui->user_event(args);
    } else if (sub == "type") {
        m_ui->type(args);
    } else if (sub == "console") {
        if (!m_ui->is_open("console")) {
            m_ui->open("console");
        }
        m_ui->console_submit(args);
    } else if (sub == "state") {
        const std::string json = m_ui->state_json();
        if (args.empty() || args == "-") {
            return json;
        }
        const std::filesystem::path p(args);
        write_file(p, std::as_bytes(std::span(json.data(), json.size())));
    } else if (sub == "get") {
        // ui get <menu name>[:<movie>]|<path> [FILE]: a value in the menu's movie (or its second movie, "Book
        // Menu:Book"), as ActionScript converts it to a string; with FILE, written there byte for byte (a value
        // may be empty or hold line breaks).
        const size_t bar = args.find('|');
        const size_t file_at = bar == std::string::npos ? std::string::npos : args.find(' ', bar);
        if (file_at != std::string::npos) {
            const std::string file = args.substr(file_at + 1);
            const std::string value = ui_command("get", args.substr(0, file_at));
            write_file(file, std::span(reinterpret_cast<const std::byte*>(value.data()), value.size()));
            return {};
        }
        const std::string target = bar == std::string::npos ? std::string() : args.substr(0, bar);
        const size_t colon = target.starts_with("movie:") ? std::string::npos : target.find(':');
        ui::Menu* menu = m_ui->find_menu(target.substr(0, colon));
        ui::player::Player* movie =
            menu ? menu->movie_named(colon == std::string::npos ? std::string_view() : std::string_view(target).substr(colon + 1))
                 : nullptr;
        if (!movie) {
            throw Error("ui get <menu>[:<movie>]|<path>: no such menu open");
        }
        auto& player = *movie;
        return *player.vm.to_string(player.get_path(args.substr(bar + 1)));
    } else if (sub == "mousebutton") {
        // ui mousebutton <0|1|2> down|up: a mouse button through the normal input path (its bound user event
        // to the top menu's input handler, press and release; the movie's mouse button).
        const auto w = words_of(args);
        if (w.size() != 2 || (w[1] != "down" && w[1] != "up")) {
            throw Error("ui mousebutton <0|1|2> down|up");
        }
        m_ui->mouse_press(std::atoi(w[0].c_str()), w[1] == "down");
    } else if (sub == "mousemove") {
        // ui mousemove <dx> <dy>: the mouse's own motion (the top menu's mouse handler, as the game's
        // The game).
        const auto w = words_of(args);
        if (w.size() != 2) {
            throw Error("ui mousemove <dx> <dy>");
        }
        m_ui->mouse_motion(std::strtof(w[0].c_str(), nullptr), std::strtof(w[1].c_str(), nullptr));
    } else if (sub == "localmap") {
        // ui localmap <file|->: the local map's camera as the Map Menu has it (LocalMapView, Skyrim units) and the
        // loaded area it keeps to, for comparison with the game's "localmap" in the game.
        std::string json = "null";
        if (const auto v = m_ui->local_map_view()) {
            float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
            ui_loaded_area(lo, hi);
            json = std::format(
                "{{\"position\": [{:.9g}, {:.9g}, {:.9g}], \"north_rotation\": {:.9g}, \"half_width\": "
                "{:.9g}, \"half_height\": {:.9g}, \"near\": {:.9g}, \"far\": {:.9g}, \"draw_land\": {}, "
                "\"loaded_area_min\": [{:.9g}, {:.9g}, {:.9g}], \"loaded_area_max\": [{:.9g}, {:.9g}, "
                "{:.9g}]}}",
                v->position[0], v->position[1], v->position[2], v->north_rotation, v->half_width,
                v->half_height, v->near_units, v->far_units, v->draw_land, lo[0], lo[1], lo[2], hi[0], hi[1],
                hi[2]);
        }
        if (args.empty() || args == "-") {
            return json;
        }
        write_file(std::filesystem::path(args), std::as_bytes(std::span(json.data(), json.size())));
    } else if (sub == "localmap_area") {
        // ui localmap_area <x0> <y0> <z0> <x1> <y1> <z1> | off: the loaded area's bounds (Skyrim units) instead of
        // the attached cells', to replay one measured in the game.
        std::array<float, 6> b{};
        if (args == "off") {
            m_local_map_area.reset();
        } else if (std::sscanf(args.c_str(), "%f %f %f %f %f %f", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            m_local_map_area = b;
        } else {
            throw Error("ui localmap_area <x0> <y0> <z0> <x1> <y1> <z1> | off");
        }
    } else if (sub == "set" || sub == "call") {
        // ui set <menu>[:<movie>]|<path> <value>: a value into a movie variable
        // JSON when it parses as JSON (" " is a one-space string), else the text as a
        // string (ui set ...htmlText <p>...</p>). ui call <menu>[:<movie>]|<path> [<number>...]: a method with
        // number arguments, the result as "name=value" pairs of an object's own properties (getLineMetrics,
        // getCharBoundaries), or as a string.
        const size_t bar = args.find('|');
        const size_t space = args.find(' ', bar == std::string::npos ? 0 : bar);
        const std::string target = bar == std::string::npos ? std::string() : args.substr(0, bar);
        const std::string path = args.substr(bar + 1, space == std::string::npos ? std::string::npos : space - bar - 1);
        const std::string rest = space == std::string::npos ? std::string() : args.substr(space + 1);
        const size_t colon = target.starts_with("movie:") ? std::string::npos : target.find(':');
        ui::Menu* menu = m_ui->find_menu(target.substr(0, colon));
        ui::player::Player* movie =
            menu ? menu->movie_named(colon == std::string::npos ? std::string_view() : std::string_view(target).substr(colon + 1))
                 : nullptr;
        if (!movie || bar == std::string::npos) {
            throw Error("ui set|call <menu>[:<movie>]|<path> ...: no such menu open");
        }
        auto& vm = movie->vm;
        if (sub == "set") {
            ui::player::json::Value value;
            std::string error;
            movie->set_path(path, ui::player::json::parse(rest, value, error) ? ui::player::json_to_value(vm, value)
                                                                             : ui::player::Value::string(rest));
            return {};
        }
        // Arguments: numbers, true / false, or "strings" (no quotes inside).
        std::vector<ui::player::Value> call_args;
        for (size_t at = 0; at < rest.size();) {
            if (rest[at] == ' ') {
                ++at;
                continue;
            }
            if (rest[at] == '"') {
                const size_t close = rest.find('"', at + 1);
                const size_t end = close == std::string::npos ? rest.size() : close;
                call_args.push_back(ui::player::Value::string(rest.substr(at + 1, end - at - 1)));
                at = end + 1;
                continue;
            }
            const size_t end = rest.find(' ', at);
            const std::string word = rest.substr(at, end == std::string::npos ? std::string::npos : end - at);
            call_args.push_back(word == "true" || word == "false"
                                    ? ui::player::Value::boolean(word == "true")
                                    : ui::player::Value::number(std::strtod(word.c_str(), nullptr)));
            at = end == std::string::npos ? rest.size() : end + 1;
        }
        const ui::player::Value r = movie->invoke(path, call_args);
        if (!r.is_object()) {
            return *vm.to_string(r);
        }
        std::string out;
        r.o->props.for_each([&](const std::string& key, const ui::player::Property& p) {
            if (p.value.is_number() || p.value.is_string()) {
                out += std::format("{}{}={}", out.empty() ? "" : " ", key, *vm.to_string(p.value));
            }
        });
        return out;
    } else if (sub == "reload") {
        m_ui->reload();
    } else if (sub == "seed") {
        // ui seed <n>: seed the UI's the game (loading-screen conditions and picks), as the game does.
        uint32_t seed = 0;
        if (std::from_chars(args.data(), args.data() + args.size(), seed).ec != std::errc()) {
            throw Error("ui seed <n>");
        }
        m_ui->set_random_seed(seed);
    } else if (sub == "loadfor") {
        // ui loadfor <world> <cell x> <cell y> [from <world>] [new_game] [location <id>]: the Loading Menu for a
        // move to that cell, with the player in <from> (default none, as at the main menu); new_game: a
        // StartNewGame load (no location posted, the char-gen quest as measured); location: post that location.
        // ui loadfor interior <cell EDID> [options]: a move into an interior cell. no_screen: the no-screen byte;
        // quest <id> <stage> <flags>, global <id> <value>: replay a state measured in the game.
        const auto w = words_of(args);
        ui::LoadingDestination d;
        size_t i = 0;
        if (w.size() >= 2 && w[0] == "interior") {
            d.interior = w[1];
            i = 2;
        } else {
            if (w.size() < 3) {
                throw Error("ui loadfor <world> <cell x> <cell y> | interior <cell>, then [from <world>] [new_game] "
                            "[location <id>] [no_screen]");
            }
            d.worldspace = w[0];
            if (std::from_chars(w[1].data(), w[1].data() + w[1].size(), d.cell_x).ec != std::errc() ||
                std::from_chars(w[2].data(), w[2].data() + w[2].size(), d.cell_y).ec != std::errc()) {
                throw Error("ui loadfor: bad cell");
            }
            i = 3;
        }
        for (; i < w.size(); ++i) {
            if (w[i] == "from" && i + 1 < w.size()) {
                d.from_worldspace = w[++i];
            } else if (w[i] == "new_game") {
                d.new_game = true;
            } else if (w[i] == "no_screen") {
                d.no_screen = true;
            } else if (w[i] == "location" && i + 1 < w.size()) {
                // An explicit location (form ID, hex).
                const std::string& h = w[++i];
                if (std::from_chars(h.data(), h.data() + h.size(), d.location, 16).ec != std::errc()) {
                    throw Error("ui loadfor: bad location");
                }
            } else if (w[i] == "quest" && i + 3 < w.size()) {
                // quest <id hex> <stage> <flags>: that quest's state as measured.
                ui::LoadingDestination::QuestState q;
                unsigned stage = 0, flags = 0;
                const std::string &id = w[i + 1], &st = w[i + 2], &fl = w[i + 3];
                if (std::from_chars(id.data(), id.data() + id.size(), q.quest, 16).ec != std::errc() ||
                    std::from_chars(st.data(), st.data() + st.size(), stage).ec != std::errc() ||
                    std::from_chars(fl.data(), fl.data() + fl.size(), flags).ec != std::errc()) {
                    throw Error("ui loadfor: quest <id> <stage> <flags>");
                }
                q.stage = static_cast<uint16_t>(stage);
                q.flags = static_cast<uint8_t>(flags);
                d.quests.push_back(q);
                i += 3;
            } else if (w[i] == "global" && i + 2 < w.size()) {
                // global <id hex> <value>: that global's value as measured.
                uint32_t id = 0;
                float value = 0;
                const std::string &h = w[i + 1], &v = w[i + 2];
                if (std::from_chars(h.data(), h.data() + h.size(), id, 16).ec != std::errc() ||
                    std::from_chars(v.data(), v.data() + v.size(), value).ec != std::errc()) {
                    throw Error("ui loadfor: global <id> <value>");
                }
                d.globals.emplace_back(id, value);
                i += 2;
            } else {
                throw Error("ui loadfor: unknown option " + w[i]);
            }
        }
        m_loading_destination = d;
        m_loading_destination_set = true;
        m_ui->open("loading");
    } else if (sub == "loadscreen") {
        // ui loadscreen <form ID, hex>: the next Loading Menu shows this LSCR instead of its pick.
        uint32_t id = 0;
        if (std::from_chars(args.data(), args.data() + args.size(), id, 16).ec != std::errc()) {
            throw Error("ui loadscreen <form ID>");
        }
        m_ui->force_loading_screen(id);
    } else if (sub == "drag") {
        // ui drag <dx> <dy>: the left mouse button held while the mouse moves by (dx, dy), then released, through the
        // same SDL events a mouse gives (the Mist Menu's inspection controls take them).
        const auto w = words_of(args);
        int dx = 0, dy = 0;
        if (w.size() != 2 || std::from_chars(w[0].data(), w[0].data() + w[0].size(), dx).ec != std::errc() ||
            std::from_chars(w[1].data(), w[1].data() + w[1].size(), dy).ec != std::errc()) {
            throw Error("ui drag <dx> <dy>");
        }
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.button = SDL_BUTTON_LEFT;
        m_ui->handle_event(e);
        e = SDL_Event{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.xrel = float(dx);
        e.motion.yrel = float(dy);
        m_ui->handle_event(e);
        e = SDL_Event{};
        e.type = SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.button = SDL_BUTTON_LEFT;
        m_ui->handle_event(e);
    } else if (sub == "pad") {
        // ui pad <hardware value, hex>: a pad button pressed and let go, through the normal input path.
        const uint32_t code = uint32_t(std::stoul(words_of(args).at(0), nullptr, 16));
        m_ui->set_gamepad_active(true);
        m_ui->gamepad_button(code, true);
        m_ui->gamepad_button(code, false);
    } else if (sub == "stick") {
        // ui stick <0 left|1 right> <x> <y>: hold a gamepad thumbstick there (x right, y up, [-1, 1]) until the next
        // ui stick; each frame's poll delivers it as the game's does.
        const auto w = words_of(args);
        int stick = 0;
        float x = 0, y = 0;
        if (w.size() != 3 || std::from_chars(w[0].data(), w[0].data() + w[0].size(), stick).ec != std::errc() ||
            std::from_chars(w[1].data(), w[1].data() + w[1].size(), x).ec != std::errc() ||
            std::from_chars(w[2].data(), w[2].data() + w[2].size(), y).ec != std::errc()) {
            throw Error("ui stick <0|1> <x> <y>");
        }
        m_ui->set_thumbstick(stick, std::clamp(x, -1.0f, 1.0f), std::clamp(y, -1.0f, 1.0f));
    } else if (sub == "item3d") {
        // ui item3d [file|-]: the item or magic menu's 3D item manager, or the
        // Stats Menu's starfield or the Book Menu's book.
        // ui item3d rotate <a> <b>: the game(a, b) on the item menu's model.
        if (args.starts_with("rotate")) {
            const auto w = words_of(args);
            float a = 0, b = 0;
            if (w.size() != 3 || std::from_chars(w[1].data(), w[1].data() + w[1].size(), a).ec != std::errc() ||
                std::from_chars(w[2].data(), w[2].data() + w[2].size(), b).ec != std::errc()) {
                throw Error("ui item3d rotate <a> <b>");
            }
            for (const char* name : {"InventoryMenu", "ContainerMenu", "BarterMenu", "GiftMenu"}) {
                if (ui::Menu* m = m_ui->find_menu(name); m && ui::items_item3d_rotate(*m, a, b)) {
                    return {};
                }
            }
            throw Error("ui item3d rotate: no item menu open");
        }
        std::string json = "null";
        for (const char* name : {"InventoryMenu", "ContainerMenu", "BarterMenu", "GiftMenu", "MagicMenu", "StatsMenu",
                                 "Book Menu", "Crafting Menu"}) {
            if (ui::Menu* m = m_ui->find_menu(name)) {
                const std::string_view n = name;
                json = n == "MagicMenu"       ? ui::magic_item3d_json(*m)
                       : n == "StatsMenu"     ? ui::stats_starfield_json(*m)
                       : n == "Book Menu"     ? ui::book_book3d_json(*m)
                       : n == "Crafting Menu" ? ui::crafting_item3d_json(*m)
                                              : ui::items_item3d_json(*m);
            }
        }
        if (args.empty() || args == "-") {
            return json;
        }
        write_file(args, std::as_bytes(std::span(json.data(), json.size())));
    } else if (sub == "page") {
        // ui page <0-3> <file.png>: the Book Menu's page target (render target 105 + page) as the last frame drew it.
        const auto w = words_of(args);
        int page = -1;
        if (w.size() != 2 || std::from_chars(w[0].data(), w[0].data() + w[0].size(), page).ec != std::errc()) {
            throw Error("ui page <0-3> <file.png>");
        }
        std::vector<uint8_t> rgba;
        uint32_t size = 0;
        if (!m_ui->read_page(page, rgba, size)) {
            throw Error("ui page: no page target " + std::string(w[0]));
        }
        save_png(w[1], rgba, size, size);
    } else if (sub == "scene") {
        // ui scene <slot> [file|-]: the last frame's UI 3D scene in that slot: camera, lights, draws (Skyrim units).
        const auto w = words_of(args);
        int slot = -1;
        if (w.empty() || std::from_chars(w[0].data(), w[0].data() + w[0].size(), slot).ec != std::errc()) {
            throw Error("ui scene <slot> [file|-]");
        }
        const std::string json = ui_scene_json(slot);
        if (w.size() < 2 || w[1] == "-") {
            return json;
        }
        write_file(w[1], std::as_bytes(std::span(json.data(), json.size())));
    } else if (sub == "loading") {
        // ui loading [file|-]: the Loading Menu's choice, the Mist Menu's model and camera.
        const std::string json = m_ui->loading_json();
        if (args.empty() || args == "-") {
            return json;
        }
        write_file(args, std::as_bytes(std::span(json.data(), json.size())));
    } else if (sub == "dump") {
        // ui dump <menu name> <file.json|->: the menu's display list.
        const size_t sp = args.rfind(' ');
        if (sp == std::string::npos) {
            throw Error("ui dump <menu> <file.json|->");
        }
        const std::string json = m_ui->dump_json(args.substr(0, sp));
        const std::string target = args.substr(sp + 1);
        if (target == "-") {
            return json;
        }
        write_file(target, std::as_bytes(std::span(json.data(), json.size())));
    } else if (sub == "movie") {
        const auto w = words_of(args);
        if (w.empty()) {
            throw Error("ui movie <name> [noScale|showAll|exactFit|noBorder] | ui movie close");
        }
        if (w[0] == "close") {
            m_ui->close_movies();
        } else {
            m_ui->open_movie(w[0], w.size() > 1 ? w[1] : std::string());
        }
    } else {
        throw Error(std::format("unknown ui command '{}' (open, close, back, activate, select, key, userevent, type, "
                                "console, state, reload, movie, seed, loading, loadfor, loadscreen, drag, stick, item3d, scene, page)",
                                sub));
    }
    return {};
}

std::string App::ui_scene_json(int slot) const {
    auto floats = [](const float* v, int n) {
        std::string o = "[";
        for (int i = 0; i < n; ++i) {
            o += std::format("{}{:.9g}", i ? ", " : "", v[i]);
        }
        return o + "]";
    };
    for (const ui::UiScene& sc : m_ui->ui_scenes()) {
        if (sc.slot != slot) {
            continue;
        }
        std::string j = std::format("{{\"slot\": {}, \"camera\": {{\"translate\": {}, \"rotate\": {}, \"frustum\": {}}}, "
                                    "\"lights\": [",
                                    sc.slot, floats(sc.camera.position, 3), floats(sc.camera.rotation, 9),
                                    floats(sc.camera.frustum, 6));
        for (size_t i = 0; i < sc.lights.size(); ++i) {
            const ui::SceneLight& l = sc.lights[i];
            j += std::format("{}{{\"position\": {}, \"color\": {}, \"radius\": {:.9g}, \"dimmer\": {:.9g}, "
                             "\"shadows\": {}}}",
                             i ? ", " : "", floats(l.position, 3), floats(l.color, 3), l.radius, l.dimmer, l.shadows);
        }
        j += "], \"draws\": [";
        for (size_t i = 0; i < sc.draws.size(); ++i) {
            const ui::SceneDraw& d = sc.draws[i];
            j += std::format("{}{{\"model\": \"{}\", \"submesh\": {}, \"rotate\": {}, \"translate\": {}, "
                             "\"alpha\": {:.9g}, \"page\": {}}}",
                             i ? ", " : "", d.model < sc.model_count ? sc.models[d.model] : std::string(), d.submesh,
                             floats(d.rotation, 9), floats(d.translation, 3), d.alpha, d.page);
        }
        return j + "]}";
    }
    return "null";
}

float App::ui_game_setting(std::string_view name, float fallback) const {
    // The Skyrim settings spud keeps under their game names (user_settings.cpp); the others are the compiled
    // values the caller passes.
    try {
        return m_user.number(std::string(name));
    } catch (const std::exception&) {
        return fallback;
    }
}

} // namespace spud::engine
