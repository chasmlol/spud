#pragma once

// A movie player: one movie view (a menu), with its own VM and _global, as each Scaleform movie has. The
// Library holds what views share: loaded movies, the font map and the translation table.
//
// Frame model and event order follow Flash Player 8 as Ruffle documents it in code:
// clips run newest first, actions go through a queue with three priorities (initialize events, constructors,
// everything else), and timeline-placed clips run their first frame as soon as they are placed.

#include "ui/player/display.hpp"
#include "ui/player/images.hpp"
#include "ui/player/movie.hpp"
#include "ui/player/vm.hpp"

#include <mutex>
#include <thread>
#include <unordered_map>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::ui::player {

struct DrawList;

struct FontRef {
    const MovieDef* movie = nullptr;
    uint32_t font = osu::k_none;
    explicit operator bool() const { return movie != nullptr; }
};

class Library {
public:
    // `root`: the cache's interface directory. Loads the font map (fontconfig) and the translations of
    // `language` (translate_<language>).
    explicit Library(std::filesystem::path root, std::string language = "english");

    // Movies by the name they are referenced with ("startmenu.swf", "Inventory components/ItemCard.swf"); null
    // if there is no such movie.
    std::shared_ptr<const MovieDef> movie(std::string_view name);
    void read_ahead(); // starts reading every movie on a thread of its own
    // A font by name and style, from the font libraries ("Futura Condensed").
    FontRef font(std::string_view name, bool bold, bool italic);
    // A logical name ("$EverywhereFont") -> the font it maps to; or the name itself if unmapped.
    FontRef logical_font(std::string_view name, bool bold = false, bool italic = false);
    // "$KEY" -> text, or nullptr.
    const std::string* translate(std::string_view key) const;
    // A string game setting from the load order ("sConfirmNew"), or nullptr.
    const std::string* game_setting(std::string_view name) const;
    // A global variable's starting value from the load order ("GameHour"), or `fallback`.
    double global(std::string_view name, double fallback) const;
    const std::filesystem::path& root() const { return m_root; }
    std::vector<std::string> errors;
    // The images the engine registers for movies (img://NAME), shared by every view as GFx's loader is.
    ImageRegistry images;

private:
    std::filesystem::path m_root;
    std::mutex m_early_mutex;
    std::unordered_map<std::string, osu::Movie> m_early; // read ahead, by file name
    std::map<std::string, std::shared_ptr<const MovieDef>, std::less<>> m_movies;
    std::vector<std::shared_ptr<const MovieDef>> m_font_libraries;
    struct Logical {
        std::string font;
        uint32_t flags;
    };
    std::unordered_map<std::string, Logical> m_logical;
    std::unordered_map<std::string, std::string> m_strings;
    std::unordered_map<std::string, std::string> m_settings;
    std::unordered_map<std::string, double> m_globals;
    std::map<std::string, FontRef, std::less<>> m_font_cache;
    std::jthread m_reader; // last: it stops before what it fills goes
};

// What a movie asks of the engine.
class PlayerHost {
public:
    virtual ~PlayerHost() = default;
    // ExternalInterface.call(method, args...): GameDelegate's channel.
    virtual Value external_call(class Player& player, const std::string& method, const std::vector<Value>& args) = 0;
};

enum class MouseButton { left, right, middle };

// Clip event flags as SWF 6+ stores them (CLIPEVENTFLAGS read as a little-endian 32-bit value; SWF File Format
// Specification 19, PlaceObject2).
enum ClipEventFlag : uint32_t {
    ev_load = 1u << 0,
    ev_enter_frame = 1u << 1,
    ev_unload = 1u << 2,
    ev_mouse_move = 1u << 3,
    ev_mouse_down = 1u << 4,
    ev_mouse_up = 1u << 5,
    ev_key_down = 1u << 6,
    ev_key_up = 1u << 7,
    ev_data = 1u << 8,
    ev_initialize = 1u << 9,
    ev_press = 1u << 10,
    ev_release = 1u << 11,
    ev_release_outside = 1u << 12,
    ev_roll_over = 1u << 13,
    ev_roll_out = 1u << 14,
    ev_drag_over = 1u << 15,
    ev_drag_out = 1u << 16,
    ev_key_press = 1u << 17,
    ev_construct = 1u << 18,
};

class Player final : public VmHooks {
public:
    Player(Library& library, std::shared_ptr<const MovieDef> movie, PlayerHost* host);
    ~Player() override;
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    Vm vm;
    Library& library;
    PlayerHost* host;
    std::shared_ptr<const MovieDef> movie;
    std::unique_ptr<MovieClip> root;

