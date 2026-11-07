#pragma once

// The game UI: Skyrim's own menus, run from the game's interface movies (src/ui/player) and drawn by
// MovieRenderer. Ui is the menu stack the game's UI keeps: menus open and close by name, draw in depth order,
// and get input as Scaleform key and mouse events converted from the control map, exactly as the engine does
// The engine feeds it SDL events first (engine/input_focus.hpp), calls
// update() once a frame and hands overlay() to the renderer; it answers UiHost questions.

#include "fx/random.hpp"
#include "ui/scene.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

struct SDL_Window;
struct VkImageView_T; // VkImageView
union SDL_Event;

namespace spud::render {
class Overlay;
class Renderer;
} // namespace spud::render

namespace spud::osu {
struct Controls;
}

namespace spud::ui {

namespace player {
class Library;
class Player;
class Object;
namespace json {
struct Value;
}
} // namespace player

// The main menu's state-dependent properties (sendMenuProperties values 4, 12 and 13).
struct MainMenuState {
    bool anniversary_upsell = false; // the Anniversary upgrade is not owned
    bool creations_icon = true;      // Creation Club connection state 3
    bool creations_enabled = false;
};

// The Mist Menu's 3D backdrop as the game sets it up: the logo,
// the camera and the three "UI player scene" lights, in Skyrim units, Z up. The engine draws it as the frame's
// scene while the Mist Menu is open, under the menu movies.
struct BackdropLight {
    float position[3];
    float color[3]; // diffuse
    float radius;
    float dimmer;   // the light's fade value
    bool shadows;
};

struct BackdropScene {
    std::string model;            // cache path of the logo or loading-screen mesh (.osm); empty: none
    bool default_logo = true;     // the default logo, not a loading screen's model
    float model_translation[3];   // the root's translation (fUIAltLogoModel_Translate* + offset, wide-screen scaled)
    float model_rotation[9];      // the root's rotation, row-major, column vectors
    float model_scale;            // |scale| (the default logo's fUIMistMenu_DefaultLogoNIFScale), wide-screen scaled
    float camera[3];              // fUIMistMenu_Camera{X,Y,Z}_G, or placed by the camera path
    float look_at[3];             // fUIMistMenu_CameraLookAt{X,Y,Z}_G, up (0, 0, 1)
    bool camera_rotation_set = false; // a camera path places the camera: camera_rotation replaces the look-at
    float camera_rotation[9];     // NiCamera world rotation, row-major: columns are the view, up and right axes
    float fov_degrees;            // fUIMistMenu_CameraFOV_G (zoomed by the inspection controls): horizontal-locked,
                                  // half slope 0.75 tan(fov pi/1200)
    float near_units, far_units;  // fUICameraNearDistance, fUICameraFarDistance
    BackdropLight lights[3];      // fUIPlayerSceneLight, ...Light2, ...Light3
    float ambient[6][3];          // rUIMistMenu_Ambient{XNeg,XPos,YNeg,YPos,ZNeg,ZPos}RGB: all zero
    double seconds = 0;           // since the scene was attached (its controllers' time)
};

// The Map Menu's local map view while it is shown: the game's
// orthographic camera over the loaded cells, in Skyrim units, Z up. It looks straight down; the screen's up is
// (sin r, cos r, 0) and its right (cos r, -sin r, 0) for the north rotation r. The engine draws it into the render
// target the movie shows as img://Local_Map (k_local_map_image), every frame it is shown.
struct LocalMapView {
    float position[3];
    float north_rotation; // radians
    float half_width, half_height;
    float near_units, far_units;
    float area_min[2], area_max[2]; // the loaded area: an interior's fog of war covers it
    // False in a worldspace with flag 0x40 (the land's cell nodes are not culled).
    bool draw_land = true;
};
inline constexpr const char* k_local_map_image = "Local_Map";

// The game clock as the calendar globals hold it (GameYear, GameMonth 0-11, GameDay, GameHour, GameDaysPassed).
struct Calendar {
    double year = 0, month = 0, day = 0, hour = 0, days_passed = 0;
};

// Where a load goes (the game's location comes from it) and where the player is as the screen appears.
struct LoadingDestination {
    std::string worldspace;      // a move into a worldspace: its EDID and the cell
    int cell_x = 0, cell_y = 0;
    std::string interior;        // a move into an interior cell: its EDID
    std::string from_worldspace; // the worldspace the player is in now ("" before a game is loaded)
    bool no_screen = false;      // the destination is already loaded, no loading screen
    // The first load of a new game: the session start posts no location, and the char-gen quest has
    // just been started.
    bool new_game = false;
    uint32_t location = 0;       // automation: this the game form ID instead of the destination's
    // Automation: quest states (form ID, stage, the game flags) and global values that replace the starting
    // values, to replay a state measured in the game.
    struct QuestState {
        uint32_t quest = 0;
        uint16_t stage = 0;
        uint8_t flags = 0;
    };
    std::vector<QuestState> quests;
    std::vector<std::pair<uint32_t, float>> globals;
};

// The Training Menu's card, from the trainer the menu opened on.
struct TrainingCard {
    std::string skill_name, gold_name;
    int max_level = 0, times_trained = 0, player_level = 1;
    double base = 0, permanent = 0, progress = 0, gold = 0, xp = 0;
};

struct PlayerInfo {
    double level = 1;
    double progress_percent = 0;            // towards the next level
    std::optional<Calendar> calendar;       // none: no game clock yet, the load order's starting values apply
};

// One entry of a System-tab settings page as the engine writes it (ID, value, defaultVal).
// A sound category the Journal's Audio page lists (interface/soundcategories.osu, docs/formats/osu.md).
struct SoundCategory {
    uint32_t form_id = 0;
    std::string name;
    double default_value = 1.0; // UNAM / 65535
};

struct OptionValue {
    int id = 0;
    double value = 0, default_value = 0;
    std::string text;      // appended entries (sound categories) carry their name
    bool written = true;   // false: the PC build leaves this entry as the movie made it
    bool appended = false; // a new entry pushed onto the movie's array
    bool checkbox = false; // an appended entry that is on/off instead of the page's slider
};

// A row of the Graphics page: `options` for a stepper, empty for a checkbox; `value` the step or 0/1.
struct GraphicsOption {
    int id = 0;
    std::string text;
    std::vector<std::string> options;
    double value = 0, default_value = 0;
};

// What the crosshair's prompt shows for the reference the player points at; no text: nothing.
struct CrosshairTarget {
    std::string text; // the verb, a line break, the name
    std::optional<double> weight, value;
    std::optional<double> field_value; // armor rating or damage
    std::string field_text;            // sHUDArmorRating, sHUDDamage
    bool hostile = false;              // an actor fighting the player

