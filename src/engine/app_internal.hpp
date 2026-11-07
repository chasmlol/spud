#pragma once

// The spud application: window or headless output, one Space, the free
// camera or the player on foot, the game UI, the debug UI and the script executor. Private to the engine.

#include "engine/destruction.hpp"
#include "engine/game_natives.hpp"
#include "engine/backdrop.hpp"
#include "engine/ui_scenes.hpp"
#include "engine/bench.hpp"
#include "engine/camera.hpp"
#include "engine/help_messages.hpp"
#include "engine/journal_source.hpp"
#include "engine/control.hpp"
#include "engine/character_menus.hpp"
#include "engine/doors.hpp"
#include "engine/game_clock.hpp"
#include "engine/world_map.hpp"
#include "engine/world_map_scene.hpp"
#include "engine/world_sim.hpp"
#include "engine/player.hpp"
#include "engine/input_focus.hpp"
#include "engine/item_menus.hpp"
#include "engine/local_map.hpp"
#include "engine/papyrus.hpp"
#include "engine/script.hpp"
#include "engine/sidebyside.hpp"
#include "engine/precip_occlusion.hpp"
#include "engine/ref_effects.hpp"
#include "engine/water.hpp"
#include "engine/vfx.hpp"
#include "engine/world_fx.hpp"
#include "engine/weather.hpp"
#include "engine/space.hpp"
#include "engine/user_settings.hpp"

#include "formats/okc.hpp"
#include "formats/oda.hpp"
#include "formats/ost.hpp"
#include "formats/osr.hpp"
#include "physics/physics.hpp"
#include "audio/system.hpp"
#include "render/renderer.hpp"

#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct SDL_Gamepad;

struct SDL_Window;
union SDL_Event;
struct ImDrawData;

namespace spud::engine {

using Clock = std::chrono::steady_clock;

inline double ms_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

struct Options {
    std::filesystem::path cache;
    std::string world = "tamriel";
    std::optional<std::array<int, 2>> cell;
    std::optional<glm::vec3> pos_units;
    bool pos_z_above_terrain = false; // "--pos x y +z"
    std::optional<float> yaw;
    std::optional<float> pitch;
    std::optional<float> draw_distance;
    uint32_t width = 0;
    uint32_t height = 0;
    std::filesystem::path screenshot;
    int frames = 5;
    bool validation = false;
    bool headless = false;
    std::filesystem::path make_test_cache;
    std::filesystem::path bench;
    std::filesystem::path bench_out;
    std::filesystem::path run;
    std::filesystem::path start; // --start: a script, then the session stays interactive
    std::filesystem::path startup; // script run in the interactive session (--startup)
    std::optional<uint16_t> control_port;
    std::optional<uint16_t> mirror_port; // --mirror: Mirror mode
    std::filesystem::path sbs_dir;       // --sbs-dir: where its telemetry goes
    int64_t sbs_qpc0 = 0;                // --sbs-t0: the performance counter at the session's start
    std::optional<std::array<int, 2>> window_pos; // --window-pos: the client area's top-left on the desktop
    bool vsync = true;
    bool vsync_given = false; // --no-vsync
    int rotation_mode = 0;
    bool show_disabled = false;
    bool walk = false; // start on foot
    uint32_t msaa = 4;
    bool msaa_given = false;
    bool play_clock = false;
    std::optional<float> time;  // game hour
    std::optional<float> time_scale;
    std::string weather;        // FormID (hex) or EditorID
    std::optional<float> fov;   // horizontal, degrees (as the game's fov command)
    bool world_given = false;  // --world
    std::optional<bool> menu;  // --menu / --no-menu; default: main menu when interactive (docs/UI.md)
    std::optional<bool> hud_recording; // --hud-recording / --no-hud-recording; default: scripted runs only
    bool windowed = false;     // --windowed: ignore the saved window mode this run
    std::filesystem::path settings; // --settings: read and write this settings file in any mode
    std::filesystem::path saves;    // --saves: saved games here instead of Documents\My Games\spud\Saves

    // Command-line options that go straight into the world, skipping the main menu.
    bool goes_to_world() const { return world_given || cell || pos_units || walk; }
    std::filesystem::path audio_cache; // default: cache
    int sound = -1;                    // -1: on when windowed, off (silent) when headless; 0 off; 1 on
};

// Game actions on keys (Settings > Controls), SDL scancodes. Index = Bind.
enum Bind : uint32_t { bind_forward, bind_back, bind_left, bind_right, bind_up, bind_down, bind_fast, bind_look,
                       k_bind_count };

class App;
// The player's head and body as the race menu edits them.
class PlayerFace final : public game::FaceHost {
public:
    explicit PlayerFace(App& app) : m_app(app) {}
    void face_changed(const game::FaceData& face) override;
    bool face_ready() const override;
    void zoom(bool face) override;

private:
    App& m_app;
};

class App final : public ScriptHost, public ui::UiHost, public ui::MapSource, public ui::SaveSource {
    friend class EngineCombat;
    friend class EngineMagic;
    friend class EngineProjectiles;
    friend class Vfx;

public:
    App(Options options, Clock::time_point process_start);
    ~App() override;

    int run_session(); // interactive window and/or --control
    int run_batch(std::vector<Command> commands); // --run, --screenshot
    int run_bench();

    // ScriptHost
    FreeCamera& camera() override { return m_camera; }
    std::optional<float> terrain_height(float x, float y) override;
    float cell_size() override;
    bool streaming_busy() override;
    void load_world(const std::string& edid) override;
    void set_resolution(uint32_t width, uint32_t height) override;
    void set_setting(const std::string& name, const std::string& value) override;
    void spawn(const std::string& mesh, const glm::vec3& position_units, float heading_deg) override;
    void play_effect(const std::string& model, const glm::vec3& position_units, float heading_deg) override;
    void request_screenshot() override;
    void request_ui_screenshot() override;
    void save_screenshot(const std::filesystem::path& file) override;
    void request_hdr_screenshot() override;
    void save_hdr_screenshot(const std::filesystem::path& file) override;
    void request_shadow_screenshot() override;
    void save_shadow_screenshot(const std::filesystem::path& file) override;
    std::string state_json() override;
    void set_walk_mode(bool walk) override;
    void place_player(const glm::vec3& feet, float heading_deg, float pitch_deg) override;
    void queue_player(const PlayerMove& move, int ticks) override;
    size_t player_scripted() override;
    void trace_player(const std::string& file) override;
    int player_action(const std::string& what, const std::string& arg) override;
    std::string ui_command(const std::string& sub, const std::string& args) override;

