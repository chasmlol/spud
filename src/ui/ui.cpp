#include "ui/ui.hpp"

#include "ui/loading_screens.hpp"
#include "ui/menu.hpp"
#include "ui/menus_internal.hpp"
#include "ui/movie_renderer.hpp"
#include "ui/player/json.hpp"
#include "ui/player/player.hpp"
#include "ui/player/text.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "formats/osu.hpp"
#include "render/renderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cctype>
#include <cstdlib>
#include <format>

namespace spud::ui {

// menus.cpp
void console_print(Menu& m, const std::string& text);
void console_clear(Menu& m);
void console_history(Menu& m, bool previous);
// menus_loading.cpp
void mist_scene(Menu& m, BackdropScene& s);
bool mist_can_process(Menu& m);
void mist_button(Menu& m, int button, bool down);
bool mist_mouse_move(Menu& m, int dx, int dy);
void mist_thumbstick(Menu& m, float x, float y);
std::string loading_state_json(Menu& loading, Menu* mist);

namespace {

using Clock = std::chrono::steady_clock;
using Value = player::Value;
constexpr size_t k_stat_window = 120;
constexpr int k_no_context = 19;
constexpr double k_repeat_delay_ms = 300; // fKeyboardRepeatDelay:Controls
constexpr double k_repeat_rate_ms = 50;   // fKeyboardRepeatRate:Controls
constexpr float k_mouse_cursor_speed = 1.0f; // fMouseCursorSpeed:Interface

// SDL scancodes (USB HID usages) -> DirectInput scan codes, the hardware values the control map uses.
uint32_t dik_of(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) {
        return 0x3B + (sc - SDL_SCANCODE_F1);
    }
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03; case SDL_SCANCODE_3: return 0x04;
    case SDL_SCANCODE_4: return 0x05; case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09; case SDL_SCANCODE_9: return 0x0A;
    case SDL_SCANCODE_0: return 0x0B; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_E: return 0x12;
    case SDL_SCANCODE_R: return 0x13; case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_O: return 0x18;
    case SDL_SCANCODE_P: return 0x19; case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B; case SDL_SCANCODE_RETURN: return 0x1C;
    case SDL_SCANCODE_LCTRL: return 0x1D; case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_S: return 0x1F;
    case SDL_SCANCODE_D: return 0x20; case SDL_SCANCODE_F: return 0x21; case SDL_SCANCODE_G: return 0x22;
    case SDL_SCANCODE_H: return 0x23; case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25;
    case SDL_SCANCODE_L: return 0x26; case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_Z: return 0x2C; case SDL_SCANCODE_X: return 0x2D; case SDL_SCANCODE_C: return 0x2E;
    case SDL_SCANCODE_V: return 0x2F; case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37; case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45; case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48; case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4A; case SDL_SCANCODE_KP_4: return 0x4B; case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: return 0x4D; case SDL_SCANCODE_KP_PLUS: return 0x4E; case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_KP_2: return 0x50; case SDL_SCANCODE_KP_3: return 0x51; case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53; case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_ENTER: return 0x9C; case SDL_SCANCODE_RCTRL: return 0x9D;
    case SDL_SCANCODE_KP_DIVIDE: return 0xB5; case SDL_SCANCODE_PRINTSCREEN: return 0xB7;
    case SDL_SCANCODE_RALT: return 0xB8; case SDL_SCANCODE_PAUSE: return 0xC5; case SDL_SCANCODE_HOME: return 0xC7;
    case SDL_SCANCODE_UP: return 0xC8; case SDL_SCANCODE_PAGEUP: return 0xC9; case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD; case SDL_SCANCODE_END: return 0xCF; case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_PAGEDOWN: return 0xD1; case SDL_SCANCODE_INSERT: return 0xD2;
    case SDL_SCANCODE_DELETE: return 0xD3;
    default: return 0;
    }
}

std::string_view str_at(const std::vector<char>& s, uint32_t offset) {
    return offset < s.size() ? std::string_view(s.data() + offset) : std::string_view();
}

std::string json_string(std::string_view s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                o += std::format("\\u{:04x}", int(c));
            } else {
                o += c;
            }
        }
    }
    return o + "\"";
}

