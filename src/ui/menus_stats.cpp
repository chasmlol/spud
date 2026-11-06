// Stats (skills and perks), Level Up, Book and Dialogue menus: the engine side of each movie's contract, from
// (addresses there).
//
// Their data comes from game systems spud does not have yet: the player's skills, perks and attributes, books,
// dialogue. Each menu asks a small source for it. Today every source reads what the game recorded the engine
// sending the movie in the real game (`record start/stop`, installed as a capture): the values of each delegate
// callback, Invoke and SetVariable, in the game's order. The menus decide when to send what, as the game
// does; a source only supplies the values. With no capture a menu opens with no data and says so once.

#include "ui/book3d.hpp"
#include "ui/character.hpp"
#include "ui/items.hpp"
#include "ui/menus_internal.hpp"
#include "ui/starfield.hpp"

#include "ui/player/json.hpp"
#include "ui/ui.hpp"

#include "core/log.hpp"
#include "core/platform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace spud::ui {

namespace {

using Value = player::Value;
namespace json = player::json;

// ---- what the engine sent a movie in the game -------------------------------------------------------------------

// A recording from the game: {"calls": [{"frame", "menu", "kind", "name", "args"}]}, kind "callback" (an
// The game callback), "invoke" or "set" (SetVariable). `menu` is the menu whose movie it
// was ("Book Menu:Book" for the Book Menu's second movie).
class Recording {
public:
    struct Call {
        int frame = 0;
        std::string menu, kind, name;
        const json::Value* args = nullptr; // an array
    };

    Recording(Ui& ui, const std::string& capture) : m_capture(capture) {
        const json::Value* c = ui.capture(capture);
        if (!c) {
            return;
        }
        for (const auto& [key, v] : c->members) {
            if (key != "calls" || v.kind != json::Value::array) {
                continue;
            }
            for (const json::Value& e : v.items) {
                Call call;
                for (const auto& [k, m] : e.members) {
                    if (k == "frame") {
                        call.frame = int(m.n);
                    } else if (k == "menu") {
                        call.menu = m.s;
                    } else if (k == "kind") {
                        call.kind = m.s;
                    } else if (k == "name") {
                        call.name = m.s;
                    } else if (k == "args") {
                        call.args = &m;
                    }
                }
                m_calls.push_back(std::move(call));
            }
        }
    }

    bool empty() const { return m_calls.empty(); }
    const std::string& name() const { return m_capture; }
    const std::vector<Call>& calls() const { return m_calls; }

    // Every call named `name` of `kind` on `menu`'s movie, in order.
    std::vector<const Call*> all(std::string_view kind, std::string_view name, std::string_view menu) const {
        std::vector<const Call*> out;
        for (const Call& c : m_calls) {
            if (c.kind == kind && c.name == name && c.menu == menu) {
                out.push_back(&c);
            }
        }
        return out;
    }
    const Call* first(std::string_view kind, std::string_view name, std::string_view menu) const {
        for (const Call& c : m_calls) {
            if (c.kind == kind && c.name == name && c.menu == menu) {
                return &c;
            }
        }
        return nullptr;
    }

private:
    std::string m_capture;
    std::vector<Call> m_calls;
};

// The arguments of a recorded call as values of `vm`.
std::vector<Value> to_values(player::Vm& vm, const json::Value* args) {
    std::vector<Value> out;
    if (args) {
        for (const json::Value& a : args->items) {
            out.push_back(player::json_to_value(vm, a));
        }
    }
    return out;
}

const json::Value* arg(const Recording::Call* c, size_t i) {
    return c && c->args && i < c->args->items.size() ? &c->args->items[i] : nullptr;
}

// One variable of a `menu capture` capture ({"paths": {path: value}}), or null.
const json::Value* captured_path(Ui& ui, const std::string& capture, std::string_view path) {
    const json::Value* c = ui.capture(capture);
    if (!c) {
        return nullptr;
    }
    for (const auto& [key, root] : c->members) {
        if (key == "paths") {
            for (const auto& [p, v] : root.members) {
                if (p == path && v.kind != json::Value::null) {
                    return &v;
                }
            }
        }
    }
    return nullptr;
}

// A menu with no capture opens with no data; it says so once per capture.
void note_missing(const std::string& capture, const std::string& menu) {
    static std::set<std::string> said;
    if (said.insert(capture).second) {
        log::info("ui {}: no capture '{}'; the menu "
                  "opens with no data",
                  menu, capture);
    }
}

// Menu sounds through the shared PlaySound handler: one string argument.
void handle_play_sound(Menu& m, Menu::Call& c) {
    if (c.args.size() == 1 && m.player()) {
        m.ui().host().ui_play_sound(*m.player()->vm.to_string(c.arg(0)));
    }
}

uint32_t cursor_flag(Ui& ui) { return ui.gamepad_active() ? 0u : uint32_t(menu_uses_cursor); }

// The game on a movie that is not the menu's own (the Book Menu's Book movie): the movie's "call"
// callback with the callback name first, as Menu::send does.
void send_to(player::Player& p, const std::string& callback, std::vector<Value> args) {
    args.insert(args.begin(), Value::string(callback));
    p.invoke_or_callback("call", args);
}

// printf's "%s" in a GMST, as the engine formats it.
std::string format_s(const std::string& pattern, const std::string& value) {
    std::string s = pattern;
    if (const size_t at = s.find("%s"); at != std::string::npos) {
        s.replace(at, 2, value);
    }
    return s;
}

// ---- Stats Menu (movie StatsMenu) ---------------------------------------------------------------------------------

// The skill under a camera direction: sectors of 360 / count degrees centred
// on each skill, halves rounded up.
int skill_at(double degrees, int count) {
    const double sector = 360.0 / count;
    return ((int(std::floor(degrees / sector + 0.5)) % count) + count) % count;
}

// The Stats Menu's data: the player's skills (the 90 values of StatsMenu.SkillStatsA), name, level, race and
// attributes (SetPlayerInfo), the perk count (SetStatsMode), and the description card of each skill and perk.
// Recorded: the capture "stats_calls" (the menu opened, one skill to the right, a held Left, a perk tree).
class StatsSource {
public:
    explicit StatsSource(Ui& ui)
        : m_rec(ui, capture_name()), m_skill_stats(captured_path(ui, "stats", "StatsMenu.SkillStatsA")) {
        index();
    }
    // Beast mode: the recording of the werewolf's or the vampire lord's menu (capture-stats-beast.osc), N = 2.
    StatsSource(Ui& ui, const std::string& calls, const char* stats, int count)
        : m_rec(ui, calls), m_skill_stats(captured_path(ui, stats, "StatsMenu.SkillStatsA")), m_count(count) {
        index();
    }

private:
    void index() {
        // A skill's card carries only its description and level; which skill it was is the selection the camera's
        // direction gave in that frame (the next SetDirection; the card comes first in its frame).
        const int count = skill_count();
        const auto& calls = m_rec.calls();
        bool perks = false;
        int skill = -1;
        for (size_t i = 0; i < calls.size(); ++i) {
            const Recording::Call& c = calls[i];
            if (c.menu != "StatsMenu") {
                continue;
            }
            if (c.kind == "callback" && c.name == "SetDirection" && arg(&c, 0)) {
                skill = skill_at(arg(&c, 0)->n, count);
            }
            if (c.kind == "callback" && c.name == "HideRing") {
                perks = true;
            }
            if (c.kind == "invoke" && c.name == "StatsMenu.StatsMenuInstance.UpdatePerkText") {
                perks = false;
            }
            if (c.kind == "set_array" && c.name == "StatsMenu.PerkNamesA" && perks && skill >= 0) {
                m_perk_names[skill] = &c; // the last, the tree at rest
            }
            if (c.kind != "callback" || c.name != "SetDescriptionCard") {
                continue;
            }
            const json::Value* mode = arg(&c, 0);
            const json::Value* text = arg(&c, 3);
            if (!mode || !text || text->s.empty()) {
                continue; // the empty card the menu shows before its scene is up
            }
            if (mode->b) {
                if (skill >= 0) {
                    m_perk_cards.emplace(skill, &c);
                }
                continue;
            }
            int s = -1;
            for (size_t j = i + 1; j < calls.size() && calls[j].frame == c.frame; ++j) {
                if (calls[j].kind == "callback" && calls[j].name == "SetDirection" && arg(&calls[j], 0)) {
                    s = skill_at(arg(&calls[j], 0)->n, count);
                    break;
                }
            }
            m_skill_cards.emplace(s >= 0 ? s : skill, &c);
        }
    }

public:
    bool empty() const { return m_rec.empty(); }
    // "stats_calls", or the capture SPUD_UI_STATS names ("levelup_calls": a player with a level to take).
    static std::string capture_name() {
        const std::string chosen = platform::env("SPUD_UI_STATS").value_or("");
        return chosen.empty() ? std::string("stats_calls") : chosen;
    }
    // The player's progress towards the next level in percent (SetPlayerInfo's third value): 100 or more is a
    // level to take (the skills object's check compares the same experience and threshold).
    bool level_up_pending() const {
        const json::Value* p = arg(first("SetPlayerInfo"), 2);
        return p && p->n >= 100.0;
    }
    // The player info once the level is taken: the first recorded SetPlayerInfo with another level.
    const Recording::Call* player_info_after_level() const {
        const json::Value* level = arg(first("SetPlayerInfo"), 1);
        for (const Recording::Call* c : m_rec.all("callback", "SetPlayerInfo", "StatsMenu")) {
            if (level && arg(c, 1) && arg(c, 1)->n != level->n) {
                return c;
            }
        }
        return nullptr;
    }

