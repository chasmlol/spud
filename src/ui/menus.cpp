// The game's menus: their Skyrim properties and the engine side of each movie's contract, implemented from
// (addresses there). Menus whose data comes from systems spud does not
// have yet answer with what the engine would send in the state spud is in (no saves, no player progress).

#include "ui/menu.hpp"
#include "ui/menus_internal.hpp"

#include "ui/ui.hpp"

#include "core/log.hpp"

#include <cstdlib>
#include <format>

namespace spud::ui {

namespace {

using Value = player::Value;

// ---- Main Menu (movie StartMenu) ------------------------------------------------------------------

class MainMenu final : public Menu {
public:
    // MainMenu::MainMenu clears the flag that skips the fade at the end of a load.
    explicit MainMenu(Ui& ui) : Menu(ui, {"Main Menu", "StartMenu", ScaleMode::show_all, 9, 0x20581, 0, 1}) {
        ui.set_new_game_load(false);
    }

    void on_show() override {
        // Show (UI message 1): the Mist Menu with the default logo, then sendMenuProperties (section 1, step 8).
        m_ui.open("Mist Menu");
        send_menu_properties();
        // The news panel: the engine hands Bethesda.net's message of the day to the movie's SetMotd once it has
        // it. spud talks to no online service; the text captured from the user's own game (the game
        // scenario capture-main.osc) stands in, and without it the panel stays hidden, as SetMotd("") leaves it.
        if (const auto motd = captured("main_menu", "_root.MenuHolder.Menu_mc.MessageOfTheDay_mc.Motd_tf.text")) {
            invoke("MenuHolder.Menu_mc.SetMotd", {*motd});
        }
    }

    void advance(double dt, double engine_ms) override {
        Menu::advance(dt, engine_ms);
        // Its per-frame function swaps the backdrop to the Anniversary logo while the upgrade is owned, which then
        // stays the default ( calling; section 2).
        if (!m_ui.host().ui_main_menu_state().anniversary_upsell) {
            m_ui.set_backdrop_logo("meshes/interface/logo/logo01ae.osm");
        }
    }

private:
    void register_handlers() override {
        handle("CONTINUE", [this](Call&) {
            if (m_ui.host().ui_has_saves()) {
                send("ConfirmContinue", {Value::string(m_ui.game_setting("sConfirmContinue"))});
            }
        });
        handle("NEW", [this](Call&) {
            const bool mods = m_ui.host().ui_mods_loaded();
            send("ConfirmNewGame", {Value::string(m_ui.game_setting(mods ? "sConfirmNewMods" : "sConfirmNew"))});
        });
        handle("StartNewGame", [this](Call&) {
            m_ui.set_new_game_load(true); // The game: its load ends without the fade
            m_ui.host().ui_action("new_game");
        });
        handle("ContinueLastSavedGame", [this](Call&) {
            if (m_ui.host().ui_has_saves()) {
                m_ui.host().ui_action("continue");
            } else {
                send("FadeInMenu"); // status 13: no save to continue, the menu fades back in
            }
        });
        handle("QuitToDesktop", [this](Call&) { m_ui.host().ui_action("quit"); });
        handle("fadeOutStarted", [this](Call&) { m_ignore_input = true; });
        handle("PlaySound", [this](Call& c) {
            if (c.args.size() == 1) {
                m_ui.host().ui_play_sound(*player()->vm.to_string(c.arg(0)));
            }
        });
        handle("OpenCreditsMenu", [this](Call&) { m_ui.open("Credits Menu"); });
        handle("DownloadAll", [this](Call&) { send_menu_properties(); });
        for (const char* noop : {"OnDisabledLoadPress", "EndPressStartState", "HELP", "OpenMarketplace", "myLog",
                                 "ChangeUser", "ActivateJoyConStrapInstruction", "OnPS5DataTransfer",
                                 "OnOunceDataTransfer", "FirstTimeOunceDataTransferCheck", "Sky10DLCPressed",
                                 "MOD", "CreationClub"}) {
            handle(noop, [](Call&) {});
        }
        handle_save_load();
    }