    // ui::UiHost (app_menus.cpp)
    void ui_action(const std::string& action) override;
    bool ui_has_saves() override;
    bool ui_mods_loaded() override { return false; }
    ui::MainMenuState ui_main_menu_state() override;
    void ui_console(const std::string& line) override;
    void ui_play_sound(const std::string& descriptor) override;
    uint32_t ui_start_sound(const std::string& descriptor) override;
    void ui_stop_sound(uint32_t id) override;
    ui::LoadingDestination ui_loading_destination() override;
    ui::PlayerInfo ui_player_info() override;
    bool ui_player_pose(float position[3], float& yaw) override;
    bool ui_player_sneaking() override;
    bool ui_can_rest(bool sleeping) override;
    void ui_pass_time(double hours) override;
    ui::SaveSource* ui_saves() override { return this; }                   // app_saves.cpp
    std::vector<ui::SaveListEntry> save_list() override;
    ui::SaveDetails save_details(size_t index) override;
    void save_over(int index) override;
    void load_save(size_t index) override;
    void delete_save(size_t index) override;
    void quick_save() override;
    bool can_save() override;
    bool can_load() override;
    void ui_autosave(const char* occasion) override;
    void ui_tween_opened() override;
    void ui_quicksave_key() override;
    void ui_quickload_key() override;
    std::vector<ui::SaveCharacter> save_characters() override;
    void select_character(uint32_t id) override;
    ui::MapSource* ui_map() override;                                      // app_world.cpp
    ui::JournalSource* ui_journal() override { return m_journal.get(); }
    void ui_control_changed(const std::string& event, int device, uint32_t code) override;
    std::string ui_text_setting(std::string_view name) const override { return m_game ? m_game->settings().text(name) : std::string(); }
    bool ui_compass_markers(std::vector<ui::CompassEntry>& out) override;
    std::vector<ui::MapEntry> map_open() override;
    bool map_frame(const ui::MapFrameInput& input, std::vector<ui::MapPlace>& places, double& heading) override;
    void map_click(size_t entry) override;
    void map_click_ground(double x, double y) override;
    void map_centre() override;
    void map_close() override;
    bool map_opens_local() override { return m_space && m_space->interior(); }
    std::string map_title() override { return save_location_text(); }
    bool papyrus_call(std::string_view target) override;
    std::vector<ui::MagicRow> ui_magic_rows() override; // app_combat.cpp
    bool ui_magic_equip(uint32_t form, int slot) override;
    bool ui_dialogue_live() override { return m_papyrus && m_papyrus->dialogue(); }
    void ui_dialogue_topic(int index) override;
    void ui_dialogue_skip() override;
    void ui_dialogue_closed() override;
    void dialogue_look(float dt); // the view in a conversation: the cursor's pan and the limits round the speaker
    void start_quests();
    void attach_dialogue();
    bool ui_loaded_area(float min[3], float max[3]) override;
    float ui_cell_north() override {
        return m_space && m_actor_data && m_space->interior() && !m_space->world().cells.empty()
                   ? m_actor_data->north_rotation(m_space->world().cells[0].form_id)
                   : 0.0f;
    }
    std::string ui_worldspace() override { return m_space ? m_space->edid() : std::string(); }
    // The worldspace or interior the camera is in: a merged city's own world, from its region tag.
    std::string current_location() const; // app_doors.cpp
    std::vector<ui::OptionValue> ui_options(const std::string& page) override;
    void ui_option_change(int id, double value) override;
    std::vector<ui::GraphicsOption> ui_graphics_options() override;
    void graphics_option_change(int id, double value);
    void prepare_graphics_settings();
    void apply_graphics_settings();
    void ui_settings_closed() override;
    float ui_game_setting(std::string_view name, float fallback) const override;
    std::string ui_scene_json(int slot) const; // ui scene
    static void save_png(const std::filesystem::path& file, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h);
    bool audio_command(const std::vector<std::string>& args, std::string& result) override;
    bool audio_busy() override;
    std::string door_command(const std::vector<std::string>& args) override; // app_doors.cpp
    // app_items.cpp: the player's items.
    std::string game_command(const std::vector<std::string>& args) override;
    void travel(const std::string& space, const Pose& pose) override;
    bool loading() override { return m_loading; }
    void attack(int hand, float seconds) override { m_script_attack[size_t(hand)] = seconds; }
    ui::ItemSource* ui_item_source() override;
    ui::CraftingSource* ui_crafting_source() override;
    ui::CharacterSource* ui_character() override;
    ui::RaceSexSource* ui_race_sex() override;
    int ui_beast_form() override;
    bool ui_meter_percent(int actor_value, double& percent) override;
    void ui_rest_started(bool sleeping, double hours) override;
    bool ui_training_card(ui::TrainingCard& card) override;
    void ui_train(int cost) override;
    void ui_rest_ended(bool sleeping, bool interrupted) override;
    bool ui_survival(double& temperature, double& cold, double& hunger, double& exhaustion) override;
    bool ui_book(ui::BookInfo& book) override;
    bool ui_lock(ui::LockInfo& lock) override;
    void ui_lock_event(std::string_view what, double pick_health) override;