std::string folded(std::string_view s) {
    std::string o;
    for (char c : s) {
        if (c != ' ' && c != '\r' && c != '\n') {
            o += char(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return o;
}

bool shown(const player::DisplayObject* d) {
    for (; d; d = d->parent) {
        if (!d->visible || d->removed || d->cxform.mul[3] <= 0.0f) {
            return false;
        }
    }
    return true;
}

// Visible text fields of a movie, top first.
void collect_texts(player::DisplayObject* d, std::vector<player::TextFieldObject*>& out) {
    if (!d->visible || d->removed) {
        return;
    }
    if (d->kind == player::DisplayKind::text) {
        out.push_back(static_cast<player::TextFieldObject*>(d));
    } else if (d->kind == player::DisplayKind::clip) {
        auto* c = static_cast<player::MovieClip*>(d);
        for (auto it = c->children.rbegin(); it != c->children.rend(); ++it) {
            collect_texts(it->second.get(), out);
        }
    }
}

class DebugMenu final : public Menu {
public:
    DebugMenu(Ui& ui, MenuDesc d) : Menu(ui, std::move(d)) {}
};

} // namespace

// ---- setup -------------------------------------------------------------------------------------

Ui::Ui(render::Renderer& renderer, UiHost& host, UiOptions options)
    : m_renderer(renderer), m_host(host), m_options(std::move(options)) {
    const auto start = Clock::now();
    m_library = std::make_unique<player::Library>(m_options.interface_dir);
    m_library->read_ahead();
    for (const auto& e : m_library->errors) {
        log::warn("ui: {}", e);
    }
    m_controls = std::make_unique<osu::Controls>();
    try {
        *m_controls = osu::read_controls(read_file(m_options.interface_dir / "controls.osu"));
        m_default_controls = std::make_unique<osu::Controls>(*m_controls);
    } catch (const std::exception& e) {
        log::warn("ui: controls: {} (import the game with skytool import)", e.what());
    }
    m_movie_renderer =
        std::make_unique<MovieRenderer>(renderer.device(), renderer.uploader(), renderer.color_format(),
                                        renderer.render_extent(), m_options.shader_dir, m_options.pipeline_cache);
    m_movie_renderer->set_engine_images(&m_library->images, &renderer.jobs(), m_options.interface_dir.parent_path());
    m_scene_models = std::make_unique<SceneModels>(
        m_options.interface_dir.parent_path(),
        [&jobs = renderer.jobs()](std::function<void()> job) { jobs.submit(0.5f, std::move(job)); });
    m_cpu_recent.assign(k_stat_window, 0.0f);
    m_gpu_recent.assign(k_stat_window, 0.0f);
    // The menu cursor starts in the middle of the screen: the start-up's the game(0, 0, width, height)
    // (the call) centres it, (min + max) x 0.5.
    m_mouse_x = float(target_width()) * 0.5f;
    m_mouse_y = float(target_height()) * 0.5f;
    // The game seeds its the game from _time64 on first use; headless runs use a fixed seed so a
    // scripted run picks the same loading screens every time ("ui seed" changes it).
    m_random.seed(m_options.window ? static_cast<uint32_t>(std::time(nullptr)) : 0u, m_crt);
    log::info("ui: interface from {}, ready in {:.1f} ms", m_options.interface_dir.string(),
              std::chrono::duration<double, std::milli>(Clock::now() - start).count());
}

Ui::~Ui() {
    m_debug_movies.clear();
    // Out of the list first: a menu's destructor can message the others (the HUD mode it held), and finds none.
    auto menus = std::move(m_menus);
    m_menus.clear();
    menus.clear();
    release_scene_textures();
    m_movie_renderer.reset();
}

int Ui::target_width() const { return int(m_renderer.render_extent().width); }
int Ui::target_height() const { return int(m_renderer.render_extent().height); }

std::string Ui::date_text(const Calendar& c, bool with_year) const {
    // The game: "%s, %d:%02d %s, %d%s%s%s" from the weekday, the 12-hour clock,
    // AM/PM, the day and its ordinal suffix, sOf and the month, then ", 4E %d" with the year. Names are GMSTs.
    static constexpr const char* k_days[7] = {"sDaySunday", "sDayMonday", "sDayTuesday", "sDayWednesday",
                                              "sDayThursday", "sDayFriday", "sDaySaturday"};
    static constexpr const char* k_months[12] = {"sMonthJanuary", "sMonthFebruary", "sMonthMarch", "sMonthApril",
                                                 "sMonthMay", "sMonthJune", "sMonthJuly", "sMonthAugust",
                                                 "sMonthSeptember", "sMonthOctober", "sMonthNovember",
                                                 "sMonthDecember"};
    const float hour = float(c.hour);
    const uint8_t h = uint8_t(int(std::floor(hour)));
    const int minutes = int(std::floor((hour - float(h)) * 60.0f));
    const int h12 = h % 12 == 0 ? 12 : h % 12;
    const std::string ampm = game_setting(hour < 12.0f ? "sTimeAM" : "sTimePM");
    const int day = int8_t(int(c.day));
    const char* suffix = "sDefaultOrdSuffix";
    switch (day % 10) {
    case 1: suffix = day % 100 == 11 ? "sDefaultOrdSuffix" : "sFirstOrdSuffix"; break;
    case 2: suffix = day % 100 == 12 ? "sDefaultOrdSuffix" : "sSecondOrdSuffix"; break;
    case 3: suffix = day % 100 == 13 ? "sDefaultOrdSuffix" : "sThirdOrdSuffix"; break;
    default: break;
    }
    const int month = int8_t(int(c.month));
    const int weekday = int(uint32_t(int64_t(c.days_passed)) % 7u);
    const std::string day_name = weekday >= 0 && weekday < 7 ? game_setting(k_days[weekday]) : "Bad Day";
    const std::string month_name = month >= 0 && month < 12 ? game_setting(k_months[month]) : std::string();
    std::string s = std::format("{}, {}:{:02} {}, {}{}{}{}", day_name, h12, minutes, ampm, day, game_setting(suffix),
                                game_setting("sOf"), month_name);
    if (with_year) {
        s += std::format(", 4E {}", int(c.year));
    }
    return s;
}

Calendar Ui::calendar(const PlayerInfo& p) const { return p.calendar ? *p.calendar : start_calendar(); }

const player::json::Value* Ui::capture(const std::string& name) {
    auto it = m_captures.find(name);
    if (it == m_captures.end()) {
        std::unique_ptr<player::json::Value> v;
        const auto file = m_options.interface_dir.parent_path() / "captures" / (name + ".json");
        if (std::filesystem::exists(file)) {
            const auto bytes = read_file(file);
            std::string error;
            auto parsed = std::make_unique<player::json::Value>();
            if (player::json::parse(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), *parsed,
                                    error)) {
                v = std::move(parsed);
            } else {
                log::warn("ui: capture {}: {}", file.string(), error);
            }
        }
        it = m_captures.emplace(name, std::move(v)).first;
    }
    return it->second.get();
}

Calendar Ui::start_calendar() const {
    // The load order's values; the game's fallbacks when a global is missing.
    Calendar c;
    c.year = m_library->global("GameYear", 77);
    c.month = m_library->global("GameMonth", 7);
    c.day = m_library->global("GameDay", 17);
    c.hour = m_library->global("GameHour", 12);
    c.days_passed = m_library->global("GameDaysPassed", 1);
    return c;
}

std::string Ui::game_setting(const std::string& name) const {
    const std::string* s = m_library->game_setting(name);
    return s ? *s : m_host.ui_text_setting(name);
}

void Ui::report_error(const std::string& text) { message(text, {"OK"}); }

const std::vector<std::string>& Ui::credits() {
    if (!m_credits_read) {
        m_credits_read = true;
        try {
            const auto bytes = read_file(m_options.interface_dir / "credits.txt");
            // As the engine's line reader with a 128-byte buffer: up to 127 bytes, ending at '\n' (dropped,
            // a '\r' stays), and the credits stop at the first empty read.
            const auto* p = reinterpret_cast<const char*>(bytes.data());
            size_t at = 0;
            for (;;) {
                std::string chunk;
                while (at < bytes.size() && chunk.size() < 127) {
                    const char c = p[at++];
                    if (c == '\n') {
                        break;
                    }
                    chunk += c;
                }
                if (chunk.empty()) {
                    break;
                }
                m_credits.push_back(std::move(chunk));
            }
        } catch (const std::exception& e) {
            log::warn("ui: credits: {}", e.what());
        }
    }
    return m_credits;
}

// ---- the menu stack ----------------------------------------------------------------------------

std::string Ui::canonical(const std::string& name) const {
    static const std::pair<const char*, const char*> k_aliases[] = {
        {"main_menu", "Main Menu"}, {"pause", "Journal Menu"},     {"settings", "Journal Menu"},
        {"console", "Console"},     {"loading", "Loading Menu"},   {"credits", "Credits Menu"},
        {"confirm", "MessageBoxMenu"}, {"message", "MessageBoxMenu"}, {"cursor", "Cursor Menu"},
        {"fader", "Fader Menu"},    {"mist", "Mist Menu"},         {"journal", "Journal Menu"}};
    for (const auto& [alias, real] : k_aliases) {
        if (name == alias) {
            return real;
        }
    }
    return name;
}

Menu* Ui::find(const std::string& name) const {
    const std::string n = canonical(name);
    for (const auto& m : m_menus) {
        if (m->name() == n) {
            return m.get();
        }
    }
    // Debug movies ("ui movie NAME") by "movie:NAME", for the automation commands that read and drive a movie.
    for (const auto& m : m_debug_movies) {
        if (m->name() == name) {
            return m.get();
        }
    }
    return nullptr;
}

bool Ui::is_open(const std::string& name) const { return find(name) != nullptr; }

bool Ui::menus_allow_saving() const {
    constexpr uint32_t k_allows_saving = 0x800;
    return std::all_of(m_menus.begin(), m_menus.end(), [](const auto& m) { return (m->desc().flags & k_allows_saving) != 0; });
}

int Ui::pausing_menus() const {
    return int(std::count_if(m_menus.begin(), m_menus.end(),
                             [](const auto& m) { return (m->desc().flags & menu_pauses_game) != 0; }));
}

std::vector<Menu*> Ui::by_depth() const {
    std::vector<Menu*> out;
    by_depth(out);
    return out;
}

void Ui::by_depth(std::vector<Menu*>& out) const {
    out.clear();
    for (const auto& m : m_menus) {
        out.push_back(m.get());
    }
    std::stable_sort(out.begin(), out.end(), [](Menu* a, Menu* b) { return a->desc().depth < b->desc().depth; });
}

void Ui::open(const std::string& name) {
    const size_t bar = name.find('|');
    const std::string menu = canonical(name.substr(0, bar));
    if (bar != std::string::npos) {
        m_show_data[menu] = name.substr(bar + 1);
    }
    m_queue.push_back({menu, true});
    process_messages();
}

std::string Ui::show_data(const std::string& menu) const {
    const auto it = m_show_data.find(canonical(menu));
    return it == m_show_data.end() ? std::string() : it->second;
}

std::string Ui::button_for_event(const std::string& event) const {
    const osu::Controls& c = *m_controls;
    for (osu::InputDevice device : {osu::InputDevice::keyboard, osu::InputDevice::mouse}) {
        for (const osu::ControlContext& cc : c.contexts) {
            for (uint32_t i = 0; i < cc.binding_count; ++i) {
                const osu::ControlBinding& b = c.bindings[cc.first_binding + i];
                if (b.device != device || str_at(c.strings, b.event) != event) {
                    continue;
                }
                for (const auto& k : c.keys) {
                    if (k.device == device && k.code == b.code) {
                        return std::string(str_at(c.strings, k.name));
                    }
                }
            }
        }
    }
    return event;
}

void Ui::open_journal(bool remembered_tab) {
    if (is_open("Journal Menu")) {
        return;
    }
    m_journal_remembered = remembered_tab;
    open("Journal Menu");
}

void Ui::close(const std::string& name) {
    m_queue.push_back({canonical(name), false});
    process_messages();
}

void Ui::close_now(const std::string& name) {
    // A movie told its menu it is done hiding (HideComplete, CloseMenu): the hide goes through.
    m_queue.push_back({"!" + canonical(name), false});
    process_messages();
}

void Ui::hud_mode(const std::string& mode, bool on) { hud_set_mode(*this, mode, on); }

void Ui::close_all(bool keep_hud) {
    for (const auto& m : m_menus) {
        if (keep_hud && (m->name() == "HUD Menu" || m->name() == "Debug Text Menu" || m->name() == "Kinect Menu")) {
            continue;
        }
        m_queue.push_back({"!" + m->name(), false});
    }
    m_messages.clear();
    process_messages();
}

void Ui::process_messages() {
    // Movies call back into the UI from their scripts; menus change only between scripts.
    if (m_script_depth > 0) {
        return;
    }
    ++m_script_depth;
    for (int guard = 0; guard < 256 && !m_queue.empty(); ++guard) {
        Message msg = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
        if (msg.show) {
            show(msg.menu);
        } else if (msg.menu.starts_with("!")) {
            hide(msg.menu.substr(1), true);
        } else {
            hide(msg.menu, false);
        }
        update_cursor();
    }
    --m_script_depth;
}

void Ui::show(const std::string& name) {
    if (Menu* existing = find(name)) {
        // A new message replaces the shown one; a fade for the open Fader Menu reaches it as the update message
        // The game posts then, which StartFade handles as the show does ().
        if (name == "MessageBoxMenu" || (name == "Fader Menu" && m_fade_request)) {
            existing->on_show();
        }
        return;
    }
    std::unique_ptr<Menu> menu = create_menu(*this, name);
    if (!menu) {
        log::warn("ui: no menu called '{}'", name);
        return;
    }
    try {
        menu->load(*m_library);
    } catch (const std::exception& e) {
        log::error("ui: {}", e.what());
        return;
    }
    if (menu->player()) {
        m_movie_renderer->prepare(*menu->player()->movie);
        for (const auto& e : menu->player()->errors()) {
            log::debug("ui {}: {}", name, e);
        }
    }
    Menu* raw = menu.get();
    m_menus.push_back(std::move(menu));
    raw->on_show();
    if (name == "Console" && !m_console_pending.empty()) {
        ui::console_print(*raw, m_console_pending.substr(0, m_console_pending.size() - 1));
        m_console_pending.clear();
    }
    log::info("ui: open {}", name);
}

void Ui::hide(const std::string& name, bool force) {
    auto it = std::find_if(m_menus.begin(), m_menus.end(), [&](const auto& m) { return m->name() == name; });
    if (it == m_menus.end()) {
        return;
    }
    if (!force && !(*it)->on_hide()) {
        return; // the movie animates out first and reports back
    }
    // Out of the list before it goes: its destructor can message the menus still open (the HUD mode it held).
    std::unique_ptr<Menu> closing = std::move(*it);
    m_menus.erase(it);
    closing.reset();
    log::info("ui: close {}", name);
    // The Mist Menu goes with the last menu that showed it.
    if (name == "Main Menu" || name == "Loading Menu") {
        if (!is_open("Main Menu") && !is_open("Loading Menu")) {
            m_queue.push_back({"!Mist Menu", false});
        }
    }
}

bool Ui::holds_mouse() const {
    const Menu* t = top_input_menu();
    return t && t->holds_cursor();
}

void Ui::update_cursor() {
    // Menus with "uses cursor" (0x4), or with 0x400 while no gamepad is active, show the Cursor Menu.
    bool want = false;
    for (const auto& m : m_menus) {
        const uint32_t f = m->desc().flags;
        if ((f & menu_uses_cursor) || ((f & menu_cursor_follows_device) && !gamepad_active())) {
            want = true;
        }
    }
    const bool open = is_open("Cursor Menu");
    if (want && !open) {
        m_queue.push_back({"Cursor Menu", true});
    } else if (!want && open) {
        m_queue.push_back({"!Cursor Menu", false});
    }
}

bool Ui::captures_input() const {
    // Also a menu that runs with the game in Menu Mode (input context 1, or the "uses menu context" flag) without
    // pausing it: the Dialogue Menu, whose movie takes the keys while the world goes on, and the Crafting Menu
    // (0x6488), which takes the keyboard through Menu Mode and Item Menus while the game runs on behind it.
    for (const auto& m : m_menus) {
        if ((m->desc().flags & (menu_pauses_game | menu_uses_menu_context)) || m->desc().context == 1 ||
            m->name() == "Console") {
            return true;
        }
    }
    return false;
}

bool Ui::visible() const { return !m_menus.empty() || !m_debug_movies.empty(); }

std::string Ui::loading_json() {
    Menu* loading = find("Loading Menu");
    return loading ? loading_state_json(*loading, find("Mist Menu")) : std::string("null");
}

const LoadingScreens* Ui::loading_screens() {
    if (!m_loading_screens_read) {
        m_loading_screens_read = true;
        try {
            m_loading_screens = LoadingScreens::load(m_options.interface_dir / "loadingscreens.osl");
        } catch (const std::exception& e) {
            log::warn("ui: loading screens: {}", e.what());
        }
    }
    return m_loading_screens.get();
}

const std::vector<SoundCategory>& Ui::sound_categories() {
    if (!m_sound_categories) {
        m_sound_categories.emplace();
        try {
            const osu::Strings t = osu::read_strings(read_file(m_options.interface_dir / "soundcategories.osu"));
            std::map<std::string, std::string> keys;
            for (const auto& e : t.entries) {
                keys.emplace(std::string(str_at(t.strings, e.key)), std::string(str_at(t.strings, e.text)));
            }
            for (int i = 0;; ++i) {
                const std::string k = std::format("{:03}", i);
                const auto id = keys.find(k + ".id");
                if (id == keys.end()) {
                    break;
                }
                SoundCategory c;
                c.form_id = uint32_t(std::strtoul(id->second.c_str(), nullptr, 16));
                c.name = keys[k + ".name"];
                c.default_value = std::strtod(keys[k + ".default"].c_str(), nullptr) / 65535.0;
                m_sound_categories->push_back(std::move(c));
            }
        } catch (const std::exception& e) {
            log::warn("ui: sound categories: {} (import the interface with skytool ui import)", e.what());
        }
    }
    return *m_sound_categories;
}

bool Ui::backdrop() const { return is_open("Mist Menu"); }

std::optional<BackdropScene> Ui::backdrop_scene() const {
    if (!backdrop()) {
        return std::nullopt;
    }
    // the game's defaults for these settings; no INI or plugin of this install overrides them (section 2). The model,
    // its placement and the camera are the Mist Menu's.
    BackdropScene s{};
    mist_scene(*find("Mist Menu"), s);
    s.near_units = 15.0f;
    s.far_units = 20480.0f;
    s.lights[0] = {{-160.0f, 160.0f, 128.0f}, {0.96f, 0.96f, 0.82f}, 1500.0f, 1.6f, true};
    s.lights[1] = {{160.0f, -96.0f, 160.0f}, {0.70f, 0.81f, 0.80f}, 1024.0f, 3.0f, false};
    s.lights[2] = {{128.0f, 160.0f, -96.0f}, {1.0f, 1.0f, 1.0f}, 1024.0f, 0.1f, false};
    s.seconds = m_backdrop_seconds;
    return s;
}

// ---- messages ----------------------------------------------------------------------------------

void Ui::message(const std::string& text, std::vector<std::string> buttons, std::function<void(int)> callback) {
    if (buttons.empty()) {
        buttons.push_back(game_setting("sOk").empty() ? "OK" : game_setting("sOk"));
    }
    if (m_auto_message >= 0) {
        m_auto_messages.push_back({text, std::move(buttons), std::move(callback)});
        return; // answered in the next update, as a box a player clicks: never inside the caller
    }
    m_messages.push_back({text, std::move(buttons), std::move(callback)});
    open("MessageBoxMenu");
}

MessageBoxData Ui::take_message() {
    if (m_messages.empty()) {
        return {};
    }
    MessageBoxData m = std::move(m_messages.back());
    m_messages.pop_back();
    return m;
}

// ---- console -----------------------------------------------------------------------------------

void Ui::set_crosshair(const CrosshairTarget& target) { hud_crosshair(*this, target); }

void Ui::notification(const std::string& text, const std::string& sound) { hud_notify(*this, text, sound); }

void Ui::set_stealth(bool sneaking, double level) { hud_stealth(*this, sneaking, level); }

void Ui::skill_notification(const std::string& skill, int level, int player_level, double xp) {
    hud_skill_increase(*this, skill, level, player_level, xp);
}

void Ui::console_print(const std::string& text, bool) {
    if (Menu* c = find("Console"); c && c->player()) {
        ++m_script_depth;
        ui::console_print(*c, text);
        --m_script_depth;
        process_messages();
    } else {
        m_console_pending += text + "\n";
    }
}

void Ui::console_clear() {
    m_console_pending.clear();
    if (Menu* c = find("Console")) {
        ui::console_clear(*c);
    }
}

void Ui::console_submit(const std::string& line) {
    if (!is_open("Console")) {
        open("Console");
    }
    // As the movie does on Enter: ExecuteCommand through the delegate.
    m_host.ui_console(line);
}

// ---- input -------------------------------------------------------------------------------------

Menu* Ui::top_input_menu() const {
    // Key and button events go to the topmost menu below depth 14 and down past menus that pass them on.
    const auto menus = by_depth();
    for (auto it = menus.rbegin(); it != menus.rend(); ++it) {
        Menu* m = *it;
        if (m->desc().depth < 14 && m->player() && !m->pass_through()) {
            return m;
        }
    }
    return nullptr;
}

std::vector<int> Ui::context_stack() const {
    // Main Gameplay at the bottom; each open menu adds Menu Mode when it uses the menu context (flag 0x8) and then
    // its own context. Contexts such as Journal (12) or Item Menus (3) hold only extra events, so a key is looked up
    // from the top down and the first context that binds it wins.
    std::vector<int> stack{0};
    for (const auto& m : m_menus) {
        if ((m->desc().flags & menu_uses_menu_context) && stack.back() != 1) {
            stack.push_back(1);
        }
        for (int c : m->input_contexts()) {
            if (stack.back() != c) {
                stack.push_back(c);
            }
        }
    }
    // The Cursor Menu pushes the Cursor context (9) unless what is up binds Cursor already: its Click is
    // the left mouse button, which the Stats Menu reads.
    if (is_open("Cursor Menu") && !binds(stack, "Cursor")) {
        stack.push_back(9);
    }
    return stack;
}

bool Ui::binds(const std::vector<int>& contexts, std::string_view event) const {
    const osu::Controls& c = *m_controls;
    for (int context : contexts) {
        if (context < 0 || size_t(context) >= c.contexts.size()) {
            continue;
        }
        const osu::ControlContext& cc = c.contexts[size_t(context)];
        for (uint32_t i = 0; i < cc.binding_count; ++i) {
            if (str_at(c.strings, c.bindings[cc.first_binding + i].event) == event) {
                return true;
            }
        }
    }
    return false;
}

std::string Ui::mouse_user_event(int button) const {
    const osu::Controls& c = *m_controls;
    const std::vector<int> stack = context_stack();
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if (*it < 0 || size_t(*it) >= c.contexts.size()) {
            continue;
        }
        const osu::ControlContext& cc = c.contexts[size_t(*it)];
        for (uint32_t i = 0; i < cc.binding_count; ++i) {
            const osu::ControlBinding& b = c.bindings[cc.first_binding + i];
            if (b.device == osu::InputDevice::mouse && b.code == uint32_t(button)) {
                return std::string(str_at(c.strings, b.event));
            }
        }
    }
    return {};
}