    // Creates the root and runs frame 1 (GFx creates movie instances with the first frame initialised).
    void start();
    // GFx Advance(dt, 2): timelines step at the movie's frame rate from the elapsed time, at most two frames.
    void advance(double dt);
    // Exactly one movie frame (scripted runs step frames deterministically).
    void step_frame();
    double frame_rate() const;
    uint64_t frames() const { return m_frames; }
    double time_ms() const { return m_time_ms; }

    // ---- engine -> movie ---------------------------------------------------------------------
    // Calls a function by path from _root ("InitExtensions", "_root.MenuHolder.Menu_mc.SetPlatform").
    Value invoke(std::string_view path, const std::vector<Value>& args);
    bool has_path(std::string_view path);
    // A value by path from _root, and setting one ("CurrentTime", "_root.CodeObj").
    Value get_path(std::string_view path);
    void set_path(std::string_view path, const Value& v);
    // An ExternalInterface.addCallback registration ("call" for GameDelegate's engine calls).
    bool has_external_callback(std::string_view name) const { return m_external_callbacks.contains(name); }
    Value external_callback(std::string_view name, const std::vector<Value>& args);
    // GFx's invoke: an external callback by that name if registered, else an ActionScript path.
    Value invoke_or_callback(std::string_view name, const std::vector<Value>& args);

    // ---- input (stage pixels; key codes are Flash's Key codes) -------------------------------
    void mouse_move(float x, float y);
    void mouse_button(MouseButton b, bool down);
    void mouse_wheel(float delta);
    void key(uint32_t code, uint32_t ascii, bool down);
    void text_input(std::u32string_view text);
    float mouse_x() const { return m_mouse_x; }
    float mouse_y() const { return m_mouse_y; }
    bool key_down(uint32_t code) const { return m_keys_down.contains(code); }
    uint32_t last_key() const { return m_last_key; }
    uint32_t last_ascii() const { return m_last_ascii; }

    // Draw commands for the current frame, in stage pixels.
    void render(DrawList& out);

    // ---- used by the display list and the built-ins ------------------------------------------
    std::unique_ptr<DisplayObject> make_display_object(const MovieDef* movie, uint32_t character);
    void apply_place(DisplayObject* obj, const osu::PlaceCommand& c, bool is_new);
    // A timeline-placed object: its ActionScript object now, its constructor queued, its first frame run.
    void construct(DisplayObject* obj, const Value& init_object);
    // The names GFx gives a new object when it is created (text fields) or named (its unnamed parents).
    void name_on_creation(DisplayObject* obj);
    // A clip member GFx answers before it searches the clip's display list by name (get_display).
    bool answered_before_display_list(const MovieClip* clip, std::string_view name);
    // attachMovie's symbol: exported by the clip's own movie (or imported by it under that name), else by the
    // movies that import from it, up the chain (GFx's FindExportedResource). Null when none has it.
    const MovieDef* find_export(const MovieDef* local, std::string_view id, uint32_t& character);
    // A script-created clip (attachMovie, createEmptyMovieClip, duplicateMovieClip): constructed at once.
    void construct_scripted(DisplayObject* obj, const Value& init_object);
    DisplayObject* replace_character(MovieClip* parent, DisplayObject* obj, uint32_t character);
    void on_removed(DisplayObject* obj);
    void retire(std::unique_ptr<DisplayObject> obj);
    void run_init_actions(MovieClip* clip, const osu::Frame& frame);
    // The frame's DoAction blocks whose tags followed `after_min` to `after_max` of its place commands.
    void queue_frame_actions(MovieClip* clip, uint32_t frame, uint32_t after_min = 0,
                             uint32_t after_max = UINT32_MAX);
    void queue_call(DisplayObject* target, std::string_view method, std::vector<Value> args = {});
    void queue_function(Value fn, Value self, std::vector<Value> args = {});
    void run_queue();
    // Clip events (onClipEvent / on) bound by the timeline, plus the matching method (onLoad, ...).
    void fire_clip_event(DisplayObject* obj, uint32_t flag, std::vector<Value> args = {});
    Object* clip_prototype() const { return m_movieclip_proto; }
    Object* text_field_prototype() const { return m_textfield_proto; }
    Object* button_prototype() const { return m_button_proto; }
    DisplayObject* focus() const { return m_focus; }
    void set_focus(DisplayObject* obj);
    // Loads a movie into a clip at the start of the next frame (loads are asynchronous in Flash): `loaded` runs
    // after the swap (false if there is no such movie), `initialized` after the loaded movie's first frame.
    void queue_load(MovieClip* clip, std::string url, std::function<void(bool)> loaded,
                    std::function<void()> initialized);
    void collect_garbage();
    // AsBroadcaster messages to a global broadcaster's listeners ("Key", "Mouse", "Stage", "Selection"), queued.
    void broadcast(std::string_view kind, std::string_view method, const std::vector<Value>& args);
    // Intervals and timeouts (setInterval, setTimeout).
    uint32_t add_interval(Value fn_or_object, Value method, double ms, std::vector<Value> args, bool once);
    void clear_interval(uint32_t id);
    // Objects natives hold on to (kept alive by the collector).
    void pin(Object* o) { m_pinned.push_back(o); }
    std::vector<std::string> errors() const;
    // Timeline objects placed without a name are called "instance1", "instance2", ... in placement order. Shapes
    // take no number: GFx names only the objects script can reach, measured in the game's display lists, where
    // list entries made one after another are instance317, instance318, ... with their shapes between them
    // spud names shapes "shape1",.. for its own dumps.
    std::string default_instance_name(bool shape) {
        return shape ? "shape" + std::to_string(++m_shape_counter) : "instance" + std::to_string(++m_instance_counter);
    }
    // SPUD_UI_TRACE=names logs every name handed out, with the member a display-list search was looking for
    // (name_lookup) when that named it.
    static bool trace_names();
    std::string_view name_lookup;