    // The dialogue system: an actor starts or stops a voice line.
    int dialogue_prompt_tag(std::string_view tag);
    game::Game* game_state() const { return m_game.get(); }
    void actor_speak(uint32_t ref, const std::string& line, int emotion, int strength); // "": it stops
    std::optional<glm::vec3> speaker_position(uint32_t ref) const;                       // metres
    uint32_t body_sound_model(uint32_t ref) const; // the player's node output model, else none
    friend class PlayerFace;
private:
    void init(bool windowed, uint32_t width, uint32_t height, bool resizable, bool imgui,
              render::PresentMode present);
    void place_camera();
    // previous_time: the clock one frame back (default: the time of the previous call).
    std::array<float, k_zone_count> run_frame(float time, ImDrawData* ui,
                                              std::optional<float> previous_time = std::nullopt);
    void pump_events(bool& quit, bool interactive);
    // Mirror mode (app_sidebyside.cpp).
    void sbs_start();
    void sbs_stop();
    void sbs_input(bool& quit);
    void sbs_follow();
    void sbs_record(float dt);
    void sbs_mark(uint32_t number);
    bool sbs_same_space(const std::string& world) const;
    // The keyboard and mouse buttons game code polls: the devices', and the game's while mirroring.
    const bool* key_state();
    uint32_t mouse_state();
    void handle_event(const SDL_Event& e, bool& quit);
    CameraInput gather_input();
    void draw_ui();
    void apply_time_of_day(float hours);
    void load_atmosphere();
    void apply_sky_options(); // --time, --weather
    void update_audio();
    void apply_audio_settings(); // the Audio menu levels from the user settings
    // The user-settings key of a sound category the audio system has on its menu ("audio.music", ...).
    std::optional<std::string> audio_menu_key(uint32_t form_id) const;
    void serve_control(ControlServer& server, ScriptExecutor& executor);
    // app_player.cpp: physics and the player on foot.
    void load_rules();
    void create_physics();
    void destroy_physics();
    void step_player();
    void update_moved_refs(float frame_dt);
    bool m_ai_stepped = false; // play's clock: the next tick's AI has run
    void run_tick_tail(float tick_dt, float real_dt);
    bool m_tick_tail = false;     // play's clock: the last tick's scripts are still to run
    float m_tick_tail_dt = 0.0f;
    void release_physics();
    void hold_far_cascade(render::SunShadows& sun, const render::View& view, const glm::vec3& shadow_dir, const Space* space);
    render::ShadowCascade m_far_cascade;
    glm::vec3 m_far_cascade_camera{0.0f}, m_far_cascade_forward{0.0f}, m_far_cascade_sun{0.0f};
    const Space* m_far_cascade_space = nullptr;
    bool m_far_cascade_valid = false;
    struct SmoothRef {
        glm::vec3 before_position{0.0f}, after_position{0.0f};
        glm::quat before_rotation{1.0f, 0.0f, 0.0f, 0.0f}, after_rotation{1.0f, 0.0f, 0.0f, 0.0f};
        float idle = 0.0f; // seconds since physics moved it
    };
    std::unordered_map<uint32_t, SmoothRef> m_smooth_refs;
    const Space* m_smooth_space = nullptr;
    void apply_preset(const std::string& name);
    uint64_t world_digest();
    void start_on_foot(); // --walk: at --pos (the feet) or under the camera
    PlayerInput walk_input();
    void handle_walk_key(const SDL_Event& e);
    bool player_busy() const;
    bool set_player_setting(const std::string& name, const std::string& value);
    std::string player_json();
    void draw_player_ui();
    // app_menus.cpp
    void create_ui();
    void load_user_settings(bool interactive);
    void rebuild_bindings();
    void apply_startup_settings();
    void apply_user_setting(const std::string& id);
    void apply_window_settings();
    void save_user_settings();
    void start_new_game();
    // Saved games (app_saves.cpp).
    std::filesystem::path saves_dir() const;
    struct SaveRecord { // one per character (id, modded)
        uint32_t next_number = 1;
        uint32_t autosave_slot = 1;
        std::map<uint32_t, std::filesystem::path> slots;
        std::filesystem::path quicksave;
    };
    SaveRecord& save_record();
    bool modded();
    bool player_dead() const;
    bool player_down() const;                  // dead, or an essential player bled out
    void player_pain(bool say, bool imod);
    void player_struck();                      // GetHit, as the game gives it without the pain delay
    void player_died();                        // what Actor::Kill does for the player alone
    void update_player_condition(float dt);    // the heartbeat, low health, drowning, and going down
    void place_down_camera();
    void end_down_view();
    void revive_player();                      // a load: back to life, on a new body
    std::string save_stem(std::string_view prefix, uint32_t n);
    void begin_save(ost::Kind kind, const std::string& stem, uint32_t number, const std::filesystem::path& replaces = {});
    void manual_save(const std::filesystem::path& replaces = {});
    void autosave();
    void notify_cant_save();
    void update_saves(float dt);
    void finish_save_write();
    void rescan_records();
    void continue_game();
    bool load_game(const std::filesystem::path& file);
    const std::vector<ost::Plugin>& save_plugins();
    std::string save_location_text();
    std::string save_race_text() const;
    void ensure_papyrus();
    // The world simulation (app_world.cpp).
    void ensure_world_sim();
    void watch_cells(Space& space);
    void reset_actors(const WorldSim::CellCheck& check);
    bool ref_place(uint32_t ref, WorldSim::Place& place, glm::vec3& position) const;
    // Where references are (app_world_natives.cpp).
    uint32_t ref_location(uint32_t ref);
    uint32_t create_ref(uint32_t base, uint32_t at, bool inside, uint32_t level);
    void connect_quest_world();
    bool save_content_ok(const ost::Save& s, const std::filesystem::path& file);
    bool m_load_anyway = false; // the player said yes to what a save is missing
    std::vector<std::byte> save_actors() const;
    void load_actors(std::span<const std::byte> bytes);
    void update_attached_cells(const WorldSim::Place& place, int32_t cell_x, int32_t cell_y);
    const Space* m_attached_space = nullptr;
    int32_t m_attached_at[2] = {};
    std::vector<uint32_t> m_attached_cells; // sorted
    std::unordered_map<uint32_t, WorldSim::Place> m_place_of_created;
    void bind_world_natives();
    void refresh_quest_marks();
    void update_world_sim();
    glm::vec3 player_position_units() const;
    WorldSim::Place player_place(int32_t& cell_x, int32_t& cell_y);
    WorldSim::TravelChecks travel_checks() const;
    float player_walk_speed() const;
    void begin_fast_travel(uint32_t marker);
    void do_fast_travel(uint32_t marker_form);
    std::optional<render::View> map_view() const; // the world map's camera while the map is open
    void return_to_main_menu();
    void update_menus(); // per frame: closes the loading screen once the world has streamed in
    bool console_alias(const std::vector<std::string>& words);
    float aspect() const {
        const auto e = m_renderer->render_extent();
        return e.height > 0 ? float(e.width) / float(e.height) : 1.0f;
    }

    Options m_opt;
    Clock::time_point m_process_start;
    bool m_sdl = false;
    bool m_imgui = false;
    SDL_Window* m_window = nullptr;
    std::unique_ptr<render::Renderer> m_renderer;
    std::unique_ptr<ActorData> m_actor_data; // before the spaces: their actors read it
    float m_last_player_yaw = 0.0f;
    void process_actor_events();             // app_actors.cpp
    void actor_command(const std::string& line);
    void update_race_camera(float dt);
    void havok_impulse(uint32_t ref, const std::array<float, 3>& direction, float magnitude);
    void damage_ref(uint32_t form_id, float amount, bool self_damage);
    void clear_destruction(uint32_t form_id);
    void update_destruction(float dt);
    void activate_children(uint32_t parent);
    void update_delayed_activations(float dt);
    void trap_hit(uint32_t target, uint32_t trap, float damage, float pushback, const glm::vec3& velocity, const glm::vec3& position);
    void update_traps(float dt);
    uint32_t footstep_material(const ActorEvent& e);
    std::unique_ptr<Space> m_space;
    FreeCamera m_camera;
    SpaceSettings m_space_settings;
    bool m_hold_actors = false; // `set hold_actors`: game state held still, as the audit compares frames
    render::RenderSettings m_render_settings;
    std::vector<render::SpaceView> m_space_views;
    render::SunShadows m_sun_shadows; // this frame's cascades for the main view
    render::SunShadowDirection m_shadow_direction;
    float m_shadow_time = 0.0f; // the frame time of the last update of m_shadow_direction