int Ui::input_context() const { return context_stack().back(); }

void Ui::deliver_key(uint32_t flash, bool down) {
    Menu* m = top_input_menu();
    if (!m) {
        return;
    }
    ++m_script_depth;
    if (!m->on_key(flash, down)) {
        m->player()->key(flash, 0, down);
    }
    --m_script_depth;
    process_messages();
}

std::vector<Ui::InputMapping> Ui::input_mappings() const {
    std::vector<InputMapping> out;
    const osu::Controls& c = *m_controls;
    if (c.contexts.empty()) {
        return out;
    }
    const osu::ControlContext& main = c.contexts[0];
    // An event keeps its place in the list whatever it's bound to: the game's sort index is the event's.
    std::vector<std::string_view> order;
    const osu::Controls& defaults = m_default_controls ? *m_default_controls : c;
    for (const osu::InputDevice device : {osu::InputDevice::mouse, osu::InputDevice::keyboard}) {
        const osu::ControlContext& first = defaults.contexts[0];
        for (uint32_t i = 0; i < first.binding_count; ++i) {
            const osu::ControlBinding& b = defaults.bindings[first.first_binding + i];
            const std::string_view name = str_at(defaults.strings, b.event);
            if (b.device == device && b.remappable && std::ranges::find(order, name) == order.end()) {
                order.push_back(name);
            }
        }
    }
    for (const osu::InputDevice device : {osu::InputDevice::mouse, osu::InputDevice::keyboard}) {
        for (uint32_t i = 0; i < main.binding_count; ++i) {
            const osu::ControlBinding& b = c.bindings[main.first_binding + i];
            if (b.device != device || !b.remappable) {
                continue;
            }
            for (const auto& k : c.keys) {
                if (k.device == device && k.code == b.code) {
                    const std::string_view event = str_at(c.strings, b.event);
                    out.push_back({std::string(event), std::string(str_at(c.strings, k.name)),
                                   int(std::ranges::find(order, event) - order.begin()), b.code});
                    break;
                }
            }
        }
    }
    return out;
}