    // Text fields: fonts by name (the field's own movie first, then the libraries), "$KEY" translation, and
    // the size and layout updates after a change.
    FontRef find_font(const MovieDef* movie, std::string_view name, bool bold, bool italic);
    std::string translate_text(std::string_view text) const;
    void text_changed(TextFieldObject* t);
    // The user changed an input field's text (typing, Backspace, Delete): onChanged to the field and its listeners.
    void user_changed(TextFieldObject* t);

    // Dragging (MovieClip.startDrag).
    void start_drag(MovieClip* clip, bool lock_center, bool constrained, float x0, float y0, float x1, float y1);
    void stop_drag();
    DisplayObject* drag_target() const { return m_drag.clip; }
    DisplayObject* hover() const { return m_hover; }
    // GFx Mouse.getTopMostEntity: the topmost object under a stage point (any, or only mouse targets).
    DisplayObject* topmost_entity(float x, float y, bool mouse_targets_only);

    // What the host sets and Stage reports (GFx: visibleRect, safeRect; scale mode and alignment).
    struct StageInfo {
        float visible[4] = {0, 0, 1280, 720}; // x0, y0, x1, y1 in stage pixels
        float safe[4] = {0, 0, 1280, 720};
        std::string scale_mode = "showAll", align; // the movie may change both (GFx applies them)
        float viewport[2] = {1280, 720};            // the target in pixels (Stage.width/height under noScale)
    } stage;
    bool cursor_shown = true; // Mouse.show / Mouse.hide

    // ---- VmHooks -----------------------------------------------------------------------------
    bool get_display(Object* o, std::string_view name, Value& out) override;
    bool set_display(Object* o, std::string_view name, const Value& v) override;
    Object* root_object() override;
    Object* parent_object(DisplayObject* d) override;
    std::string target_path(DisplayObject* d) override;
    void goto_frame(DisplayObject* d, uint32_t frame, bool play) override;
    bool goto_label(DisplayObject* d, std::string_view label, bool play) override;
    void play(DisplayObject* d, bool playing) override;
    void step(DisplayObject* d, int direction) override;
    DisplayObject* resolve_target(DisplayObject* from, std::string_view path) override;
    Object* display_object(DisplayObject* d) override;
    Object* revive(Object* dead) override;

private:
    enum class ActionKind : uint8_t { block, method, call, construct, broadcast, native };
    struct QueuedAction {
        ActionKind kind = ActionKind::block;
        DisplayObject* target = nullptr;
        const MovieDef* movie = nullptr;
        uint32_t function = osu::k_none; // block
        std::string method;              // method, broadcast (with `listener_kind`)
        std::string listener_kind;
        std::vector<Value> args;
        Value fn, self;                // call; construct: fn is the constructor
        std::vector<uint32_t> events;  // construct: onClipEvent(construct) blocks
        bool unload = false;           // runs even though the target was removed
        bool look_up = false;          // method: looked up even when the target has no such method (GFx's Load)
        std::function<void()> native;
    };
    struct PendingLoad {
        Object* clip; // the clip's object: the clip may be gone by the time the load runs
        std::string url;
        std::function<void(bool)> loaded;
        std::function<void()> initialized;
    };
    struct Interval {
        uint32_t id;
        Value fn_or_object, method;
        std::vector<Value> args;
        double period_ms, next_ms;
        bool once;
    };
    struct Drag {
        MovieClip* clip = nullptr;
        bool lock_center = false, constrained = false;
        float off_x = 0, off_y = 0;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    // Script runs from the engine nest (ExternalInterface handlers call back in); only the outermost one drains
    // the action queue and frees removed objects.
    struct ScriptScope {
        Player& p;
        explicit ScriptScope(Player& player) : p(player) { ++p.m_script_depth; }
        ~ScriptScope();
    };