    bool m_ui_visible = true;
    bool m_rmb_look = false;
    bool m_tab_look = false;
    float m_mouse_dx = 0, m_mouse_dy = 0, m_wheel = 0;
    std::deque<float> m_frame_ms;
    float m_last_cpu_ms = 0;
    std::array<float, k_zone_count> m_last_zones{};
    render::GpuTiming m_last_gpu;
    std::vector<render::GpuTiming> m_gpu_timings; // since the last take (bench matching)
    std::vector<render::GpuTiming> m_taken_timings; // scratch for take_gpu_timings
    int m_teleport[2] = {0, 0};
    float m_sun_azimuth = 40.0f, m_sun_elevation = 35.0f;
    float m_script_time = 0.0f; // the animation clock of scripted runs (`set animation_time`)
    float m_last_time = 0.0f;   // the world's clock of the previous frame
    float m_last_real_time = 0.0f;
    float m_paused_time = 0.0f; // real seconds menus have paused the game: the world's clock is real time less these
    // Exterior lighting from the imported climate and weather data. Without
    // atmosphere data the placeholder sun (azimuth, elevation) is used.
    std::optional<osa::Atmosphere> m_atmosphere;
    std::unique_ptr<WaterSystem> m_water_system;
    std::unique_ptr<WorldFx> m_world_fx;
    std::unique_ptr<Vfx> m_vfx; // made with the projectile world
    std::unique_ptr<RefEffects> m_ref_effects;
    std::vector<Space::FxShape> m_fx_shapes;
    bool m_was_swimming = false;
    float m_water_time = 0.0f;
    std::unique_ptr<Sky> m_sky; // made with the atmosphere (load_atmosphere)
    std::unique_ptr<PrecipOcclusion> m_precip_occlusion;
    std::span<const render::OffscreenView> m_prepass;
    // The sky options until the sky exists (--time, --weather, `set time_of_day`, `set weather`).
    float m_sky_hour = 12.0f;
    GameClock m_clock;
    struct PendingSave {
        ost::Save save;
        std::filesystem::path path, replaces;
        int frames_left = 0; // until the frame's picture is ready
    };
    std::unique_ptr<PendingSave> m_pending_save;
    std::future<void> m_save_write;
    struct SaveFile {
        std::filesystem::path path;
        ost::Header header;
        std::string location, player_name, race;
        std::filesystem::file_time_type written{};
        bool corrupt = false;
    };
    static constexpr uint32_t k_all_characters = 0xFFFFFFFFu; // $ShowAllSave
    std::vector<ost::Plugin> m_save_plugins;
    std::optional<bool> m_modded;
    std::map<uint64_t, SaveRecord> m_save_records;
    std::filesystem::path m_last_save;   // what a death reloads
    std::filesystem::path m_load_request; // loaded at the next frame's start
    Clock::time_point m_last_save_time = Clock::now();
    bool m_autosave_after_move = false;
    bool m_saving_disabled = false;      // Game.SetInChargen
    int m_autosave_in = 0;               // frames until the Tween Menu's autosave
    int m_after_loading = 0;
    // A load with its screen up, waiting to run (the game comes first).
    struct PendingLoad {
        ost::Save save;
        std::filesystem::path file;
    };
    std::unique_ptr<PendingLoad> m_load;
    bool m_loading_save_screen = false; // up for a save's load
    void finish_load();
    bool m_load_go = false; // the screen is up: the next frame starts the load
    enum class LoadSettle : uint8_t { none, wanted, tick, done }; // a save's first tick, still behind its screen
    LoadSettle m_load_settle = LoadSettle::none;
    void behind_screen(const std::function<void()>& work);
    float m_death_timer = 0;
    bool m_down_view = false;        // The game is up
    float m_down_yaw = 0.0f;
    View m_view_before_down = View::first_person;
    audio::SoundId m_down_loop{};    // MAGShoutSlowTimeActiveLP
    audio::SoundId m_heartbeat{};
    int m_heartbeat_band = 0;        // 0 none, 1 slow, 2 fast
    audio::SoundId m_drowning{};
    float m_low_health_wait = 0.0f;
    float m_pain_wait = 0.0f;
    uint32_t m_falls_seen = 0;
    void refresh_saves();
    std::vector<SaveFile> m_saves; // what the panel's list shows, newest first
    uint32_t m_save_character = 0; // the list's filter
    bool m_loading_save = false; // until the screen lifts: the game writes no detach times and checks no cells
    uint32_t m_character_id = 0;
    double m_play_seconds = 0;
    std::unique_ptr<WorldSim> m_world_sim;
    bool m_world_sim_failed = false;
    // The player's worldspace, worked out again when the cell changes.
    const Space* m_place_space = nullptr;
    int32_t m_place_cell[2] = {0, 0};
    uint32_t m_place_world = 0;
    glm::vec3 m_last_exterior{0.0f}; // player
    uint32_t m_last_exterior_world = 0;
    // The world map.
    struct MapRow {
        uint32_t marker;  // 0: not a map marker
        uint32_t world;
        float position[3]; // on the map's coordinates
        bool quest = false;  // shown off the screen too, at its edge
        bool hidden = false; // a later objective on a target already listed
        bool custom = false;
    };
    // Quest targets as the map and compass show them: the target, or the load door that leads to it.
    struct QuestMark {
        uint32_t ref = 0;
        glm::vec3 position{0.0f}; // Skyrim units
        glm::vec3 walk{0.0f};     // where the way there leaves this space, the target itself if it doesn't
        uint32_t world = 0;
        bool door = false;
        std::string text;
    };
    std::vector<QuestMark> m_quest_marks;
    int m_quest_marks_in = 0; // frames until they're worked out again
    WorldMapCamera m_map_camera;
    std::vector<MapRow> m_map_rows;
    bool m_map_open = false;
    std::optional<glm::vec3> m_custom_marker;
    // Fast travel: the fade before the map hides, then the pending move.
    uint32_t m_travel_fade_marker = 0, m_travel_pending = 0;
    float m_travel_fade = 0;
    float m_last_travel_hours = 0;
    bool m_travel_from_map = false;
    std::vector<uint32_t> m_high_actors;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_actors_by_cell; // by their records' cells
    uint32_t m_actor_resets = 0;
    uint32_t m_sky_weather = 0;
    std::optional<float> m_sky_time_scale;
    bool m_sky_weather_pending = false; // m_sky_weather is forced (fw) once the sky has a climate
    bool m_vsync = true;
    uint64_t m_control_seq = 0;
    std::unique_ptr<SideBySide> m_sbs;
    std::array<bool, SDL_SCANCODE_COUNT> m_sbs_keys{};
    bool m_sbs_resync = false;
    double m_sbs_apart_since = -1, m_sbs_look_since = -1, m_sbs_view_since = -1, m_sbs_rest_since = -1;
    double m_sbs_last_row = 0, m_sbs_last_world = 0;
    float m_sbs_drift = -1, m_sbs_look_drift = -1; // from the game's player: units, degrees; -1 when not comparable
    uint64_t m_sbs_frame = 0;
    uint32_t m_sbs_weather = 0, m_sbs_crosshair = 0;
    std::string m_sbs_menus, m_sbs_location, m_sbs_missing_world;