void Ui::start_remap(std::string event, std::function<void(bool)> done) {
    m_remap_event = std::move(event);
    m_remap_done = std::move(done);
}

bool Ui::finish_remap(int device, uint32_t code) {
    if (!m_remap_done) {
        return false;
    }
    const bool took = remap(m_remap_event, device, code);
    std::exchange(m_remap_done, {})(took);
    return true;
}

bool Ui::remap(const std::string& event, int device, uint32_t code) {
    osu::Controls& c = *m_controls;
    if (c.contexts.empty()) {
        return false;
    }
    const auto to_device = [](int d) {
        return d == 0 ? osu::InputDevice::keyboard : d == 1 ? osu::InputDevice::mouse : osu::InputDevice::gamepad;
    };
    const osu::InputDevice wanted = to_device(device);
    const osu::ControlContext& main = c.contexts[0];
    osu::ControlBinding* mine = nullptr;
    osu::ControlBinding* other = nullptr;
    for (uint32_t i = 0; i < main.binding_count; ++i) {
        osu::ControlBinding& b = c.bindings[main.first_binding + i];
        if (str_at(c.strings, b.event) == event && (b.device == wanted || !mine) && b.remappable) {
            mine = &b;
        } else if (b.device == wanted && b.code == code) {
            other = &b;
        }
    }
    if (!mine || (other && !other->remappable)) {
        return false;
    }
    const osu::InputDevice old_device = mine->device;
    const uint32_t old_code = mine->code;
    const std::string other_event(other ? str_at(c.strings, other->event) : std::string_view());
    // The menus' contexts copy gameplay's keys ("!0,Name"): they follow it.
    for (osu::ControlBinding& b : c.bindings) {
        if (&b != mine && b.event == mine->event && b.device == old_device && b.code == old_code) {
            b.device = wanted;
            b.code = code;
        }
    }
    mine->device = wanted;
    mine->code = code;
    if (other) {
        other->device = old_device;
        other->code = old_code;
    }
    m_host.ui_control_changed(event, device, code);
    if (old_device != wanted) {
        m_host.ui_control_changed(event, old_device == osu::InputDevice::keyboard ? 0 : old_device == osu::InputDevice::mouse ? 1 : 2, 0xFF);
    }
    if (other) {
        m_host.ui_control_changed(other_event, old_device == osu::InputDevice::keyboard ? 0 : old_device == osu::InputDevice::mouse ? 1 : 2, old_code);
    }
    return true;
}

void Ui::reset_controls() {
    if (m_default_controls) {
        *m_controls = *m_default_controls;
    }
    m_host.ui_control_changed({}, 0, 0);
}

void Ui::set_gamepad_active(bool active) {
    if (active == m_gamepad_active) {
        return;
    }
    m_gamepad_active = active;
    for (const auto& m : m_menus) {
        if (m->has_path("SetPlatform")) {
            m->invoke("SetPlatform", {player::Value::number(active ? 1 : 0), player::Value::boolean(false)});
        }
    }
    process_messages();
}

void Ui::gamepad_button(uint32_t code, bool down) {
    if (down && finish_remap(2, code)) {
        return;
    }
    const osu::Controls& c = *m_controls;
    const std::vector<int> stack = context_stack();
    const osu::ControlBinding* row = nullptr;
    const osu::ControlContext* context = nullptr;
    for (auto it = stack.rbegin(); it != stack.rend() && !row; ++it) {
        if (*it < 0 || size_t(*it) >= c.contexts.size()) {
            continue;
        }
        const osu::ControlContext& cc = c.contexts[size_t(*it)];
        for (uint32_t i = 0; i < cc.binding_count && !row; ++i) {
            const osu::ControlBinding& b = c.bindings[cc.first_binding + i];
            if (b.device == osu::InputDevice::gamepad && b.code == code) {
                row = &b;
                context = &cc;
            }
        }
    }
    if (row) {
        // A control map line has all three devices: the pad does what the line's key does.
        for (uint32_t i = 0; i < context->binding_count; ++i) {
            const osu::ControlBinding& b = c.bindings[context->first_binding + i];
            if (b.event == row->event && b.device == osu::InputDevice::keyboard) {
                keyboard(b.code, down);
                return;
            }
        }
        if (down && captures_input()) {
            user_event(std::string(str_at(c.strings, row->event)));
        }
    }
    for (const auto& k : c.keys) {
        if (k.device == osu::InputDevice::gamepad && k.code == code && k.flash != osu::k_no_flash && captures_input()) {
            deliver_key(k.flash, down);
            return;
        }
    }
}