    bool operator==(const CrosshairTarget&) const = default;
};

class JournalSource; // ui/journal.hpp
class ItemSource;
class CraftingSource;
class CharacterSource;
class RaceSexSource;
struct BookInfo;
struct LockInfo;

// The world map's live data: what the Map Menu shows of the running game. Without one
// (no world loaded) the menu falls back to what a capture recorded.
struct MapEntry {
    std::string name;
    double icon = 0; // the movie's IconTypes index
    bool undiscovered = false;
};
struct MapPlace {
    bool shown = false;
    double x = -1, y = -1; // fractions of the map
    double rotation = 0;
};
struct MapFrameInput {
    double dt = 0;
    float pan[2] = {0, 0};
    float look[2] = {0, 0}; // the frame's mouse motion, y down
    float zoom = 0;
};
class MapSource {
public:
    virtual ~MapSource() = default;
    // The entries, in the movie's order. Their places come from each map_frame.
    virtual std::vector<MapEntry> map_open() = 0;
    // False while the camera is still moving in: nothing is selected then. `heading`: the player marker's rotation.
    virtual bool map_frame(const MapFrameInput& input, std::vector<MapPlace>& places, double& heading) = 0;
    virtual void map_click(size_t entry) = 0; // MarkerClick on a selected entry
    virtual void map_click_ground(double x, double y) = 0; // a click on no marker, fractions of the screen
    virtual void map_centre() = 0;            // PlayerPosition: back on the player
    virtual void map_close() = 0;
    virtual bool map_opens_local() { return false; } // the player is in an interior
    virtual std::string map_title() { return {}; }   // the local map's: the cell, location or worldspace
};

// Saved games as the Save and Load panels show them, newest first.
struct SaveListEntry {
    std::string file_num; // the number, or sAutosaveAbbrev / sQuicksaveAbbrev
    std::string text;     // the location
};
struct SaveDetails {
    bool corrupt = false, obsolete = false;
    std::string name, race, play_time, date;
    double level = 1;
    std::string key; // which picture, so it uploads once
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> image; // RGBA8
};
struct SaveCharacter {
    std::string text;
    uint32_t id = 0;
};
class SaveSource {
public:
    virtual ~SaveSource() = default;
    virtual std::vector<SaveListEntry> save_list() = 0;
    virtual SaveDetails save_details(size_t index) = 0;
    virtual void save_over(int index) = 0; // the movie's index: 0 is [NEW SAVE]
    virtual void load_save(size_t index) = 0;
    virtual void delete_save(size_t index) = 0;
    virtual void quick_save() = 0;
    virtual bool can_save() = 0;
    virtual bool can_load() = 0; // and there is something to load
    virtual std::vector<SaveCharacter> save_characters() = 0;
    virtual void select_character(uint32_t id) = 0;
};

// A map marker on the compass.
struct CompassEntry {
    double position[3];
    double type;
    bool can_travel;
    bool player_set = false; // the custom destination
    bool quest = false;      // a quest target
    bool quest_door = false; // the load door on the way to one
};

// What the UI asks of the application.
// A row of the Dialogue Menu's topic list.
struct DialogueRow {
    std::string text;
    bool is_new = false;
    int index = 0;
};

// One entry of the Magic Menu.
struct MagicRow {
    std::string text;
    uint32_t form = 0;
    uint32_t filter_flag = 0; // its category's bit
    int equip_state = 0;      // 2 left, 3 right, 4 both, 1 an equipped shout or power
};

class UiHost {
public:
    virtual ~UiHost() = default;
    // The Magic Menu: what the player knows, sorted by text; empty to replay the capture. `slot` 0 any, 1 right, 2 left.
    virtual std::vector<MagicRow> ui_magic_rows() { return {}; }
    virtual bool ui_magic_equip(uint32_t, int) { return false; }
    // The Dialogue Menu: true when the dialogue system drives it, false to replay the capture.
    virtual bool ui_dialogue_live() { return false; }
    virtual void ui_dialogue_topic(int) {}
    virtual void ui_dialogue_skip() {}
    virtual void ui_dialogue_closed() {}
    // Game-level actions from menus: "new_game", "continue", "main_menu", "quit".
    virtual void ui_action(const std::string& action) = 0;
    virtual bool ui_has_saves() = 0;
    virtual bool ui_mods_loaded() = 0;
    virtual MainMenuState ui_main_menu_state() = 0;
    // A command line from the console; output comes back through Ui::console_print.
    virtual void ui_console(const std::string& line) = 0;
    // A UI sound descriptor by editor ID ("UIMenuOK").
    virtual void ui_play_sound(const std::string& descriptor) = 0;
    // The same for a sound that runs until stopped (the lock's tension): its id, 0 for none.
    virtual uint32_t ui_start_sound(const std::string& descriptor) {
        (void)descriptor;
        return 0;
    }
    virtual void ui_stop_sound(uint32_t id) { (void)id; }
    // The load the Loading Menu is shown for.
    virtual LoadingDestination ui_loading_destination() = 0;
    virtual PlayerInfo ui_player_info() = 0;
    // System tab settings: "RequestGameplayOptions", "RequestDisplayOptions", "RequestAudioOptions".
    virtual std::vector<OptionValue> ui_options(const std::string& page) = 0;
    virtual void ui_option_change(int id, double value) = 0;
    virtual std::vector<GraphicsOption> ui_graphics_options() { return {}; }
    virtual void ui_settings_closed() = 0; // a good moment to persist
    // The HUD's compass: the player's position in Skyrim units and the camera's yaw in
    // radians, clockwise from north. False while there is no world.
    virtual bool ui_player_pose(float position[3], float& yaw) {
        (void)position;
        (void)yaw;
        return false;
    }
    // The HUD's stealth meter: sneaking on foot, not swimming.
    virtual bool ui_player_sneaking() { return false; }
    virtual bool ui_can_rest(bool) { return true; } // false has told the player why
    virtual SaveSource* ui_saves() { return nullptr; }
    // The running game's map, or null without a world.
    virtual MapSource* ui_map() { return nullptr; }
    // The map markers the compass shows, nearest first; false without a world (recorded markers stand).
    virtual bool ui_compass_markers(std::vector<CompassEntry>& out) {
        (void)out;
        return false;
    }
    // An autosave's occasions: "rest" and "wait", the moment OK is pressed, before any time passes.
    virtual void ui_autosave(const char* occasion) { (void)occasion; }
    virtual void ui_tween_opened() {}
    // Game.SetBeastForm: 0 no, 1 the werewolf or Vampire Lord, 2 the Vampire Lord (default object
    // 346), who keeps the Favorites Menu.
    virtual int ui_beast_form() { return 0; }
    virtual void ui_quicksave_key() {}
    virtual void ui_quickload_key() {}
    // Sleep/Wait: game time passes (game hours); the calendar of later PlayerInfo answers includes it.
    virtual void ui_pass_time(double hours) { (void)hours; }
    // The Training Menu: the card, and a paid session (false: no trainer, the menu falls back on its capture).
    virtual bool ui_training_card(TrainingCard& card) {
        (void)card;
        return false;
    }
    virtual void ui_train(int cost) { (void)cost; }
    // The game and StopSleepWait: `hours` of sleep or wait begin; the sleep or wait is over.
    virtual void ui_rest_started(bool sleeping, double hours) {
        (void)sleeping;
        (void)hours;
    }
    virtual void ui_rest_ended(bool sleeping, bool interrupted) {
        (void)sleeping;
        (void)interrupted;
    }
    // Survival's HUD values: default objects 331 to 334; false: Survival Mode is off.
    virtual bool ui_survival(double& temperature, double& cold, double& hunger, double& exhaustion) {
        (void)temperature;
        (void)cold;
        (void)hunger;
        (void)exhaustion;
        return false;
    }
    // The loaded area (TES's LoadedAreaBound: the attached cells around the player), Skyrim units; false while
    // there is no world. The local map's camera keeps to it.
    virtual bool ui_loaded_area(float min[3], float max[3]) {
        (void)min;
        (void)max;
        return false;
    }
    // The worldspace the player is in (its EDID), "" without one.
    virtual std::string ui_worldspace() { return {}; }
    virtual float ui_cell_north() { return 0.0f; } // the player's interior's north rotation, radians
    // The quests and misc stats the Journal shows (ui/journal.hpp); null: the Journal uses captured game data.
    virtual JournalSource* ui_journal() { return nullptr; }
    // The game's inventory system behind the item menus (ui/items.hpp); null: they replay captured data.
    virtual ItemSource* ui_item_source() { return nullptr; }
    // ... and behind the Crafting Menu; null: it replays captured data.
    virtual CraftingSource* ui_crafting_source() { return nullptr; }
    // The game's character behind the Stats and Level Up menus (ui/character.hpp); null: they replay captured data.
    virtual CharacterSource* ui_character() { return nullptr; }
    // ... and behind the RaceSex Menu; null: it replays captured data.
    virtual RaceSexSource* ui_race_sex() { return nullptr; }
    // The game of a running game; false: there is none, the HUD's capture stands.
    virtual bool ui_meter_percent(int actor_value, double& percent) {
        (void)actor_value;
        (void)percent;
        return false;
    }
    // The lock to pick; false: the Lockpicking Menu replays a recording. Then what happens to it: "broken", "unlocked".
    virtual bool ui_lock(LockInfo& lock) {
        (void)lock;
        return false;
    }
    virtual void ui_lock_event(std::string_view what, double pick_health) {
        (void)what;
        (void)pick_health;
    }
    // The book to read; false: the Book Menu replays a recording.
    virtual bool ui_book(BookInfo& book) {
        (void)book;
        return false;
    }
    // A game setting's value as the player has it (the Skyrim settings spud keeps, "fMouseHeadingSensitivity:
    // Controls"), or `fallback` (the compiled value) for one it does not keep.
    // A binding changed (code 0xFF: the device lost it), or all went back to the defaults (empty event).
    virtual void ui_control_changed(const std::string& event, int device, uint32_t code) {
        (void)event;
        (void)device;
        (void)code;
    }
    // A string game setting the interface library doesn't carry.
    virtual std::string ui_text_setting(std::string_view name) const {
        (void)name;
        return {};
    }
    virtual float ui_game_setting(std::string_view name, float fallback) const {
        (void)name;
        return fallback;
    }
};

struct UiOptions {
    std::filesystem::path interface_dir; // the game's interface, converted into the cache
    std::filesystem::path shader_dir;    // compiled SPIR-V
    std::filesystem::path pipeline_cache;
    SDL_Window* window = nullptr; // null when headless: no text input or cursor
    bool hud_recording = false;   // the HUD plays the "hud" capture: test data, never a player's session
};

struct MessageBoxData {
    std::string text;
    std::vector<std::string> buttons;
    std::function<void(int)> callback;
};

class Menu;
class MovieRenderer;
class LoadingScreens;
struct MovieLayer;

class Ui {
public:
    Ui(render::Renderer& renderer, UiHost& host, UiOptions options);
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // Each SDL event goes here first. True: consumed, the game must not see it.
    bool handle_event(const SDL_Event& e);
    // A menu that pauses the game is open: the game must not read the keyboard or mouse.
    bool captures_input() const;
    bool visible() const;
    // The Mist Menu (the 3D logo and fog behind the main menu and loading screens) is open.
    bool backdrop() const;
    // Its scene while it is open, for a target of the current size.
    std::optional<BackdropScene> backdrop_scene() const;
    // The Map Menu's local map while it is shown, for the engine to draw into the target it registered.
    std::optional<LocalMapView> local_map_view() const;
    // A render target the engine draws that movies show as img://NAME (the local map's), sampled in the frame it
    // is drawn; the view stays valid for the UI's life.
    void set_render_target_image(const std::string& name, ::VkImageView_T* view);
    // An image the engine owns the pixels of, shown under `name`; `key` says which, so a picture uploads once.
    void set_image_pixels(const std::string& name, const std::string& key, uint32_t width, uint32_t height,
                          const std::vector<uint8_t>& rgba);
    // The logo the Mist Menu shows by default; Main Menu switches it to the Anniversary one (section 2).
    void set_backdrop_logo(std::string cache_path) { m_backdrop_logo = std::move(cache_path); }
    const std::string& backdrop_logo() const { return m_backdrop_logo; }