    // On foot (app_player.cpp).
    std::optional<osr::Rules> m_rules;
    std::unique_ptr<Papyrus> m_papyrus; // made when something first needs a script
    std::unique_ptr<game::DialogueOutput> m_dialogue_output;
    std::unique_ptr<game::CombatWorld> m_combat_world;
    std::unique_ptr<game::MagicWorld> m_magic_world;
    struct AttackHold {
        float seconds = -1, next_power = 0; // below 0: not held
        bool armed = false;
    };
    std::array<AttackHold, 2> m_attack_held; // left, right
    uint32_t m_player_beast = 0;
    // The furniture the player is in: the ref, and how long getting up has left.
    struct Seat {
        uint32_t ref = 0;
        float leaving = -1;
        bool view_back = false; // work furniture is played in third person: back to first after
        std::string enter;      // the idle's event, until the graph takes it
        float enter_left = 3.0f;
    } m_seat;
    void furniture_default(const game::Target& target);
    void sit_player(uint32_t ref);
    void stand_player();
    void update_seat(float dt, bool moving);
    void script_door(uint32_t form_id, bool open);
    bool swing_room_door(const osw::Ref& door);
    // ObjectReference.TetherToHorse and Actor.SetVehicle (app_carts.cpp): units, radians.
    struct CartTether {
        uint32_t cart = 0, horse = 0;
        glm::vec2 front{0.0f}, rear{0.0f}; // the axles
        float z = 0, pitch = 0, roll = 0, heading = 0;
        bool held = false; // SetMotionType keyframed: it stays where it is
        std::deque<glm::vec3> trail; // where the horse has been
    };
    std::vector<CartTether> m_carts;
    std::unordered_map<uint32_t, uint32_t> m_vehicles; // rider: what it rides
    std::unordered_map<uint32_t, std::string> m_vehicle_idles; // PlayIdle that waits for a rider's body
    std::unordered_map<uint32_t, std::string> m_seat_idles;    // what each rider sits as
    float m_sitting_rotation = 0;
    std::optional<float> m_vehicle_base;
    bool m_first_person_geometry = true; // Game.ShowFirstPersonGeometry
    const CartTether* cart_of(uint32_t rider) const;
    void tether_cart(uint32_t cart, uint32_t horse);
    void hold_cart(uint32_t cart, bool held);
    void set_vehicle(uint32_t rider, uint32_t vehicle);
    void leave_vehicle(uint32_t rider);
    void update_carts(float dt);
    glm::vec3 npc_rides(const game::Agent& rider, ActorSet& actors, const glm::vec3& feet);
    std::unordered_map<uint32_t, uint32_t> m_npc_mounts; // who is in the saddle: the horse
    void vehicle_look();
    void vehicle_view();
    void seat_left();
    bool m_player_ai_driven = false; // Game.SetPlayerAIDriven: a script walks the player, the keys don't
    // A kill move begun in first person, waiting for the third-person body; then the view to go back to.
    struct PendingKillMove {
        uint32_t lead = 0, target = 0;
        std::string event;
        float waited = 0.0f;
    } m_kill_move;
    bool m_kill_move_view = false;
    // The kill camera (app_kill_camera.cpp).
    struct KillCamera {
        bool active = false, running = false, ending = false, placed = false, ranged = false, near_done = false;
        uint32_t projectile = 0; // what the shots follow; 0 until it's launched
        uint32_t target = 0;
        const game::CameraPath* path = nullptr;
        size_t shot = 0;
        int phase = 0;                                  // 0 before the blow, 2 once it has landed
        float elapsed = 0, total = 0, limit = 0, max = 0, min = 0, started = 0, delay = 0;
        const okc::Model* model = nullptr;
        glm::vec3 location{0.0f}, look{0.0f};           // the anchors, units
        std::string image_space;                        // EditorID
        float fov = 0;                                  // the view's own
        float rate = 1;                                 // the time multiplier, easing to the shot's
        glm::vec3 last_position{0.0f};
        float last_yaw = 0, last_pitch = 0;
        glm::mat3 last_sway{1.0f};
    };
    KillCamera m_kill_camera;
    std::unique_ptr<okc::Cameras> m_kill_cameras;
    bool m_kill_cameras_loaded = false;
    const okc::Model* kill_camera_model(const std::string& path);
    float vats_area_free(uint32_t subject, int side);
    float vats_target_visible(uint32_t subject, uint32_t target, int side);
    const game::CameraPath* pick_kill_camera(uint32_t target, int action);
    void try_ranged_kill_camera(game::LaunchData& shot);
    uint32_t aim_target(const game::LaunchData& shot);
    bool ranged_odds(uint32_t target, const game::Item* weapon);
    bool spell_kills(uint32_t target, const game::Spell& spell, bool dual);
    void start_kill_camera(uint32_t target, const game::CameraPath* path);
    void end_kill_camera();
    void next_kill_shot();
    void begin_kill_shot(const game::CameraShot& shot);
    glm::vec3 kill_shot_node(int place);
    void place_kill_camera(const game::CameraShot& shot);
    void update_kill_camera(float seconds);
    void step_kill_rate(float target, float seconds);
    // pieces with a body, gone after their time.
    struct DebrisPiece {
        uint32_t id = 0;
        float left = 0.0f;
    };
    std::vector<DebrisPiece> m_debris;
    uint32_t m_next_debris = 0xFD000000u;
    void spawn_debris(uint32_t debr, uint32_t count, const glm::vec3& at, float scale);
    void wire_world_fx();
    std::vector<uint32_t> m_paired; // NPCs in a pair: held, and left where the marks put them
    bool m_body_third_person = false; // the view whose body has been told how the weapon stands
    struct Dying {
        uint32_t ref = 0, killer = 0;
        float left = 0;
    };
    std::vector<Dying> m_dying; // between OnDying and OnDeath
    struct ScriptMove {
        uint32_t target = 0;
        std::array<float, 3> at{};
        bool match_rotation = false;
        uint32_t stack = 0;
        bool wakes = false;
    };
    std::vector<ScriptMove> m_script_moves;
    void apply_script_moves();
    // Telekinesis: what the player holds in the air, and what they threw until it lands.
    struct Telekinesis {
        uint32_t ref = 0;
        bool thrown = false, dual = false, at_hand = false, armed = false;
        float speed = 0, life = 0, hold = 0, reach = 0, charge = 0;
    } m_telekinesis;
    void update_telekinesis(float dt);
    std::unordered_map<uint32_t, float> m_sent_speed; // weaponSpeedMult as the graphs have it
    float m_time_rate = 1.0f;   // Slow Time: the world's clocks
    float m_player_rate = 1.0f; // and the player's own
    std::array<float, 3> m_script_attack{}; // `attack`: seconds the hand's button stays held
    double m_fixed_step = 0; // a tour's world tick in seconds; 0: real time
    static constexpr double k_world_tick = 1.0 / 60.0;
    bool m_tour = false;          // a bench tour: everything by its clock, nothing by real time
    double m_tick_accum = 0;      // play: real time not yet stepped
    std::array<int32_t, 3> m_warmed_at{}; // the space and cell the navmeshes were last warmed round
    double m_player_step = 0;     // a tour: what this frame moves the player's clock by
    float m_shown_fraction = 0.0f; // a tour: how far this frame is between two ticks
    float m_tick_blend = 1.0f;     // the same, on a frame that steps the world: 1 where every frame steps it
    bool m_world_still = false;   // a tour: a frame between ticks: it draws, nothing steps
    std::unique_ptr<game::ProjectileWorld> m_projectile_world;
    bool m_quests_started = false;
    MovementTuning m_tuning;
    std::unique_ptr<physics::World> m_physics;
    std::unique_ptr<Destruction> m_destruction;
    struct DelayedActivation {
        uint32_t ref = 0;
        uint32_t parent = 0;
        float seconds = 0.0f;
    };
    std::vector<DelayedActivation> m_delayed_activations;
    struct AnimationWait {
        uint32_t ref = 0;
        std::string event; // lowercase
        uint32_t stack = 0;
    };
    std::vector<AnimationWait> m_animation_waits;
    std::unique_ptr<game::FaceHost> m_face_host;
    // the pose it moves to and how far along it is.
    bool m_race_camera_face = true;
    float m_race_camera_time = 1.0f;
    glm::vec3 m_race_camera_from{0.0f};
    glm::vec2 m_race_camera_from_angles{0.0f};
    bool m_race_camera_on = false;
    struct TrapContact {
        float wait = 0.0f; // seconds until it can hit again
        bool touching = false;
    };
    std::unordered_map<uint32_t, TrapContact> m_traps;
    std::unique_ptr<Player> m_player;
    bool m_walk = false;
    float m_camera_orbit = 0.0f; // radians, `set camera_orbit`
    std::optional<std::array<float, 5>> m_camera_free; // units and degrees, `camera free`
    float m_fly_fov = 0;
    Clock::time_point m_last_step{};
    glm::vec2 m_walk_look{0.0f}; // walk mode: mouse counts since the last frame
    bool m_sneak_pressed = false;
    bool m_sprint_held = false; // walk mode: the sprint key last frame
    bool m_script_run = true; // scripts: the run flag ('run on|off')
    bool m_ai_processing = true; // scripts: 'ai on|off' (the console's ToggleAI), kept for players made later
    std::vector<uint32_t> m_watch; // scripts: 'watch' references, likewise kept for players made later
    bool m_always_run = true;
    bool m_auto_move = false;
    bool m_jump_pressed = false;
    void gamepad_input(PlayerInput& in);
    // Each frame: the pad as the rest of the game sees it (Ui::gamepad_button, the actions below).
    void poll_gamepad();
    void walk_event(std::string_view event); // a control map event of the main context
    bool pad_held(const char* event) const;
    std::array<bool, 16> m_pad_held{};
    uint16_t m_pad_seen = 0;
    bool m_pad_trigger[2] = {};
    uint32_t m_pad_search = 0;
    SDL_Gamepad* m_gamepad = nullptr; // walk mode: the first gamepad, opened on first use
    uint16_t m_pad_buttons = 0;       // its buttons last frame, as XInput bits
    std::filesystem::path m_trace_file;
    // Game UI (src/ui) and what it drives.
    std::unique_ptr<ui::Ui> m_ui;
    std::unique_ptr<Backdrop> m_backdrop; // the Mist Menu's scene, drawn while it is open
    std::unique_ptr<LocalMap> m_local_map; // the Map Menu's local map, drawn while it is shown
    // uGridsToLoad:General, the attached cells' square (the image's default 5; "set grids_to_load N").
    int m_grids_to_load = 5;
    std::optional<std::array<float, 6>> m_local_map_area; // "ui localmap_area": a measured loaded area replayed
    std::unique_ptr<WorldMapScene> m_map_scene; // what the world map draws
    std::unique_ptr<UiScenes> m_ui_scenes; // the menus' UI 3D scenes (item preview, starfield, book)
    InputFocus m_focus;
    UserSettings m_user;
    std::filesystem::path m_user_file; // empty: settings are not persisted (scripts, benches, control)
    std::array<int, k_bind_count> m_binds{};
    bool m_invert_y = false;
    bool m_free_look = false;          // console "tfc": mouse look without holding a key
    ScriptExecutor* m_executor = nullptr; // the running loop's, for console lines
    uint64_t m_console_seq = 0;
    double m_ui_dt = 0;                // UI clock step for the next frame
    bool m_quit_requested = false;     // Quit from a menu
    bool m_loading = false;
    bool m_loading_new_game = false; // this load is New Game's: its quests start behind the screen
    ui::LoadingDestination m_loading_destination; // where the load shown by the Loading Menu goes
    bool m_loading_destination_set = false;
    int m_loading_frames = 0;
    Clock::time_point m_loading_start;
    std::unique_ptr<audio::AudioSystem> m_audio;
    audio::Environment m_audio_env;
    std::optional<uint32_t> m_audio_interior; // `audio env interior`: an interior cell for the audio rules
    // Water ambience sampling, the game job: one point of an N x N grid per frame.
    struct WaterSampler {
        uint32_t i = 0, j = 0;
        std::vector<glm::vec3> samples;
        glm::vec3 emitter{0};
        bool have_emitter = false;
        uint32_t water = 0;
    } m_water;
    void sample_water(audio::Environment& env);