void Ui::keyboard(uint32_t dik, bool down) {
    if (down && finish_remap(0, dik)) {
        return;
    }
    const osu::Controls& c = *m_controls;
    const std::vector<int> stack = context_stack();
    std::string_view event;
    int from = -1;
    for (auto it = stack.rbegin(); it != stack.rend() && from < 0; ++it) {
        if (*it < 0 || size_t(*it) >= c.contexts.size()) {
            continue;
        }
        const osu::ControlContext& cc = c.contexts[size_t(*it)];
        for (uint32_t i = 0; i < cc.binding_count; ++i) {
            const osu::ControlBinding& b = c.bindings[cc.first_binding + i];
            if (b.device == osu::InputDevice::keyboard && b.code == dik) {
                event = str_at(c.strings, b.event);
                from = *it;
                break;
            }
        }
    }
    // The game posts every mapped button press to the top menu as a user event (type 7,
    // The game).
    if (down && !event.empty() && captures_input()) {
        user_event(std::string(event));
    }
    // Gameplay's own events don't reach the movies; inside a menu they leave the key unbound.
    if (from == 0 && captures_input()) {
        event = {};
    }
    // Engine-level keys before the movie: the console toggle, the console's history, Pause in the world.
    if (event == "Console" && down) {
        if (is_open("Console")) {
            close("Console");
        } else {
            open("Console");
        }
        return;
    }
    if (!captures_input()) {
        // The game: Pause (Esc) opens the Journal on its remembered tab, Journal (J)
        // from the keyboard or mouse on the Quests tab.
        if ((event == "Pause" || event == "Journal") && down && m_in_world) {
            open_journal(event == "Pause");
        } else if (event == "Tween Menu" && down && m_in_world) {
            if (m_host.ui_beast_form()) {
                open("StatsMenu"); // The game(1), the beast's tree
            } else {
                // The game: the Tween Menu, and the Favorites Menu goes.
                open("TweenMenu");
                close("FavoritesMenu");
                m_host.ui_tween_opened();
            }
        } else if (event == "Favorites" && down && m_in_world && m_host.ui_beast_form() != 1) {
            open("FavoritesMenu"); // The game
        } else if (event == "Quick Magic" && down && m_in_world && !m_host.ui_beast_form()) {
            open("MagicMenu"); // The game: the direct keys need the flag clear
        }
        // The Quicksave and Quickload keys: nothing while a menu pauses the game.
        if ((event == "Quicksave" || event == "Quickload") && down && m_in_world && pausing_menus() == 0) {
            if (event == "Quicksave") {
                m_host.ui_quicksave_key();
            } else {
                m_host.ui_quickload_key();
            }
        }
        // The game: Quick Map, pressed while no menu pauses the game, shows MapMenu.
        if (event == "Quick Map" && down && m_in_world) {
            open("MapMenu");
        }
        // The Wait event opens Sleep/Wait in wait mode (the game calling
        // SleepWaitMenu::Open(false)).
        if (event == "Wait" && down && m_in_world && host().ui_can_rest(false)) {
            open("Sleep/Wait Menu");
        }
        return; // the world's own controls
    }
    // A menu input handler of the top menu sees the user event, press and release (Lockpicking's keys).
    if (Menu* top = top_input_menu(); top && !event.empty()) {
        ++m_script_depth;
        const bool taken = top->input_event(event, down ? 1.0f : 0.0f);
        --m_script_depth;
        process_messages();
        if (taken) {
            return;
        }
    }
    if (from == 2 && down && (event == "Up" || event == "Down")) {
        if (Menu* con = find("Console")) {
            ++m_script_depth;
            console_history(*con, event == "Up");
            --m_script_depth;
            process_messages();
        }
        return;
    }
    // Menus with their own input handler (a game object) see the button before any Scaleform event is made of
    // it, pressed and released.
    if (!event.empty()) {
        if (Menu* m = top_input_menu()) {
            ++m_script_depth;
            const bool used = m->on_button(event, down);
            --m_script_depth;
            process_messages();
            if (used) {
                return;
            }
        }
    }
    // Mapped user events first (section 3, Input): Accept 13, Cancel 9, PageUp 33, PageDown 34.
    static const std::pair<std::string_view, uint32_t> k_mapped[] = {
        {"Accept", 13}, {"Cancel", 9}, {"PageUp", 33}, {"PageDown", 34}};
    for (const auto& [name, code] : k_mapped) {
        if (event == name) {
            deliver_key(code, down);
            return;
        }
    }
    uint32_t flash = osu::k_no_flash;
    for (const auto& k : c.keys) {
        if (k.device == osu::InputDevice::keyboard && k.code == dik) {
            flash = k.flash;
            break;
        }
    }
    if (flash == osu::k_no_flash || flash > 0xE5) {
        return;
    }
    // Enter and the arrows go raw only when no user event takes them; Tab raw only into a focused text field.
    if ((flash == 13 || (flash >= 37 && flash <= 40)) && !event.empty()) {
        return;
    }
    if (flash == 9) {
        Menu* m = top_input_menu();
        if (!m || !m->player()->focus() || m->player()->focus()->kind != player::DisplayKind::text) {
            return;
        }
    }
    deliver_key(flash, down);
    const bool repeats = flash == 8 || (flash >= 37 && flash <= 40) || flash == 46;
    if (down && repeats) {
        m_repeat_dik = dik;
        m_repeat_next = m_time_ms + k_repeat_delay_ms;
    } else if (!down && dik == m_repeat_dik) {
        m_repeat_dik = 0;
    }
}

void Ui::centre_cursor() {
    mouse_to_menus(float(target_width()) * 0.5f, float(target_height()) * 0.5f);
}

void Ui::mouse_to_menus(float x, float y) {
    m_mouse_x = x;
    m_mouse_y = y;
    // Mouse moves go to the Cursor Menu, which passes them on to the menu below.
    std::vector<Menu*> targets;
    if (Menu* c = find("Cursor Menu"); c && c->player()) {
        targets.push_back(c);
    }
    if (Menu* t = top_input_menu()) {
        targets.push_back(t);
    }
    for (const auto& d : m_debug_movies) {
        targets.push_back(d.get());
    }
    ++m_script_depth;
    for (Menu* m : targets) {
        const player::Matrix2D inv = m->stage_to_pixels(float(target_width()), float(target_height())).inverse();
        float sx, sy;
        inv.apply(x, y, sx, sy);
        m->player()->mouse_move(sx, sy);
    }
    --m_script_depth;
    process_messages();
}

void Ui::send_mouse_position(Menu& m) {
    if (!m.player()) {
        return;
    }
    const player::Matrix2D inv = m.stage_to_pixels(float(target_width()), float(target_height())).inverse();
    float sx, sy;
    inv.apply(m_mouse_x, m_mouse_y, sx, sy);
    m.player()->mouse_move(sx, sy);
}

void Ui::mouse_button(int button, bool down) {
    if (down && finish_remap(1, uint32_t(button))) {
        return;
    }
    Menu* m = top_input_menu();
    if (!m) {
        return;
    }
    ++m_script_depth;
    bool taken = false;
    if (const std::string event = mouse_user_event(button); !event.empty()) {
        // A bound mouse button is a user event too: the menu's input handler sees the press and the release
        // as the game hands it every the game (value 1, then 0; the local map's drag), and
        // a press is posted to the menu as well (LeftEquip / RightEquip in the item menus). What the handler
        // takes makes no Scaleform event: the map's look button is no click (message 6).
        taken = m->input_event(event, down ? 1.0f : 0.0f);
        if (down) {
            m->on_user_event(event);
        }
    }
    if (!taken) {
        m->player()->mouse_button(button == 0 ? player::MouseButton::left
                                  : button == 1 ? player::MouseButton::right
                                                : player::MouseButton::middle,
                                  down);
    }
    --m_script_depth;
    process_messages();
}

void Ui::mouse_motion(float dx, float dy) {
    if (Menu* t = top_input_menu(); t && captures_input()) {
        ++m_script_depth;
        t->mouse_delta(dx, dy);
        --m_script_depth;
        process_messages();
    }
}

