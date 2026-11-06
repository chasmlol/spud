#pragma once

// One of the game's menus: a Scaleform movie with Skyrim's menu properties (scale mode, depth, flags) and the
// engine side of its the game calls. Every fact about a menu (what the engine calls in the movie, in which order,
// with which values) is; menus.cpp implements them from there.

#include "ui/player/draw.hpp"
#include "ui/player/player.hpp"
#include "ui/scene.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace spud::ui {

class Ui;
struct LocalMapView; // ui.hpp

// IMenu flags.
enum MenuFlag : uint32_t {
    menu_pauses_game = 0x1,
    menu_always_open = 0x2,
    menu_uses_cursor = 0x4,
    menu_uses_menu_context = 0x8,
    menu_modal = 0x10,
    menu_freeze_frame = 0x20,
    menu_disable_pause_menu = 0x80,
    menu_requires_update = 0x100,
    menu_top_most_renderer = 0x200, // drawing starts at the topmost open menu with this flag
    menu_cursor_follows_device = 0x400,
    menu_allow_saving = 0x800,
    menu_dont_hide_cursor = 0x4000,
    menu_custom_rendering = 0x8000,
    menu_application = 0x20000,
};

// GFx scale modes, in the order the engine passes them.
enum class ScaleMode { no_scale = 0, show_all = 1, exact_fit = 2, no_border = 3 };

struct MenuDesc {
    std::string name;  // "Main Menu"
    std::string movie; // "StartMenu" (Interface/<movie>.swf)
    ScaleMode scale = ScaleMode::show_all;
    int depth = 3;
    uint32_t flags = 0;
    float background_alpha = 0; // the loader's argument; a root BackgroundAlpha variable overrides it
    int context = 19;           // input context (controlmap.txt order); 19 = none
};

class Menu : public player::PlayerHost {
public:
    using Value = player::Value;
    // A game object call from the movie: its parameters, and the way to answer while it runs.
    struct Call {
        Menu& menu;
        double response_id;
        const std::vector<Value>& args;
        Value arg(size_t i) const { return i < args.size() ? args[i] : Value{}; }
        void respond(std::vector<Value> values) const { menu.respond(response_id, std::move(values)); }
    };
    using Handler = std::function<void(Call&)>;

    Menu(Ui& ui, MenuDesc desc);
    ~Menu() override;
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    const MenuDesc& desc() const { return m_desc; }
    const std::string& name() const { return m_desc.name; }
    player::Player* player() { return m_player.get(); }
    bool has_movie() const { return m_player != nullptr; }
    Ui& ui() { return m_ui; }

    // Loads the movie (section 3: first frame, scale mode, safe rect, delegate, Advance(0), InitExtensions,
    // SetPlatform), then on_loaded.
    void load(player::Library& library);
    // UI messages: show (1), hide (3); hide answers false while the menu waits for its movie (Console, Journal).
    virtual void on_show() {}
    virtual bool on_hide() { return true; }
    // Each frame while open (the menu's advance: CurrentTime, then Advance(dt, 2)).
    virtual void advance(double dt, double engine_ms);
    // A key or mouse button event reached this menu; false passes it to the menu below (Cursor Menu).
    virtual bool pass_through() const { return false; }
    // A user event ("YButton", "LeftEquip", "Quick Magic",...): the game posts every mapped button press to the
    // top menu as message type 7. True:
    // handled.
    virtual bool on_user_event(const std::string&) { return false; }
    // A mapped button ("Left", "NextPage", ...) pressed or released, for menus that read buttons themselves the way
    // the engine hands ButtonEvents to a menu's the game (the Stats Menu) before any Scaleform key is made
    // of them. True: consumed, the movie gets no key.
    virtual bool on_button(std::string_view /*event*/, bool /*down*/) { return false; }
    // A Scaleform key event (Flash key code) as the menu's ProcessMessage sees it before its movie (message 6), as
    // the Stats and Book menus look at keys 9, 13, 37-40. True: consumed.
    virtual bool on_key(uint32_t /*code*/, bool /*down*/) { return false; }
    // A movie of this menu by name: "" is its own; a menu with a second movie names it (the Book Menu's "Book").
    virtual player::Player* movie_named(std::string_view which) { return which.empty() ? player() : nullptr; }
    // The dialogue system's calls; only the Dialogue Menu answers.
    virtual void dialogue_speaker(const std::string&) {}
    virtual void dialogue_text(const std::string&) {}
    virtual void dialogue_topics(const std::vector<struct DialogueRow>&, bool) {}
    virtual void dialogue_voice_ready() {}