    void on_loaded() override {
        // _root.CodeObj: thirteen native functions (the Bethesda.net login hooks), then onCodeObjectInit.
        const Value code = player()->get_path("CodeObj");
        if (!code.is_object()) {
            return;
        }
        for (const char* name : {"initLogin", "BeginLogin", "startEditText", "endEditText", "onLoginScreenOpen",
                                 "onLoginScreenClose", "attemptLogin", "PopulateEULA", "AcceptLegalDoc",
                                 "createQuickAccount", "GetBnetUpdate", "CClubBlockedByBnet",
                                 "CClubBlockedByPermissions"}) {
            player()->vm.define_method(code.o, name, [](player::NativeCall&) -> Value { return {}; });
        }
        if (has_path("onCodeObjectInit")) {
            invoke("onCodeObjectInit");
        }
    }

    void send_menu_properties() {
        // sendMenuProperties: the 15 values the PC build sends (section 1, table).
        const MainMenuState s = m_ui.host().ui_main_menu_state();
        send("sendMenuProperties", {
                                       Value::boolean(true),                // QUIT
                                       Value::boolean(m_ui.host().ui_has_saves()), // CONTINUE, LOAD enabled
                                       Value{},                             // no version text on PC
                                       Value::boolean(false),               // press-start screen
                                       Value::boolean(s.anniversary_upsell), // Sky10 banner
                                       Value::boolean(false),               // DLC entry
                                       Value::boolean(false),               // HELP
                                       Value::boolean(false),               // MOD MANAGER
                                       Value::boolean(true),                // CREATIONS
                                       Value::boolean(true),                // no Bethesda.net login state
                                       Value::boolean(false),               // unused
                                       Value::boolean(false),               // PS5 transfer
                                       Value::boolean(s.creations_icon),    // CREATIONS icon
                                       Value::boolean(s.creations_enabled), // CREATIONS enabled
                                       Value::boolean(false),               // Switch transfer
                                   });
    }

    bool m_ignore_input = false;
};

// ---- Cursor Menu ----------------------------------------------------------------------------------

class CursorMenu final : public Menu {
public:
    explicit CursorMenu(Ui& ui) : Menu(ui, {"Cursor Menu", "CursorMenu", ScaleMode::show_all, 13, 0x8800}) {}
    bool pass_through() const override { return true; }
    void on_show() override { m_ui.send_mouse_position(*this); }
};

// ---- Fader Menu -----------------------------------------------------------------------------------

class FaderMenu final : public Menu {
public:
    explicit FaderMenu(Ui& ui) : Menu(ui, {"Fader Menu", "FaderMenu", ScaleMode::exact_fit, 3, 0x8802}) {}

    // A fade queued with the show or update message. The menu stays up after a
    // fade to opaque that asked to stay (FaderData false and true), and hides after any other.
    void on_show() override {
        if (const auto r = m_ui.take_fade_request()) {
            m_keep = !r->arg0 && r->stay;
            fade(r->arg0, r->arg1, r->seconds, r->min_seconds);
        }
    }

    void advance(double dt, double engine_ms) override {
        Menu::advance(dt, engine_ms);
        // The game passes the frame timer's float delta widened to a double
        // (g_FrameTimer, cvtps2pd): 0.01666666753590107 at 60 fps, as traced in the game. The movie adds it up
        // against the minimum time, so the rounding decides the frame: initFade(true, true, 1, 1.5) ends after 149
        // updates in the game (run hud-script1) and after 150 with an exact 1/60.
        if (m_fading) {
            invoke("mc_FaderMenu.updateFade", {Value::number(double(float(dt)))});
        }
    }
    // A fade request, passed to the movie's initFade as it is: `from_opaque` true fades the colour out (alpha 100 to
    // 0), false in (0 to 100); `black` true is black, false white (fadermenu.swf, initFade); over `seconds`, staying up
    // at least `min_seconds`.
    void fade(bool from_opaque, bool black, double seconds, double min_seconds) {
        m_fading = true;
        invoke("mc_FaderMenu.initFade", {Value::boolean(from_opaque), Value::boolean(black), Value::number(seconds),
                                         Value::number(min_seconds)});
    }

private:
    void register_handlers() override {
        handle("FadeDone", [this](Call&) {
            m_fading = false;
            if (!m_keep) {
                m_ui.close(name());
            }
        });
    }
    bool m_fading = false, m_keep = false;
};

// ---- MessageBoxMenu -------------------------------------------------------------------------------

class MessageBoxMenu final : public Menu {
public:
    explicit MessageBoxMenu(Ui& ui) : Menu(ui, {"MessageBoxMenu", "MessageBox", ScaleMode::exact_fit, 10, 0x11, 0, 1}) {}