bool Ui::handle_event(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (e.key.repeat) {
            return captures_input(); // repeats come from fKeyboardRepeatDelay/Rate, not the OS
        }
        const bool was = captures_input();
        keyboard(dik_of(e.key.scancode), e.type == SDL_EVENT_KEY_DOWN);
        return was || captures_input();
    }
    case SDL_EVENT_TEXT_INPUT: {
        Menu* m = top_input_menu();
        if (!m || !captures_input()) {
            return false;
        }
        std::u32string text = player::utf8_to_u32(e.text.text);
        std::erase(text, U'`'); // the console swallows the backquote (its toggle key)
        ++m_script_depth;
        m->player()->text_input(text);
        --m_script_depth;
        process_messages();
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (m_menus.empty()) {
            return false;
        }
        // The Mist Menu's input handler sees mouse moves before the menus do, for a loading screen's model.
        if (Menu* mist = find("Mist Menu"); mist && mist_can_process(*mist) &&
            mist_mouse_move(*mist, static_cast<int>(e.motion.xrel), static_cast<int>(e.motion.yrel))) {
            return true;
        }
        float sx = 1, sy = 1;
        if (m_options.window) {
            int w = 0, h = 0;
            SDL_GetWindowSize(m_options.window, &w, &h);
            sx = w > 0 ? float(target_width()) / float(w) : 1.0f;
            sy = h > 0 ? float(target_height()) / float(h) : 1.0f;
        }
        if (Menu* t = top_input_menu(); t && captures_input()) {
            t->mouse_delta(e.motion.xrel * sx, e.motion.yrel * sy);
            if (t->holds_cursor()) {
                return true;
            }
        }
        if (m_options.window && SDL_GetWindowRelativeMouseMode(m_options.window)) {
            // The game's own cursor (bBackgroundMouse:Controls off, its default; the game
            //): the mouse's motion moves it by dx x (width x 0.00078125) x fMouseCursorSpeed:Interface,
            // the same factor for y, clamped to the screen. fMouseCursorSpeed is 1.0 in the game and in this
            // install's SkyrimPrefs.ini.
            const float w = float(target_width()), h = float(target_height());
            const float k = w * 0.00078125f * k_mouse_cursor_speed;
            mouse_to_menus(std::clamp(m_mouse_x + e.motion.xrel * k, 0.0f, w),
                           std::clamp(m_mouse_y + e.motion.yrel * k, 0.0f, h));
        } else {
            mouse_to_menus(e.motion.x * sx, e.motion.y * sy);
        }
        return captures_input();
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!captures_input()) {
            return false;
        }
        const int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
        if (Menu* mist = find("Mist Menu"); mist && mist_can_process(*mist)) {
            mist_button(*mist, b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            return true;
        }
        mouse_button(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        Menu* m = top_input_menu();
        if (!m || !captures_input()) {
            return false;
        }
        // Gathered for the frame's poll, as the mouse device reads DirectInput's wheel once a frame
        // (poll_wheel).
        m_wheel += e.wheel.y;
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        // The sticks' raw state; poll_thumbsticks turns it into the game's thumbstick events each frame.
        const int axis = e.gaxis.axis;
        const int stick = axis <= SDL_GAMEPAD_AXIS_LEFTY ? 0 : axis <= SDL_GAMEPAD_AXIS_RIGHTY ? 1 : -1;
        if (stick < 0) {
            return false;
        }
        // The game's scaling: raw x 1/32767, clamped to [-1, 1]; XInput's y is up, SDL's down.
        const bool is_x = axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_RIGHTX;
        const float raw = is_x ? float(e.gaxis.value) : -float(e.gaxis.value);
        m_stick[stick][is_x ? 0 : 1] = std::clamp(raw * (1.0f / 32767.0f), -1.0f, 1.0f);
        return find("Mist Menu") != nullptr;
    }
    default:
        return false;
    }
}

bool Ui::back() {
    if (m_menus.empty()) {
        return false;
    }
    deliver_key(9, true); // Esc in menus is the Cancel user event, key 9
    deliver_key(9, false);
    return true;
}

player::Object* Ui::button_mapping(player::Player& p) {
    // Per device in use (mouse, then keyboard): each remappable binding of the main context with a key name, as
    // {mapName, buttonName}.
    const osu::Controls& c = *m_controls;
    std::vector<Value> entries;
    if (!c.contexts.empty()) {
        for (osu::InputDevice device : {osu::InputDevice::mouse, osu::InputDevice::keyboard}) {
            const osu::ControlContext& main = c.contexts[0];
            for (uint32_t i = 0; i < main.binding_count; ++i) {
                const osu::ControlBinding& b = c.bindings[main.first_binding + i];
                if (b.device != device || !b.remappable) {
                    continue;
                }
                std::string_view key;
                for (const auto& k : c.keys) {
                    if (k.device == device && k.code == b.code) {
                        key = str_at(c.strings, k.name);
                        break;
                    }
                }
                if (key.empty()) {
                    continue;
                }
                player::Object* o = p.vm.make_object();
                p.vm.set(Value::object(o), "mapName", Value::string(str_at(c.strings, b.event)));
                p.vm.set(Value::object(o), "buttonName", Value::string(key));
                entries.push_back(Value::object(o));
            }
        }
    }
    return p.vm.make_array(std::move(entries));
}

std::string Ui::button_name(std::string_view user_event) const {
    const osu::Controls& c = *m_controls;
    if (c.contexts.empty()) {
        return {};
    }
    const osu::ControlContext& main = c.contexts[0];
    for (osu::InputDevice device : {osu::InputDevice::keyboard, osu::InputDevice::mouse}) {
        for (uint32_t i = 0; i < main.binding_count; ++i) {
            const osu::ControlBinding& b = c.bindings[main.first_binding + i];
            if (b.device != device || str_at(c.strings, b.event) != user_event) {
                continue;
            }
            for (const auto& k : c.keys) {
                if (k.device == device && k.code == b.code) {
                    return std::string(str_at(c.strings, k.name));
                }
            }
        }
    }
    return {};
}

// ---- automation --------------------------------------------------------------------------------

void Ui::key(const std::string& name, int count) {
    const osu::Controls& c = *m_controls;
    uint32_t dik = 0;
    for (const auto& k : c.keys) {
        if (k.device == osu::InputDevice::keyboard && folded(str_at(c.strings, k.name)) == folded(name)) {
            dik = k.code;
            break;
        }
    }
    if (dik == 0) {
        throw Error("ui key: no keyboard key called '" + name + "' (names as in the game's keyboard_english.txt)");
    }
    for (int i = 0; i < count; ++i) {
        keyboard(dik, true);
        keyboard(dik, false);
    }
}

void Ui::key_state(const std::string& name, bool down) {
    const osu::Controls& c = *m_controls;
    for (const auto& k : c.keys) {
        if (k.device == osu::InputDevice::keyboard && folded(str_at(c.strings, k.name)) == folded(name)) {
            keyboard(k.code, down);
            return;
        }
    }
    throw Error("ui key: no keyboard key called '" + name + "' (names as in the game's keyboard_english.txt)");
}

void Ui::user_event(const std::string& event) {
    Menu* m = top_input_menu();
    if (!m) {
        return;
    }
    ++m_script_depth;
    m->on_user_event(event);
    --m_script_depth;
    process_messages();
}

void Ui::type(const std::string& text) {
    Menu* m = top_input_menu();
    if (!m) {
        throw Error("ui type: no menu is open");
    }
    ++m_script_depth;
    m->player()->text_input(player::utf8_to_u32(text));
    --m_script_depth;
    process_messages();
}

namespace {

// The visible text field showing `text` in a menu's movie, and its centre in target pixels.
bool find_text(Menu& m, const std::string& text, const std::string& translated, float tw, float th, float& x,
               float& y) {
    std::vector<player::TextFieldObject*> fields;
    collect_texts(m.player()->root.get(), fields);
    for (player::TextFieldObject* t : fields) {
        if (!shown(t)) {
            continue;
        }
        const std::string shown_text = folded(player::u32_to_utf8(t->plain_text()));
        if (shown_text != folded(text) && shown_text != folded(translated)) {
            continue;
        }
        // The middle of the glyphs (a person clicks the words, not the field's box, which can reach under a
        // neighbouring button).
        const player::TextLayout& l = t->laid_out();
        float lx0 = 1e9f, lx1 = -1e9f, ly0 = 1e9f, ly1 = -1e9f;
        for (const auto& g : l.glyphs) {
            lx0 = std::min(lx0, g.x);
            lx1 = std::max(lx1, g.x + g.size * 0.5f);
            ly0 = std::min(ly0, g.y - g.size * 0.7f);
            ly1 = std::max(ly1, g.y);
        }
        if (l.glyphs.empty()) {
            continue;
        }
        float sx, sy;
        t->world_matrix().apply(t->origin_x + (lx0 + lx1) * 0.5f, t->origin_y + (ly0 + ly1) * 0.5f, sx, sy);
        m.stage_to_pixels(tw, th).apply(sx, sy, x, y);
        return true;
    }
    return false;
}

} // namespace