    // Once a frame before rendering: menu messages, movies, draw lists. Returns at once while nothing is open.
    void update(double dt);
    render::Overlay* overlay();

    // Menus by name ("Main Menu", "Journal Menu", ...; also main_menu, pause, console, loading, credits).
    // "NAME|DATA" opens NAME with show data (what the game passes as the show message's data: the Crafting Menu's
    // station, the Tutorial Menu's message), which the menu reads with show_data.
    void open(const std::string& name);
    // SleepWaitMenu::Open(true): a bed's, against the Wait key's.
    void open_sleep() {
        m_sleep_request = true;
        open("Sleep/Wait Menu");
    }
    bool take_sleep_request() { return std::exchange(m_sleep_request, false); }
    bool m_sleep_request = false;
    void close(const std::string& name); // asks the menu to hide; some wait for their movie
    void close_now(const std::string& name);
    void close_all(bool keep_hud = false); // keep_hud: UI_CloseAllMenus, which leaves the HUD and debug text up
    // A HUD mode the game turns on or off (SetHudCartMode, a kill camera's playback). It holds across the HUD's
    // reset after a load: a script can set it behind the loading screen, before the HUD is up.
    void hud_mode(const std::string& mode, bool on);
    const std::vector<std::string>& hud_modes() const { return m_hud_modes; }
    void clear_hud_modes() { m_hud_modes.clear(); } // a game starts or loads with none
    bool is_open(const std::string& name) const;
    // The game's cursor is up: the window then hides the OS pointer and gives the mouse's own motion
    // (relative mode), which moves the cursor as the game's does.
    bool shows_cursor() const { return is_open("Cursor Menu"); }
    // The top menu takes the mouse's motion for itself with no cursor up: the OS pointer stays hidden too.
    bool holds_mouse() const;
    void centre_cursor();
    bool is_top_input(const Menu* menu) const { return top_input_menu() == menu; }
    int pausing_menus() const; // open menus with flag 0x1 (the UI's pause count, UI)
    // The game's count: above 0 the world is blurred behind the menus.
    int blur_count() const { return m_blur_count; }
    void add_blur(int delta) { m_blur_count = std::max(m_blur_count + delta, 0); }
    bool menus_allow_saving() const; // every open menu has flag 0x800 (UI)
    bool back(); // what Esc does in menus (the Cancel user event); false if nothing is open
    // Automation and tests, through each movie's own input path: click or hover the item showing `text`
    // (translated), press a key by its control-map name ("Enter", "Esc", "Up", "E"), type text.
    void activate(const std::string& text);
    void select(const std::string& text);
    // The cursor to (x, y) in target pixels.
    void mouse(float x, float y) { mouse_to_menus(x, y); }
    float mouse_x() const { return m_mouse_x; }
    float mouse_y() const { return m_mouse_y; }
    // A mouse button pressed or released, and the mouse's own motion, through the normal input path
    // (automation).
    void mouse_press(int button, bool down) { mouse_button(button, down); }
    void mouse_motion(float dx, float dy);
    void key(const std::string& name, int count = 1);
    void key_state(const std::string& name, bool down); // one key down or up, for keys held across frames
    // A user event for the top menu, as the game posts each mapped button press ("YButton", "LeftEquip",...).
    void user_event(const std::string& event);
    void type(const std::string& text);
    void console_submit(const std::string& line);
    void console_print(const std::string& text, bool error = false);
    // The HUD's crosshair prompt and notifications.
    void set_crosshair(const CrosshairTarget& target);
    void notification(const std::string& text, const std::string& sound = {});
    void set_stealth(bool sneaking, double level);
    // A skill's increase with the level meter (HUD data type 20).
    void skill_notification(const std::string& skill, int level, int player_level, double xp);
    void console_clear();
    // A message box (MessageBoxMenu): text, buttons, and what to do with the button pressed.
    void message(const std::string& text, std::vector<std::string> buttons = {},
                 std::function<void(int)> callback = {});
    // The HUD's corner message (identical texts held back for 5 s), a location's discovery
    // and a known location's name (HUD data types 1, 26 and 29).
    // A script that answers every message box with that button; < 0: they wait for a
    // player.
    void set_auto_message(int button) { m_auto_message = button; }
    void hud_corner_message(const std::string& text);
    // The tutorials already shown: the game keeps them in the save.
    std::vector<int> tutorials_shown_list() const;
    void set_tutorials_shown(const std::vector<int>& shown);
    void hud_location_discovered(const std::string& name, int type);
    void hud_location_name(const std::string& name);
    void hud_tutorial_hint(const std::string& text); // empty: hide