    // The game (movie vtable): a menu whose movie is invisible is not drawn (the Dialogue Menu
    // under a menu that pauses the game).
    virtual bool movie_visible() const { return true; }
    // The input contexts this menu holds on the control map, in push order: desc().context when it has one; a menu
    // may push others itself (InventoryMenu: Menu Mode, Item Menus and Inventory) or take one off for a
    // while (the item menus' quantity slider).
    const std::vector<int>& input_contexts() const { return m_contexts; }
    // The menu's own input handler (a game object registered with the menu controls, as Lockpicking's): the
    // user event of each mapped button press (value 1) and release (0) while this menu is on top. True: taken, no
    // Scaleform key event follows.
    virtual bool input_event(std::string_view event, float value) {
        (void)event;
        (void)value;
        return false;
    }
    // Relative mouse movement in pixels while this menu is on top (menus whose engine side reads mouse deltas).
    virtual void mouse_delta(float dx, float dy) {
        (void)dx;
        (void)dy;
    }
    // True while the menu takes the mouse's motion for itself and the cursor stays put (the map's look mode).
    virtual bool holds_cursor() const { return false; }
    // A view the engine draws for this menu into a render target (the Map Menu's local map), null if none.
    virtual const LocalMapView* local_map_view() const { return nullptr; }
    // The UI 3D scene this menu shows this frame, drawn where the menu's display function
    // renders its slot: before the movie (scene_after_movie false: the Stats, Book and Magic menus) or after it (the
    // item menus). False: none.
    virtual bool ui_scene(UiScene& out) {
        (void)out;
        return false;
    }
    virtual bool scene_after_movie() const { return false; }
    // Off-screen pages this menu draws before the frame's menus (the Book Menu's).
    virtual void scene_pages(std::vector<ScenePage>& out) { (void)out; }

    // engine -> movie: the movie's "call" callback with the callback name first.
    void send(const std::string& callback, std::vector<Value> args = {});
    // An answer to the running the game call.
    void respond(double response_id, std::vector<Value> values);
    Value invoke(std::string_view path, std::vector<Value> args = {});
    bool has_path(std::string_view path);
    // Sets the variables of a capture (Ui::capture) in this menu's movie, in the capture's order. False if there is
    // no such capture.
    bool apply_capture(const std::string& name);
    // One variable of a capture as a value of this menu's movie, or nullopt.
    std::optional<Value> captured(const std::string& name, std::string_view path);

    // Stage <-> target pixels for this menu's scale mode on a target of `w` x `h`.
    player::Matrix2D stage_to_pixels(float w, float h) const;
    void layout(float w, float h); // updates Stage.visibleRect / safeRect for the target
    player::DrawList& draw_list() { return m_list; }
    uint32_t background() const; // RGBA8 for MovieLayer::background

    // player::PlayerHost
    Value external_call(player::Player& p, const std::string& method, const std::vector<Value>& args) override;

protected:
    void handle(const std::string& name, Handler h) { m_handlers[name] = std::move(h); }
    // The save and load calls of the Journal and the Main Menu (menus_saves.cpp).
    void handle_save_load();
    std::function<void()> m_next_advance; // once, after the next advance
    virtual void register_handlers() {}
    virtual void on_loaded() {}

    Ui& m_ui;
    MenuDesc m_desc;
    std::unique_ptr<player::Player> m_player;
    player::DrawList m_list;
    float m_background_alpha = 0;
    bool m_sized = false;
    std::vector<int> m_contexts;
    std::map<std::string, Handler, std::less<>> m_handlers;
};

// Registers every menu the game has by name (menus.cpp).
std::unique_ptr<Menu> create_menu(Ui& ui, const std::string& name);

} // namespace spud::ui