    // The skills, 5 values each: level, name, percent, colour, legendary count, as the movie held them (the capture
    // "stats"; the engine fills the array element by element).
    const json::Value* skill_stats() const {
        if (const Recording::Call* c = m_rec.first("set_array", "StatsMenu.SkillStatsA", "StatsMenu")) {
            if (m_skill_array.items.empty() && c->args && c->args->items.size() > 1) {
                // SetVariableArray(index 0, the 90 values): the values after the index.
                m_skill_array.kind = json::Value::array;
                m_skill_array.items.assign(c->args->items.begin() + 1, c->args->items.end());
            }
            return &m_skill_array;
        }
        return m_skill_stats;
    }
    int skill_count() const {
        if (m_count > 0) {
            return m_count;
        }
        const json::Value* s = skill_stats();
        return s && s->items.size() >= 5 ? int(s->items.size() / 5) : 18;
    }
    const Recording::Call* first(std::string_view name) const { return m_rec.first("callback", name, "StatsMenu"); }
    const Recording::Call* skill_card(int skill) const { return find(m_skill_cards, skill); }
    const Recording::Call* perk_card(int skill) const { return find(m_perk_cards, skill); }
    const Recording::Call* perk_names(int skill) const { return find(m_perk_names, skill); }

private:
    static const Recording::Call* find(const std::map<int, const Recording::Call*>& m, int skill) {
        auto it = m.find(skill);
        return it == m.end() ? nullptr : it->second;
    }
    Recording m_rec;
    const json::Value* m_skill_stats;
    int m_count = 0; // the skill count when given (beast mode: 2)
    mutable json::Value m_skill_array;
    std::map<int, const Recording::Call*> m_skill_cards, m_perk_cards, m_perk_names;
};

// The skills' perk trees (the actor values' the game trees, ActorValueInfo), which the
// starfield places its stars and lines from: until spud has the actor-value system, captured from the game
std::vector<PerkTree> captured_perk_trees(Ui& ui, const char* capture = "stats-perktrees") {
    std::vector<PerkTree> out;
    const json::Value* skills = captured_path(ui, capture, "skills");
    if (!skills || skills->kind != json::Value::array) {
        return out;
    }
    auto num = [](const json::Value& o, std::string_view k) {
        for (const auto& [name, v] : o.members) {
            if (name == k && v.kind == json::Value::number) {
                return v.n;
            }
        }
        return 0.0;
    };
    auto find = [](const json::Value& o, std::string_view k) -> const json::Value* {
        for (const auto& [name, v] : o.members) {
            if (name == k) {
                return &v;
            }
        }
        return nullptr;
    };
    for (const json::Value& sk : skills->items) {
        PerkTree t;
        t.actor_value = uint32_t(num(sk, "av"));
        t.max_grid_x = int(num(sk, "max_grid_x"));
        if (const json::Value* nodes = find(sk, "nodes")) {
            for (const json::Value& n : nodes->items) {
                PerkTree::Node nd;
                nd.index = uint32_t(num(n, "index"));
                const json::Value* perk = find(n, "perk");
                nd.perk = perk && perk->kind == json::Value::string ? uint32_t(std::stoul(perk->s, nullptr, 16)) : 0;
                nd.x = float(num(n, "x"));
                nd.y = float(num(n, "y"));
                nd.h = float(num(n, "h"));
                nd.v = float(num(n, "v"));
                nd.state = int(num(n, "state"));
                if (const json::Value* c = find(n, "children")) {
                    for (const json::Value& k : c->items) {
                        nd.children.push_back(uint32_t(k.n));
                    }
                }
                t.nodes.push_back(std::move(nd));
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

class StatsMenu final : public Menu {
public:
    // StatsMenu::StatsMenu: ExactFit, depth 3, flags 0x8189 ( without a gamepad), context 8.
    explicit StatsMenu(Ui& ui)
        : Menu(ui, {"StatsMenu", "StatsMenu", ScaleMode::exact_fit, 3, 0x8189u | cursor_flag(ui), 0, 8}),
          m_source(ui), m_starfield(ui), m_live(ui.host().ui_character()) {
        m_count = m_live ? 18 : m_source.skill_count();
        m_starfield.set_trees(m_live ? m_live->perk_trees(false) : captured_perk_trees(ui));
        // The skill it opens on is g_StatsMenuLastSkill: compiled 15, then the last one shown. A recording made
        // later in a game session opened on the one remembered then (its first SetDirection).
        if (!opened_before()) {
            const json::Value* first = m_live ? nullptr : arg(m_source.first("SetDirection"), 0);
            remembered_skill() = first ? skill_at(first->n, m_count) : 15;
        }
        opened_before() = true;
        m_selected = remembered_skill() % m_count;
        m_angle = 360.0 / m_count * m_selected;
        // fStatsRotationSpeedMax, fStatsRotationRampTime, fResetTransitSoundRotationSpeed: the game's values
        // (); no plugin of this install changes them (measured: settings.csv of
        // the game run).
        auto setting = [&ui](const char* name, double fallback) {
            const std::string s = ui.game_setting(name);
            return s.empty() ? fallback : std::strtod(s.c_str(), nullptr);
        };
        m_speed_max = setting("fStatsRotationSpeedMax", 0.5);
        m_ramp_ms = setting("fStatsRotationRampTime", 2000.0);
        m_reset_sound_speed = setting("fResetTransitSoundRotationSpeed", 0.3);
    }

    ~StatsMenu() override { remembered_skill() = m_selected; }

    void on_show() override {
        // Beast mode (g_StatsMenuBeastMode, which the werewolf's and the vampire lord's menus set; spud
        // has no transformation yet, so a show with "beast" or "beast:werewolf" stands for it): the vampire skydome
        // with the two beast trees (capture stats-perktrees-beast), N = 2, and the selected tree 0 when the player's
        // race is the Werewolf Race (DOBJ 345), else 1 (StatsMenu::StatsMenu); the ring does not spin.
        const std::string show = m_ui.show_data(name());
        if (show.starts_with("beast") || (m_live && m_live->beast_mode())) {
            m_beast = true;
            m_count = 2;
            m_selected = (m_live ? m_live->werewolf() : show == "beast:werewolf") ? 0 : 1;
            // The engine's traffic into the movie in beast mode, recorded in the game (the werewolf's with the
            // player's race the Werewolf Race, the vampire lord's otherwise): no skill list, the beast card.
            const bool werewolf = m_selected == 0;
            if (!m_live) {
                m_source = StatsSource(m_ui, werewolf ? "stats_werewolf_calls" : "stats_beast_calls",
                                       werewolf ? "stats_werewolf" : "stats_beast", m_count);
            }
            m_angle = 360.0 / m_count * m_selected;
            m_starfield.set_beast(true);
            m_starfield.set_trees(m_live ? m_live->perk_trees(true) : captured_perk_trees(m_ui, "stats-perktrees-beast"));
        }
        // Message 1.
        m_ui.host().ui_play_sound("UISkillsTransitionUp");
        if (!m_live && m_source.empty()) {
            note_missing("stats_calls", name());
        }
    }

    bool on_hide() override {
        // Message 3: FadeOut; the menu goes once the skydome's closing sequence has played (3D). Measured in the
        // game: closed 41 frames at 60 fps after the hide message.
        if (!m_closing) {
            m_closing = true;
            m_state = state_closing;
            m_close_frames = 41;
            invoke("gotoAndPlay", {Value::string("FadeOut")});
            m_starfield.close();
            // the black the Tween Menu faded to goes (the game(0, 1, s, 0, 0)).
            if (m_ui.is_open("Fader Menu")) {
                m_ui.request_fade({true, true, 0.5, 0.0});
            }
        }
        return m_close_frames <= 0;
    }

    void advance(double dt, double engine_ms) override {
        m_now = std::floor(engine_ms); // the engine's integer millisecond clock
        m_dt = dt;
        if (m_closing) {
            if (--m_close_frames <= 0) {
                Ui& ui = m_ui;
                ui.close_now(name());
                tween_menu_update(ui); // the update posts its message to the Tween Menu, which shows itself again
                return;
            }
        } else {
            step();
        }
        m_starfield.update(m_now, m_selected, m_angle);
        Menu::advance(dt, engine_ms);
    }

    // The game: slot 6 with the Stats camera, then the movie over it.
    bool ui_scene(UiScene& out) override { return m_starfield.scene(out); }
    bool scene_after_movie() const override { return false; }
    std::string starfield_json() const { return m_starfield.state_json(); }

    bool on_button(std::string_view event, bool down) override {
        // The game (CanProcess, ProcessButton): its events are handled in the next
        // update, after the frame's clock, as the game hands them over before the UI's update.
        if (event == "Left" || event == "Right" || event == "Up" || event == "Down") {
            m_events.push_back({std::string(event), down});
            return true;
        }
        // On the skills the mouse's button is a stick that depends on where the cursor is (see skills_click).
        if (event == "Click" && (m_state == state_skills || !down)) {
            m_click = down ? click_press : m_click != click_none ? click_release : click_none;
            return true;
        }
        if (m_live && down && (event == "Jump" || event == "YButton")) {
            ask_legendary();
            return true;
        }
        if (m_live && down && event == "Click" && m_state == state_perks) {
            const float w = float(std::max(m_ui.target_width(), 1)), h = float(std::max(m_ui.target_height(), 1));
            const int skill = m_selected;
            if (m_starfield.click_star(m_ui.mouse_x() / w, m_ui.mouse_y() / h, m_selected)) {
                ask_perk();
            } else if (m_selected != skill || m_starfield.selected_node()) {
                send_card();
            }
            return true;
        }
        return false;
    }

    bool input_event(std::string_view event, float value) override {
        // A wheel notch is the stick's y for a press (): up as Up into a tree, down as Down.
        if (event == "Zoom In" || event == "Zoom Out") {
            m_events.push_back({event == "Zoom In" ? "Up" : "Down", value > 0.0f});
            return true;
        }
        return event == "Click" && on_button(event, value > 0.0f);
    }

    // Answers from the message boxes.
    void take_selected_perk() {
        const PerkTree::Node* node = m_starfield.selected_node();
        if (!m_live || !node || !m_live->can_take_perk(node->perk)) {
            return;
        }
        m_live->take_perk(node->perk, m_beast);
        m_starfield.update_states(m_live->perk_trees(m_beast));
        m_ui.host().ui_play_sound("UISkillsPerkSelect2D");
        send_card();
        if (m_live->perk_points(m_beast) == 0) {
            m_level_taken = false;
        }
    }

    void reset_legendary() {
        m_ui.host().ui_play_sound("UILevelUp");
        m_live->reset_legendary(m_selected);
        m_starfield.update_states(m_live->perk_trees(false));
        send_card();
        send_skill_list();
    }

    bool on_key(uint32_t code, bool down) override {
        if (m_live && down && m_state != state_closing) {
            // ProcessMessage, message 6: 13 on a perk that can be taken, 32 on a skill at 100.
            if (code == 13 && m_state == state_perks) {
                ask_perk();
                return true;
            }
            if (code == 32) {
                ask_legendary();
                return true;
            }
        }
        // ProcessMessage, message 6: key 9 (the Cancel user event's key), or 40 without a gamepad on
        // the skills while the camera rests, closes the menu with UIMenuCancel (then the Tween Menu's update).
        if (down && m_state != state_closing &&
            (code == 9 || (code == 40 && !m_ui.gamepad_active() && m_state == state_skills && m_speed == 0.0))) {
            m_ui.host().ui_play_sound("UIMenuCancel");
            m_ui.close(name());
            return true;
        }
        return false;
    }

    bool on_user_event(const std::string& event) override {
        // Message 7: the Quick Stats user event (g_UserEvents) closes it the same way.
        if (event == "Quick Stats" && m_state != state_closing) {
            m_ui.host().ui_play_sound("UIMenuCancel");
            m_ui.close(name());
            return true;
        }
        return false;
    }

private:
    enum State { state_skills = 0, state_perks = 1, state_closing = 3 };
    struct Event {
        std::string name;
        bool down;
    };

    static int& remembered_skill() {
        static int skill = 15; // g_StatsMenuLastSkill: compiled 15, written when the menu goes
        return skill;
    }
    static bool& opened_before() {
        static bool opened = false;
        return opened;
    }

    void ask_perk() {
        // The confirm box needs a point and a perk that can be taken.
        const PerkTree::Node* node = m_starfield.selected_node();
        if (!node || m_live->perk_points(m_beast) <= 0 || !m_live->can_take_perk(node->perk)) {
            return;
        }
        Ui& ui = m_ui;
        ui.message(ui.game_setting("sStatsPerkConfirm"), {ui.game_setting("sOk"), ui.game_setting("sCancel")}, [&ui](int button) {
            if (Menu* m = ui.find_menu("StatsMenu"); m && button == 0) {
                static_cast<StatsMenu*>(m)->take_selected_perk();
            }
        });
    }

    void ask_legendary() {
        if (m_beast || m_state != state_skills || !m_live->legendary_ready(m_selected)) {
            return;
        }
        Ui& ui = m_ui;
        const std::vector<std::string> buttons = {ui.game_setting("sNo"), ui.game_setting("sYes")};
        ui.message(ui.game_setting("sLegendaryResetConfirm"), buttons, [&ui, buttons](int button) {
            if (button != 1) {
                return;
            }
            ui.message(ui.game_setting("sLegendaryResetSecondConfirm"), buttons, [&ui](int second) {
                if (Menu* m = ui.find_menu("StatsMenu"); m && second == 1) {
                    static_cast<StatsMenu*>(m)->reset_legendary();
                }
            });
        });
    }

    void register_handlers() override {
        handle("MoveCamera", [](Call&) {}); // the 3D camera between its poses
        handle("SetFadedIn", [this](Call&) { m_faded_in = true; });
    }

    void step() {
        switch (m_phase) {
        case 0:
            // The first update with the movie (frame 5 of the recording): the skills, the direction, the player,
            // an empty card, the stats mode.
            send_skill_list();
            send_direction();
            send_player_info(m_live ? nullptr : m_source.first("SetPlayerInfo"));
            send("SetDescriptionCard", {Value::boolean(false), Value::string(""), Value::number(0), Value::string(""),
                                        Value::string(""), Value::number(0), Value::string(""), Value::number(0)});
            send_stats_mode();
            m_phase = 1;
            return;
        case 1:
            // The scene is up (frame 43 there, after the models loaded): the selected skill's card.
            send_card();
            send_direction();
            m_phase = 2;
            return;
        case 2:
            // The opening sequence done (state 2 -> 0): FadeIn, and the per-frame spin from now on.
            invoke("gotoAndPlay", {Value::string("FadeIn")});
            m_phase = 3;
            break;
        default: break;
        }
        // While the Level Up Menu (or a message box, both modal) is over it the menu gets no update: measured, no
        // callback from the Stats Menu for as long as the Level Up Menu was open (capture levelup_calls).
        if (m_ui.is_open("LevelUp Menu") || m_ui.is_open("MessageBoxMenu")) {
            m_events.clear();
            return;
        }
        level_up();
        skills_click();
        // Input first, as the game delivers it before the update.
        for (const Event& e : std::exchange(m_events, {})) {
            if (m_live && m_state == state_perks) {
                if (e.down) {
                    tree_stick(e.name == "Left" ? -0.9 : e.name == "Right" ? 0.9 : 0.0,
                               e.name == "Up" ? 0.9 : e.name == "Down" ? -0.9 : 0.0);
                }
                continue;
            }
            if (e.name == "Left" || e.name == "Right") {
                m_stick_x = e.down ? (e.name == "Right" ? 0.9 : -0.9) : 0.0;
                m_pressed = e.down;
                spin(m_stick_x); // the press (or release) event's own pass
            } else if (e.down) {
                vertical(e.name == "Up" ? 0.9 : -0.9);
            }
        }
        if (m_state == state_perks) {
            // Once the camera has zoomed into the tree (UISkillsStop; measured: the first perk names 59 frames after
            // HideRing), every frame the stars' projected names and UpdatePerkText, which
            // steps the names' fade-in one frame per call.
            if (++m_perk_frames == k_zoom_frames) {
                m_ui.host().ui_play_sound("UISkillsStop");
            }
            if (m_perk_frames >= k_zoom_frames && m_live) {
                send_perk_names();
            } else if (m_perk_frames >= k_zoom_frames) {
                if (const Recording::Call* names = m_source.perk_names(m_selected)) {
                    set_array(names);
                    invoke("StatsMenu.StatsMenuInstance.UpdatePerkText", {});
                }
            }
            return;
        }
        if (m_turning) {
            if (!std::exchange(m_pressed, false) && m_stick_x != 0.0) {
                spin(m_stick_x); // the held button's event of this frame
            }
        } else {
            spin(0.0); // the update's own pass: the ease into the next skill
        }
    }

    void send_skill_list() {
        if (m_beast) {
            return; // beast mode sends no skill list (measured: run stats-beast1, both trees)
        }
        auto& vm = player()->vm;
        if (m_live) {
            std::vector<Value> values;
            for (const StatsSkill& k : m_live->skills()) {
                values.insert(values.end(), {Value::number(k.level), Value::string(k.name), Value::number(k.percent),
                                             Value::string(k.color), Value::number(k.legendary)});
            }
            player()->set_path("StatsMenu.SkillStatsA", Value::object(vm.make_array(std::move(values))));
            send("UpdateSkillList", {Value::boolean(true)});
            return;
        }
        if (const json::Value* stats = m_source.skill_stats()) {
            player()->set_path("StatsMenu.SkillStatsA", player::json_to_value(vm, *stats));
        }
        const Recording::Call* c = m_source.first("UpdateSkillList");
        send("UpdateSkillList", c ? to_values(vm, c->args) : std::vector<Value>{Value::boolean(true)});
    }

    void send_stats_mode() {
        // SetStatsMode(abStats, perk points): true and the player's points; false (the movie's LEVEL_UP state)
        // while a perk point waits (recorded: true, 0 at the start of a game; false, 1 after a level).
        if (m_live) {
            const int points = m_live->perk_points(m_beast);
            send("SetStatsMode", {Value::boolean(points == 0), Value::number(points)});
            return;
        }
        if (m_perk_points > 0) {
            send("SetStatsMode", {Value::boolean(false), Value::number(m_perk_points)});
            return;
        }
        const Recording::Call* c = m_source.first("SetStatsMode");
        send("SetStatsMode", c ? to_values(player()->vm, c->args) : std::vector<Value>{Value::boolean(true)});
    }

    // The level-up check of the update: once the movie has faded in, a level to take refreshes the
    // player info, shows "LevelUp Menu" and adds a perk point. With a perk point (or a level just taken) every
    // update refreshes the player info and the card.
    void level_up() {
        if (m_live) {
            if (m_faded_in && !m_beast && m_live->can_level_up()) {
                send_player_info(nullptr);
                m_live->count_level();
                m_ui.open("LevelUp Menu");
                m_live->add_perk_point(false);
                m_level_taken = true;
            } else if (m_level_taken) {
                send_player_info(nullptr);
                send_card();
            }
            return;
        }
        if (m_faded_in && !m_level_taken && m_source.level_up_pending()) {
            m_level_taken = true;
            send_player_info(m_source.first("SetPlayerInfo"));
            m_ui.open("LevelUp Menu");
            ++m_perk_points;
            return;
        }
        if (m_level_taken) {
            const Recording::Call* after = m_source.player_info_after_level();
            send_player_info(after ? after : m_source.first("SetPlayerInfo"));
            send_card();
        }
    }

    void send_player_info(const Recording::Call* c) {
        if (m_live) {
            const StatsPlayer p = m_live->player();
            std::vector<Value> values = {Value::string(p.name), Value::number(p.level), Value::number(p.percent),
                                         Value::string(p.race)};
            for (const StatsAttribute* a : {&p.magicka, &p.health, &p.stamina}) {
                values.insert(values.end(), {Value::number(a->current), Value::number(a->maximum), Value::string(a->color)});
            }
            send("SetPlayerInfo", std::move(values));
        } else if (c) {
            send("SetPlayerInfo", to_values(player()->vm, c->args));
        }
    }

    void send_card() {
        // The game: the selected skill's card, or its perk's in a tree, then the
        // stats mode.
        if (m_live) {
            const PerkTree::Node* node = m_state == state_perks ? m_starfield.selected_node() : nullptr;
            const StatsCard c = node ? m_live->perk_card(m_selected, node->perk, m_beast) : m_live->skill_card(m_selected);
            send("SetDescriptionCard", {Value::boolean(c.perk), Value::string(c.name), Value::number(c.meter),
                                        Value::string(c.description), Value::string(c.requirements), Value::number(c.level),
                                        Value::string(c.skill), Value::number(c.legendary)});
            send_stats_mode();
            if (!m_beast && m_live->legendary_ready(m_selected)) {
                send("ShowLegendaryButtonHint");
            }
            return;
        }
        const Recording::Call* c = m_state == state_perks ? m_source.perk_card(m_selected) : m_source.skill_card(m_selected);
        if (c) {
            send("SetDescriptionCard", to_values(player()->vm, c->args));
        }
        send_stats_mode();
    }

    // SetDirection(360 - the camera's yaw in degrees): in (0, 360], so a yaw of 0 sends 360 (measured: the
    // werewolf's tree, run stats-beast1).
    void send_direction() { send("SetDirection", {Value::number(m_angle <= 0.0 ? m_angle + 360.0 : m_angle)}); }

    // The game with the stick's x: a new direction ramps the camera's yaw speed towards
    // +-1/fStatsRotationSpeedMax rad/s over fStatsRotationRampTime ms (sine ease); while held it turns; released,
    // it eases into the next skill (speed at most 2 x what is left of the sector, x1.3 a frame below that, stopped
    // under 0.01 rad/s). Then the selection, and SetDirection.
    void spin(double x) {
        if (m_beast) {
            x = 0.0; // N = 2: the game takes no spin in beast mode
        }
        if (m_state == state_skills) {
            const double target = 1.0 / m_speed_max;
            const double dir = x <= 0.0 ? -1.0 : 1.0;
            if (x == 0.0 || dir == m_last_dir) {
                if (m_turning) {
                    if (x == 0.0) {
                        m_last_dir = 0.0;
                    } else {
                        double ease = 1.0;
                        if (!m_ramp_done) {
                            const double t = m_now < m_ramp_start + m_ramp_ms ? (m_now - m_ramp_start) / m_ramp_ms : 1.0;
                            m_ramp_done = m_now > m_ramp_start + m_ramp_ms;
                            ease = std::clamp(std::cos(1.5707964 - t * 1.5707964), 0.0, 1.0);
                        }
                        m_speed = m_ramp_from + (m_ramp_to - m_ramp_from) * ease;
                        turn(m_speed * m_dt);
                    }
                } else if (m_speed != 0.0) {
                    const double sector = 360.0 / m_count;
                    double frac = m_angle / sector - std::floor(m_angle / sector);
                    double limit = target;
                    if (m_last_dir <= 0.0) {
                        frac = 1.0 - frac;
                    }
                    if (m_speed <= 0.0) {
                        frac = 1.0 - frac;
                        limit = -limit;
                    }
                    const double v = limit * frac;
                    m_speed = v <= 0.0 ? std::max(m_speed, v) : std::min(m_speed, v);
                    if (std::abs(m_speed) < std::abs(v)) {
                        m_speed *= 1.3;
                    }
                    if (std::abs(m_speed) < m_reset_sound_speed) {
                        m_focus_sound = false;
                    }
                    if (std::abs(m_speed) >= 0.01) {
                        turn(m_speed * m_dt);
                    } else {
                        m_speed = 0.0;
                        m_ui.host().ui_play_sound("UISkillsGlow");
                        m_focus_sound = false;
                        m_starfield.spin_stopped(); // the selected constellation fades in
                    }
                }
            } else {
                m_starfield.spin_started(); // the lit constellation fades out
                if (!m_focus_sound) {
                    m_focus_sound = true;
                    m_ui.host().ui_play_sound("UISkillsFocus");
                }
                m_ramp_from = m_speed;
                m_ramp_to = dir * target;
                m_ramp_start = m_now;
                m_ramp_done = false;
                m_last_dir = dir;
            }
            m_turning = x != 0.0;
        } else {
            m_speed = 0.0;
        }
        update_selection();
        if (m_state == state_skills) {
            send_direction();
        }
    }

    void turn(double radians) {
        // The camera turns about Z; the direction the movie gets is 360 - its yaw in degrees.
        m_angle = std::fmod(m_angle + radians * 57.295776, 360.0);
        if (m_angle < 0.0) {
            m_angle += 360.0;
        }
    }

    void update_selection() {
        // a new skill under the camera plays UIMenuFocus and, on the skills, shows its card.
        const int index = skill_at(m_angle, m_count);
        if (index != m_selected) {
            m_selected = index;
            if (m_state == state_skills) {
                m_ui.host().ui_play_sound("UIMenuFocus");
                send_card();
            }
        }
    }

    // The game's Click on the skills, for each event of the button: a press in the upper
    // middle (above 0.6 of the height, between 0.33 and 0.66 of the width) goes into the tree; below 0.9 of the height
    // it leaves the menu; the release is the stick let go; otherwise the stick is full over the cursor's half.
    void skills_click() {
        if (m_click == click_none || m_state != state_skills) {
            m_click = click_none;
            return;
        }
        const double w = std::max(m_ui.target_width(), 1), h = std::max(m_ui.target_height(), 1);
        const double x = m_ui.mouse_x(), y = m_ui.mouse_y();
        const Click click = m_click;
        m_click = click == click_release ? click_none : click_held;
        if (click == click_press && y < h * 0.6 && x > w * 0.33 && x < w * 0.66) {
            m_click = click_none;
            m_events.push_back({"Up", true});
        } else if (y > h * 0.9) {
            m_click = click_none;
            m_ui.host().ui_play_sound("UIMenuCancel");
            m_ui.close(name());
        } else if (click == click_release) {
            m_events.push_back({m_stick_x < 0.0 ? "Left" : "Right", false});
        } else if (const char* side = x <= w * 0.5 ? "Left" : "Right"; m_stick_x != (x <= w * 0.5 ? -0.9 : 0.9)) {
            m_events.push_back({side, true});
        }
    }

    void vertical(double y) {
        // up on the skills zooms into the selected skill's perk tree (HideRing, the perk's
        // card); down in a tree goes back: the perk names hidden, the camera on the skill again, its card.
        if (m_state == state_skills && y > 0.0) {
            m_state = state_perks;
            m_speed = 0.0;
            m_turning = false;
            m_stick_x = 0.0;
            m_last_dir = 0.0;
            m_ui.host().ui_play_sound("UISkillsFocusSD");
            send("HideRing");
            m_starfield.enter_tree();
            send_card();
            m_perk_frames = 0;
        } else if (m_state == state_perks && y < 0.0) {
            m_state = state_skills;
            m_angle = 360.0 / m_count * m_selected;
            m_starfield.leave_tree();
            invoke("StatsMenu.StatsMenuInstance.UpdatePerkText", {Value::boolean(false)});
            send_direction();
            send_skill_list();
            send_card();
            send_direction();
        }
    }

    void tree_stick(double x, double y) {
        // The game in a tree: the star that way, here or in the skill beside this one; none
        // and down, the ring.
        if (m_starfield.move_star(x, y, m_selected)) {
            m_ui.host().ui_play_sound("UISkillsFocusSD");
            send_card();
        } else if (y < 0.0) {
            m_ui.host().ui_play_sound("UISkillsBackward");
            vertical(y);
        }
    }

    void send_perk_names() {
        // three values a star on the screen.
        auto& vm = player()->vm;
        m_starfield.labels(m_labels);
        std::vector<Value> values;
        for (const Starfield::StarLabel& l : m_labels) {
            values.insert(values.end(), {Value::string(m_live->perk_label(l.node->perk)), Value::number(l.x), Value::number(l.y)});
        }
        player()->set_path("StatsMenu.PerkNamesA", Value::object(vm.make_array(std::move(values))));
        invoke("StatsMenu.StatsMenuInstance.UpdatePerkText", {});
    }

    // A recorded SetVariableArray: index, then the elements.
    void set_array(const Recording::Call* c) {
        auto& vm = player()->vm;
        std::vector<Value> values = to_values(vm, c->args);
        if (values.empty()) {
            return;
        }
        const size_t index = size_t(vm.to_number(values[0]));
        std::vector<Value> elements;
        if (const Value old = player()->get_path(c->name); old.is_object()) {
            elements = old.o->elements;
        }
        elements.resize(std::max(elements.size(), index + values.size() - 1));
        std::copy(values.begin() + 1, values.end(), elements.begin() + std::ptrdiff_t(index));
        player()->set_path(c->name, Value::object(vm.make_array(std::move(elements))));
    }

    StatsSource m_source;
    Starfield m_starfield;
    CharacterSource* m_live; // the running game's data; null: the capture's
    std::vector<Starfield::StarLabel> m_labels;
    State m_state = state_skills;
    int m_phase = 0;
    int m_selected = 15, m_count = 18;
    bool m_beast = false;
    double m_angle = 0;              // the camera's direction, degrees, what SetDirection sends
    double m_speed = 0;              // the camera's yaw speed, rad/s
    double m_last_dir = 0;           // the direction being held
    bool m_turning = false;          // a direction is held
    double m_stick_x = 0;
    enum Click { click_none, click_press, click_held, click_release };
    Click m_click = click_none;      // the mouse button on the skills, for the next update
    bool m_pressed = false;
    double m_ramp_from = 0, m_ramp_to = 0, m_ramp_start = 0;
    bool m_ramp_done = false, m_focus_sound = false;
    double m_speed_max = 0.5, m_ramp_ms = 2000, m_reset_sound_speed = 0.3;
    double m_now = 0, m_dt = 0;
    std::vector<Event> m_events;
    bool m_faded_in = false, m_closing = false;
    int m_close_frames = 0;
    int m_perk_frames = 0;
    int m_perk_points = 0;
    bool m_level_taken = false;
    // The camera's zoom into a perk tree (3D): measured in the game, 59 frames at 60 fps
    // from HideRing to the first perk names (capture-stats.osc, run stats4).
    static constexpr int k_zoom_frames = 59;
};

// ---- Level Up Menu (movie LevelUpMenu) ----------------------------------------------------------------------------

// The attributes' names (the actor values' AVIF names) the confirmation text uses. Recorded:
// the message box texts of the capture "levelup_calls", which asked about each attribute once.
class AttributeNames {
public:
    explicit AttributeNames(Ui& ui) {
        const Recording rec(ui, "levelup_calls");
        // Which attribute each question was about: the movie's own add* call just before it.
        int av = -1;
        const std::string pattern = ui.game_setting("sConfirmAttribute");
        const size_t at = pattern.find("%s");
        for (const Recording::Call& c : rec.calls()) {
            if (c.kind == "delegate" && c.menu == "LevelUp Menu") {
                av = c.name == "addHealth" ? 24 : c.name == "addMagicka" ? 25 : c.name == "addStamina" ? 26 : av;
            }
            if (c.kind == "callback" && c.menu == "MessageBoxMenu" && c.name == "setMessageText" && av >= 0 &&
                at != std::string::npos) {
                const json::Value* text = arg(&c, 0);
                const std::string prefix = pattern.substr(0, at), suffix = pattern.substr(at + 2);
                if (text && text->s.size() >= prefix.size() + suffix.size() && text->s.starts_with(prefix) &&
                    text->s.ends_with(suffix)) {
                    m_names[av] = text->s.substr(prefix.size(), text->s.size() - prefix.size() - suffix.size());
                }
                av = -1;
            }
        }
        if (m_names.empty()) {
            note_missing("levelup_calls", "LevelUp Menu");
        }
    }
    std::string name(int av) const {
        auto it = m_names.find(av);
        return it == m_names.end() ? std::string() : it->second;
    }

private:
    std::map<int, std::string> m_names;
};

class LevelUpMenu final : public Menu {
public:
    // LevelUpMenu::Create: ExactFit, depth 3, flags 0x91 ( without a gamepad), Menu Mode.
    explicit LevelUpMenu(Ui& ui)
        : Menu(ui, {"LevelUp Menu", "LevelUpMenu", ScaleMode::exact_fit, 3, 0x91u | cursor_flag(ui), 0, 1}),
          m_names(ui) {}

    static void answered(Ui& ui, int av, int button) {
        // The game: only while the Stats Menu is open; OK raises the attribute,
        // applies the level and closes the menu. Any answer ends the question.
        if (ui.is_open("StatsMenu") && button == 0) {
            if (CharacterSource* live = ui.host().ui_character()) {
                live->choose_attribute(av);
            } else {
                ui.host().ui_action(av == 24 ? "level_up_health" : av == 25 ? "level_up_magicka" : "level_up_stamina");
            }
            ui.close("LevelUp Menu");
        }
        if (Menu* m = ui.find_menu("LevelUp Menu")) {
            static_cast<LevelUpMenu*>(m)->m_asking = false;
        }
    }

private:
    void register_handlers() override {
        handle("addHealth", [this](Call&) { ask(24); });
        handle("addMagicka", [this](Call&) { ask(25); });
        handle("addStamina", [this](Call&) { ask(26); });
        handle("PlaySound", [this](Call& c) { handle_play_sound(*this, c); });
    }

    void on_loaded() override {
        // Create: with a gamepad the selection takes the focus. (RefreshPlatform, on a device
        // change, also clears it for the mouse with Selection.setFocus(null).)
        if (m_ui.gamepad_active()) {
            invoke("Selection.captureFocus");
        }
    }

    void ask(int av) {
        // "Increase %s?" with the attribute's name, OK / Cancel; one question at a time.
        if (m_asking) {
            return;
        }
        m_asking = true;
        Ui& ui = m_ui;
        CharacterSource* live = ui.host().ui_character();
        ui.message(format_s(ui.game_setting("sConfirmAttribute"), live ? live->attribute_name(av) : m_names.name(av)),
                   {ui.game_setting("sOk"), ui.game_setting("sCancel")},
                   [&ui, av](int button) { answered(ui, av, button); });
    }

    AttributeNames m_names;
    bool m_asking = false;
};

// ---- Book Menu (movies BookMenu and Book) ---------------------------------------------------------------------------

// The book being read (the game's data): its text after the engine's processing and whether it is a
// note, and whether there is a reference to take. Recorded: SetBookText and ShowTakeButton of the capture
// "book_calls" (or "note_calls" for a note).
struct BookData {
    bool present = false;
    std::string text;
    bool note = false;
    bool has_reference = false, steal = false; // ShowTakeButton(abShow, abSteal): Take, or Steal when it is owned
    // The book's 3D model and where it starts (the game's data: the INAM static's model; from the world
    // the fixed point and the reference's world rotation and local scale), from the game: the capture "book-models",
    // keyed by the recording.
    std::string model;
    float start_position[3] = {-23.0f, -501.0f, 60.0f};
    float start_rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float start_scale = 1.0f;
};

BookData recorded_book(Ui& ui, const std::string& capture) {
    BookData b;
    const Recording rec(ui, capture);
    if (const Recording::Call* c = rec.first("callback", "SetBookText", "Book Menu:Book")) {
        b.present = true;
        b.text = arg(c, 0) ? arg(c, 0)->s : std::string();
        b.note = arg(c, 1) && arg(c, 1)->b;
    }
    if (const Recording::Call* c = rec.first("callback", "ShowTakeButton", "Book Menu")) {
        b.has_reference = arg(c, 0) && arg(c, 0)->b;
        b.steal = arg(c, 1) && arg(c, 1)->b;
    }
    if (const json::Value* m = captured_path(ui, "book-models", capture)) {
        auto member = [&](std::string_view k) -> const json::Value* {
            for (const auto& [name, v] : m->members) {
                if (name == k) {
                    return &v;
                }
            }
            return nullptr;
        };
        if (const json::Value* v = member("model"); v && v->kind == json::Value::string) {
            b.model = v->s;
        }
        auto floats = [&](std::string_view k, float* out, size_t n) {
            if (const json::Value* v = member(k); v && v->kind == json::Value::array && v->items.size() == n) {
                for (size_t i = 0; i < n; ++i) {
                    out[i] = float(v->items[i].n);
                }
            }
        };
        floats("start_position", b.start_position, 3);
        floats("start_rotation", b.start_rotation, 9);
        if (const json::Value* v = member("start_scale"); v && v->kind == json::Value::number) {
            b.start_scale = float(v->n);
        }
    }
    return b;
}

// The images a book's text names (the text processing): for every "<img" tag, the src after "img://" up
// to its closing quote (the text processing has lower-cased it and turned '/' into '\'). The menu
// registers each with the image loader under that name for as long as it lives (its the game
// array, BookMenu): a PNG by its path, which the loader reads and decodes, any other file as
// a texture it loads; the importer brought both into the cache.
std::vector<std::string> book_images(std::string_view text) {
    std::vector<std::string> out;
    for (size_t i = text.find("<img"); i != std::string_view::npos; i = text.find("<img", i + 4)) {
        if (i + 10 > text.size() || (text[i + 9] != '\'' && text[i + 9] != '"')) {
            continue;
        }
        const size_t end = text.find_first_of("'\"", i + 10);
        if (end == std::string_view::npos) {
            break;
        }
        const std::string_view name = player::ImageRegistry::name_of(text.substr(i + 10, end - i - 10));
        if (!name.empty()) {
            out.emplace_back(name);
        }
    }
    return out;
}

class BookMenu final : public Menu {
public:
    // BookMenu (NoBorder) and Book (ShowAll), depth 1, flags 0x1389, context 10 (Book).
    explicit BookMenu(Ui& ui)
        : Menu(ui, {"Book Menu", "BookMenu", ScaleMode::no_border, 1, 0x1389, 0, 10}), m_book3d(ui) {}
    ~BookMenu() override {
        for (const auto& name : m_images) {
            m_ui.library().images.release(name);
        }
    }

    // Which recorded book the Book Menu shows (the engine's OpenBookMenu data): "book_calls", or the capture named
    // by SPUD_UI_BOOK ("note_calls" for the recorded note).
    static std::string next_book() {
        const std::string chosen = platform::env("SPUD_UI_BOOK").value_or("");
        return chosen.empty() ? std::string("book_calls") : chosen;
    }

    // BookMenu::Create puts the HUD into "BookMode"; ~BookMenu ends it.
    HudModeHold m_hud_mode{m_ui, "BookMode"};
    BlurHold m_blur{m_ui};

    void on_show() override {
        if (BookInfo info; m_ui.host().ui_book(info) && info.present) {
            m_book.present = true;
            m_book.text = std::move(info.text);
            m_book.has_reference = info.has_reference;
            m_book.steal = info.steal;
            m_book.model = std::move(info.model);
        } else {
            m_book = recorded_book(m_ui, next_book());
        }
        if (!m_book.present) {
            note_missing(next_book(), name());
        }
        m_book3d.open(m_book.model, m_book.start_position, m_book.start_rotation, m_book.start_scale);
    }

    bool on_hide() override {
        // Message 3: once the open flight is done, PrepForClose on the Book movie and the flight back, the menu kept
        // until it has ended (answered 1); during the open flight the menu goes at once (answered 0).
        if (m_book3d.closing_done()) {
            return true;
        }
        if (m_book3d.ready() && m_book3d.close()) {
            if (!m_prepped) {
                m_prepped = true;
                book_invoke("BookMenu.BookMenuInstance.PrepForClose", {});
            }
            return false;
        }
        if (!m_book3d.ready()) {
            book_invoke("BookMenu.BookMenuInstance.PrepForClose", {}); // no book: nothing flies back
        }
        return true;
    }

    void advance(double dt, double engine_ms) override {
        if (!m_set_up) {
            // The first update with the book model in: the pages' render targets, SetBookText to the
            // Book movie, then ShowTakeButton.
            m_set_up = true;
            auto& images = m_ui.library().images;
            for (const auto& name : book_images(m_book.text)) {
                if (images.add_texture(name, player::ImageRegistry::cache_texture_path(name)) !=
                    player::ImageRegistry::k_none) {
                    m_images.push_back(name);
                } else {
                    log::info("ui: Book Menu: image {} is not in the cache (skytool ui import)", name);
                }
            }
            send_book("SetBookText", {Value::string(m_book.text), Value::boolean(m_book.note)});
            send("ShowTakeButton", {Value::boolean(m_book.has_reference), Value::boolean(m_book.steal)});
        }
        // the Book movie advances with the menu, then the menu's own movie.
        if (m_book_player) {
            m_book_player->set_path("CurrentTime", Value::number(std::floor(engine_ms)));
            m_book_player->advance(dt);
        }
        Menu::advance(dt, engine_ms);
        m_book3d.update(engine_ms);
        if (m_book3d.ready() && !m_viewport_set && m_book_player) {
            // The game: the Book movie's viewport on the page target, from PageText's UVs.
            m_viewport_set = true;
            m_book3d.viewport(uint32_t(m_ui.target_height()), m_viewport);
            layout_book(float(m_viewport[2]), float(m_viewport[3]));
            m_ui.set_page_paper(m_book3d.paper());
        }
        if (m_book3d.closing_done()) {
            m_ui.close(name()); // the first update after the flight back posts the hide that removes the menu
        }
    }

    // The game: the BookMenu movie, then slot 3.
    bool ui_scene(UiScene& out) override { return m_book3d.scene(out); }
    bool scene_after_movie() const override { return true; }
    // The game, before the frame: per page, ShowPageAtOffset(i) and the Book movie into render
    // target 105 + i.
    void scene_pages(std::vector<ScenePage>& out) override {
        if (!m_book_player || !m_book3d.ready() || !m_viewport_set) {
            return;
        }
        const auto& hd = m_book_player->movie->data().header;
        const float vw = float(m_viewport[2]), vh = float(m_viewport[3]);
        const float sc = std::min(vw / hd.stage_width, vh / hd.stage_height); // ShowAll
        const player::Matrix2D to_page{sc, 0, 0, sc, float(m_viewport[0]) + (vw - hd.stage_width * sc) * 0.5f,
                                       float(m_viewport[1]) + (vh - hd.stage_height * sc) * 0.5f};
        for (int i = 0; i < m_book3d.pages() && i < 4; ++i) {
            book_invoke("BookMenu.BookMenuInstance.ShowPageAtOffset", {Value::number(i)});
            m_book_player->render(m_page_lists[size_t(i)]);
            out.push_back({&m_page_lists[size_t(i)], to_page, i});
        }
    }
    std::string book3d_json() const { return m_book3d.state_json(); }

    static bool book_event(std::string_view event) {
        return event == "PrevPage" || event == "NextPage" || event == "LeftEquip" || event == "RightEquip" ||
               event == "Cancel" || event == "Accept";
    }

    // The buttons the menu takes as user events (below) make no Scaleform key as well.
    bool on_button(std::string_view event, bool) override { return book_event(event); }

    bool on_user_event(const std::string& event) override {
        // user-event messages (type 7, the press): page turns, Cancel, Accept (take).
        if (event == "PrevPage" || event == "LeftEquip" || event == "NextPage" || event == "RightEquip") {
            turn_page(event == "NextPage" || event == "RightEquip" ? 1 : -1);
            return true;
        }
        if (event == "Cancel") {
            m_ui.close(name());
            return true;
        }
        if (event == "Accept") {
            if (m_book.has_reference) {
                m_ui.host().ui_action("take_book"); // the player picks the reference up (vtable)
            }
            m_ui.close(name());
            return true;
        }
        return false;
    }

    bool on_key(uint32_t code, bool down) override {
        // Message 6: keys 37 / 39 turn the page, and on a note 38 / 40 as well; the movie never sees them.
        const bool turn = code == 37 || code == 39 || (m_book.note && (code == 38 || code == 40));
        if (turn && down) {
            turn_page(code == 39 || code == 40 ? 1 : -1);
        }
        return turn;
    }

    player::Player* movie_named(std::string_view which) override {
        return which.empty() ? player() : which == "Book" ? m_book_player.get() : nullptr;
    }

private:
    void on_loaded() override {
        // The second movie, loaded the same way (section 3 of the engine spec) into IMenu.
        auto def = m_ui.library().movie("Book.swf");
        if (!def) {
            log::warn("ui: Book Menu: no movie Book in the interface cache");
            return;
        }
        m_book_player = std::make_unique<player::Player>(m_ui.library(), def, this);
        m_book_player->stage.scale_mode = "showAll";
        m_book_player->stage.viewport[0] = float(m_ui.target_width());
        m_book_player->stage.viewport[1] = float(m_ui.target_height());
        m_book_player->start();
        send_to(*m_book_player, "SetButtonMapping", {Value::object(m_ui.button_mapping(*m_book_player))});
        m_book_player->advance(0);
        if (m_book_player->has_path("InitExtensions")) {
            m_book_player->invoke("InitExtensions", {});
        }
        if (m_book_player->has_path("SetPlatform")) {
            m_book_player->invoke("SetPlatform",
                                  {Value::number(m_ui.gamepad_active() ? 1 : 0), Value::boolean(false)});
        }
    }

    void send_book(const std::string& callback, std::vector<Value> args) {
        if (m_book_player) {
            send_to(*m_book_player, callback, std::move(args));
        }
    }
    Value book_invoke(std::string_view path, std::vector<Value> args) {
        return m_book_player ? m_book_player->invoke(path, args) : Value{};
    }

    void turn_page(int direction) {
        // TurnPage on the Book movie by two pages (a spread), on a note by one (the doubles 2.0 / 1.0,
        // negated backwards; measured TurnPage(2) for a book); when it turns, the book plays its page animation (3D),
        // a note the sound ITMNotePageTurn.
        const double delta = direction * (m_book.note ? 1.0 : 2.0);
        const Value turned = book_invoke("BookMenu.BookMenuInstance.TurnPage", {Value::number(delta)});
        if (m_book_player && m_book_player->vm.to_boolean(turned) && m_book.note) {
            m_ui.host().ui_play_sound("ITMNotePageTurn");
        }
    }

    // The Book movie's Stage for its viewport (Stage.visibleRect as Menu::layout gives a menu's own movie).
    void layout_book(float w, float h) {
        if (!m_book_player || w <= 0 || h <= 0) {
            return;
        }
        const auto& hd = m_book_player->movie->data().header;
        const float sc = std::min(w / hd.stage_width, h / hd.stage_height);
        const float x0 = -(w - hd.stage_width * sc) * 0.5f / sc, y0 = -(h - hd.stage_height * sc) * 0.5f / sc;
        auto& st = m_book_player->stage;
        st.visible[0] = x0;
        st.visible[1] = y0;
        st.visible[2] = x0 + w / sc;
        st.visible[3] = y0 + h / sc;
        constexpr float k_safe = 15.0f;
        st.safe[0] = st.visible[0] + k_safe;
        st.safe[1] = st.visible[1] + k_safe;
        st.safe[2] = st.visible[2] - k_safe;
        st.safe[3] = st.visible[3] - k_safe;
        st.viewport[0] = w;
        st.viewport[1] = h;
    }

    BookData m_book;
    bool m_set_up = false;
    std::vector<std::string> m_images; // the engine images the text registered (book_images)
    std::unique_ptr<player::Player> m_book_player;
    Book3D m_book3d;
    bool m_viewport_set = false, m_prepped = false;
    int m_viewport[4] = {0, 0, 0, 0};
    std::array<player::DrawList, 4> m_page_lists;
};

// ---- Dialogue Menu (movie DialogueMenu) -----------------------------------------------------------------------------

// A conversation as the dialogue system ran it in the game: what the engine sent the movie from the Show message on,
// split at each topic the player chose (the movie's TopicClicked). Recorded: the capture "dialogue_calls".
class Conversation {
public:
    struct Step {
        int frame; // after the start of its part
        std::string kind, name;
        const json::Value* args;
    };

    explicit Conversation(Ui& ui) : m_rec(ui, "dialogue_calls") {
        // Menus the conversation handed over to (a topic's barter or training): the span of their traffic.
        std::map<std::string, std::pair<int, int>> handoffs;
        for (const Recording::Call& c : m_rec.calls()) {
            const std::string menu = c.menu.substr(0, c.menu.find(':'));
            if (menu != "Dialogue Menu") {
                auto [it, fresh] = handoffs.try_emplace(menu, c.frame, c.frame);
                it->second.second = c.frame;
            }
        }
        std::vector<Step>* part = &m_parts.emplace_back();
        m_entries.push_back({});
        int base = 0;
        bool started = false;
        std::string open_handoff;
        int handoff_end = 0;
        for (const Recording::Call& c : m_rec.calls()) {
            const std::string menu = c.menu.substr(0, c.menu.find(':'));
            if (menu != "Dialogue Menu") {
                // The hand-over: the other menu opens (its first traffic) and the conversation waits for it; what
                // the dialogue system sent once it closed starts the next part.
                if (open_handoff.empty() && c.frame == handoffs[menu].first) {
                    part->push_back({c.frame - base, "open", menu, nullptr});
                    open_handoff = menu;
                    handoff_end = handoffs[menu].second;
                }
                continue;
            }
            if (!started) {
                base = c.frame;
                started = true;
            }
            if (!open_handoff.empty() && c.frame > handoff_end) {
                part = &m_parts.emplace_back();
                m_entries.push_back({"", open_handoff});
                base = handoff_end;
                open_handoff.clear();
            }
            if (c.kind == "delegate") {
                if (c.name == "TopicClicked") {
                    const json::Value* index = arg(&c, 0);
                    part = &m_parts.emplace_back();
                    m_entries.push_back({std::to_string(index ? int(index->n) : -1), ""});
                    base = c.frame;
                }
                continue;
            }
            // What every menu's loader and advance send (Menu::load, Menu::advance) and the menu's own answers
            // (StartHideMenu on hide) are the menu's, not the dialogue system's.
            static const std::set<std::string, std::less<>> k_own = {"SetButtonMapping", "InitExtensions",
                                                                      "SetPlatform", "CurrentTime", "StartHideMenu"};
            if (k_own.contains(c.name)) {
                continue;
            }
            part->push_back({c.frame - base, c.kind, c.name, c.args});
        }
    }
    bool empty() const { return m_rec.empty(); }
    // Part 0 follows the Show message; each later part follows a topic choice or a handed-over menu's closing.
    const std::vector<Step>* part(size_t i) const { return i < m_parts.size() ? &m_parts[i] : nullptr; }
    // What starts part i: the topic index the recorded player chose ("" if none), or the menu that closed.
    const std::string& topic(size_t i) const { return m_entries[i < m_entries.size() ? i : 0].topic; }
    const std::string& returned_from(size_t i) const { return m_entries[i < m_entries.size() ? i : 0].menu; }
    size_t parts() const { return m_parts.size(); }

private:
    struct Entry {
        std::string topic, menu;
    };
    Recording m_rec;
    std::vector<std::vector<Step>> m_parts;
    std::vector<Entry> m_entries;
};

class DialogueMenu final : public Menu {
public:
    // ExactFit, depth 3, flags 0x4400, Menu Mode.
    explicit DialogueMenu(Ui& ui)
        : Menu(ui, {"Dialogue Menu", "DialogueMenu", ScaleMode::exact_fit, 3, 0x4400, 0, 1}), m_talk(ui) {}

    void on_show() override {
        if (m_ui.host().ui_dialogue_live()) {
            return;
        }
        if (m_talk.empty()) {
            note_missing("dialogue_calls", name());
        }
        start_part(0);
    }

    // The dialogue system's side of the movie.
    void dialogue_speaker(const std::string& speaker) override { send("SetSpeakerName", {Value::string(speaker)}); }
    void dialogue_text(const std::string& text) override {
        send("ShowDialogueText", {Value::string(text)});
    }
    void dialogue_topics(const std::vector<DialogueRow>& rows, bool refreshed) override {
        std::vector<Value> args;
        for (const DialogueRow& row : rows) {
            args.push_back(Value::string(row.text));
            args.push_back(Value::boolean(row.is_new));
            args.push_back(Value::number(row.index));
        }
        args.push_back(Value::number(0));
        send("PopulateDialogueList", std::move(args));
        send("ShowDialogueList", {Value::boolean(refreshed), Value::boolean(false)});
    }
    void dialogue_voice_ready() override { send("NotifyVoiceReady"); }

    bool on_hide() override {
        // Message 3 with the dialogue's "fade out" byte: StartHideMenu; the movie fades and calls CloseMenu, then
        // FadeDone, which hides the menu.
        if (m_faded) {
            return true;
        }
        if (!m_hiding) {
            m_hiding = true;
            send("StartHideMenu");
        }
        return false;
    }

    void advance(double dt, double engine_ms) override {
        // The menu's the game sink: with a menu open that pauses the game (the barter) the UI
        // update "DMFOStr" hides the movie (SetVisible(false), movie vtable) and the menu passes its input on;
        // once none is left it shows it again. Measured: the game's barter screen shows no dialogue, and Tab goes to
        // the BarterMenu.
        if (const int pausing = m_ui.pausing_menus(); pausing != m_pausing) {
            m_pausing = pausing;
            // The same update turns DialogueMode off while hidden and on again after ( /).
            if (pausing > 0) {
                m_hidden = m_inert = true;
                m_hud_mode.set(false);
            } else if (m_inert) {
                m_hidden = m_inert = false;
                m_hud_mode.set(true);
            }
        }
        // A menu the conversation handed over to: once it has closed, the dialogue system goes on.
        if (!m_handoff.empty() && !m_ui.is_open(m_handoff)) {
            for (size_t i = m_part + 1; i < m_talk.parts(); ++i) {
                if (m_talk.returned_from(i) == m_handoff) {
                    start_part(i);
                    break;
                }
            }
            m_handoff.clear();
        }
        // The dialogue system's updates arrive at the frames they arrived in the game.
        if (m_steps && m_handoff.empty()) {
            while (m_next < m_steps->size() && (*m_steps)[m_next].frame <= m_frame && m_handoff.empty()) {
                play((*m_steps)[m_next++]);
            }
            ++m_frame;
        }
        Menu::advance(dt, engine_ms);
    }

private:
    void register_handlers() override {
        handle("SkipText", [this](Call&) { m_ui.host().ui_dialogue_skip(); });
        handle("TopicClicked", [this](Call& c) {
            // the topic manager takes the chosen topic and goes on.
            if (c.args.size() == 1 && c.args[0].type == player::Type::number) {
                if (m_ui.host().ui_dialogue_live()) {
                    m_ui.host().ui_dialogue_topic(int(c.args[0].n));
                } else {
                    topic(int(c.args[0].n));
                }
            }
        });
        handle("FadeDone", [this](Call&) {
            m_faded = true;
            m_ui.close(name());
        });
        // the dialogue is closing (the menu passes its input on from now on) and DialogueMode ends
        // the HUD update belongs to the game's side.
        handle("CloseMenu", [this](Call&) {
            m_inert = true;
            m_hud_mode.release();
            m_ui.host().ui_dialogue_closed();
        });
        handle("PlaySound", [this](Call& c) { handle_play_sound(*this, c); });
    }

    void start_part(size_t i) {
        m_part = i;
        m_steps = m_talk.part(i);
        m_next = 0;
        m_frame = 0;
    }

    void topic(int index) {
        // Only the conversation the game recorded can continue: the same choice as the player made there.
        for (size_t i = m_part + 1; i < m_talk.parts(); ++i) {
            if (!m_talk.topic(i).empty()) {
                if (m_talk.topic(i) == std::to_string(index)) {
                    start_part(i);
                    return;
                }
                break;
            }
        }
        log::info("ui Dialogue Menu: topic {} has no recorded response", index);
    }

    void play(const Conversation::Step& s) {
        if (s.kind == "open") {
            // The topic's barter or training: the dialogue system opens that menu over the conversation.
            m_handoff = s.name;
            m_ui.open(s.name);
            return;
        }
        std::vector<Value> args = to_values(player()->vm, s.args);
        if (s.kind == "callback") {
            send(s.name, std::move(args));
        } else if (s.kind == "invoke") {
            invoke(s.name, std::move(args));
        } else if (s.kind == "set" && !args.empty()) {
            player()->set_path(s.name, args[0]);
        }
    }

    // Message 6 while hidden or closing (byte): the event is passed on (2) to the menus below.
    bool pass_through() const override { return m_inert; }
    bool movie_visible() const override { return !m_hidden; }

    // DialogueMenu::Create puts the HUD into "DialogueMode". A hide without the fade byte would end it
    // at once; spud's always fades, and CloseMenu ends it.
    HudModeHold m_hud_mode{m_ui, "DialogueMode"};
    Conversation m_talk;
    const std::vector<Conversation::Step>* m_steps = nullptr;
    size_t m_part = 0, m_next = 0;
    int m_frame = 0, m_pausing = 0;
    std::string m_handoff; // the menu the conversation waits for
    bool m_hiding = false, m_faded = false;
    bool m_inert = false;  // byte_1432563F9: hidden by a pausing menu, or closing
    bool m_hidden = false; // the movie set invisible ("DMFOStr")
};

} // namespace

std::string stats_starfield_json(Menu& m) {
    if (auto* sm = dynamic_cast<StatsMenu*>(&m)) {
        return sm->starfield_json();
    }
    return "null";
}

std::string book_book3d_json(Menu& m) {
    if (auto* bm = dynamic_cast<BookMenu*>(&m)) {
        return bm->book3d_json();
    }
    return "null";
}

std::unique_ptr<Menu> create_stats_menu(Ui& ui, const std::string& name) {
    if (name == "StatsMenu") return std::make_unique<StatsMenu>(ui);
    if (name == "LevelUp Menu") return std::make_unique<LevelUpMenu>(ui);
    if (name == "Book Menu") return std::make_unique<BookMenu>(ui);
    if (name == "Dialogue Menu") return std::make_unique<DialogueMenu>(ui);
    return nullptr;
}

} // namespace spud::ui