    // Journal > System > Controls: the remappable bindings of the main context, and
    // a binding changed by the next press. `done` hears whether it took; a reserved key doesn't.
    struct InputMapping {
        std::string event, button;
        int sort_index = 0;
        uint32_t code = 0;
    };
    std::vector<InputMapping> input_mappings() const;
    void start_remap(std::string event, std::function<void(bool)> done);
    bool remapping() const { return static_cast<bool>(m_remap_done); }
    // ControlMap's remap: a key another event has swaps with it, and a key takes the event off the mouse
    // and the other way round. False: the event isn't remappable, or the key is reserved. device 0 keyboard, 1 mouse,
    // 2 gamepad.
    bool remap(const std::string& event, int device, uint32_t code);
    void reset_controls();
    void hud_quest_notice(int type, uint32_t quest, const std::string& text); // HUD data 14 to 19
    std::string state_json() const;
    std::vector<std::string> open_menus() const; // by depth, bottom first
    // The open Loading Menu's choice and the Mist Menu's model and camera as JSON ("null" when it is not open).
    std::string loading_json();
    // A menu's movie as JSON: every display object with its path, kind, visibility, transform, alpha, frames,
    // text and stage bounds.
    std::string dump_json(const std::string& menu) const;
    void reload() {} // movies come from the cache; nothing to re-read
    void set_in_world(bool in_world) { m_in_world = in_world; }
    bool in_world() const { return m_in_world; }