    void frame();
    // The loads requested so far land: GFx's load queue, processed after each frame's actions in the same Advance.
    void land_loads();
    void maybe_collect();
    bool load_movie_into(MovieClip* clip, std::string_view url);
    void run_frame_avm1(MovieClip* clip); // Load or EnterFrame, then the timeline
    void advance_timeline(MovieClip* clip);
    void run_intervals();
    void push(int priority, QueuedAction a);
    void execute(QueuedAction& a);
    void flush_retired();
    void detach(DisplayObject* obj);
    void mark_removed(DisplayObject* obj);
    Object* make_display_as_object(DisplayObject* obj, Object* proto);
    Object* registered_class(const MovieDef* movie, uint32_t character);
    void apply_text_definition(TextFieldObject* t);
    // A movie this one imports from: its DoInitAction blocks (the classes of what it exports) run once, when the
    // first of its symbols is used.
    void init_library(const MovieDef* m);
    Value resolve_path_value(std::string_view path, Value* holder);

    // Mouse
    DisplayObject* topmost_mouse_target(float x, float y);
    DisplayObject* pick(DisplayObject* obj, float x, float y);
    bool wants_mouse(DisplayObject* obj);
    void button_event(DisplayObject* obj, uint32_t flag, std::vector<Value> args = {});
    void update_hover();
    void broadcast_clip_event(uint32_t flag, std::string_view method, const std::vector<Value>& args);

    // Drawing
    void draw(DisplayObject* obj, const Matrix2D& parent, const ColorXform& parent_cx, DrawList& out, bool mask);
    void draw_children(MovieClip* clip, const Matrix2D& m, const ColorXform& cx, DrawList& out, bool mask);
    void draw_content(DisplayObject* obj, const Matrix2D& m, const ColorXform& cx, DrawList& out, bool mask);
    void draw_drawing(const Drawing& d, const Matrix2D& m, const ColorXform& cx, DrawList& out);
    void draw_text(TextFieldObject* t, const Matrix2D& m, const ColorXform& cx, DrawList& out, bool mask);
    void draw_scale9(MovieClip* clip, const Matrix2D& m, const ColorXform& cx, DrawList& out, bool mask);

    Object* m_movieclip_proto = nullptr;
    Object* m_textfield_proto = nullptr;
    Object* m_button_proto = nullptr;
    std::deque<QueuedAction> m_queue[3]; // 0 normal, 1 constructors, 2 initialize events
    int m_script_depth = 0;
    bool m_draining = false;
    std::vector<PendingLoad> m_loads;
    std::vector<MovieClip*> m_exec; // clips by creation, oldest first; frames run newest first
    std::vector<std::unique_ptr<DisplayObject>> m_retired;
    std::vector<Interval> m_intervals;
    uint32_t m_next_interval = 1;
    uint32_t m_instance_counter = 0, m_shape_counter = 0;
    std::vector<Object*> m_pinned;
    double m_time_ms = 0;
    double m_frame_accum = 0;
    uint64_t m_frames = 0;
    bool m_advanced = false; // the first Advance has run
    float m_mouse_x = 0, m_mouse_y = 0;
    bool m_mouse_down = false;
    DisplayObject* m_hover = nullptr;
    DisplayObject* m_pressed = nullptr;
    DisplayObject* m_focus = nullptr;
    Drag m_drag;
    // ExternalInterface.addCallback(name, object, function).
    std::map<std::string, std::pair<Value, Value>, std::less<>> m_external_callbacks;
    std::set<uint32_t> m_keys_down;
    uint32_t m_last_key = 0, m_last_ascii = 0;
    std::vector<std::shared_ptr<const MovieDef>> m_loaded; // movies loaded into clips
    std::set<std::pair<const MovieDef*, uint32_t>> m_init_run; // DoInitAction blocks that ran
    std::vector<std::pair<DisplayObject*, std::pair<Matrix2D, ColorXform>>> m_topmost; // drawn last
    bool m_drawing_topmost = false;
    friend void install_display_library(Player& p);
    friend class DisplayAccess;
};

void install_display_library(Player& p);

} // namespace spud::ui::player