    void on_show() override { show_next(); }

private:
    void register_handlers() override {
        handle("buttonPress", [this](Call& c) {
            m_ui.host().ui_play_sound("UIMenuOK");
            const int index = int(player()->vm.to_number(c.arg(0)));
            MessageBoxData msg = m_ui.take_message();
            if (m_ui.has_message()) {
                show_next();
            } else {
                m_ui.close(name());
            }
            if (msg.callback) {
                msg.callback(index);
            }
        });
        handle("PlaySound", [this](Call& c) { m_ui.host().ui_play_sound(*player()->vm.to_string(c.arg(0))); });
    }

    void show_next() {
        const MessageBoxData* m = m_ui.current_message();
        if (!m) {
            return;
        }
        send("setMessageText", {Value::string(m->text), Value::boolean(false)});
        send("setIsVertical", {Value::boolean(false)});
        // The number is the message data's integer; -1 for the game's own message boxes, measured on the
        // Level Up Menu's question.
        send("setIsCancellable", {Value::boolean(false), Value::number(-1)});
        std::vector<Value> buttons{Value::boolean(m_ui.gamepad_active())};
        for (const auto& b : m->buttons) {
            buttons.push_back(Value::string(b));
        }
        send("setButtons", std::move(buttons));
    }
};

// ---- Console --------------------------------------------------------------------------------------

class ConsoleMenu final : public Menu {
public:
    explicit ConsoleMenu(Ui& ui) : Menu(ui, {"Console", "Console", ScaleMode::no_scale, 12, 0x807, 0, 2}) {}

    void on_show() override {
        // On show (section 4, Console): size, colours, text size and history size, then Show.
        invoke("Console.SetSize", {Value::number(40)});           // iConsoleSizeScreenPercent:Menu
        invoke("Console.SetTextColor", {Value::number(0xFFFFFF)}); // rConsoleTextColor:Menu >> 8
        invoke("Console.SetTextSize", {Value::number(20)});        // iConsoleTextSize:Menu
        invoke("Console.SetHistoryTextColor", {Value::number(0x999999)});
        invoke("Console.SetHistoryCharBufferSize", {Value::number(16384)});
        invoke("Console.Show");
        flush();
    }
    bool on_hide() override {
        // The movie animates out and reports HideComplete; only then does the menu close.
        if (player() && player()->vm.to_boolean(player()->get_path("Console.QShown"))) {
            invoke("Console.Hide");
            m_hiding = true;
            return false;
        }
        return !m_hiding || !player() || !player()->vm.to_boolean(player()->get_path("Console.QHiding"));
    }

    void print(const std::string& text) {
        // Every printed line ends in "\n"; the whole pending text goes to AddHistory at once.
        m_pending += text;
        m_pending += '\n';
        flush();
    }
    void clear() { invoke("Console.ClearHistory"); }
    void previous_command() { invoke("Console.PreviousCommand"); }
    void next_command() { invoke("Console.NextCommand"); }

private:
    void register_handlers() override {
        handle("ExecuteCommand", [this](Call& c) { m_ui.host().ui_console(*player()->vm.to_string(c.arg(0))); });
        handle("HideComplete", [this](Call&) {
            m_hiding = false;
            m_ui.close_now(name());
        });
    }
    void flush() {
        if (m_pending.empty() || !player()) {
            return;
        }
        const std::string text = std::move(m_pending);
        m_pending.clear();
        invoke("Console.AddHistory", {Value::string(text)});
    }

