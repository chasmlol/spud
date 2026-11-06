// Map, Crafting, Race Sex, Tutorial and the remaining menus, implemented (addresses
// there). What their game systems would supply (map markers, recipes and the inventory, races and the face, help
// messages) comes from the user's own game through the game until spud has those systems. The seam is one
// place per menu: the values the engine wrote into the movie (Menu::captured, a capture's "paths") and the calls it
// made (engine_calls, a capture's "calls"), which a game system will produce instead. Without a capture a menu
// still opens and behaves per its contract with empty data, and says once in the log what it is missing.

#include "ui/item3d.hpp"
#include "ui/items.hpp"
#include "ui/character.hpp"
#include "ui/menus_internal.hpp"

#include "ui/player/json.hpp"
#include "ui/ui.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "formats/osu.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace spud::ui {

namespace {

using Value = player::Value;
using Object = player::Object;
namespace json = player::json;

const json::Value* member(const json::Value* o, std::string_view name) {
    if (!o) {
        return nullptr;
    }
    for (const auto& [k, v] : o->members) {
        if (k == name) {
            return &v;
        }
    }
    return nullptr;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// Once per capture name and process: what a menu runs without.
void missing(const std::string& menu, const std::string& capture, const char* what) {
    static std::set<std::string> said;
    if (said.insert(capture).second) {
        log::info("ui {}: no capture '{}' ({}); the menu shows no {}",
                  menu, capture, what, what);
    }
}

// The movies trace through GameDelegate.call("myLog",...); the game registers no such call for these menus, so
// The game ignores it. It is answered quietly here rather than logged as unhandled.
class WorldMenu : public Menu {
public:
    WorldMenu(Ui& ui, MenuDesc d) : Menu(ui, std::move(d)) {}
    // Script errors the movie reports are logged as they appear (the engine's data driving a movie it was not
    // meant for shows up here first).
    void advance(double dt, double engine_ms) override {
        Menu::advance(dt, engine_ms);
        if (m_close_posted) {
            m_close_posted = false;
            m_ui.close(name());
        }
        if (player()) {
            const auto errors = player()->errors();
            for (size_t i = m_errors_logged; i < errors.size() && i < 20; ++i) {
                log::warn("ui {}: {}", name(), errors[i]);
            }
            m_errors_logged = errors.size();
        }
    }

protected:
    void register_handlers() final {
        handle("myLog", [](Call&) {});
        handlers();
    }
    virtual void handlers() {}
    // A hide the menu posts for itself on a user event: the game queues it (UIMessageQueue), so it is handled with
    // the next messages, after the key event of the same press has reached this menu's movie.
    void post_close() { m_close_posted = true; }
    size_t m_errors_logged = 0;
    bool m_close_posted = false;
};

// ---- engine data recorded in the game ------------------------------------------------------------------------
// the game's "record start NAME.capture.json MENU...". "record stop" writes every call the game's engine makes
// into a menu's movie as a capture {"menu", "frame", "calls": [{frame, menu, kind, name, args}]}: the game
// callbacks (kind "callback", name the callback), the game ("invoke", name the path), SetVariable
// ("set", args[0] the value), SetVariableArray ("set_array", args[0] the first index, then the elements), and the
// movies' own GameDelegate calls to the engine ("delegate"). Captures from this branch's earlier recorder have
// callbacks as {"kind": "delegate", "name": "call", args: [callback, ...]} and set's value in "value"; both are read.
// A menu replays the calls it has no game system for, in the engine's order and at the engine's frame offsets from
// the first call.

struct EngineCall {
    double frame = 0;
    std::string kind; // "callback", "invoke", "set", "set_array", "delegate"
    std::string name; // callback: its name; invoke / set / set_array: the path; delegate: the movie's call
    const json::Value* args = nullptr;
    const json::Value* value = nullptr;
    // The earlier recorder's the game callback: kind "delegate", name "call", the callback first.
    bool old_callback() const { return kind == "delegate" && name == "call"; }
    // A game object callback's name, or empty.
    std::string callback() const {
        if (kind == "callback") {
            return name;
        }
        return old_callback() && args && !args->items.empty() ? args->items[0].s : std::string();
    }
    // The callback's own arguments start here in args.
    size_t first_arg() const { return old_callback() ? 1 : 0; }
};

std::vector<EngineCall> engine_calls(Ui& ui, const std::string& capture) {
    std::vector<EngineCall> out;
    const json::Value* calls = member(ui.capture(capture), "calls");
    if (!calls) {
        return out;
    }
    for (const json::Value& c : calls->items) {
        EngineCall e;
        const json::Value* f = member(&c, "frame");
        e.frame = f ? f->n : 0;
        if (const json::Value* k = member(&c, "kind")) {
            e.kind = k->s;
        }
        if (const json::Value* n = member(&c, "name")) {
            e.name = n->s;
        }
        e.args = member(&c, "args");
        e.value = member(&c, "value");
        if (!e.value && e.kind == "set" && e.args && !e.args->items.empty()) {
            e.value = &e.args->items[0];
        }
        out.push_back(std::move(e));
    }
    return out;
}

// A value the game wrote: display objects come as {"path": "/a/b", "kind":...} and resolve to the same clip here.
Value replay_value(Menu& m, const json::Value& v) {
    if (v.kind == json::Value::object) {
        const json::Value* path = member(&v, "path");
        const json::Value* kind = member(&v, "kind");
        if (path && kind && path->kind == json::Value::string && !path->s.empty() && path->s[0] == '/') {
            std::string p = "_root";
            for (char c : path->s.substr(1)) {
                p += c == '/' ? '.' : c;
            }
            return m.player()->get_path(p);
        }
    }
    return player::json_to_value(m.player()->vm, v);
}

std::vector<Value> replay_args(Menu& m, const json::Value* args, size_t first) {
    std::vector<Value> out;
    for (size_t i = first; args && i < args->items.size(); ++i) {
        out.push_back(replay_value(m, args->items[i]));
    }
    return out;
}

// Plays one recorded call into the menu's movie as the engine made it. The movie's own calls to the engine
// ("delegate") are not the engine's and are not played.
void replay(Menu& m, const EngineCall& c) {
    if (!m.player()) {
        return;
    }
    if (!c.callback().empty()) {
        m.send(c.callback(), replay_args(m, c.args, c.first_arg()));
    } else if (c.kind == "invoke") {
        m.invoke(c.name, replay_args(m, c.args, 0));
    } else if (c.kind == "set" && c.value) {
        m.player()->set_path(c.name, replay_value(m, *c.value));
    } else if (c.kind == "set_array" && c.args && !c.args->items.empty()) {
        // SetVariableArray: the elements from the index on, the array made when the path holds none.
        auto& vm = m.player()->vm;
        Value arr = m.player()->get_path(c.name);
        if (!arr.is_object() || arr.o->kind != player::ObjKind::array) {
            arr = Value::object(vm.make_array());
            m.player()->set_path(c.name, arr);
        }
        const size_t at = size_t(std::max(0.0, c.args->items[0].n));
        for (size_t i = 1; i < c.args->items.size(); ++i) {
            if (arr.o->elements.size() <= at + i - 1) {
                arr.o->elements.resize(at + i);
            }
            arr.o->elements[at + i - 1] = replay_value(m, c.args->items[i]);
        }
    }
}

// The calls of a capture from the first one the menu wants, played as the menu's frames pass.
class CallPlayer {
public:
    using Filter = std::function<bool(const EngineCall&)>;
    bool load(Ui& ui, const std::string& capture, Filter keep) {
        m_calls.clear();
        for (EngineCall& c : engine_calls(ui, capture)) {
            if (keep(c)) {
                m_calls.push_back(std::move(c));
            }
        }
        m_next = 0;
        m_frames = 0;
        return !m_calls.empty();
    }
    // These calls from the next frame on, at their recorded offsets from the first.
    void play(std::vector<EngineCall> calls) {
        m_calls = std::move(calls);
        m_next = 0;
        m_frames = 0;
    }
    // The calls due by this frame of the menu (the first call's frame is the menu's first).
    void advance(Menu& m) {
        if (m_calls.empty()) {
            return;
        }
        const double due = m_calls.front().frame + double(m_frames);
        for (; m_next < m_calls.size() && m_calls[m_next].frame <= due; ++m_next) {
            replay(m, m_calls[m_next]);
        }
        ++m_frames;
    }
    bool done() const { return m_next >= m_calls.size(); }

private:
    std::vector<EngineCall> m_calls;
    size_t m_next = 0;
    uint64_t m_frames = 0;
};

// What the loader does itself (SetButtonMapping, InitExtensions, SetPlatform) and the cursor, which spud sends
// on its own, are never replayed.
bool loader_call(const EngineCall& c) {
    const std::string cb = c.callback();
    return cb == "SetButtonMapping" || c.name == "InitExtensions" || c.name == "SetPlatform" ||
           c.name == "_root.InitExtensions" || c.name == "_root.SetPlatform" ||
           (c.kind == "delegate" && !c.old_callback());
}

// ---- help messages: the Tutorial Menu --------------------------------------------------------------------------
// ShowTutorial takes a default-object index; the default object is a MESG whose title (FULL) and text
// (DESC) the Tutorial Menu shows. The messages' text is not in the import cache yet, so it comes from the game
// (capture tutorial-<key>, Menu_mc.HelpTitle.text and Menu_mc.HelpText.text as the game set them).

struct Tutorial {
    int index;       // into the default object table
    const char* key; // its DOBJ key
};
// The help messages the world group's menus ask for (default object table entries, form type 105 MESG).
constexpr Tutorial k_tutorials[] = {
    {239, "HBFG"}, {240, "HBCO"}, {241, "HBML"}, {242, "HBTA"}, {243, "HBOC"},
    {244, "HBEC"}, {245, "HBSM"}, {246, "HBSA"}, {247, "HBAL"}, {251, "HBMM"},
};

struct HelpMessage {
    std::string title, text;
};

std::optional<HelpMessage> help_message(Menu& m, const std::string& key) {
    const std::string capture = "tutorial-" + lower(key);
    const auto title = m.captured(capture, "Menu_mc.HelpTitle.text");
    const auto text = m.captured(capture, "Menu_mc.HelpText.text");
    if (!title || !text || !m.player()) {
        return std::nullopt;
    }
    return HelpMessage{*m.player()->vm.to_string(*title), *m.player()->vm.to_string(*text)};
}

// The tutorials shown (the game keeps the list in the save).
std::set<int>& tutorials_shown() {
    static std::set<int> shown;
    return shown;
}

// ShowTutorial: once per message, while bShowTutorials:Interface is on (compiled default 1), and only when the
// message exists (here: when it was captured).
void show_tutorial(Menu& from, int index) {
    const Tutorial* t = nullptr;
    for (const Tutorial& k : k_tutorials) {
        if (k.index == index) {
            t = &k;
        }
    }
    if (!t || tutorials_shown().contains(index)) {
        return;
    }
    if (!help_message(from, t->key)) {
        missing(from.name(), "tutorial-" + lower(t->key), "help message");
        return;
    }
    from.ui().open(std::string("Tutorial Menu|") + t->key);
    tutorials_shown().insert(index);
}

class TutorialMenu final : public WorldMenu {
public:
    // Creator: TutorialMenu, ExactFit, depth 10, flags 0x411, Menu Mode; keeps Menu_mc.
    explicit TutorialMenu(Ui& ui)
        : WorldMenu(ui, {"Tutorial Menu", "TutorialMenu", ScaleMode::exact_fit, 10, 0x411, 0, 1}), m_blur(ui) {}

    void on_show() override {
        // Show with a game object: Menu_mc.HelpTitle.SetText(the message's name),
        // Menu_mc.HelpText.text = its description with carriage returns as spaces, then HideAmiiboArt, ApplyButtonArt.
        const auto msg = help_message(*this, m_ui.show_data(name()));
        if (!msg) {
            return; // no data: nothing set (the game answers 1 and leaves the movie as it is)
        }
        std::string text = msg->text;
        std::replace(text.begin(), text.end(), '\r', ' ');
        const Value title = player()->get_path("Menu_mc.HelpTitle");
        player()->vm.call_method(title, "SetText", {Value::string(msg->title)});
        player()->vm.set(player()->get_path("Menu_mc.HelpText"), "text", Value::string(text));
        invoke("Menu_mc.HideAmiiboArt");
        invoke("Menu_mc.ApplyButtonArt");
    }
    bool on_user_event(const std::string& event) override {
        // UI message 7 with the Cancel user event (UserEvents) posts the hide.
        if (event == "Cancel") {
            post_close();
            return true;
        }
        return false;
    }

private:
    BlurHold m_blur;
    void handlers() override {
        handle("GetButtonFromUserEvent", [this](Call& c) {
            c.respond({Value::string(m_ui.button_for_event(*player()->vm.to_string(c.arg(0))))});
        });
        handle("CloseMenu", [this](Call&) { m_ui.close(name()); }); // hide "Tutorial Menu"
    }
};

// ---- Map Menu ------------------------------------------------------------------------------------------------
// The Flash layer: markers, the bottom bar, the date, the local map button. With a world loaded the host's MapSource
// supplies the markers and places them through the map camera; without one they are
// where a capture of the game's map camera put them.
//
// Markers (): the engine sets WorldMap.MarkerData to {name, icon type,
// undiscovered} per marker and calls WorldMap.SetNumMarkers(n); each frame, while WorldMap.GetCreatingMarkers()
// answers true it calls WorldMap.CreateMarkers() (the movie makes ten a call), afterwards it sets MarkerData to
// {show, x, y, rotation} per marker (x, y in 0..1 of the map) and sends RefreshMarkers and SetSelectedMarker(index).

// What the importer wrote of each worldspace for the local map (<cache>/interface/worldspaces.osu): its flag
// byte and the north rotation of its NorthMarker (importer/convert/ui_worlds.cpp). Read once, on the first
// map.
struct WorldspaceMap {
    uint32_t flags = 0;
    float north = 0;
};

WorldspaceMap worldspace_map(Ui& ui, std::string edid) {
    static std::optional<std::map<std::string, std::string>> table;
    if (!table) {
        table.emplace();
        try {
            const osu::Strings t = osu::read_strings(read_file(ui.interface_dir() / "worldspaces.osu"));
            for (const auto& e : t.entries) {
                auto at = [&](uint32_t o) {
                    return std::string(o < t.strings.size() ? t.strings.data() + o : "");
                };
                table->emplace(at(e.key), at(e.text));
            }
        } catch (const std::exception& e) {
            log::warn("ui: worldspaces: {} (import the interface with skytool ui import)", e.what());
        }
    }
    for (char& c : edid) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    WorldspaceMap w;
    if (auto it = table->find(edid + ".flags"); it != table->end()) {
        w.flags = uint32_t(std::strtoul(it->second.c_str(), nullptr, 10));
    }
    if (auto it = table->find(edid + ".north"); it != table->end()) {
        w.north = std::strtof(it->second.c_str(), nullptr);
    }
    return w;
}

class MapMenu final : public WorldMenu {
public:
    // Constructor: movie "Map", ExactFit, depth 3, flags 0x9005, input context 7 (Map). Its LocalMapMenu
    // registers render target 16 with the image loader under the name "Local_Map" for as long as the
    // menu lives; the movie's InitMap loads img://Local_Map into its LocalMapHolder_mc.
    explicit MapMenu(Ui& ui) : WorldMenu(ui, {"MapMenu", "Map", ScaleMode::exact_fit, 3, 0x9005, 0, 7}) {
        m_ui.library().images.add_render_target(k_local_map_image);
    }
    ~MapMenu() override {
        m_ui.library().images.release(k_local_map_image);
        if (m_source) {
            m_source->map_close(); // closed without a hide: every menu going at once
        }
    }

    const LocalMapView* local_map_view() const override { return m_local && m_camera.valid ? &m_camera.view : nullptr; }

    void on_show() override {
        // Show (message 1): the open fade is queued, the game with (0, 1,
        // fMapTransitionSpeed:MapMenu x 0.7, 0, x 0.3): initFade(true, true, 0.525, 0.225), which the Fader movie
        // plays as black fading to clear over 0.525 s after 0.225 s. Then the date with the year goes to
        // SetDateString.
        m_ui.request_fade({true, true, k_transition * 0.7, k_transition * 0.3});
        m_hud_mode.set(true); // "WorldMapMode"
        const PlayerInfo p = m_ui.host().ui_player_info();
        send("SetDateString", {Value::string(m_ui.date_text(m_ui.calendar(p), true))});
        m_source = m_ui.host().ui_map();
        init_local_map();
        if (m_source) {
            m_open_local = m_source->map_opens_local();
            load_live(m_world, m_source->map_open());
        } else {
            load(m_world);
        }
        show_tutorial(*this, 251); // "Help - Map Menu", HelpWorldMapShortPC
    }
    bool on_hide() override {
        // Hide (message 3): the world fades back in from black (initFade(true, true, fMapTransitionSpeed x 0.75, 0))
        // and the TweenMenu the map may have been opened from goes too.
        m_ui.request_fade({true, true, k_transition * 0.75, 0});
        m_hud_mode.release();
        m_ui.close("TweenMenu");
        if (m_source) {
            m_source->map_close();
            m_source = nullptr;
        }
        return true;
    }
    void advance(double dt, double engine_ms) override {
        WorldMenu::advance(dt, engine_ms);
        if (!player()) {
            return;
        }
        // Each frame: the local map's camera and markers while it is up, then the world map's
        if (m_local) {
            update_local_camera();
            frame(m_local_markers);
        }
        m_frame_dt = dt;
        frame(m_world);
        // Once the map is ready, centred on a player in an interior: the local map, in front of the world map.
        if (m_open_local && m_settled) {
            m_open_local = false;
            toggle_local_map();
        }
    }
    // The game (vtable) while the local map is shown (CanProcess):
    // LocalMapMoveMode (the left mouse button in the Map context) pressed with the cursor inside the local
    // map's extents starts the drag, released ends it (a release outside ends it too); while
    // dragging, the mouse's moves add (-dx, dy) x fMapLocalMousePanSpeed (20) to the camera's pan input
    // which the next update scales and turns onto the translation.
    bool input_event(std::string_view event, float value) override {
        // The game (vtable, ProcessButton): the world map's move keys while held, and the
        // look mode on MapLookMode, the cursor going to the middle of the screen as it starts and ends.
        for (size_t i = 0; i < std::size(k_move_events); ++i) {
            if (event == k_move_events[i]) {
                m_move_held[i] = value != 0;
                return !m_local;
            }
        }
        if (event == "MapLookMode" && !m_local) {
            m_looking = value != 0;
            m_ui.centre_cursor();
            return true;
        }
        if (event != "LocalMapMoveMode" || !m_local) {
            return false;
        }
        const player::Matrix2D m = stage_to_pixels(float(m_ui.target_width()), float(m_ui.target_height()));
        float cx = 0, cy = 0;
        m.apply(player()->mouse_x(), player()->mouse_y(), cx, cy);
        const bool inside = cx >= m_local_extents[0] && cx <= m_local_extents[2] &&
                            cy >= m_local_extents[1] && cy <= m_local_extents[3];
        if (!inside) {
            if (value == 0) {
                m_dragging = false;
            }
            return false;
        }
        m_dragging = value != 0;
        return true;
    }
    void mouse_delta(float dx, float dy) override {
        if (m_local && m_dragging) {
            m_camera.pan[0] += -dx * k_mouse_pan_speed;
            m_camera.pan[1] += dy * k_mouse_pan_speed;
        }
        if (m_looking) {
            m_look_input[0] += dx;
            m_look_input[1] += dy;
        }
    }
    bool holds_cursor() const override { return m_looking; }

    bool on_user_event(const std::string& event) override {
        // UI message 7: Cancel or Quick Map close the map, LocalMap toggles the local map, Journal
        // opens the journal, PlayerPosition goes back to the player (as CurrentLocationCallback).
        if (event == "Cancel" || event == "Quick Map") {
            close_map();
            return true;
        }
        if (event == "LocalMap") {
            toggle_local_map();
            return true;
        }
        if (event == "Journal") {
            m_ui.open_journal(true); // with 1: the remembered tab
            return true;
        }
        if (event == "LocalMapMoveMode") {
            return true; // the input handler took it (input_event)
        }
        if ((event == "Zoom In" || event == "Zoom Out") && !m_local) {
            // The world map's zoom input: the event's value x the real frame time x fMapZoomMouseSpeed:MapMenu (2).
            m_zoom_input += float((event == "Zoom In" ? 1.0 : -1.0) * m_frame_dt * 2.0);
            return true;
        }
        if ((event == "Zoom In" || event == "Zoom Out") && m_local) {
            // The game (vtable, ProcessButton): the event's value times
            // fMapLocalMouseZoomSpeed:MapMenu (0.1) from the mouse, negative for Zoom Out, onto the camera's zoom input
            // the next update adds it to the zoom. A wheel notch is a press of value 1.
            m_camera.zoom_input += event == "Zoom In" ? k_mouse_zoom_speed : -k_mouse_zoom_speed;
            return true;
        }
        if (event == "PlayerPosition") {
            if (m_source) {
                m_source->map_centre();
            }
            return true;
        }
        return event == "PlacePlayerMarker";
    }

private:
    static constexpr double k_transition = 0.75; // fMapTransitionSpeed:MapMenu
    static constexpr std::string_view k_move_events[] = {"Up", "Down", "Left", "Right"};
    // fMapMoveKeyboardSpeed:MapMenu (0.02) goes onto the camera's pan input on each frame a key is held, with no frame
    // time: the game's 60 frames a second make that 1.2 of the pan speed.
    static constexpr float k_key_pan_speed = 0.02f * 60.0f;

    void on_loaded() override {
        // After loading, the constructor sets WorldMap.PlayerLocationMarkerType = 66 (the IconTypes index of
        // YouAreHereMarker).
        player()->set_path("WorldMap.PlayerLocationMarkerType", Value::number(66));
    }
    void handlers() override {
        handle("PlaySound", [this](Call& c) { m_ui.host().ui_play_sound(*player()->vm.to_string(c.arg(0))); });
        handle("ToggleMapCallback", [this](Call&) { toggle_local_map(); });
        handle("OpenJournalCallback", [this](Call&) { m_ui.open_journal(true); }); // with 1
        // GetMouseButtonForSetDestination answers an empty string.
        handle("GetMouseButtonForSetDestination", [](Call& c) { c.respond({Value::string("")}); });
        // SetLocalMapExtents(x0, y0, x1, y1): the movie's local map rectangle in stage pixels, which the
        // engine turns into screen pixels through the movie view and keeps (LocalMapMenu) to pick the
        // local map's markers under the cursor.
        handle("SetLocalMapExtents", [this](Call& c) {
            auto& vm = player()->vm;
            const float w = float(m_ui.target_width()), h = float(m_ui.target_height());
            const player::Matrix2D m = stage_to_pixels(w, h);
            m.apply(float(vm.to_number(c.arg(0))), float(vm.to_number(c.arg(1))), m_local_extents[0],
                    m_local_extents[1]);
            m.apply(float(vm.to_number(c.arg(2))), float(vm.to_number(c.arg(3))), m_local_extents[2],
                    m_local_extents[3]);
        });
        // MarkerClick acts on the marker the movie names; CurrentLocationCallback centres on the
        // player.
        handle("MarkerClick", [this](Call& c) {
            if (m_source && c.args.size() == 1) {
                m_source->map_click(size_t(player()->vm.to_number(c.arg(0))));
            }
        });
        handle("CurrentLocationCallback", [this](Call&) {
            if (m_source) {
                m_source->map_centre();
            }
        });
        // ClickCallback, the movie's onMouseDown off the bottom bar: the selected marker, or PlacePlayerMarker.
        handle("ClickCallback", [this](Call&) {
            if (!m_source || m_local) {
                return;
            }
            if (m_world.described >= 0) {
                send("ClickSelectedMarker");
            } else {
                const auto& hd = player()->movie->data().header;
                m_source->map_click_ground(player()->mouse_x() / hd.stage_width, player()->mouse_y() / hd.stage_height);
            }
        });
    }

    // ---- the local map's camera -----------------------
    // fMapLocalHeight:MapMenu and fMapLocalMinFrustumWidth:MapMenu, their values in the
    // image (the game's live values in run images1 too); the minimum frustum's height is the width x 0.5625
    // and the camera starts at zoom 0.5 (DefaultState).
    static constexpr float k_local_height = 40000.0f;
    static constexpr float k_min_frustum_width = 4000.0f;
    static constexpr float k_min_frustum_aspect = 0.5625f;
    static constexpr float k_mouse_zoom_speed = 0.1f; // fMapLocalMouseZoomSpeed:MapMenu
    static constexpr float k_mouse_pan_speed = 20.0f; // fMapLocalMousePanSpeed:MapMenu
    struct LocalCamera {
        bool valid = false;
        float bounds_min[3] = {0, 0, 0}, bounds_max[3] = {0, 0, 0};
        float initial[3] = {0, 0, 0}; // the player's position, z + fMapLocalHeight
        float translation[3] = {0, 0, 0};
        float zoom = 0.5f;
        float north = 0;
        float min_half_width = k_min_frustum_width * 0.5f, min_half_height = k_min_frustum_width * k_min_frustum_aspect * 0.5f;
        float pan[3] = {0, 0, 0}, zoom_input = 0; // this frame's input
        LocalMapView view{};
    };
    // The game(true): the camera back to its default state (translation 0, zoom 0.5), the
    // player's position as its initial position, the loaded area as its bounds with fMapLocalHeight
    // added to the top, the north rotation of the player's cell (
    // for an exterior the worldspace's, 0 for Tamriel as measured; spud's worlds are exteriors) and, in an
    // interior, the bounds 2000 units wider on each side.
    void reset_local_camera() {
        m_camera = LocalCamera{};
        float pos[3];
        float yaw = 0;
        float lo[3], hi[3];
        if (!m_ui.host().ui_player_pose(pos, yaw) || !m_ui.host().ui_loaded_area(lo, hi)) {
            log::info("ui: Map Menu: no world, the local map has nothing to show");
            return;
        }
        m_camera.initial[0] = pos[0];
        m_camera.initial[1] = pos[1];
        m_camera.initial[2] = pos[2] + k_local_height;
        for (int i = 0; i < 3; ++i) {
            m_camera.bounds_min[i] = lo[i];
            m_camera.bounds_max[i] = hi[i];
        }
        m_camera.bounds_max[2] += k_local_height;
        // The worldspace's north rotation, negated: its NorthMarker's for a
        // worldspace with flag 0x40, else the
        // worldspace's own 0 (the game's constructor).
        const WorldspaceMap w = worldspace_map(m_ui, m_ui.host().ui_worldspace());
        m_camera.north = -(w.north + m_ui.host().ui_cell_north()); // an interior's own, from its NorthMarker
        m_camera.view.draw_land = !(w.flags & 0x40u);
        m_camera.valid = true;
    }
    // DefaultState::Update, every frame the local map is shown.
    void update_local_camera() {
        LocalCamera& c = m_camera;
        if (!c.valid) {
            return;
        }
        const float s = std::sin(c.north), co = std::cos(c.north);
        // The bounds' corners in screen axes (right = (cos, -sin), up = (sin, cos)): their extent there.
        const float xs[2] = {c.bounds_min[0], c.bounds_max[0]}, ys[2] = {c.bounds_min[1], c.bounds_max[1]};
        float rx0 = 3.4028235e38f, rx1 = -3.4028235e38f, ry0 = 3.4028235e38f, ry1 = -3.4028235e38f;
        for (float x : xs) {
            for (float y : ys) {
                const float rx = x * co - y * s, ry = x * s + y * co;
                rx0 = std::min(rx0, rx);
                rx1 = std::max(rx1, rx);
                ry0 = std::min(ry0, ry);
                ry1 = std::max(ry1, ry);
            }
        }
        c.zoom = std::clamp(c.zoom_input + c.zoom, 0.0f, 1.0f);
        // Zoomed in fully the frustum is the minimum; out fully it spans the bounds' width.
        const float half_w =
            (1.0f - c.zoom) * std::max((rx1 - rx0) * 0.5f - c.min_half_width, 0.0f) + c.min_half_width;
        const float aspect = c.min_half_height == 0 || c.min_half_width == 0 ? 1.0f : c.min_half_height / c.min_half_width;
        // Panning, in screen axes and scaled with the frustum.
        float px = c.pan[0], py = c.pan[1], pz = c.pan[2];
        if (c.min_half_width != 0) {
            const float k = half_w / c.min_half_width;
            px *= k;
            py *= k;
            pz *= k;
        }
        c.translation[0] += px * co + py * s;
        c.translation[1] += py * co - px * s;
        c.translation[2] += pz;
        // The view stays inside the bounds: the translation in screen axes is kept to the bounds' slack around the
        // initial position.
        const float slack_x = std::max((rx1 - rx0) * 0.5f - half_w, 0.0f);
        const float slack_y = std::max((ry1 - ry0) * 0.5f - aspect * half_w, 0.0f);
        const float off_x = (c.initial[0] * co - c.initial[1] * s) - (rx1 + rx0) * 0.5f;
        const float off_y = (c.initial[0] * s + c.initial[1] * co) - (ry1 + ry0) * 0.5f;
        float tx = c.translation[0] * co - c.translation[1] * s;
        float ty = c.translation[0] * s + c.translation[1] * co;
        tx = std::max(-slack_x - off_x, std::min(tx, slack_x - off_x));
        ty = std::max(-slack_y - off_y, std::min(ty, slack_y - off_y));
        c.translation[0] = tx * co + ty * s;
        c.translation[1] = ty * co - tx * s;
        c.pan[0] = c.pan[1] = c.pan[2] = 0;
        c.zoom_input = 0;
        // The NiCamera: orthographic, left/right -+half_w, top/bottom +-aspect x half_w, near 100, far the bounds'
        // height + 100, at the initial position plus the translation, 100 units above the bounds' top.
        c.view.position[0] = c.initial[0] + c.translation[0];
        c.view.position[1] = c.initial[1] + c.translation[1];
        c.view.position[2] = c.bounds_max[2] + 100.0f;
        c.view.north_rotation = c.north;
        c.view.half_width = half_w;
        c.view.half_height = aspect * half_w;
        c.view.near_units = 100.0f;
        c.view.far_units = c.bounds_max[2] - c.bounds_min[2] + 100.0f;
        c.view.area_min[0] = c.bounds_min[0];
        c.view.area_min[1] = c.bounds_min[1];
        c.view.area_max[0] = c.bounds_max[0];
        c.view.area_max[1] = c.bounds_max[1];
    }

    void init_local_map() {
        // WorldMap.LocalMapMenu.InitMap, a new array as its IconDisplay.MarkerData, and SetTitle with the
        // name of the player's cell, location or worldspace and, for a cleared location, the "cleared" text. The
        // names come from the game until spud has locations (capture map, the two text fields SetTitle fills).
        invoke("WorldMap.LocalMapMenu.InitMap");
        const Value icons = player()->get_path("WorldMap.LocalMapMenu.IconDisplay");
        if (icons.is_object()) {
            player()->vm.set(icons, "MarkerData", Value::object(player()->vm.make_array()));
        }
        // The cleared text is only passed for a cleared location; otherwise the second argument stays undefined
        // (the movie shows "" for undefined).
        const auto title = m_source ? std::optional<Value>(Value::string(m_source->map_title()))
                                    : captured("map", "WorldMap.LocalMapMenu.LocationDescription.text");
        const auto cleared = captured("map", "WorldMap.LocalMapMenu.ClearedDescription.text");
        const bool is_cleared = cleared && !player()->vm.to_string(*cleared)->empty();
        invoke("WorldMap.LocalMapMenu.SetTitle",
               {title ? *title : Value::string(""), is_cleared ? Value::string(m_ui.game_setting("sCleared")) : Value{}});
    }
    // A set of markers on the map: the world map's (WorldMap) or the local map's (WorldMap.LocalMapMenu.IconDisplay,
    // a second Map.MapMenu). Both run the same way: the engine sets MarkerData to the create stride {name, icon type,
    // undiscovered} and calls SetNumMarkers(n); while GetCreatingMarkers() answers true it calls CreateMarkers()
    // (ten a call), afterwards it sets MarkerData to the refresh stride {shown, x, y, rotation} (x, y as fractions of
    // the map) and has the movie RefreshMarkers and SetSelectedMarker(index), describes a newly selected marker and
    // turns the player marker to the player's heading. The world map gets RefreshMarkers and SetSelectedMarker as
    // The game callbacks, the local map as method calls on its IconDisplay.
    struct Markers {
        std::string path;           // in the movie
        std::string create_capture; // the create stride as the game set it, on the first frame
        std::string refresh_capture;
        bool callbacks;
        size_t count = 0;
        double described = -1; // the marker the description shows (-1 at first)
        std::vector<std::string> names; // the running game's entries
    };
    void load(Markers& m) {
        const auto create = captured(m.create_capture, m.path + ".MarkerData");
        if (!create || !create->is_object() || !captured(m.refresh_capture, m.path + ".MarkerData")) {
            missing(name(), m.create_capture, "markers");
            return;
        }
        m.count = create->o->elements.size() / 3;
        if (const auto type = captured(m.create_capture, m.path + ".PlayerLocationMarkerType")) {
            player()->set_path(m.path + ".PlayerLocationMarkerType", *type);
        }
        player()->set_path(m.path + ".MarkerData", *create);
        invoke(m.path + ".SetNumMarkers", {Value::number(double(m.count))});
    }
    // The running game's markers: the create stride from the host's entries.
    void load_live(Markers& m, const std::vector<MapEntry>& entries) {
        auto& vm = player()->vm;
        Value data = Value::object(vm.make_array());
        data.o->elements.reserve(entries.size() * 3);
        m.names.clear();
        for (const MapEntry& e : entries) {
            data.o->elements.push_back(Value::string(e.name));
            data.o->elements.push_back(Value::number(e.icon));
            data.o->elements.push_back(Value::boolean(e.undiscovered));
            m.names.push_back(e.name);
        }
        m.count = entries.size();
        player()->set_path(m.path + ".MarkerData", data);
        invoke(m.path + ".SetNumMarkers", {Value::number(double(m.count))});
    }
    // The game on the host's places.
    void frame_live(Markers& m) {
        auto& vm = player()->vm;
        // The cursor pans once it nears the edge: nothing inside fMapWorldCursorMoveArea:MapMenu (0.9) of the
        // half-screen, rising to full speed at the edge.
        const auto& hd = player()->movie->data().header;
        auto edge = [](double at) {
            const double from_middle = at * 2.0 - 1.0;
            const double over = (std::abs(from_middle) - 0.9) / 0.1;
            return float(over <= 0.0 ? 0.0 : std::copysign(std::min(over, 1.0), from_middle));
        };
        MapFrameInput input;
        input.dt = m_frame_dt;
        // A cursor that hasn't moved yet stands at the origin; the game's starts mid-screen.
        if (!m_local && (player()->mouse_x() != 0 || player()->mouse_y() != 0)) {
            input.pan[0] = edge(player()->mouse_x() / hd.stage_width);
            input.pan[1] = -edge(player()->mouse_y() / hd.stage_height);
        }
        // A key let go under another menu never reaches this one.
        if (!m_ui.is_top_input(this)) {
            std::fill(std::begin(m_move_held), std::end(m_move_held), false);
            m_looking = false;
        }
        m_looking = m_looking && !m_local;
        if (!m_local) {
            input.pan[0] += (float(m_move_held[3]) - float(m_move_held[2])) * k_key_pan_speed;
            input.pan[1] += (float(m_move_held[0]) - float(m_move_held[1])) * k_key_pan_speed;
            input.look[0] = m_look_input[0];
            input.look[1] = m_look_input[1];
        }
        m_look_input[0] = m_look_input[1] = 0;
        input.zoom = m_local ? 0.0f : m_zoom_input;
        m_zoom_input = 0;
        double heading = 0;
        const bool settled = m_source->map_frame(input, m_places, heading);
        m_settled = settled;
        Value refresh = Value::object(vm.make_array());
        refresh.o->elements.reserve(m_places.size() * 4);
        for (const MapPlace& p : m_places) {
            refresh.o->elements.push_back(Value::boolean(p.shown));
            refresh.o->elements.push_back(Value::number(p.x));
            refresh.o->elements.push_back(Value::number(p.y));
            refresh.o->elements.push_back(Value::number(p.rotation));
        }
        player()->set_path(m.path + ".MarkerData", refresh);
        const double selected = !settled || m_local ? -1 : marker_under_cursor(m, refresh);
        send("RefreshMarkers");
        send("SetSelectedMarker", {Value::number(selected)});
        if (selected != m.described) {
            m.described = selected;
            const Value desc = player()->get_path(m.path + ".MarkerDescriptionObj");
            if (selected >= 0 && size_t(selected) < m.names.size() && desc.is_object()) {
                vm.call_method(desc, "SetDescription",
                               {Value::string(m.names[size_t(selected)]), Value::object(vm.make_array())});
            }
        }
        player()->set_path(m.path + ".YouAreHereMarker._rotation", Value::number(heading));
    }
    void frame(Markers& m) {
        if (m.count == 0) {
            return;
        }
        if (player()->vm.to_boolean(invoke(m.path + ".GetCreatingMarkers"))) {
            invoke(m.path + ".CreateMarkers");
            return;
        }
        if (m_source && m.callbacks) {
            frame_live(m);
            return;
        }
        if (m_source) {
            // {custom destination: none}, {the player: the middle of the local map, as the camera starts}.
            auto& vm = player()->vm;
            Value refresh = Value::object(vm.make_array());
            for (const double v : {-1.0, -1.0, 0.0}) {
                if (refresh.o->elements.empty()) {
                    refresh.o->elements.push_back(Value::boolean(false));
                }
                refresh.o->elements.push_back(Value::number(v));
            }
            refresh.o->elements.push_back(Value::boolean(true));
            // Its icon turns with the player's heading in the map's own north: 180 at heading 0 in a cell whose north is
            // 180 (measured in the game, Bleak Falls Barrow).
            float at[3] = {}, yaw = 0.0f;
            m_ui.host().ui_player_pose(at, yaw);
            const double turned = double(glm::degrees(yaw + m_ui.host().ui_cell_north()));
            for (const double v : {0.5, 0.5, turned}) {
                refresh.o->elements.push_back(Value::number(v));
            }
            player()->set_path(m.path + ".MarkerData", refresh);
            invoke(m.path + ".RefreshMarkers");
            invoke(m.path + ".SetSelectedMarker", {Value::number(1)});
            return;
        }
        // A fresh array each frame, as the engine sets one each frame (values held outside the movie are not
        // garbage-collection roots).
        double selected = -1;
        if (const auto refresh = captured(m.refresh_capture, m.path + ".MarkerData")) {
            player()->set_path(m.path + ".MarkerData", *refresh);
            // The world map selects nothing while the local map is up (the game skips the search;
            // measured: SetSelectedMarker(-1) for the local map's frames).
            selected = !m.callbacks ? captured_selection(m) : m_local ? -1 : marker_under_cursor(m, *refresh);
        }
        if (m.callbacks) {
            send("RefreshMarkers");
            send("SetSelectedMarker", {Value::number(selected)});
        } else {
            invoke(m.path + ".RefreshMarkers");
            invoke(m.path + ".SetSelectedMarker", {Value::number(selected)});
        }
        if (selected != m.described) {
            m.described = selected;
            describe(m, selected);
        }
        // The player marker's heading, as the engine projects it through the map camera onto the map (SetDisplayInfo
        // on YouAreHereMarker).
        if (const auto rotation = captured(m.refresh_capture, m.path + ".YouAreHereMarker._rotation")) {
            player()->set_path(m.path + ".YouAreHereMarker._rotation", *rotation);
        }
    }
    // The world map's selection: of the shown markers, the nearest to the cursor in screen fractions
    // (x / width, y / height), by squared distance below fMaxMarkerSelectionDist:MapMenu (0.003); a later marker
    // wins a tie.
    double marker_under_cursor(const Markers& m, const Value& refresh) {
        constexpr double k_max_dist = 0.003;
        if (!refresh.is_object()) {
            return -1;
        }
        auto& vm = player()->vm;
        const auto& hd = player()->movie->data().header;
        const double cx = player()->mouse_x() / hd.stage_width, cy = player()->mouse_y() / hd.stage_height;
        const auto& e = refresh.o->elements;
        double best = -1, best_d = 3.4028235e38;
        for (size_t i = 0; i + 2 < e.size() && i / 4 < m.count; i += 4) {
            if (!vm.to_boolean(e[i])) {
                continue;
            }
            const double dx = vm.to_number(e[i + 1]) - cx, dy = vm.to_number(e[i + 2]) - cy;
            const double d = dy * dy + dx * dx;
            if (d <= best_d && d < k_max_dist) {
                best_d = d;
                best = double(i / 4);
            }
        }
        return best;
    }
    // The local map selects through its own camera's extents; the selection the game made with the
    // cursor where it was stands in (capture map-local: IconDisplay.SelectedMarker.Index).
    double captured_selection(const Markers& m) {
        const auto index = captured(m.refresh_capture, m.path + ".SelectedMarker.Index");
        return index && index->is_number() ? index->n : -1;
    }
    // A newly selected marker's description: MarkerDescriptionObj.SetDescription(title, line items) with
    // the marker's name as the title; the line items are the quests of a quest marker and "Cleared" (sCleared) for a
    // cleared location, which need the quest and location systems: none here.
    void describe(const Markers& m, double selected) {
        const auto create = captured(m.create_capture, m.path + ".MarkerData");
        if (selected < 0 || !create || !create->is_object()) {
            return;
        }
        const size_t at = size_t(selected) * 3;
        const auto& e = create->o->elements;
        const Value desc = player()->get_path(m.path + ".MarkerDescriptionObj");
        if (at < e.size() && desc.is_object()) {
            player()->vm.call_method(desc, "SetDescription", {e[at], Value::object(player()->vm.make_array())});
        }
    }
    void toggle_local_map() {
        // ToggleMapCallback and the LocalMap user event switch between the world map and the local map
        // the local map's state flag, then WorldMap.LocalMapMenu.Show(local), which fades
        // the local map in or out and relabels the bottom bar's button. Shown, the local map's camera starts from the
        // player and the engine draws its picture (img://Local_Map) every frame (local_map_view).
        m_local = !m_local;
        if (m_local) {
            reset_local_camera();
            update_local_camera();
        }
        invoke("WorldMap.LocalMapMenu.Show", {Value::boolean(m_local)});
        if (m_local && m_local_markers.count == 0) {
            if (m_source) {
                // The list measured at Riverwood: the custom destination, then the player's marker.
                load_live(m_local_markers, {{m_ui.game_setting("sPlayerSetMarker"), 65, false},
                                            {m_ui.game_setting("sCurrentLocation"), 66, false}});
            } else {
                load(m_local_markers);
            }
        }
    }
    void close_map() {
        // UIMenuCancel, the cursor released, the closing fade to black (the game with
        // (1, 1, fMapTransitionSpeed x 0.5, 1, 0): initFade(false, true, 0.375, 0), the Fader staying up), then the
        // menu goes; its hide fades the world back in.
        m_ui.host().ui_play_sound("UIMenuCancel");
        m_ui.request_fade({false, true, k_transition * 0.5, 0, true});
        post_close();
    }

    MapSource* m_source = nullptr; // the running game's markers; null: a capture's
    std::vector<MapPlace> m_places;
    float m_zoom_input = 0;
    bool m_move_held[4] = {};                // Up, Down, Left, Right
    bool m_looking = false;
    float m_look_input[2] = {0, 0};
    double m_frame_dt = 0;
    Markers m_world{"WorldMap", "map-create", "map", true};
    Markers m_local_markers{"WorldMap.LocalMapMenu.IconDisplay", "map-local-create", "map-local", false};
    bool m_local = false;
    bool m_open_local = false, m_settled = false;
    LocalCamera m_camera;
    float m_local_extents[4] = {0, 0, 0, 0}; // SetLocalMapExtents, screen pixels
    bool m_dragging = false;                 // LocalMapMenu
    // MapMenu::ProcessMessage turns "WorldMapMode" on at show and off at hide.
    HudModeHold m_hud_mode{m_ui, "WorldMapMode", false};
};

// ---- Crafting Menu -------------------------------------------------------------------------------------------
// The station decides the submenu: its workbench BenchType 1 ConstructibleObject (forge, smelter,
// tanning rack, cooking pot), 2 and 7 Smithing (grindstone, armor workbench), 3 and 4 EnchantConstruct, 5 Alchemy.
// The submenu goes to that frame of the movie's Menu clip and calls its Initialize; the lists, the item
// card and the button texts are filled from the player's inventory and the recipes, which spud doesn't have:
// they come from the game (capture craft-<station>, recorded with "record").

struct Station {
    const char* name;     // show data: "Crafting Menu|forge"
    const char* frame;    // the Menu clip's frame label the submenu goes to
    int tutorial;         // the help message ShowTutorial is asked for
    int bench;            // the workbench's BenchType
};
constexpr Station k_stations[] = {
    {"forge", "ConstructibleObject", 239, 1},   {"smelter", "ConstructibleObject", 241, 1},
    {"tanning", "ConstructibleObject", 242, 1}, {"cooking", "ConstructibleObject", 240, 1},
    {"grindstone", "Smithing", 245, 2},         {"armor", "Smithing", 246, 7},
    {"enchant", "EnchantConstruct", 244, 3},    {"alchemy", "Alchemy", 247, 5},
};

class CraftingMenu final : public WorldMenu {
public:
    // Creator: CraftingMenu, ExactFit, depth 0, flags 0x6488, input context 3 (Item Menus).
    explicit CraftingMenu(Ui& ui)
        : WorldMenu(ui, {"Crafting Menu", "CraftingMenu", ScaleMode::exact_fit, 0, 0x6488, 0, 3}), m_item3d(ui) {}

private:
    // The creator puts the HUD into "InventoryMode"; the destructor ends it
    HudModeHold m_hud_mode{m_ui, "InventoryMode"};
    BlurHold m_blur{m_ui};

public:

    void on_show() override {
        const std::string station = m_ui.show_data(name());
        m_source = m_ui.host().ui_crafting_source();
        if (m_source && !m_source->begin(station)) {
            m_source = nullptr;
        }
        for (const Station& s : k_stations) {
            if (m_source ? s.bench == m_source->bench() : station == s.name) {
                m_station = &s;
                break;
            }
        }
        if (!m_station) {
            // No station: the game's show finds no workbench and leaves the movie on its first frame.
            log::info("ui {}: no station (open it as \"Crafting Menu|forge\", |grindstone, |armor, |alchemy, "
                      "|enchant, ...)",
                      name());
            return;
        }
        // CraftingSubMenu: Menu.gotoAndStop(frame), Advance, Menu.Initialize, the button texts 0, 1
        // and 3 (sSelect, sExit, sCraft) and Menu.UpdateButtonText(); then the submenu fills the lists.
        invoke("Menu.gotoAndStop", {Value::string(m_station->frame)});
        player()->advance(0);
        invoke("Menu.Initialize");
        // The constructor's Inventory3DManager::Begin(1): the item menus' slot, base and input
        // (measured, run scenes-craft1: base (-29, -500, 12), scale 1.5, interactive, normalised).
        m_item3d.begin(1);
        set_button_text(0, m_ui.game_setting("sSelect"));
        set_button_text(1, m_ui.game_setting("sExit"));
        set_button_text(3, m_ui.game_setting("sCraft"));
        invoke("Menu.UpdateButtonText");
        const std::string capture = std::string("craft-") + m_station->name;
        const std::string_view frame = m_station->frame;
        if (m_source && frame == "ConstructibleObject") {
            constructible();
            set_description(m_source->furniture_name(), m_ui.game_setting("sConstructibleMenuDescription"));
            constructible_categories();
            fill();
        } else if (m_source && frame == "Smithing") {
            smithing();
            invoke("Menu.CategoryList.SetCategoriesList",
                   {Value::string(m_ui.game_setting(m_station->bench == 2 ? "sWeapons" : "sArmor")), Value::number(1),
                    Value::boolean(true)});
            fill();
            invoke("Menu.CategoryList.ShowItemsList");
            player()->set_path("Menu.bCanExpandPanel", Value::boolean(false));
        } else if (m_source && frame == "Alchemy") {
            set_description(m_ui.game_setting("sAlchemy"), m_ui.game_setting("sAlchemyMenuDescription"));
            invoke("Menu.SetPartitionedFilterMode", {Value::boolean(true)});
            alchemy_categories();
            fill();
        } else if (m_source && frame == "EnchantConstruct") {
            enchanting();
            fill();
        } else if (frame == "Alchemy") {
            alchemy(capture);
        } else {
            if (frame == "Smithing") {
                smithing();
            } else if (frame == "EnchantConstruct") {
                enchanting();
            } else {
                constructible();
            }
            // The items, the recipes and the skill: the inventory, the constructible objects and the actor values.
            if (!m_calls.load(m_ui, capture + ".calls", [](const EngineCall& c) { return !loader_call(c); }) &&
                !apply_capture(capture)) {
                missing(name(), capture, "items");
            }
        }
        // A running game shows each station's help the first time only.
        static std::set<int> s_shown;
        if (!m_source || s_shown.insert(m_station->tutorial).second) {
            show_tutorial(*this, m_station->tutorial);
        }
    }
    void advance(double dt, double engine_ms) override {
        WorldMenu::advance(dt, engine_ms);
        m_calls.advance(*this);
        m_dt = float(dt);
        m_item3d.update(m_dt);
    }
    // CraftingMenu::Display: the submenu's vtable slot 4 says whether the movie goes first; the
    // AlchemyMenu's answers true, so the movie, then slot 1.
    bool ui_scene(UiScene& out) override { return m_item3d.scene(out); }
    bool scene_after_movie() const override { return true; }
    void mouse_delta(float dx, float dy) override {
        if (m_rotating) {
            m_item3d.mouse_move(dx, dy, m_dt); // The game's flag, as the item menus
        }
    }
    std::string item3d_json() const { return m_item3d.state_json(); }
    bool on_user_event(const std::string& event) override {
        // Cancel (UserEvents): the submenu may take it (a slider, the rename field); otherwise the menu
        // closes ( posts the hide).
        if (m_slider) {
            return true; // the slider has the input (EnchantConstructMenu)
        }
        if (event == "Cancel") {
            if (chooser() && (m_station->bench == 5 || m_source->anything_chosen())) {
                // sQuitAlchemy always; sQuitEnchanting with anything chosen.
                m_ui.message(m_ui.game_setting(m_station->bench == 5 ? "sQuitAlchemy" : "sQuitEnchanting"),
                             {m_ui.game_setting("sYes"), m_ui.game_setting("sNo")}, [this](int button) {
                                 if (button == 0) {
                                     post_close();
                                 }
                             });
                return true;
            }
            post_close();
            return true;
        }
        // CraftButtonPress posts XButton (UserEvents) to the menu, which the submenu's input takes.
        if (event == "XButton" && m_source) {
            craft_pressed();
            return true;
        }
        if (event == "YButton" && chooser()) {
            aux_pressed();
            return true;
        }
        return false;
    }

private:
    // ConstructibleObjectMenu's categories: 31 of them, flag 1 << i, none shown when empty. The four
    // Dragonborn ones are the movie's translation keys.
    void constructible_categories() {
        static constexpr const char* k_names[31] = {
            "sWood", "sHide", "sDraugr", "sIron", "sStudded", "sImperial", "sSteel", "sLeather", "sDwarven",
            "sElven", "sFalmer", "sOrcish", "sEbony", "sGlass", "sDragon", "sDaedric", "sJewelry", "sFood",
            "sGenericCraftKeywordName01", "sGenericCraftKeywordName02", "sGenericCraftKeywordName03",
            "sGenericCraftKeywordName04", "sGenericCraftKeywordName05", "sGenericCraftKeywordName06",
            "sGenericCraftKeywordName07", "sGenericCraftKeywordName08", "$Crafting_$DLC2ArmorBonemold",
            "$Crafting_$DLC2ArmorChitin", "$Crafting_$DLC2ArmorNordic", "$Crafting_$DLC2ArmorStalhrim", "sMisc"};
        std::vector<Value> cats;
        for (int i = 0; i < 31; ++i) {
            const std::string_view n = k_names[i];
            cats.push_back(Value::string(n[0] == '$' ? std::string(n) : m_ui.game_setting(k_names[i])));
            cats.push_back(Value::number(double(1u << i)));
            cats.push_back(Value::boolean(false));
        }
        invoke("Menu.CategoryList.SetCategoriesList", std::move(cats));
    }

    bool chooser() const { return m_source && m_station && m_station->bench != 1 && m_station->bench != 2 && m_station->bench != 7; }

    // AlchemySendCategories: sIngredients, then the effects the player knows, greyed when only one
    // ingredient has them.
    void alchemy_categories() {
        auto& vm = player()->vm;
        std::vector<Value> cats{Value::string(m_ui.game_setting("sIngredients")), Value::number(4294967295.0),
                                Value::boolean(true)};
        const std::vector<CraftCategory> list = m_source->categories();
        for (const CraftCategory& c : list) {
            cats.push_back(Value::string(c.text));
            cats.push_back(Value::number(c.flag));
            cats.push_back(Value::boolean(true));
        }
        invoke("Menu.CategoryList.SetCategoriesList", std::move(cats));
        const Value entries = player()->get_path("Menu.CategoryList.CategoriesList.entryList");
        for (size_t i = 0; entries.is_object() && i < list.size() && i + 1 < entries.o->elements.size(); ++i) {
            vm.set(entries.o->elements[i + 1], "enabled", Value::boolean(list[i].enabled));
        }
        invoke("Menu.CategoryList.InvalidateListData");
    }

    void update_buttons() {
        const std::vector<std::string> texts = m_source->buttons();
        for (size_t i = 0; i < texts.size(); ++i) {
            set_button_text(i, texts[i]);
        }
        invoke("Menu.UpdateButtonText");
    }

    // The list and everything that follows it, around the movie's rebuild calls.
    void refill() {
        fill();
        if (chooser()) {
            update_buttons();
        }
    }

    void choose(int entry) {
        const CraftChoice c = m_source->choose(entry);
        if (!c.sound.empty()) {
            m_ui.host().ui_play_sound(c.sound);
        }
        if (!c.notification.empty()) {
            m_ui.notification(m_ui.game_setting(c.notification), c.notification_sound);
        }
        if (!c.confirm.empty()) {
            m_ui.message(m_ui.game_setting(c.confirm), {m_ui.game_setting("sYes"), m_ui.game_setting("sNo")},
                         [this](int button) {
                             m_source->confirmed(button == 0);
                             refill();
                             invoke("Menu.UpdateItemDisplay");
                         });
            return;
        }
        refill();
        if (c.slider_max > 0) {
            m_slider = true;
            invoke("Menu.ShowSlider", {Value::number(c.slider_max), Value::number(1), Value::number(c.slider_value),
                                       Value::number(c.slider_step)});
        }
    }

    void craft_pressed() {
        if (!chooser()) {
            attempt_craft(m_chosen);
            return;
        }
        if (const std::string refusal = m_source->refusal(-1); !refusal.empty()) {
            m_ui.notification(m_ui.game_setting(refusal), "UIActivateFail");
            return;
        }
        const auto finish = [this] {
            invoke("Menu.PreRebuildList");
            const CraftOutcome o = m_source->finish();
            refill();
            invoke("Menu.PostRebuildList", {Value::boolean(true)});
            invoke("Menu.UpdateItemDisplay");
            m_ui.host().ui_play_sound(o.sound);
            if (!o.message.empty()) {
                m_ui.message(o.message, {m_ui.game_setting("sOk")}, nullptr);
            }
        };
        if (const std::string question = m_source->craft_question(); !question.empty()) {
            m_ui.message(m_ui.game_setting(question), {m_ui.game_setting("sYes"), m_ui.game_setting("sNo")},
                         [finish](int button) {
                             if (button == 0) {
                                 finish();
                             }
                         });
        } else {
            finish();
        }
    }

    // YButton: the lab clears its selections, the enchanter names the item.
    void aux_pressed() {
        if (m_station->bench == 5) {
            m_source->clear_choices();
            refill();
            invoke("Menu.UpdateItemDisplay");
        } else if (const std::string name = m_source->rename_text(); !name.empty()) {
            invoke("Menu.EditItemName", {Value::string(name), Value::number(m_ui.host().ui_game_setting(
                                                                  "uMaxCustomItemNameLength:Interface", 32))});
        }
    }

    // The fill ( and; and for smithing): the list emptied and one object per
    // entry, the categories told, then enabled and equipState, Menu.UpdateItemList, the selection.
    void fill() {
        auto& vm = player()->vm;
        const Value entries = player()->get_path("Menu.ItemList.entryList");
        if (!entries.is_object()) {
            return;
        }
        m_filling = true; // the movie's list calls come back while it rebuilds
        const std::vector<CraftEntry> list = m_source->entries();
        entries.o->elements.clear();
        for (const CraftEntry& e : list) {
            Object* o = vm.make_object();
            vm.set(Value::object(o), "text", Value::string(e.text));
            vm.set(Value::object(o), "count", Value::number(e.count));
            vm.set(Value::object(o), "filterFlag", Value::number(e.filter_flag));
            entries.o->elements.push_back(Value::object(o));
        }
        invoke("Menu.CategoryList.InvalidateListData");
        for (size_t i = 0; i < list.size(); ++i) {
            vm.set(entries.o->elements[i], "enabled", Value::boolean(list[i].enabled));
            vm.set(entries.o->elements[i], "equipState", Value::number(list[i].equip_state));
        }
        invoke("Menu.UpdateItemList", {Value::boolean(m_station->bench == 2 || m_station->bench == 7)});
        if (m_station->bench == 5) {
            alchemy_categories();
        }
        m_filling = false;
        update_selection();
    }

    // The selection update (): the card, the requirements, the bottom bar.
    void update_selection() {
        auto& vm = player()->vm;
        const int size = int(m_source->entries().size());
        if (m_selected >= size) {
            return;
        }
        const int shown = m_chosen >= 0 ? m_chosen : m_selected;
        const json::Value card = m_source->card(shown);
        player()->set_path("Menu.ItemInfoHolder.ItemInfo.itemInfo", player::json_to_value(vm, card));
        std::vector<Value> rows;
        for (const CraftIngredient& i : m_source->ingredients(shown)) {
            Object* o = vm.make_object();
            vm.set(Value::object(o), "Name", Value::string(i.name));
            vm.set(Value::object(o), "RequiredCount", Value::number(i.required));
            vm.set(Value::object(o), "PlayerCount", Value::number(i.have));
            rows.push_back(Value::object(o));
        }
        invoke("Menu.UpdateIngredients", {Value::string(m_ui.game_setting("sRequirementsText")),
                                          Value::object(vm.make_array(std::move(rows))), Value::boolean(!chooser())});
        if (m_station->bench == 2 || m_station->bench == 7) {
            player()->set_path("Menu.bCanCraft", Value::boolean(m_chosen >= 0));
        } else if (chooser()) {
            player()->set_path("Menu.bCanCraft", Value::boolean(m_source->has_result()));
        }
        update_bottom_bar(card);
    }

    // UpdateCraftingInfo for the bench's skill; a bench without one shows the player info.
    void update_bottom_bar(const json::Value& card) {
        if (m_source->has_skill()) {
            const CraftSkill s = m_source->skill();
            invoke("Menu.BottomBarInfo.UpdateCraftingInfo",
                   {Value::string(s.name), Value::number(s.level), Value::number(s.percent)});
            return;
        }
        invoke("Menu.BottomBarInfo.UpdatePlayerInfo", {player::json_to_value(player()->vm, m_source->player_info())});
        if (const json::Value* type = member(&card, "type")) {
            invoke("Menu.BottomBarInfo.SetPlayerInfoType", {Value::number(type->n)});
        }
    }

    // The craft attempt: a refusal notifies with UIActivateFail (default object 134); otherwise the confirmation.
    void attempt_craft(int entry) {
        if (entry < 0) {
            return;
        }
        if (const std::string refusal = m_source->refusal(entry); !refusal.empty()) {
            m_ui.notification(m_ui.game_setting(refusal), "UIActivateFail");
            return;
        }
        const bool smith = m_station->bench == 2 || m_station->bench == 7;
        m_ui.message(m_ui.game_setting(smith ? "sSmithingConfirm" : "sConstructibleMenuConfirm"),
                          {m_ui.game_setting("sYes"), m_ui.game_setting("sNo")}, [this, entry](int button) {
                              if (button != 0) {
                                  return;
                              }
                              const std::string sound = m_source->craft_sound(entry);
                              m_source->craft(entry);
                              m_chosen = -1;
                              m_source->preview(-1);
                              fill();
                              m_ui.host().ui_play_sound(sound);
                          });
    }

    // The game on Menu.ButtonText (an array the movie made), as the submenus do.
    // The first card a capture holds for an entry of that name (craft_captures.py's {"cards": [{"name", ...}]}).
    const json::Value* captured_card(const std::string& capture, const std::string& name) {
        const json::Value* cards = member(m_ui.capture(capture), "cards");
        if (!cards || cards->kind != json::Value::array) {
            return nullptr;
        }
        for (const auto& c : cards->items) {
            const json::Value* n = member(&c, "name");
            if (n && n->kind == json::Value::string && n->s == name) {
                return &c;
            }
        }
        return nullptr;
    }

    void set_button_text(size_t i, const std::string& text) {
        const Value b = player()->get_path("Menu.ButtonText");
        if (!b.is_object() || b.o->kind != player::ObjKind::array) {
            return;
        }
        if (b.o->elements.size() <= i) {
            b.o->elements.resize(i + 1);
        }
        b.o->elements[i] = Value::string(text);
    }

    void set_description(const std::string& title, const std::string& text) {
        player()->set_path("Menu.MenuDescription.text", Value::string(std::format("{}: {}", title, text)));
    }

    // SmithingMenu (constructor): the description "%s: %s" of sWeaponSmithing (BenchType 2) or
    // sArmorSmithing and sSmithingMenuDescription; then UpdateCraftingInfo for actor value 10 (Smithing), which needs
    // the player's skills. The items it can improve come with the inventory.
    void smithing() {
        set_description(m_ui.game_setting(m_station->bench == 2 ? "sWeaponSmithing" : "sArmorSmithing"),
                        m_ui.game_setting("sSmithingMenuDescription"));
    }

    // EnchantConstructMenu (constructor): the description of sEnchanting and sEnchantMenuDescription,
    // Menu.bCanCraft = true, then CategoryList.SetCategoriesList with five fixed categories (sDisenchant 10, a
    // divider "" 0, sItem 5, sEnchantment 48, sSoulGem 64, each shown when empty), the second entry's divider set,
    // CategoryList.InvalidateListData. The lists' contents and UpdateCraftingInfo (actor value 23)
    // need the inventory, the known enchantments and the skills.
    void enchanting() {
        auto& vm = player()->vm;
        set_description(m_ui.game_setting("sEnchanting"), m_ui.game_setting("sEnchantMenuDescription"));
        player()->set_path("Menu.bCanCraft", Value::boolean(true));
        std::vector<Value> cats;
        const std::pair<std::string, double> k_cats[] = {{m_ui.game_setting("sDisenchant"), 10},
                                                         {"", 0},
                                                         {m_ui.game_setting("sItem"), 5},
                                                         {m_ui.game_setting("sEnchantment"), 48},
                                                         {m_ui.game_setting("sSoulGem"), 64}};
        for (const auto& [text, flag] : k_cats) {
            cats.push_back(Value::string(text));
            cats.push_back(Value::number(flag));
            cats.push_back(Value::boolean(true));
        }
        invoke("Menu.CategoryList.SetCategoriesList", std::move(cats));
        const Value list = player()->get_path("Menu.CategoryList.CategoriesList.entryList");
        if (list.is_object() && list.o->elements.size() > 1) {
            vm.set(list.o->elements[1], "divider", Value::boolean(true));
            invoke("Menu.CategoryList.InvalidateListData");
        }
    }

    // ConstructibleObjectMenu (constructor): the description "%s: %s" of the workbench's name and
    // sConstructibleMenuDescription (the furniture form's name is the game data's, so the description is left to
    // a capture), button 0 sCreate and Menu.UpdateButtonText(), Menu.bHideAdditionalDescription = true,
    // Menu.FadeInfoCard(true); its categories and items come from the recipes the player can make.
    void constructible() {
        set_button_text(0, m_ui.game_setting("sCreate"));
        invoke("Menu.UpdateButtonText");
        player()->set_path("Menu.bHideAdditionalDescription", Value::boolean(true));
        invoke("Menu.FadeInfoCard", {Value::boolean(true)});
    }

    // AlchemyMenu (constructor, after the base). The player's ingredients, the effects the player knows
    // (one category each), the chosen ingredients and the Alchemy skill are the game's inventory, magic and actor
    // value systems; until spud has them they come from the game (capture craft-alchemy: the lists and values
    // the engine had written when the menu was up). Without the capture the menu shows its empty contract.
    void alchemy(const std::string& capture) {
        auto& vm = player()->vm;
        const auto gmst = [this](const char* n) { return m_ui.game_setting(n); };
        // The description: Menu.MenuDescription.text = "%s: %s" of sAlchemy and sAlchemyMenuDescription.
        player()->set_path("Menu.MenuDescription.text",
                           Value::string(std::format("{}: {}", gmst("sAlchemy"), gmst("sAlchemyMenuDescription"))));
        set_button_text(0, gmst("sAdd"));
        set_button_text(2, gmst("sClearSelections"));
        invoke("Menu.SetPartitionedFilterMode", {Value::boolean(true)});
        // The list fill. First the three slots: Menu.UpdateIngredients(sRequirementsText,
        // [{Name, RequiredCount 1, PlayerCount}], false), each slot's Name the chosen ingredient's or sIngredient,
        // sIngredient, sOptional (none chosen on opening).
        std::vector<Value> slots;
        for (const char* n : {"sIngredient", "sIngredient", "sOptional"}) {
            Object* o = vm.make_object();
            vm.set(Value::object(o), "Name", Value::string(gmst(n)));
            vm.set(Value::object(o), "RequiredCount", Value::number(1));
            vm.set(Value::object(o), "PlayerCount", Value::number(0));
            slots.push_back(Value::object(o));
        }
        invoke("Menu.UpdateIngredients", {Value::string(gmst("sRequirementsText")),
                                          Value::object(vm.make_array(std::move(slots))), Value::boolean(false)});
        // The categories: CategoryList.SetCategoriesList with sIngredients (every filter bit, shown when
        // empty) and then each effect the player knows of the carried ingredients (its name, its number from 1,
        // shown), each effect's entry then "enabled" as the engine set it; CategoryList.InvalidateListData().
        std::vector<Value> cats{Value::string(gmst("sIngredients")), Value::number(4294967295.0), Value::boolean(true)};
        const auto known = captured(capture, "Menu.CategoryList.CategoriesList.entryList");
        const Object* known_list = known && known->is_object() ? known->o : nullptr;
        for (size_t i = 1; known_list && i < known_list->elements.size(); ++i) {
            const Value& e = known_list->elements[i];
            cats.push_back(vm.get(e, "text"));
            cats.push_back(Value::number(double(i)));
            cats.push_back(Value::boolean(true));
        }
        invoke("Menu.CategoryList.SetCategoriesList", std::move(cats));
        const Value list = player()->get_path("Menu.CategoryList.CategoriesList.entryList");
        for (size_t i = 1; known_list && list.is_object() && i < known_list->elements.size() &&
                           i < list.o->elements.size(); ++i) {
            const Value enabled = vm.get(known_list->elements[i], "enabled");
            if (!enabled.is_undefined()) {
                vm.set(list.o->elements[i], "enabled", enabled);
            }
        }
        invoke("Menu.CategoryList.InvalidateListData");
        // The ingredients: Menu.ItemList.entryList emptied, then {text, count, enabled, filterFlag,
        // equipState} per ingredient in the engine's order; CategoryList.InvalidateListData().
        const auto items = captured(capture, "Menu.ItemList.entryList");
        const Value entries = player()->get_path("Menu.ItemList.entryList");
        std::vector<player::json::Value> keys;
        if (entries.is_object()) {
            entries.o->elements.clear();
            for (size_t i = 0; items && items->is_object() && i < items->o->elements.size(); ++i) {
                const Value& src = items->o->elements[i];
                Object* o = vm.make_object();
                for (const char* k : {"text", "count", "enabled", "filterFlag", "equipState"}) {
                    vm.set(Value::object(o), k, vm.get(src, k));
                }
                entries.o->elements.push_back(Value::object(o));
                // The entry as the 3D item's capture keys it (text, count, filterFlag).
                player::json::Value key;
                key.kind = player::json::Value::object;
                const Value text = vm.get(src, "text");
                key.members.push_back({"text", {}});
                key.members.back().second.kind = player::json::Value::string;
                key.members.back().second.s = text.is_undefined() ? "" : *vm.to_string(text);
                for (const char* k : {"count", "filterFlag"}) {
                    key.members.push_back({k, {}});
                    key.members.back().second.kind = player::json::Value::number;
                    key.members.back().second.n = vm.to_number(vm.get(src, k));
                }
                keys.push_back(std::move(key));
            }
        }
        m_models = captured_item_models(m_ui, "CraftingMenu", keys);
        invoke("Menu.CategoryList.InvalidateListData");
        // The selection ( with -1): button 0 sAdd (sRemove for a chosen ingredient), button 2
        // sClearSelections while anything is chosen, else empty.
        set_button_text(0, gmst("sAdd"));
        set_button_text(2, "");
        // The skill ( with actor value 16, Alchemy): Menu.BottomBarInfo.UpdateCraftingInfo(the skill's
        // name, its level, the progress to the next level in percent).
        const auto skill = captured(capture, "Menu.BottomBarInfo.PlayerInfoCard_mc.SkillLevelLabel.text");
        const auto level = captured(capture, "Menu.BottomBarInfo.PlayerInfoCard_mc.SkillLevelCurrent.text");
        const auto percent = captured(capture, "Menu.BottomBarInfo.LevelMeter.TargetPercent");
        if (skill && level && percent) {
            invoke("Menu.BottomBarInfo.UpdateCraftingInfo",
                   {*skill, Value::number(vm.to_number(*level)), Value::number(vm.to_number(*percent))});
        }
        if (!known || !items || !skill) {
            missing(name(), capture, "ingredients");
        }
    }

    void handlers() override {
        // The submenus register these (...); what they do needs the inventory and the recipes.
        handle("PlaySound", [this](Call& c) { m_ui.host().ui_play_sound(*player()->vm.to_string(c.arg(0))); });
        handle("CloseMenu", [this](Call&) { m_ui.close(name()); });
        handle("CanFadeItemInfo", [this](Call& c) {
            if (chooser()) {
                c.respond({Value::boolean(!m_source->has_result())});
            }
        });
        handle("CraftSelectedItem", [](Call&) {});
        handle("CraftButtonPress", [this](Call&) {
            if (m_source) {
                craft_pressed();
            }
        });
        handle("AuxButtonPress", [this](Call&) {
            if (chooser()) {
                aux_pressed();
            }
        });
        handle("SetSelectedCategory", [this](Call& c) {
            if (chooser() && m_station->bench != 5 && !m_filling && !c.args.empty()) {
                m_source->set_category(int(player()->vm.to_number(c.arg(0))));
                refill();
            }
        });
        handle("EndItemRename", [this](Call& c) {
            if (chooser() && c.args.size() >= 2 && player()->vm.to_boolean(c.arg(0))) {
                m_source->rename(*player()->vm.to_string(c.arg(1)));
                refill();
            }
        });
        handle("CalculateCharge", [this](Call& c) {
            if (chooser() && !c.args.empty()) {
                if (!m_source->set_slider(player()->vm.to_number(c.arg(0)))) {
                    m_ui.notification(m_ui.game_setting("sEnchantInsufficientCharge"), "UIActivateFail");
                }
                refill();
            }
        });
        handle("SliderClose", [this](Call& c) {
            if (chooser()) {
                m_slider = false;
                m_source->slider_closed(!c.args.empty() && player()->vm.to_boolean(c.arg(0)));
                refill();
            }
        });
        // ChooseItem(index): a create-object bench goes straight to crafting; a smithing bench toggles
        // the entry chosen, showing it improved, with UISelectOn.
        handle("ChooseItem", [this](Call& c) {
            if (!m_source || c.args.empty()) {
                return;
            }
            const int i = int(player()->vm.to_number(c.arg(0)));
            if (m_station->bench == 1) {
                attempt_craft(i);
                return;
            }
            if (chooser()) {
                choose(i);
                return;
            }
            m_chosen = m_chosen == i ? -1 : i;
            m_source->preview(m_chosen);
            m_ui.host().ui_play_sound("UISelectOn");
            fill();
        });
        // SetSelectedItem(index) (the AlchemyMenu's the game, which goes to
        // The game): the entry's item card into Menu.ItemInfoHolder.ItemInfo.itemInfo (the
        // card clip's setter draws it), then the button texts 0 and 2 (measured, run craft-card2: an ingredient not
        // chosen gets sAdd and ""). The card is the item's inventory data; until spud has it, it comes from the
        // game, by the entry's name.
        handle("SetSelectedItem", [this](Call& c) {
            if (c.args.empty() || !m_station) {
                return;
            }
            auto& vm = player()->vm;
            const int i = int(vm.to_number(c.arg(0)));
            if (m_source) {
                if (m_chosen < 0) {
                    m_selected = std::max(i, 0);
                    if (chooser()) {
                        m_source->select(m_selected);
                        update_buttons();
                    }
                    update_selection();
                }
                m_ui.host().ui_play_sound("UIMenuFocus");
                return;
            }
            const Value text = player()->get_path(std::format("Menu.ItemList.entryList.{}.text", i));
            const json::Value* card = captured_card(std::string("craft-") + m_station->name + "-cards",
                                                    text.is_undefined() ? std::string() : *vm.to_string(text));
            if (!card) {
                return;
            }
            if (const json::Value* info = member(card, "itemInfo")) {
                player()->set_path("Menu.ItemInfoHolder.ItemInfo.itemInfo", player::json_to_value(vm, *info));
            }
            if (const json::Value* buttons = member(card, "buttons"); buttons && buttons->kind == json::Value::object) {
                for (const auto& [index, label] : buttons->members) {
                    if (label.kind == json::Value::string) {
                        set_button_text(size_t(std::stoul(index)), label.s);
                    }
                }
            }
        });
        // ShowItem3D(show) (the AlchemyMenu's): true with an entry selected (Menu.ItemList.selectedIndex
        // below the count), Inventory3DManager::UpdateItem3D with that entry's item; otherwise Clear3D.
        handle("ShowItem3D", [this](Call& c) {
            const Value sel = player()->get_path("Menu.ItemList.selectedIndex");
            const double i = sel.is_undefined() ? -1.0 : player()->vm.to_number(sel);
            if (m_source) {
                if (!c.args.empty() && player()->vm.to_boolean(c.arg(0)) && i >= 0.0) {
                    m_item3d.show(m_source->model(int(i)));
                } else {
                    m_item3d.clear();
                }
                return;
            }
            if (!c.args.empty() && player()->vm.to_boolean(c.arg(0)) && i >= 0.0 && size_t(i) < m_models.size()) {
                m_item3d.show(m_models[size_t(i)]);
            } else {
                m_item3d.clear();
            }
        });
        handle("StartMouseRotation", [this](Call&) { m_rotating = true; });
        handle("StopMouseRotation", [this](Call&) { m_rotating = false; });
    }

    const Station* m_station = nullptr;
    CraftingSource* m_source = nullptr; // the game behind the menu; null: captured data
    int m_selected = 0, m_chosen = -1;  // the highlighted entry; the entry a smithing bench has chosen
    bool m_slider = false;              // the enchanter's slider is up
    bool m_filling = false;
    CallPlayer m_calls;
    Item3D m_item3d;
    std::vector<std::string> m_models; // per Menu.ItemList entry
    bool m_rotating = false;
    float m_dt = 0.0f;
};

// ---- Race Sex Menu -------------------------------------------------------------------------------------------
// Character creation. Its PostCreate loads RaceSex_menu, aims the race-menu camera at the player and
// sends the race list, sliders, categories and the name (SetNameText); every change (ChangeRace, the sliders, the
// presets) rebuilds the player's face and body, which needs actors, skinning and the face system. Until then the
// lists are what the engine sent in the game (capture racesex.calls).

class RaceSexMenu final : public WorldMenu {
public:
    // Constructor: depth 3, flags 0x709, input context 3; the movie loads in PostCreate: RaceSex_menu,
    // ExactFit.
    explicit RaceSexMenu(Ui& ui) : WorldMenu(ui, {"RaceSex Menu", "RaceSex_menu", ScaleMode::exact_fit, 3, 0x709, 0, 3}) {}

    void on_show() override {
        m_live = m_ui.host().ui_race_sex();
        m_limited = m_ui.show_data(name()) == "limited";
        if (m_live) {
            post_create();
            return;
        }
        // The recording holds PostCreate's burst (the lists, the name) and then the engine's replies to the movie's
        // changes (ChangeRace: SetRaceText, SetOptionSliders, HideLoadingIcon a few frames later). The burst is
        // played on opening; a reply only when the movie asks for the race it answered.
        std::vector<EngineCall> calls;
        for (EngineCall& c : engine_calls(m_ui, "racesex.calls")) {
            if (!loader_call(c)) {
                calls.push_back(std::move(c));
            }
        }
        if (calls.empty()) {
            missing(name(), "racesex.calls", "races");
            return;
        }
        std::vector<std::vector<EngineCall>> groups(1);
        for (size_t i = 0; i < calls.size(); ++i) {
            if (i > 0 && calls[i].frame - calls[i - 1].frame > 30) {
                groups.emplace_back();
            }
            groups.back().push_back(std::move(calls[i]));
        }
        for (const EngineCall& c : groups[0]) {
            if (c.callback() == "SetRaceList" && c.args) {
                for (size_t i = c.first_arg(); i < c.args->items.size(); i += 3) {
                    m_races.push_back(c.args->items[i].s);
                }
            }
        }
        m_calls.play(std::move(groups[0]));
        m_replies.assign(std::make_move_iterator(groups.begin() + 1), std::make_move_iterator(groups.end()));
    }

    bool on_user_event(const std::string& event) override {
        // UI message 7: XButton (UserEvents) asks whether the character is done, as ConfirmDone,
        // while the control map takes no text input (its count is zero: not while the name is typed).
        if (event == "XButton") {
            if (m_text_input == 0) {
                confirm_done();
            }
            return true;
        }
        return false;
    }
    void advance(double dt, double engine_ms) override {
        WorldMenu::advance(dt, engine_ms);
        m_calls.advance(*this);
        if (m_live && m_reload && m_live->ready()) {
            // RaceSexMenu's AdvanceMovie: once the body is loaded, the sliders of what it now is.
            m_reload = false;
            send_sliders();
            send("HideLoadingIcon");
        }
        if (m_closing && ++m_closing_frames > 1) {
            m_ui.close(name());
        }
    }

private:
    // PostCreate: the categories, the name and the race list; the sliders with the first update.
    void post_create() {
        player()->set_path("RaceSexMenuBaseInstance.RaceSexPanelsInstance.bLimitedMenu", Value::boolean(m_limited));
        std::vector<Value> categories;
        const char* labels[8] = {"sRSMRace", "sRSMBody", "sRSMHead", "sRSMFace", "sRSMEyes", "sRSMBrow", "sRSMMouth", "sRSMHair"};
        for (int i = 0; i < 8; ++i) {
            categories.push_back(Value::string(i == 0 && m_limited ? std::string() : m_ui.game_setting(labels[i])));
            categories.push_back(Value::number(2 << i));
        }
        send("SetCategoriesList", std::move(categories));
        send("SetNameText", {Value::string(m_live->player_name())});
        std::vector<Value> races;
        for (const RaceSexRace& r : m_live->races()) {
            races.insert(races.end(), {Value::string(r.name), Value::string(r.description), Value::number(r.current ? 1 : 0)});
            m_races.push_back(r.name);
        }
        send("SetRaceList", std::move(races));
        m_reload = true;
    }

    void send_sliders() {
        std::vector<Value> values;
        for (const RaceSexSlider& s : m_live->sliders(m_limited)) {
            values.insert(values.end(), {Value::string(s.label), Value::number(s.filter), Value::string(s.callback), Value::number(s.min),
                                         Value::number(s.max), Value::number(s.position), Value::number(s.step), Value::number(s.id)});
        }
        send("SetOptionSliders", std::move(values));
    }

    void handlers() override {
        handle("PlaySound", [this](Call& c) { m_ui.host().ui_play_sound(*player()->vm.to_string(c.arg(0))); });
        handle("ConfirmDone", [this](Call&) { confirm_done(); });
        // ChangeName(name): with exactly one argument, a name that passes the name check (
        // not empty, every character in the font config's validNameChars) becomes the player's, goes
        // back to the movie as SetNameText(name), and the menu finishes (the movie's FadeOut callback,
        // then the hide). The movie also calls it with no argument when its name entry fades out unused (the
        // replayed engine calls do that while the menu opens), which changes nothing. The player's name is the
        // actor system's; here it is kept by the menu. validNameChars: the font map osu::FontMap doesn't carry it,
        // so only the empty name is refused here.
        handle("ChangeName", [this](Call& c) {
            // Every call gives back the text input SetAllowTextInput took (ControlMap with false).
            m_text_input = std::max(0, m_text_input - 1);
            if (c.args.size() != 1 || player()->vm.to_string(c.arg(0))->empty()) {
                return;
            }
            m_name = *player()->vm.to_string(c.arg(0));
            if (m_live) {
                m_live->set_name(m_name);
            }
            send("SetNameText", {Value::string(m_name)});
            send("FadeOut");
            m_closing = true;
        });
        // SetAllowTextInput: the control map's text-input count up ( with true).
        handle("SetAllowTextInput", [this](Call&) { ++m_text_input; });
        // ChangeRace(list index, slider id): the movie sends it 630 ms after the race list's selection
        // moved (PrepForCallCode, then CallCode) with its loading icon up. With two arguments the engine clamps the
        // index to the list, makes the player that race, answers SetRaceText(the race's name) at once, and once the
        // new body is loaded (the menu's advance) SetOptionSliders(...) and HideLoadingIcon. The body is
        // the actor, skinning and face systems'; a reply the game gave for that race is replayed, for another race
        // the name goes back and, with no body to load, the loading icon is hidden (the sliders stay as they were).
        handle("ChangeRace", [this](Call& c) {
            if (c.args.size() != 2 || m_races.empty()) {
                return;
            }
            const double i = std::clamp(player()->vm.to_number(c.arg(0)), 0.0, double(m_races.size() - 1));
            const std::string& race = m_races[size_t(i)];
            if (m_live) {
                m_live->change_race(int(i));
                send("SetRaceText", {Value::string(race)});
                m_reload = true;
                return;
            }
            for (const auto& reply : m_replies) {
                for (const EngineCall& r : reply) {
                    if (r.callback() == "SetRaceText" && r.args && r.args->items.size() > r.first_arg() &&
                        r.args->items[r.first_arg()].s == race) {
                        m_calls.play(reply);
                        return;
                    }
                }
            }
            log::info("ui {}: ChangeRace to {}: the body and its sliders need the actor and face systems", name(),
                      race);
            send("SetRaceText", {Value::string(race)});
            send("HideLoadingIcon");
        });
        handle("ZoomPC", [this](Call& c) {
            if (m_live && !c.args.empty()) {
                m_live->zoom(player()->vm.to_boolean(c.arg(0)));
            }
        });
        handle("ShowVirtualKeyboard", [](Call&) {});
        handle("ChangeSex", [this](Call& c) {
            if (m_live && !c.args.empty()) {
                m_live->change_sex(player()->vm.to_number(c.arg(0)) != 0.0);
                m_reload = true;
            }
        });
        handle("ChangeHeadPreset", [this](Call& c) {
            if (!m_live || c.args.empty()) {
                return;
            }
            const int index = int(player()->vm.to_number(c.arg(0)));
            if (!m_live->preset_would_lose_changes()) {
                m_live->apply_preset(index);
                send_sliders();
                return;
            }
            // The game: Cancel puts the sliders back.
            m_ui.message(m_ui.game_setting("sRSMConfirmDestruction"), {m_ui.game_setting("sOk"), m_ui.game_setting("sCancel")},
                         [ui = &m_ui, index](int button) {
                             if (Menu* menu = ui->find_menu("RaceSex Menu")) {
                                 auto* m = static_cast<RaceSexMenu*>(menu);
                                 if (button == 0) {
                                     m->m_live->apply_preset(index);
                                 }
                                 m->send_sliders();
                             }
                         });
        });
        // The sliders: [value, sliderID] (RaceSexMenu::Accept).
        for (const char* n : {"ChangeWeight", "ChangeFaceDetails", "ChangeMorph", "ChangeDoubleMorph", "ChangePreset",
                              "ChangeHeadPart", "ChangeHairColorPreset", "ChangeTintingMask", "ChangeMask", "ChangeMaskColor"}) {
            handle(n, [this, n](Call& c) {
                if (m_live && c.args.size() >= 2) {
                    m_live->change(n, player()->vm.to_number(c.arg(0)), int(player()->vm.to_number(c.arg(1))));
                }
            });
        }
    }
    // ConfirmDone and XButton: unless a text field takes the keyboard, a message box (ShowMessageBox,
    // buttons sOk / sCancel) asks sRSMConfirm (sRSMFinishedWarning when the byte is set). Ok
    // opens the name entry: callbacks ShowTextEntry(b) and
    // ShowTextEntryField, b = the platform byte is clear. The movie shows its own entry field for
    // ShowTextEntry(true) and asks for the platform's keyboard (ShowVirtualKeyboard) for false; the game on PC sent
    // ShowTextEntry(true) (recorded, capture racesex-done.calls).
    void confirm_done() {
        if (m_limited) {
            // Limited: sRSMFinishedWarning; no name to enter.
            m_ui.message(m_ui.game_setting("sRSMFinishedWarning"), {m_ui.game_setting("sOk"), m_ui.game_setting("sCancel")},
                         [ui = &m_ui](int button) {
                             Menu* menu = ui->find_menu("RaceSex Menu");
                             if (button == 0 && menu) {
                                 static_cast<RaceSexMenu*>(menu)->finish();
                             }
                         });
            return;
        }
        m_ui.message(m_ui.game_setting("sRSMConfirm"), {m_ui.game_setting("sOk"), m_ui.game_setting("sCancel")},
                     [ui = &m_ui](int button) {
                         Menu* menu = ui->find_menu("RaceSex Menu");
                         if (button != 0 || !menu) {
                             return;
                         }
                         menu->send("ShowTextEntry", {Value::boolean(true)});
                         menu->send("ShowTextEntryField");
                     });
    }

    void finish() {
        send("FadeOut");
        m_closing = true;
    }

    CallPlayer m_calls;
    RaceSexSource* m_live = nullptr; // the running game's data; null: the capture's
    bool m_limited = false, m_reload = false;
    std::vector<std::string> m_races;                 // SetRaceList's names, in the list's order
    std::vector<std::vector<EngineCall>> m_replies;   // the engine's recorded replies to the movie's changes
    std::string m_name;
    bool m_closing = false;
    int m_closing_frames = 0;
    int m_text_input = 0; // the control map's text-input count while this menu is up
};

// ---- the rest -------------------------------------------------------------------------------------------------

// TitleSequence Menu (creator): Titles, ExactFit, depth 4. Papyrus Game.ShowTitleSequenceMenu /
// HideTitleSequenceMenu ( /) show and hide it; an update carrying a sequence name
// (Game.StartTitleSequence) plays _root.<name, lower case> from frame 1. Show data: the sequence.
class TitleSequenceMenu final : public WorldMenu {
public:
    explicit TitleSequenceMenu(Ui& ui) : WorldMenu(ui, {"TitleSequence Menu", "Titles", ScaleMode::exact_fit, 4, 0}) {}
    void on_show() override {
        const std::string sequence = lower(m_ui.show_data(name()));
        if (!sequence.empty() && has_path("_root." + sequence)) {
            player()->vm.call_method(player()->get_path("_root." + sequence), "gotoAndPlay", {Value::number(1)});
        }
    }
};

// SafeZoneMenu (creator): SafeZone, ExactFit, depth 11, the IMenu defaults otherwise. Nothing in the PC
// build opens it.
class SafeZoneMenu final : public WorldMenu {
public:
    explicit SafeZoneMenu(Ui& ui) : WorldMenu(ui, {"SafeZoneMenu", "SafeZone", ScaleMode::exact_fit, 11, 0}) {}
};

// LoadWaitSpinner (creator): ExactFit, depth 10, flags 0x802, no input context; keeps Menu_mc. Shown by
// four event sinks (,) while the game waits on a load or save. Its
// ProcessMessage invokes Menu_mc.FadeInMenu on the first show and Menu_mc.FadeOutMenu on the hide
// that brings its show count back to zero.
class LoadWaitSpinner final : public WorldMenu {
public:
    explicit LoadWaitSpinner(Ui& ui) : WorldMenu(ui, {"LoadWaitSpinner", "LoadWaitSpinner", ScaleMode::exact_fit, 10, 0x802}) {}
    void on_show() override { invoke("Menu_mc.FadeInMenu"); }
    bool on_hide() override {
        invoke("Menu_mc.FadeOutMenu");
        return true;
    }
};

// Kinect Menu (creator): KinectMenu, NoBorder, depth 6, flags 0x18800; keeps Menu_mc. For the Kinect
// sensor (Xbox); the PC build creates no Kinect device.
class KinectMenu final : public WorldMenu {
public:
    explicit KinectMenu(Ui& ui) : WorldMenu(ui, {"Kinect Menu", "KinectMenu", ScaleMode::no_border, 6, 0x18800}) {}
};

// The Bethesda.net screens: Login Menu (LoginDialog, creator), Mod Manager Menu (ModManager)
// and Marketplace Menu (Creations: the Bethesda.net SDK's own interface over a CreditsMenu host movie),
// all ShowAll, background alpha 1.0, depth 9, flags 0x799, Menu Mode. They talk to Bethesda.net through _root.CodeObj;
// spud talks to no online service, so they open with their movie and nothing behind it.
class NetMenu final : public WorldMenu {
public:
    NetMenu(Ui& ui, std::string name, std::string movie)
        : WorldMenu(ui, {std::move(name), std::move(movie), ScaleMode::show_all, 9, 0x799, 1.0f, 1}) {}
};

// Console Native UI Menu (creator): no movie, flags 0x1 (pauses the game) while the platform's own
// interface is up; shown and hidden by the platform callback.
class ConsoleNativeUiMenu final : public WorldMenu {
public:
    explicit ConsoleNativeUiMenu(Ui& ui) : WorldMenu(ui, {"Console Native UI Menu", "", ScaleMode::show_all, 3, 0x1}) {}
};

} // namespace

std::string crafting_item3d_json(Menu& m) {
    if (auto* c = dynamic_cast<CraftingMenu*>(&m)) {
        return c->item3d_json();
    }
    return "null";
}

std::unique_ptr<Menu> create_world_menu(Ui& ui, const std::string& name) {
    if (name == "MapMenu") return std::make_unique<MapMenu>(ui);
    if (name == "Crafting Menu") return std::make_unique<CraftingMenu>(ui);
    if (name == "RaceSex Menu") return std::make_unique<RaceSexMenu>(ui);
    if (name == "Tutorial Menu") return std::make_unique<TutorialMenu>(ui);
    if (name == "TitleSequence Menu") return std::make_unique<TitleSequenceMenu>(ui);
    if (name == "SafeZoneMenu") return std::make_unique<SafeZoneMenu>(ui);
    if (name == "LoadWaitSpinner") return std::make_unique<LoadWaitSpinner>(ui);
    if (name == "Kinect Menu") return std::make_unique<KinectMenu>(ui);
    if (name == "Login Menu") return std::make_unique<NetMenu>(ui, name, "LoginDialog");
    if (name == "Mod Manager Menu") return std::make_unique<NetMenu>(ui, name, "ModManager");
    if (name == "Marketplace Menu") return std::make_unique<NetMenu>(ui, name, "CreditsMenu");
    if (name == "Console Native UI Menu") return std::make_unique<ConsoleNativeUiMenu>(ui);
    return nullptr;
}

std::vector<int> Ui::tutorials_shown_list() const {
    const std::set<int>& shown = tutorials_shown();
    return {shown.begin(), shown.end()};
}

void Ui::set_tutorials_shown(const std::vector<int>& shown) {
    tutorials_shown() = std::set<int>(shown.begin(), shown.end());
}

} // namespace spud::ui