std::string Ui::translated(const std::string& key) const {
    const std::string* text = m_library->translate(key);
    return text ? *text : key;
}

void Ui::select(const std::string& text) {
    Menu* m = top_input_menu();
    float x, y;
    const std::string* tr = m_library->translate(text);
    if (!m || !find_text(*m, text, tr ? *tr : text, float(target_width()), float(target_height()), x, y)) {
        throw Error("ui select: nothing shows '" + text + "'");
    }
    mouse_to_menus(x, y);
}

void Ui::activate(const std::string& text) {
    select(text);
    mouse_button(0, true);
    mouse_button(0, false);
}

// ---- frame -------------------------------------------------------------------------------------

void Ui::poll_wheel() {
    // The game: the wheel's motion since the last poll, whatever its size, is one
    // notch of its direction: mouse button 8 (up) or 9 (down) pressed with value 1 and released in the same
    // poll. Its user event where the control map binds it (the Map Menu's
    // Zoom In / Zoom Out: the zoom input + 1 x 0.1), the press and the release to the menu's input handler,
    // and, from the press only, a GFx mouse-wheel event of delta +1 or -1 (the game
    //).
    const float wheel = m_wheel;
    m_wheel = 0;
    Menu* m = top_input_menu();
    if (wheel == 0 || !m || !captures_input()) {
        return;
    }
    ++m_script_depth;
    if (const std::string event = mouse_user_event(wheel > 0 ? 8 : 9); !event.empty()) {
        m->input_event(event, 1.0f);
        m->on_user_event(event);
        m->input_event(event, 0.0f);
    }
    m->player()->mouse_wheel(wheel > 0 ? 1.0f : -1.0f);
    --m_script_depth;
    process_messages();
}

void Ui::poll_thumbsticks() {
    // The game: each stick through the radial dead zone (the game
    // s / |s| x clamp((|s| - fLThumbDeadzone) / (fLThumbDeadzoneMax - fLThumbDeadzone), 0, 1), the
    // [Controls] defaults 0.25 and 0.97 for both sticks), then a thumbstick event (11 left, 12 right) with this poll's
    // value whenever it or the last poll's is non-zero: every frame while a stick is held, once more on release.
    // The Mist Menu (ProcessThumbstick) takes both sticks; nothing else here reads them yet.
    constexpr float k_dead_zone = 0.25f, k_dead_zone_max = 0.97f;
    Menu* mist = find("Mist Menu");
    for (int s = 0; s < 2; ++s) {
        const float x = m_stick[s][0], y = m_stick[s][1];
        const float len = std::sqrt(x * x + y * y);
        const float k =
            len <= 0.0f ? 0.0f : std::clamp((len - k_dead_zone) / (k_dead_zone_max - k_dead_zone), 0.0f, 1.0f) / len;
        const float px = m_stick_polled[s][0], py = m_stick_polled[s][1];
        m_stick_polled[s][0] = k * x;
        m_stick_polled[s][1] = k * y;
        const bool send = px != 0.0f || py != 0.0f || m_stick_polled[s][0] != 0.0f || m_stick_polled[s][1] != 0.0f;
        if (send && mist && mist_can_process(*mist)) {
            mist_thumbstick(*mist, m_stick_polled[s][0], m_stick_polled[s][1]);
        }
    }
    // with a menu up the left stick steps as the arrow keys do, at once, then again after
    // fMenuKeyRepeatLong, the wait closing on fMenuKeyRepeatShort as it's held.
    if (mist || !captures_input()) {
        m_direction = 0;
        return;
    }
    const float dead = m_host.ui_game_setting("fDirectionalDeadzone:Controls", 0.5f);
    float x = m_stick[0][0], y = m_stick[0][1];
    x = std::abs(x) < dead ? 0.0f : x;
    y = std::abs(y) < dead ? 0.0f : y;
    uint32_t direction = 0;
    if (const float len = std::sqrt(x * x + y * y); len > 1e-6f) {
        direction = std::abs(x / len) >= 0.70710677f ? (x < 0.0f ? 37u : 39u) : (y < 0.0f ? 40u : 38u);
    }
    if (direction != m_direction) {
        m_direction = direction;
        m_direction_steps = 0;
        m_direction_next = m_ui_clock;
    }
    if (direction && m_ui_clock >= m_direction_next) {
        constexpr int k_steps_to_short = 5; // of the handler: not read, inferred
        const double t = std::min(double(m_direction_steps) / k_steps_to_short, 1.0);
        const double wait = 0.5 + (0.1 - 0.5) * t;
        m_direction_next = m_ui_clock + wait;
        ++m_direction_steps;
        deliver_key(direction, true);
        deliver_key(direction, false);
    }
}

void Ui::update(double dt) {
    m_ui_clock += dt;
    m_frame_scene_count = 0;
    for (MessageBoxData& m : std::exchange(m_auto_messages, {})) {
        if (m.callback) {
            m.callback(std::min(m_auto_message, int(m.buttons.size()) - 1));
        }
    }
    process_messages();
    m_backdrop_seconds = backdrop() ? m_backdrop_seconds + dt : 0.0;
    if (!visible()) {
        return;
    }
    const auto start = Clock::now();
    m_time_ms += dt * 1000.0;
    if (m_repeat_dik && m_time_ms >= m_repeat_next) {
        m_repeat_next = m_time_ms + k_repeat_rate_ms;
        keyboard(m_repeat_dik, true);
    }
    poll_wheel();
    poll_thumbsticks();
    const float w = float(target_width()), h = float(target_height());
    ++m_script_depth;
    // The HUD keeps the UI open during play: this runs every frame, so its lists reuse their storage.
    by_depth(m_frame_menus);
    for (Menu* m : m_frame_menus) {
        m->layout(w, h);
        m->advance(dt, m_time_ms);
    }
    for (const auto& d : m_debug_movies) {
        d->layout(w, h);
        d->advance(dt, m_time_ms);
    }
    --m_script_depth;
    process_messages();
    // Drawing starts at the topmost open menu with flag 0x200 and goes up by depth.
    by_depth(m_frame_menus);
    const std::vector<Menu*>& menus = m_frame_menus;
    size_t first = 0;
    for (size_t i = menus.size(); i-- > 0;) {
        if (menus[i]->desc().flags & menu_top_most_renderer) {
            first = i;
            break;
        }
    }
    std::vector<MovieLayer>& layers = m_frame_layers;
    layers.clear();
    m_frame_pages.clear();
    for (size_t i = first; i < menus.size(); ++i) {
        add_menu_layers(*menus[i], w, h, layers); // its movie, with its UI 3D scene before or after it
    }
    for (const auto& d : m_debug_movies) {
        d->player()->render(d->draw_list());
        layers.push_back({&d->draw_list(), d->stage_to_pixels(w, h)});
    }
    add_page_layers(layers);
    m_movie_renderer->set_layers(layers); // swaps: the old list comes back for the next frame
    record_stats(float(std::chrono::duration<double, std::milli>(Clock::now() - start).count()));
}

render::Overlay* Ui::overlay() { return visible() ? m_movie_renderer.get() : nullptr; }

std::optional<LocalMapView> Ui::local_map_view() const {
    for (const auto& m : m_menus) {
        if (const LocalMapView* v = m->local_map_view()) {
            return *v;
        }
    }
    return std::nullopt;
}

void Ui::set_image_pixels(const std::string& name, const std::string& key, uint32_t width, uint32_t height,
                          const std::vector<uint8_t>& rgba) {
    m_movie_renderer->set_image_pixels(name, key, width, height, rgba);
}

void Ui::set_render_target_image(const std::string& name, ::VkImageView_T* view) {
    m_movie_renderer->set_render_target_image(name, view);
}