    std::string m_pending;
    bool m_hiding = false;
};

// ---- Credits Menu ---------------------------------------------------------------------------------

class CreditsMenu final : public Menu {
public:
    // Constructor: ShowAll, background alpha 0.8, depth 10, flags 0x1, Menu Mode.
    explicit CreditsMenu(Ui& ui) : Menu(ui, {"Credits Menu", "CreditsMenu", ScaleMode::show_all, 10, 0x1, 0.8f, 1}) {}

private:
    void register_handlers() override { handle("myLog", [](Call&) {}); }

    void on_loaded() override {
        // _root.CodeObj gets requestCredits, getScrollSpeed and closeMenu (handler).
        const Value code = player()->get_path("CodeObj");
        if (!code.is_object()) {
            return;
        }
        auto& vm = player()->vm;
        vm.define_method(code.o, "requestCredits", [this](player::NativeCall& c) -> Value {
            // Every line of the credits file goes to the field with appendHtml; with two fields the first
            // iCreditsFileMaxLinesLength (1000) lines fill the first ().
            const std::vector<std::string>& lines = m_ui.credits();
            const size_t split = c.args.size() > 1 ? std::min<size_t>(1000, lines.size()) : lines.size();
            for (size_t i = 0; i < lines.size(); ++i) {
                const Value field = i < split ? c.arg(0) : c.arg(1);
                c.vm.call_method(field, "appendHtml", {Value::string(lines[i])});
            }
            return {};
        });
        vm.define_method(code.o, "getScrollSpeed", [this](player::NativeCall&) -> Value {
            // The game setting fCreditsScrollSpeed: 1.0 in the game, 2.0 from Skyrim.esm's GMST. The
            // movie moves its text by it each of its 30 fps frames, so 60 px a second: measured in the game, 296 px
            // over 4.93 s of the UI clock.
            const std::string s = m_ui.game_setting("fCreditsScrollSpeed");
            return Value::number(s.empty() ? 1.0 : std::strtod(s.c_str(), nullptr));
        });
        vm.define_method(code.o, "closeMenu", [this](player::NativeCall&) -> Value {
            m_ui.close(name());
            if (Menu* main = m_ui.find_menu("Main Menu")) {
                main->send("FadeInMenu"); // "FadeIn" to Main Menu
            }
            return {};
        });
        if (has_path("onCodeObjectInit")) {
            invoke("onCodeObjectInit");
        }
    }
};

// ---- the rest: their movie and default menu properties until their contract is implemented --------

class GenericMenu final : public Menu {
public:
    GenericMenu(Ui& ui, MenuDesc d) : Menu(ui, std::move(d)) {}
};

} // namespace

std::unique_ptr<Menu> create_menu(Ui& ui, const std::string& name) {
    if (name == "Main Menu") return std::make_unique<MainMenu>(ui);
    if (name == "Cursor Menu") return std::make_unique<CursorMenu>(ui);
    if (name == "Fader Menu") return std::make_unique<FaderMenu>(ui);
    if (name == "MessageBoxMenu") return std::make_unique<MessageBoxMenu>(ui);
    if (name == "Console") return std::make_unique<ConsoleMenu>(ui);
    if (name == "Credits Menu") return std::make_unique<CreditsMenu>(ui);
    if (auto m = create_loading_menu(ui, name)) {
        return m;
    }
    if (auto m = create_journal_menu(ui, name)) {
        return m;
    }
    if (auto m = create_items_menu(ui, name)) {
        return m;
    }
    if (auto m = create_magic_menu(ui, name)) {
        return m;
    }
    if (auto m = create_hud_menu(ui, name)) {
        return m;
    }
    if (auto m = create_stats_menu(ui, name)) {
        return m;
    }
    if (auto m = create_world_menu(ui, name)) {
        return m;
    }
    return nullptr;
}

// Hooks for Ui (ui.cpp) into the menus it drives directly.
void console_print(Menu& m, const std::string& text) { static_cast<ConsoleMenu&>(m).print(text); }
void console_clear(Menu& m) { static_cast<ConsoleMenu&>(m).clear(); }
void console_history(Menu& m, bool previous) {
    previous ? static_cast<ConsoleMenu&>(m).previous_command() : static_cast<ConsoleMenu&>(m).next_command();
}

} // namespace spud::ui