    // Debugging: any movie by file name, above the menus.
    void open_movie(const std::string& name, const std::string& scale_mode);
    void close_movies();

    struct Stats {
        float cpu_ms = 0, cpu_ms_avg = 0, gpu_ms = 0, gpu_ms_avg = 0;
        uint32_t draws = 0, layers = 0, menus = 0;
        uint64_t updates = 0;
    };
    Stats stats() const;

    // ---- used by the menus ---------------------------------------------------------------
    UiHost& host() { return m_host; }
    player::Library& library() { return *m_library; }
    std::string game_setting(const std::string& name) const;
    std::string translated(const std::string& key) const; // a $ key's text, the key itself without one
    // The calendar as the game writes it ("Sundas, 8:00 AM, 17th of Last Seed, 4E 201"), and the clock a player
    // info answer uses.
    std::string date_text(const Calendar& c, bool with_year = true) const;
    Calendar calendar(const PlayerInfo& p) const;
    // The calendar globals as the load order sets them.
    Calendar start_calendar() const;
    // Engine data captured from the game for a menu spud has no game system behind yet: <cache>/captures/<name>.json
    // as the game's "menu capture" writes it ({"menu", "frame", "paths": {path: value}}), or null.
    const player::json::Value* capture(const std::string& name);
    bool hud_recording() const { return m_options.hud_recording; }
    bool gamepad_active() const { return m_gamepad_active; }
    // The last input came from a pad or not: open menus are told.
    void set_gamepad_active(bool active);
    // A pad button by its hardware value (gamepad.txt): the event it has in the input contexts, as keyboard() does.
    void gamepad_button(uint32_t code, bool down);
    int target_width() const;
    int target_height() const;
    // SetButtonMapping's array for the device in use (mouse then keyboard).
    player::Object* button_mapping(player::Player& p);
    // The user event a mouse button maps to in the current input contexts ("" if none): ControlMap's lookup that
    // CheckForMouseEquip makes.
    std::string mouse_user_event(int button) const;
    // The button bound to a user event in the main gameplay context, keyboard first, then mouse ("Activate" ->
    // "E"), as looks it up without a gamepad; "" when unbound.
    std::string button_name(std::string_view user_event) const;
    void send_mouse_position(Menu& m);
    void report_error(const std::string& text);
    const MessageBoxData* current_message() const { return m_messages.empty() ? nullptr : &m_messages.back(); }
    bool has_message() const { return !m_messages.empty(); }
    MessageBoxData take_message();
    int journal_tab() const { return m_journal_tab; }
    // The data the last "NAME|DATA" open gave this menu, or empty.
    std::string show_data(const std::string& menu) const;
    // The button bound to a user event, as the engine's GetButtonFromUserEvent answers: the first
    // binding in control-map context order for the keyboard, then the mouse; the event's own name if none.
    std::string button_for_event(const std::string& event) const;
    // The loading screens (interface/loadingscreens.osl), null without them; the game, which their
    // conditions and picks draw from (seeded from the clock as the game's is, or by set_random_seed).
    const LoadingScreens* loading_screens();
    // The sound categories of the Audio page, in the game's order (empty without the import).
    const std::vector<SoundCategory>& sound_categories();
    const std::filesystem::path& interface_dir() const { return m_options.interface_dir; }
    fx::BsRandom& random() { return m_random; }
    void set_random_seed(uint32_t seed) { m_random.seed(seed, m_crt); }
    // Automation: the next loading screen shows this LSCR (form ID) instead of the pick.
    void force_loading_screen(uint32_t form_id) { m_forced_loading_screen = form_id; }
    uint32_t take_forced_loading_screen() { return std::exchange(m_forced_loading_screen, 0u); }
    // A thumbstick's raw position (0 left, 1 right; x right, y up, in [-1, 1]), as a gamepad's axis events set it;
    // each frame's poll delivers it (poll_thumbsticks).
    void set_thumbstick(int stick, float x, float y) {
        m_stick[stick & 1][0] = x;
        m_stick[stick & 1][1] = y;
    }
    // A fade as the game queues one: the data comes with the Fader Menu's show, or
    // with an update when it is open already, and the movie's initFade gets the
    // first four values. `stay` keeps the menu up after a fade to opaque.
    struct FadeRequest {
        bool arg0, arg1;
        double seconds, min_seconds;
        bool stay = false;
    };
    void request_fade(FadeRequest r) {
        m_fade_request = r;
        open("Fader Menu");
    }
    std::optional<FadeRequest> take_fade_request() { return std::exchange(m_fade_request, std::nullopt); }
    // Set while the load the main menu's New Game started is under way: that load ends without the Fader's fade in
    // from black (the byte).
    void set_new_game_load(bool on) { m_new_game_load = on; }
    bool take_new_game_load() { return std::exchange(m_new_game_load, false); }
    // The UI timer's whole milliseconds (the clock the Mist Menu's camera path runs on).
    uint32_t clock_ms() const { return static_cast<uint32_t>(m_time_ms); }
    // The Journal's show message carries a byte: open on the remembered tab (1) or on Quests (0). Esc and every other
    // opener pass 1, J from the keyboard 0. The next show of the Journal takes it.
    void open_journal(bool remembered_tab);
    bool take_journal_remembered() { return std::exchange(m_journal_remembered, true); }
    Menu* find_menu(const std::string& name) const { return find(name); }
    // ---- the UI 3D scenes -----------------------------------------
    // This frame's scenes, in the order the overlay draws them among the menus (MovieLayer::scene).
    std::span<const UiScene> ui_scenes() const { return {m_frame_scenes.data(), m_frame_scene_count}; }
    // The models they show, loaded off the render thread.
    SceneModels& scene_models() { return *m_scene_models; }
    // The bindless texture of Book Menu page `page` (render target 105 + page), for the 3D book's page shapes.
    uint32_t page_texture(int page);
    // The texture each page starts from (the game copies Base Page1's diffuse first): a cache path.
    void set_page_paper(const std::string& texture);
    // Automation (ui page): page `page`'s target as the last frame drew it, RGBA8, `size` squared.
    bool read_page(int page, std::vector<uint8_t>& rgba, uint32_t& size);
    // Interface/Credits.txt, line by line (the reader takes at most 127 bytes per line).
    const std::vector<std::string>& credits();
    void set_journal_tab(int tab) { m_journal_tab = tab; }

private:
    friend class Menu; // movies call back into the UI from their scripts (script depth)
    struct Message {
        std::string menu;
        bool show;
    };
    std::string canonical(const std::string& name) const;
    void process_messages();
    void show(const std::string& name);
    void hide(const std::string& name, bool force);
    Menu* find(const std::string& name) const;
    Menu* top_input_menu() const;
    std::vector<Menu*> by_depth() const;
    void by_depth(std::vector<Menu*>& out) const;
    void update_cursor();
    void poll_thumbsticks();
    void poll_wheel();
    int input_context() const;
    std::vector<int> context_stack() const;
    bool binds(const std::vector<int>& contexts, std::string_view event) const;
    // Control-map keyboard keys (DirectInput scan codes) -> Scaleform key events (section 3, Input).
    void keyboard(uint32_t dik, bool down);
    void deliver_key(uint32_t flash_code, bool down);
    void mouse_to_menus(float x, float y);
    void mouse_button(int button, bool down);
    void record_stats(float cpu_ms);
    // ui_scenes.cpp: a menu's movie layer with its UI 3D scene before or after it; the frame's pages.
    void add_menu_layers(Menu& m, float w, float h, std::vector<MovieLayer>& layers);
    void add_page_layers(std::vector<MovieLayer>& layers);
    void release_scene_textures();

