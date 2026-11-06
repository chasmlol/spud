#include "ui/menu.hpp"

#include "ui/player/json.hpp"

#include "ui/ui.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/platform.hpp"

#include <utility>
#include <algorithm>
#include <cmath>
#include <format>

namespace spud::ui {

namespace {

// SPUD_UI_TRACE=1 logs every delegate call between the movies and the engine (=text: script text, see
// builtins_display.cpp).
bool trace() {
    static const bool on = platform::env("SPUD_UI_TRACE").value_or("") == "1";
    return on;
}

std::string args_text(player::Vm& vm, const std::vector<player::Value>& args) {
    std::string s;
    for (const auto& a : args) {
        s += (s.empty() ? "" : ", ") + *vm.to_string(a);
    }
    return s;
}

} // namespace

Menu::Menu(Ui& ui, MenuDesc desc) : m_ui(ui), m_desc(std::move(desc)) {
    if (m_desc.context != 19) {
        m_contexts.push_back(m_desc.context);
    }
}

Menu::~Menu() = default;

void Menu::load(player::Library& library) {
    if (m_desc.movie.empty()) {
        return; // the Mist Menu draws a 3D scene, not a movie
    }
    auto def = library.movie(m_desc.movie + ".swf");
    if (!def) {
        throw Error(std::format("ui: {}: no movie {} in the interface cache", m_desc.name, m_desc.movie));
    }
    // The loader's order: an instance with its first frame
    // initialised; scale mode, safe rect and viewport; the delegate's callbacks and SetButtonMapping; Advance(0, 2);
    // InitExtensions; SetPlatform.
    m_player = std::make_unique<player::Player>(library, def, this);
    // The first frame runs before the engine sets the scale mode, under the view's default (showAll), so a movie
    // that reads Stage.height while it initialises sees its own height.
    m_player->stage.scale_mode = "showAll";
    m_player->stage.viewport[0] = float(m_ui.target_width());
    m_player->stage.viewport[1] = float(m_ui.target_height());
    m_player->start();
    static constexpr const char* k_modes[] = {"noScale", "showAll", "exactFit", "noBorder"};
    m_player->stage.scale_mode = k_modes[int(m_desc.scale)];
    // Background alpha: the movie's own BackgroundAlpha root variable, clamped, else the loader's argument.
    m_background_alpha = m_desc.background_alpha;
    const Value bg = m_player->get_path("BackgroundAlpha");
    if (!bg.is_undefined()) {
        m_background_alpha = float(std::clamp(m_player->vm.to_number(bg), 0.0, 1.0));
    }
    layout(float(m_ui.target_width()), float(m_ui.target_height()));
    register_handlers();
    send("SetButtonMapping", {Value::object(m_ui.button_mapping(*m_player))});
    m_player->advance(0);
    if (has_path("InitExtensions")) {
        invoke("InitExtensions");
    }
    if (has_path("SetPlatform")) {
        invoke("SetPlatform", {Value::number(m_ui.gamepad_active() ? 1 : 0), Value::boolean(false)});
    }
    on_loaded();
}

void Menu::advance(double dt, double engine_ms) {
    if (!m_player) {
        return;
    }
    // IMenu's default advance: root CurrentTime = engine milliseconds, then Advance(dt, 2).
    m_player->set_path("CurrentTime", Value::number(std::floor(engine_ms)));
    m_player->advance(dt);
    if (m_next_advance) {
        std::exchange(m_next_advance, {})();
    }
}

void Menu::send(const std::string& callback, std::vector<Value> args) {
    if (!m_player) {
        return;
    }
    // the movie's "call" callback with the callback name first.
    if (trace()) {
        log::info("ui {} <- {}({})", m_desc.name, callback, args_text(m_player->vm, args));
    }
    // hands it to the external callback "call" or else to the path "call"; a movie without GameDelegate
    // (SafeZone, Titles, LoadWaitSpinner) has neither, and the call does nothing.
    if (!m_player->has_external_callback("call") && !m_player->has_path("call")) {
        return;
    }
    args.insert(args.begin(), Value::string(callback));
    m_player->invoke_or_callback("call", args);
}

void Menu::respond(double response_id, std::vector<Value> values) {
    if (!m_player) {
        return;
    }
    values.insert(values.begin(), Value::number(response_id));
    m_player->invoke_or_callback("respond", values);
}

Menu::Value Menu::invoke(std::string_view path, std::vector<Value> args) {
    return m_player ? m_player->invoke(path, args) : Value{};
}

bool Menu::has_path(std::string_view path) { return m_player && m_player->has_path(path); }

std::optional<Menu::Value> Menu::captured(const std::string& name, std::string_view path) {
    const player::json::Value* c = m_ui.capture(name);
    if (!c || !m_player) {
        return std::nullopt;
    }
    for (const auto& [key, root] : c->members) {
        if (key == "paths") {
            for (const auto& [p, v] : root.members) {
                if (p == path) {
                    return player::json_to_value(m_player->vm, v);
                }
            }
        }
    }
    return std::nullopt;
}

bool Menu::apply_capture(const std::string& name) {
    const player::json::Value* c = m_ui.capture(name);
    if (!c || !m_player) {
        return false;
    }
    for (const auto& [key, root] : c->members) {
        if (key != "paths") {
            continue;
        }
        for (const auto& [path, v] : root.members) {
            m_player->set_path(path, player::json_to_value(m_player->vm, v));
        }
    }
    return true;
}

Menu::Value Menu::external_call(player::Player& p, const std::string& method, const std::vector<Value>& args) {
    // GameDelegate.call(name, params): ExternalInterface.call(name, responseId, params...).
    if (trace()) {
        log::info("ui {} -> {}({})", m_desc.name, method, args_text(p.vm, args));
    }
    auto it = m_handlers.find(method);
    if (it == m_handlers.end()) {
        log::info("ui {}: unhandled delegate call {}", m_desc.name, method);
        return {};
    }
    const double id = args.empty() ? 0 : p.vm.to_number(args[0]);
    const std::vector<Value> params(args.empty() ? args.end() : args.begin() + 1, args.end());
    Call c{*this, id, params};
    ++m_ui.m_script_depth; // menus open and close only once the script has finished
    try {
        it->second(c);
    } catch (const std::exception& e) {
        // Engine errors must not unwind through the movie's script.
        log::error("ui {}: {}: {}", m_desc.name, method, e.what());
        m_ui.report_error(e.what());
    }
    --m_ui.m_script_depth;
    return {};
}

uint32_t Menu::background() const {
    if (!m_player || m_background_alpha <= 0) {
        return 0;
    }
    const uint32_t a = uint32_t(std::lround(m_background_alpha * 255.0f));
    return (m_player->movie->data().header.background & 0xFFFFFFu) | (a << 24);
}

player::Matrix2D Menu::stage_to_pixels(float w, float h) const {
    if (!m_player) {
        return {};
    }
    const auto& hd = m_player->movie->data().header;
    const float sw = hd.stage_width, sh = hd.stage_height;
    // The engine sets the scale mode and no alignment; a movie may set both through Stage (Console: noScale, BL).
    const std::string& mode = m_player->stage.scale_mode;
    float sx = 1, sy = 1;
    if (mode == "exactFit") {
        sx = w / sw;
        sy = h / sh;
    } else if (mode == "showAll") {
        sx = sy = std::min(w / sw, h / sh);
    } else if (mode == "noBorder") {
        sx = sy = std::max(w / sw, h / sh);
    }
    const std::string& a = m_player->stage.align;
    const bool left = a.find('L') != std::string::npos, right = a.find('R') != std::string::npos;
    const bool top = a.find('T') != std::string::npos, bottom = a.find('B') != std::string::npos;
    const float free_x = w - sw * sx, free_y = h - sh * sy;
    return {sx, 0, 0, sy, left ? 0 : right ? free_x : free_x * 0.5f, top ? 0 : bottom ? free_y : free_y * 0.5f};
}

void Menu::layout(float w, float h) {
    if (!m_player || w <= 0 || h <= 0) {
        return;
    }
    // Stage.visibleRect: the target in stage coordinates. The safe rect is inset by fSafeZoneX/Y (4:3 or narrower)
    // or fSafeZoneXWide/YWide, all 15 (section 3, step 6).
    const player::Matrix2D inv = stage_to_pixels(w, h).inverse();
    float x0, y0, x1, y1;
    inv.apply(0, 0, x0, y0);
    inv.apply(w, h, x1, y1);
    auto& s = m_player->stage;
    s.visible[0] = x0;
    s.visible[1] = y0;
    s.visible[2] = x1;
    s.visible[3] = y1;
    constexpr float k_safe = 15.0f;
    s.safe[0] = x0 + k_safe;
    s.safe[1] = y0 + k_safe;
    s.safe[2] = x1 - k_safe;
    s.safe[3] = y1 - k_safe;
    const bool resized = s.viewport[0] != w || s.viewport[1] != h;
    s.viewport[0] = w;
    s.viewport[1] = h;
    // Stage listeners hear about a resize under noScale (Flash 8 Stage.onResize).
    if (resized && m_sized && s.scale_mode == "noScale") {
        m_player->broadcast("Stage", "onResize", {});
    }
    m_sized = true;
}

} // namespace spud::ui