void Ui::open_movie(const std::string& name, const std::string& scale_mode) {
    MenuDesc d;
    d.name = "movie:" + name;
    d.movie = name.ends_with(".swf") ? name.substr(0, name.size() - 4) : name;
    d.scale = scale_mode == "noScale"    ? ScaleMode::no_scale
              : scale_mode == "exactFit" ? ScaleMode::exact_fit
              : scale_mode == "noBorder" ? ScaleMode::no_border
                                         : ScaleMode::show_all;
    d.depth = 14;
    auto m = std::make_unique<DebugMenu>(*this, d);
    m->load(*m_library);
    m_movie_renderer->prepare(*m->player()->movie);
    m_debug_movies.push_back(std::move(m));
}

void Ui::close_movies() { m_debug_movies.clear(); }

void Ui::record_stats(float cpu_ms) {
    m_last_cpu_ms = cpu_ms;
    m_cpu_recent[m_recent_at] = cpu_ms;
    m_gpu_recent[m_recent_at] = m_movie_renderer->stats().gpu_ms;
    m_recent_at = (m_recent_at + 1) % k_stat_window;
    ++m_updates;
}

Ui::Stats Ui::stats() const {
    Stats s;
    s.cpu_ms = m_last_cpu_ms;
    s.gpu_ms = m_movie_renderer->stats().gpu_ms;
    const size_t n = std::min<uint64_t>(m_updates, k_stat_window);
    for (size_t i = 0; i < n; ++i) {
        s.cpu_ms_avg += m_cpu_recent[i];
        s.gpu_ms_avg += m_gpu_recent[i];
    }
    if (n > 0) {
        s.cpu_ms_avg /= float(n);
        s.gpu_ms_avg /= float(n);
    }
    s.draws = m_movie_renderer->stats().draws;
    s.layers = m_movie_renderer->stats().layers;
    s.menus = uint32_t(m_menus.size());
    s.updates = m_updates;
    return s;
}

std::string Ui::dump_json(const std::string& menu) const {
    // "<menu>:<movie>" names a menu's second movie (the Book Menu's "Book"); "movie:NAME" a debug movie.
    const size_t colon = menu.starts_with("movie:") ? std::string::npos : menu.find(':');
    Menu* m = find(menu.substr(0, colon));
    player::Player* movie =
        m ? m->movie_named(colon == std::string::npos ? std::string_view() : std::string_view(menu).substr(colon + 1))
          : nullptr;
    if (!movie) {
        throw Error("ui dump: '" + menu + "' is not open or has no movie");
    }
    std::string out = "[";
    bool first = true;
    std::function<void(player::DisplayObject*)> walk = [&](player::DisplayObject* d) {
        const char* kind = d->kind == player::DisplayKind::clip    ? "clip"
                           : d->kind == player::DisplayKind::shape ? "shape"
                           : d->kind == player::DisplayKind::text  ? "text"
                                                                   : "button";
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        const bool has_bounds = d->bounds_in(nullptr, x0, y0, x1, y1);
        // x and y as the movie's _x and _y read them, so the dump compares with the game's dump of the game: a text
        // field's are the top-left of its box, not its registration point.
        float px = d->matrix.tx, py = d->matrix.ty;
        if (d->kind == player::DisplayKind::text) {
            const auto* t = static_cast<const player::TextFieldObject*>(d);
            d->matrix.apply(t->origin_x, t->origin_y, px, py);
        }
        std::string e = std::format(
            "{{\"path\": {}, \"kind\": \"{}\", \"depth\": {}, \"visible\": {}, \"x\": {:.2f}, \"y\": {:.2f}, "
            "\"xscale\": {:.2f}, \"yscale\": {:.2f}, \"rotation\": {:.2f}, \"alpha\": {:.2f}",
            json_string(d->target_path()), kind, d->depth, d->visible ? "true" : "false", px, py,
            d->scale_x, d->scale_y, d->rotation, d->cxform.mul[3] * 100.0f);
        if (has_bounds) {
            e += std::format(", \"bounds\": [{:.1f}, {:.1f}, {:.1f}, {:.1f}]", x0, y0, x1, y1);
        }
        // What else the drawing depends on, where it isn't the default: the colour transform (multipliers, then
        // offsets 0..1), the filters, the blend mode. ActionScript can't read these on a text field.
        const player::ColorXform& cx = d->cxform;
        if (cx.mul[0] != 1 || cx.mul[1] != 1 || cx.mul[2] != 1 || cx.mul[3] != 1 || cx.add[0] != 0 || cx.add[1] != 0 ||
            cx.add[2] != 0 || cx.add[3] != 0) {
            e += std::format(", \"cxform\": [{:.4g}, {:.4g}, {:.4g}, {:.4g}, {:.4g}, {:.4g}, {:.4g}, {:.4g}]", cx.mul[0],
                             cx.mul[1], cx.mul[2], cx.mul[3], cx.add[0], cx.add[1], cx.add[2], cx.add[3]);
        }
        if (!d->filters.empty()) {
            e += std::format(", \"filters\": {}", d->filters.size());
        }
        if (d->blend != osu::Blend::normal) {
            e += std::format(", \"blend\": {}", int(d->blend));
        }
        if (d->kind == player::DisplayKind::clip) {
            auto* c = static_cast<player::MovieClip*>(d);
            e += std::format(", \"frame\": {}, \"frames\": {}", c->current_frame + 1, c->frame_count());
        } else if (d->kind == player::DisplayKind::text) {
            auto* t = static_cast<player::TextFieldObject*>(d);
            e += ", \"text\": " + json_string(player::u32_to_utf8(t->plain_text()));
        }
        out += (first ? "\n" : ",\n") + e + "}";
        first = false;
        if (d->kind == player::DisplayKind::clip) {
            // As the game's dump walks GFx's display list: its entry for a clip calls the
            // clip's getDepth, a member lookup that searches the clip's display list and so names its unnamed
            // children; then it asks each child for its getDepth too, naming the child's children, before it goes
            // into any of them.
            auto& children = static_cast<player::MovieClip*>(d)->children;
            for (auto& [depth, c] : children) {
                c->resolved_name();
            }
            for (auto& [depth, c] : children) {
                if (c->kind == player::DisplayKind::clip) {
                    for (auto& [inner_depth, g] : static_cast<player::MovieClip*>(c.get())->children) {
                        g->resolved_name();
                    }
                }
            }
            for (auto& [depth, c] : children) {
                walk(c.get());
            }
        }
    };
    walk(movie->root.get());
    return out + "\n]\n";
}

std::vector<std::string> Ui::open_menus() const {
    std::vector<std::string> names;
    for (Menu* m : by_depth()) {
        names.push_back(m->name());
    }
    return names;
}

std::string Ui::state_json() const {
    std::string open;
    for (Menu* m : by_depth()) {
        open += (open.empty() ? "" : ", ") + json_string(m->name());
    }
    std::string j = "{";
    j += std::format("\"open\": [{}], \"captures_input\": {}, \"in_world\": {}", open,
                     captures_input() ? "true" : "false", m_in_world ? "true" : "false");
    if (Menu* t = top_input_menu()) {
        j += ", \"top\": " + json_string(t->name());
        std::vector<player::TextFieldObject*> fields;
        collect_texts(t->player()->root.get(), fields);
        std::string texts;
        for (auto* f : fields) {
            if (!shown(f)) {
                continue;
            }
            const std::string s = player::u32_to_utf8(f->plain_text());
            if (!s.empty() && s.find_first_not_of(" \r\n") != std::string::npos) {
                texts += (texts.empty() ? "" : ", ") + json_string(s);
            }
        }
        j += ", \"texts\": [" + texts + "]";
        const auto errors = t->player()->errors();
        j += std::format(", \"script_errors\": {}", errors.size());
        const player::DisplayObject* hover = t->player()->hover();
        j += ", \"hover\": " + (hover ? json_string(hover->target_path()) : std::string("null"));
        const player::DisplayObject* focus = t->player()->focus();
        j += ", \"focus\": " + (focus ? json_string(focus->target_path()) : std::string("null"));
    } else {
        j += ", \"top\": null, \"texts\": []";
    }
    j += std::format(", \"console\": {{\"open\": {}}}", is_open("Console") ? "true" : "false");
    const Stats st = stats();
    j += std::format(", \"stats\": {{\"cpu_ms\": {:.4f}, \"cpu_ms_avg\": {:.4f}, \"gpu_ms\": {:.4f}, "
                     "\"gpu_ms_avg\": {:.4f}, \"draws\": {}, \"layers\": {}, \"menus\": {}, \"updates\": {}}}",
                     st.cpu_ms, st.cpu_ms_avg, st.gpu_ms, st.gpu_ms_avg, st.draws, st.layers, st.menus, st.updates);
    return j + "}";
}

} // namespace spud::ui