    render::Renderer& m_renderer;
    UiHost& m_host;
    UiOptions m_options;
    std::unique_ptr<player::Library> m_library;
    std::unique_ptr<osu::Controls> m_controls;
    std::unique_ptr<osu::Controls> m_default_controls;
    bool m_gamepad_active = false;
    // The game: where the left stick points in a menu, and when it next steps.
    uint32_t m_direction = 0;
    double m_direction_next = 0;
    int m_direction_steps = 0;
    double m_ui_clock = 0;
    std::string m_remap_event;
    std::function<void(bool)> m_remap_done;
    bool finish_remap(int device, uint32_t code);
    std::unique_ptr<MovieRenderer> m_movie_renderer;
    std::vector<std::unique_ptr<Menu>> m_menus; // open, in opening order
    // The Mist Menu's default logo (the game keeps it in a global) and how long its scene has been up.
    std::string m_backdrop_logo = "meshes/interface/logo/logo.osm";
    std::map<std::string, std::unique_ptr<player::json::Value>> m_captures; // loaded on first use (null: none)
    std::map<std::string, std::string> m_show_data;
    double m_backdrop_seconds = 0;
    std::unique_ptr<LoadingScreens> m_loading_screens;
    std::optional<std::vector<SoundCategory>> m_sound_categories;
    bool m_loading_screens_read = false;
    fx::CrtRandom m_crt;
    fx::BsRandom m_random;
    float m_stick[2][2] = {{0, 0}, {0, 0}};        // gamepad thumbsticks (left, right), x and y up, in [-1, 1]
    float m_stick_polled[2][2] = {{0, 0}, {0, 0}}; // after the dead zone, as the last poll sent them
    std::optional<FadeRequest> m_fade_request;
    bool m_new_game_load = false;
    uint32_t m_forced_loading_screen = 0;
    std::vector<Message> m_queue;
    int m_script_depth = 0;
    int m_blur_count = 0;
    std::vector<MessageBoxData> m_messages; // newest last
    std::vector<std::string> m_hud_modes;   // hud_mode's, on now
    std::vector<MessageBoxData> m_auto_messages; // waiting for set_auto_message's answer
    int m_auto_message = -1;
    std::vector<std::unique_ptr<Menu>> m_debug_movies;
    std::string m_console_pending;
    std::vector<std::string> m_credits;
    bool m_credits_read = false;
    bool m_in_world = false;
    int m_journal_tab = 0;
    bool m_journal_remembered = true;
    double m_time_ms = 0;
    float m_mouse_x = 0, m_mouse_y = 0; // target pixels
    float m_wheel = 0;                  // the wheel's motion since the last frame's poll (poll_wheel)
    // Auto-repeat of Backspace, the arrows and Delete (fKeyboardRepeatDelay 0.3 s, fKeyboardRepeatRate 0.05 s).
    uint32_t m_repeat_dik = 0;
    double m_repeat_next = 0;
    std::vector<float> m_cpu_recent, m_gpu_recent;
    size_t m_recent_at = 0;
    float m_last_cpu_ms = 0;
    uint64_t m_updates = 0;
    std::vector<Menu*> m_frame_menus;          // update(): reused every frame
    std::vector<MovieLayer> m_frame_layers;
    std::vector<UiScene> m_frame_scenes;       // [0, m_frame_scene_count) are this frame's
    size_t m_frame_scene_count = 0;
    std::vector<ScenePage> m_frame_pages;
    std::unique_ptr<SceneModels> m_scene_models;
    std::array<uint32_t, 4> m_page_textures = {~0u, ~0u, ~0u, ~0u};
    uint32_t m_page_generation = 0;
    std::string m_paper;
    uint32_t m_paper_texture = ~0u;
};

} // namespace spud::ui