    // app_doors.cpp: load doors as seamless portals between spaces.
    struct DoorPortal {
        uint32_t index = 0;    // into m_space->world().doors
        Doorway near, far;     // far: in the destination space
        PortalTransform to_far;
        bool same_space = false; // the far door is in this space too
        bool gate = false;       // ... back to back: walked through on real ground, nothing carried through
        bool seamless = false;   // both doorways known
        bool faces_camera = false; // the far door doesn't lead back: the near doorway's side is the camera's
        bool open = false;
        bool activated = false;  // opened by the player (stays open until they walk away)
        bool automatic = false;  // opens as anyone comes near
        bool travels = false;    // nothing to open (a ladder, a hatch): through the loading screen, as the game
    };
    // Another space kept loaded: what open doors lead to, or the one just left.
    struct OtherSpace {
        std::string name; // lowercase EditorID (its file)
        std::unique_ptr<physics::World> physics; // when walking; declared first: its actors leave it as the space goes
        std::unique_ptr<Space> space;
        std::future<std::unique_ptr<Space>> loading; // read and built on a worker
        uint64_t used = 0; // frame it was last needed
    };
    // A door moving or standing open, seen from both spaces: one leaf, drawn in the space on each side of the doorway
    // The partner door stays hidden, but for its frame.
    struct DoorSwing {
        // A moving part as it stands now in the door's own space: mesh space to there, and its box in mesh space.
        struct Leaf {
            glm::mat3 rotation{1.0f};
            glm::vec3 position{0.0f}, lo{0.0f}, hi{0.0f};
        };
        std::vector<Leaf> leaves;
        uint32_t door = 0, partner = 0; // the reference that moves, and the one at the doorway's other end
        std::string space, partner_space;
        const oda::Door* anim = nullptr;
        const oda::Door* partner_anim = nullptr;
        glm::mat3 rotation{1.0f}, partner_rotation{1.0f}; // with scale
        glm::vec3 position{0.0f}, partner_position{0.0f};
        PortalTransform to_partner;
        Doorway near, far;
        float time = 0.0f; // seconds along Open, or Close once closing
        bool closing = false;
    };
    std::optional<float> leaf_hit(const DoorSwing& s) const;
    std::vector<DoorSwing> m_swings;
    std::unordered_map<uint32_t, oda::Door> m_door_anims; // by DOOR
    float m_swing_time = 0.0f;
    DoorSwing* find_swing(uint32_t door);
    Space* space_named(const std::string& name);
    void turn_doors(Space& space);
    DoorPortal* aimed_door(float& distance);
    const osw::Ref* aimed_room_door();
    physics::World* physics_of(const Space* space);
    uint32_t aimed_hidden_door(float& distance);
    bool door_open(uint32_t form_id);
    void start_swing(const osw::Door& d, const DoorPortal& door);
    // Doors that lead nowhere: a room's. Here, references of an animated DOOR, as World::refs indices.
    std::vector<uint32_t> m_room_doors;
    bool activate_room_door();
    void update_swings(float time);
    void play_door_sound(const oda::Door* anim, bool open, const glm::vec3& position);
    void set_door_shut(uint32_t form_id, const std::string& space);
    std::vector<DoorPortal> m_doors;
    std::vector<std::unique_ptr<OtherSpace>> m_others;
    struct RetiredSpace {
        std::unique_ptr<physics::World> physics; // first, as in OtherSpace
        std::unique_ptr<Space> space;
        uint64_t free_at = 0; // door frame
    };
    std::vector<RetiredSpace> m_retired;
    std::string m_sky_world = "tamriel"; // the exterior last in force: its climate holds in interiors
    bool m_doors_auto = true; // the free camera opens doors it comes near
    bool m_doors_preload = true; // a load in an interior also makes the exteriors its doors lead to
    int m_portal_door = -1;   // index into m_doors drawn through this frame, -1 none
    render::View m_portal_view;
    // A door's far frame (render/renderer_portal.cpp): what the main frame's space has, for the far one.
    render::SunShadows m_portal_sun;
    render::SunShadowDirection m_portal_shadow_direction;
    std::unique_ptr<WorldFx> m_portal_fx;
    std::unique_ptr<WaterSystem> m_portal_water;
    std::vector<Space::FxShape> m_portal_fx_shapes;
    float m_portal_time = 0.0f;
    render::PortalInput m_portal_input;
    glm::vec3 m_door_last_camera{std::numeric_limits<float>::quiet_NaN()};
    uint64_t m_door_frame = 0;
    uint32_t m_crossings = 0;
    void rebuild_doors();
    void finish_space_loads();
    void preload_exteriors();
    void update_doors(float time);
    void retire_spaces();
    void pick_portal(const render::View& view);
    void prepare_portal(const render::View& view, float time);
    void load_door_animations();
    bool set_door_open(DoorPortal& door, bool open, bool activated);
    void cross_door(const DoorPortal& door);
    void activate_door();
    bool travel_door(uint32_t form_id);
    void update_head_tracking(float dt);
    void update_gazes(float dt, ActorSet& actors, const glm::vec3& eye, uint32_t speaker);
    bool can_head_track(uint32_t ref, const game::Agent& a, uint32_t target, bool current, bool large);
    void swap_ref_model(const osw::Ref& ref, const std::string& mesh);
    std::string base_model(uint32_t base) const;
    int play_gamebryo(uint32_t ref, std::string_view name, bool start_over, float ease_in);
    void update_object_sequences();
    const std::unordered_map<uint32_t, ots::Swap>* texture_swaps();
    std::optional<std::unordered_map<uint32_t, ots::Swap>> m_texture_swaps;
    std::unordered_map<uint64_t, ots::Swap> m_addon_swaps;
    // The lid of a container (the game activation and the menu's end, both through).
    void container_open(uint32_t ref, bool open);
    uint32_t m_open_container = 0;
    // References a script plays a sequence on (PlayGamebryoAnimation): the effect shows them while the placed model hides.
    struct ObjectSequence {
        uint32_t handle = 0;
        std::string sequence; // to start; "": it runs
        std::string running;
        bool start_over = false;
        float ease_in = 0.0f;
        bool back = false; // when it ends the placed model shows again
    };
    std::unordered_map<uint32_t, ObjectSequence> m_object_sequences;
    void apply_destruction();
    uint32_t interior_sky_weather(const Space& space);
    std::unordered_map<uint32_t, uint32_t> m_interior_sky; // Show Sky cell -> this visit's weather
    void update_player_worn();
    std::vector<uint32_t> m_player_worn;
    size_t m_max_decals = 1000; // uMaxDecals:Decals
    uint32_t m_player_right = 0, m_player_left = 0;
    bool m_player_dressed = false;
    bool m_redraw_weapon = false; // the rebuilt player body draws as soon as it can
    uint32_t m_head_tracker = 0; // the actor told to look at the player
    struct Gaze {
        uint32_t target = 0; // the default slot, high
        float update = 0.0f; // Actor
        float hold = -1.0f;  // high
        int large = -1;      // race size 3, once asked
        bool along_path = false; // looking where it walks
        float eye_height = 0.0f; // its head above its feet, units, as of the last update
    };
    std::unordered_map<uint32_t, Gaze> m_gazes; // who high-process actors look at when nothing else asks
    std::unordered_map<uint32_t, uint32_t> m_look_at; // Actor.SetLookAt: actor to what it looks at
    // app_items.cpp: the game's item side, made by a new game or the player on foot; without it the menus replay
    // captures. The crosshair's target each frame, and the Activate key.
    void start_game();
    void connect_game(); // the game and the scripts know each other, once both exist
    void update_character(float dt);
    void papyrus_activate(uint32_t ref, uint32_t base); // OnActivate on a reference's scripts
    // app_refs.cpp: scripted references as cells come and go, and the player in their trigger volumes.
    void index_script_refs();
    void reset_papyrus();
    void cell_scripts(uint32_t cell, bool loaded);
    void update_triggers();
    void trigger_moved(uint32_t ref); // a script moved a reference: it's a trigger where it goes
    const Space* m_primitive_space = nullptr;
    std::unordered_map<uint32_t, uint32_t> m_primitive_index; // reference: its primitive in this space
    struct TriggerRef {
        uint32_t ref = 0;
        bool inside = false;
        std::vector<uint32_t> actors; // who else is in it
        // Its shape, in metres: from the world's table, or a trigger activator's own primitive (XPRM).
        bool has_box = false, sphere = false;
        glm::vec3 position{0.0f}, half{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
    std::vector<TriggerRef> m_trigger_refs;
    // Who hears which actor's animation events (RegisterForAnimationEvent), and what is moving (TranslateTo).
    struct AnimationListener {
        uint64_t handle = 0;
        uint32_t sender = 0;
        std::string event; // lower case
    };
    std::vector<AnimationListener> m_animation_listeners;
    std::vector<Translation> m_translations;
    void update_translations(float dt);
    std::function<void(uint32_t ref)> m_ref_moved;
    std::future<std::unordered_map<uint32_t, std::vector<uint32_t>>> m_script_refs_ready;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_script_refs;
    uint32_t pick_ref();
    uint32_t pick_loose(float range_m); // the nearest moving object on the crosshair
    void update_crosshair();
    void activate();
    void activate_target(const game::Target& target);
    void take_ref(const game::Target& target);
    void drop_item(const game::Stack& stack);
    std::unique_ptr<game::Game> m_game;
    std::unique_ptr<ItemMenus> m_item_menus;
    std::unique_ptr<CharacterMenus> m_character_menus;
    float m_body_health = -1, m_body_stamina = 0; // what the movement simulation was last given
    const physics::World* m_physics_seen = nullptr;
    float m_alias_spell_timer = 0;
    std::unordered_set<uint64_t> m_sleep_listeners; // RegisterForSleep
    std::vector<uint32_t> m_alias_spells; // what quest aliases on the player cast
    uint32_t m_crosshair_ref = 0;
    bool m_crosshair_open = false;
    ActorSet::Entry m_pick_actor{};
    game::Target target_of(uint32_t ref);
    uint32_t m_player_cell = 0;
    uint32_t m_watching = 0; // `game watch`
    // app_ai.cpp: the actors' AI, once the placed actors are read.
    void update_ai(float dt, bool early = false);
    void connect_combat();                   // app_combat.cpp
    RefEffects& ref_effects();               // app_combat.cpp
    bool magic_node(uint32_t ref, uint8_t source, glm::vec3& out); // app_combat.cpp: metres
    uint32_t player_space() const;           // app_ai.cpp: its WRLD, or the interior CELL
    uint32_t outer_space(uint32_t space) const;
    void player_noise(int level);
    int m_action_sound = 0;       // actor, the player's
    float m_action_sound_left = 0;
    void update_combat(float dt);
    void update_attack_input(float dt);
    void update_projectiles(float dt);       // app_projectiles.cpp
    void projectile_command(const std::string& line);
    // Riding (app_mount.cpp).
    enum class MountStage : uint8_t { none, entering, riding, leaving };
    struct MountState {
        uint32_t ref = 0, npc = 0;
        MountStage stage = MountStage::none;
        float timeout = 0;
        bool steal = false;
        game::MountData data;
        bool in_air = false;
        float air_time = 0;
        bool dragon = false;
        bool waiting_for_body = false; // the paired clips play on the third-person body
        bool landing = false;     // a landing request is on
        float landing_timer = 0;  // fFlyingMountLandingRequestTimer
        glm::vec3 leash{0.0f};    // the point the dragon circles, units
    };
    MountState m_mount;
    void update_actor_bodies(ActorSet& actors);
    std::vector<ActorSet::Solid> m_solids;
    std::vector<physics::World::ActorBody> m_actor_bodies;
    std::unordered_map<uint32_t, bool> m_race_is_npc; // by RACE: has ActorTypeNPC
    bool m_mount_first_person = false; // the view to go back to
    HelpMessages m_help;
    std::vector<uint32_t> m_talkers; // NPCs looking at each other
    void show_harvested(const osw::Ref& r);
    // bindings the player changed, kept beside the settings file. (event, device) -> code.
    std::map<std::pair<std::string, int>, uint32_t> m_control_changes;
    std::vector<osr::KeyBinding> m_default_keys;
    bool m_controls_dirty = false, m_controls_loaded = false;
    void load_control_map();
    void save_control_map();
    // a round robin, one real check an update.
    struct LosRecord {
        uint64_t handle = 0;
        uint32_t viewer = 0, target = 0;
        int type = 0;           // 0 single gain, 1 single lost, 2 continuous
        int last = 0;           // 0 new, 1 had it, 2 didn't
    };
    std::vector<LosRecord> m_los;
    size_t m_los_cursor = 0;
    void update_los_events();
    std::unique_ptr<GameJournal> m_journal;
    uint32_t m_trainer = 0; // the Training Menu's
    std::optional<std::pair<int, int>> trainer_skill() const; // the actor value taught, and the level it stops at
    // The camera shake: what's left of it, and the loop's clock.
    float m_shake_strength = 0.0f, m_shake_left = 0.0f, m_shake_clock = 0.0f;
    // Game.ShakeCamera and what else shakes: `source` in Skyrim units, duration 0 for fCameraShakeTime.
    void shake_camera(const glm::vec3& source, float strength, float duration);
    void step_camera_shake(float dt);
    void follow_camera_node(float dt);
    void sprint_breath(float dt);
    float m_body_direction = 0.0f; // the Direction channel: turns clockwise from ahead
    void player_air_actions();
    physics::ControllerState m_air_state = physics::ControllerState::on_ground;
    float m_air_velocity_z = 0.0f; // units/s, as the last frame left it
    float m_breath_wait = 0.0f;
    // ThirdPersonState's animated camera: the node, the blend's direction and weight, and its last pose.
    struct CameraDelta {
        std::string node;
        float rate = -1.0f, weight = 0.0f;
        glm::vec3 translation{0.0f}; // game units
        glm::quat rotation{1, 0, 0, 0};
    };
    CameraDelta m_camera_delta;
    bool m_ride_any_dragon = false; // `game ride`: as if Bend Will had taken it
    void mount(const game::Target& target);
    game::Target target_of_actor(uint32_t ref, uint32_t npc);
    void mounted();
    void dismount();
    void dismounted();
    void update_mount(float dt);
    void mount_dragon(const game::Target& target, const game::MountData& data);
    void update_dragon(float dt);
    void dragon_camera();
    bool tamed_dragon(uint32_t ref) const;
    void end_mount_stage();
    void apply_roster(Space& space);
    void offer_furniture();
    const Space* m_ai_furniture_space = nullptr;
    float player_light();
    void update_detection(float dt);
    void update_greetings(float dt);
    float m_detection_timer = 0, m_player_light = 0, m_ai_walk_speed = 0, m_ai_run_speed = 0;
    std::future<std::vector<WorldActor>> m_ai_placing;
    bool m_ai_ready = false;
    uint32_t m_ai_roster = 0;
    uint32_t m_roster_wait = 0; // ticks since the actors last took the roster
    bool m_seen = false; // `game seen`: stands for AI's detection in automation
    std::vector<game::Witness> crime_witnesses();
    void apply_jail_move();
    float m_encumbered_reminder = 0;
    void update_encumbrance(float dt);
    std::vector<const osw::Ref*> m_pick_refs;
    OtherSpace* other_space(const std::string& name, bool load);
    void evaluate_environment(Space& space, float dt);
    std::string doors_json() const;
};

} // namespace spud::engine
