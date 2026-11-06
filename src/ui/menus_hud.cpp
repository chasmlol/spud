// HUD Menu, Sleep/Wait, Lockpicking and Training menus: the engine side of each movie, implemented from
// (addresses and evidence there).
//
// The game systems behind these menus (actor values, quests and map markers, the crosshair pick, skills and
// gold, locks) don't exist in spud yet. Each menu reads what those systems would supply through a small
// source; today the sources play back what the game recorded in the real game (captures `hud`,
// `sleepwait`, `training`, `lockpicking` in <cache>/captures). The menus' own logic (when the engine calls
// which movie function with which values, the HUD objects' per-frame rules, the lockpicking simulation) is
// the game's.

#include "ui/lock3d.hpp"
#include "ui/items.hpp"
#include "ui/menus_internal.hpp"

#include "ui/player/json.hpp"
#include "ui/ui.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace spud::ui {

void hud_set_mode(Ui& ui, const std::string& mode, bool on);

namespace {

using Value = player::Value;
using Object = player::Object;
namespace json = player::json;

constexpr double k_rad_to_deg = 57.295776; // the constant the HUD uses

const json::Value* member(const json::Value* v, std::string_view key) {
    if (!v || v->kind != json::Value::object) {
        return nullptr;
    }
    for (const auto& [k, m] : v->members) {
        if (k == key) {
            return &m;
        }
    }
    return nullptr;
}

double number(const json::Value* v, double fallback = 0) {
    return v && v->kind == json::Value::number ? v->n : fallback;
}

std::string text(const json::Value* v) {
    return v && v->kind == json::Value::string ? v->s : std::string();
}

bool flag(const json::Value* v, bool fallback = false) {
    return v && v->kind == json::Value::boolean  ? v->b
           : v && v->kind == json::Value::number ? v->n != 0
                                                 : fallback;
}

// Wraps degrees into [0, 360) as Compass::Update does: above 360 fmod 360, below 0 fmod + 360.
double wrap_degrees(double d) {
    if (d > 360.0) {
        d = std::fmod(d, 360.0);
    } else if (d < 0.0) {
        d = std::fmod(d, 360.0) + 360.0;
    }
    return d;
}

// ---- recorded engine data
// ----------------------------------------------------------------------------------------

// A timeline of what the game's systems handed the HUD, recorded by the game (`trace`) and converted by
// entries {"f": frame since the recording started,...}, played back from the
// frame the menu opened. The menu applies each entry when its frame comes up.
class Timeline {
  public:
    void load(const json::Value* capture) {
        m_entries.clear();
        m_next = 0;
        const json::Value* t = member(capture, "timeline");
        if (!t || t->kind != json::Value::array) {
            return;
        }
        for (const json::Value& e : t->items) {
            m_entries.push_back(&e);
        }
        std::stable_sort(m_entries.begin(), m_entries.end(), [](const json::Value* a, const json::Value* b) {
            return number(member(a, "f")) < number(member(b, "f"));
        });
    }
    bool empty() const { return m_entries.empty(); }
    // The entries whose frame is due by `frame`, in order.
    template <typename F> void play(uint64_t frame, const F& apply) {
        while (m_next < m_entries.size() && number(member(m_entries[m_next], "f")) <= double(frame)) {
            apply(*m_entries[m_next++]);
        }
    }

  private:
    std::vector<const json::Value*> m_entries;
    size_t m_next = 0;
};

Value json_value(player::Vm& vm, const json::Value& v) {
    return player::json_to_value(vm, v);
}

// ===========================================================================================================
// HUD Menu (HUDMenu, movie HUDMenu)
// ===========================================================================================================

// HUD data message types (HUDData).
enum HudType : int {
    hud_message = 1,        // ShowMessage(text)
    hud_crosshair = 2,      // SetCrosshairTarget(10 values)
    hud_crosshair_text = 3, // the same, text only (a grabbed reference)
    hud_load_door = 4,      // SetLoadDoorInfo(bool, text)
    hud_subtitle = 5,       // ShowSubtitle(text)
    hud_subtitle_hide = 6,  // HideSubtitle()
    hud_arrows = 7,         // ShowArrowCount(count, hide, text)
    hud_enemy_target = 8,   // EnemyHealth
    hud_subtitles_enabled = 9,
    hud_meter_blink = 11,
    hud_fade_stamina = 12,
    hud_refresh_meters = 13,
    hud_quest_started = 14,
    hud_quest_completed = 15,
    hud_quest_failed = 16,
    hud_objective = 17,
    hud_objective_completed = 18,
    hud_objective_failed = 19,
    hud_skill = 20,
    hud_word_of_power = 21,
    hud_dragon_soul = 22,
    hud_mode = 23, // ShowElements(mode, on)
    hud_stealth = 24,
    hud_charge_fade = 25,
    hud_location_discovered = 26,
    hud_favor = 27,
    hud_crosshair_alert = 28,
    hud_location_name = 29, // SetLocationName(text)
    hud_tutorial_hint = 30, // ShowTutorialHintText(text, bool)
    hud_crosshair_enabled = 31,
    hud_opacity = 32,
    hud_refresh_activate_art = 33,
    hud_reset = 34,
    hud_survival = 35,
};

// A compass marker as the marker builders hand it over: the reference's
// position and the marker's frame in the "Compass Marker" clip.
struct CompassMarker {
    double position[3];
    double frame;
};

class HudMenu final : public Menu {
  public:
    // Constructor: movie HUDMenu, ExactFit (2), background alpha 0, flags 0x18902, depth 2; the
    // input context is not set (19).
    explicit HudMenu(Ui& ui) : Menu(ui, {"HUD Menu", "HUDMenu", ScaleMode::exact_fit, 2, 0x18902, 0, 19}) {}

    // HUDMenu::ProcessMessage answers 2 ("pass on") to everything, key events included: they reach the
    // menu below it, such as the Book Menu at depth 1.
    bool pass_through() const override { return true; }

    void on_show() override {
        // The game resets the HUD when a game has loaded (the game from).
        message(hud_reset);
    }

    void advance(double dt, double engine_ms) override {
        // The UI's per-frame update message (type 0, no data) reaches the HUD from the game before
        // the menus advance: markers, then each HUD object's Update.
        if (player() && m_root.is_object()) {
            if (m_survival == 1) {
                show_survival(); // the type-35 message the last advance posted
            }
            if (!m_ui.host().ui_item_source()) { // a running game has its own events
                m_capture.play(m_frame, [this](const json::Value& e) { apply(e); });
            }
            m_time += dt;
            update();
            ++m_frame;
        }
        Menu::advance(dt, engine_ms);
        // The game: the survival values (default objects 328, 331-334) against the
        // last ones sent, which start at -1: the first advance posts type 35, handled with the
        // next messages.
        if (m_survival == 0) {
            m_survival = 1;
        }
        double t = 0, c = 0, h = 0, e = 0;
        const bool on = m_ui.host().ui_survival(t, c, h, e);
        if (m_survival == 2 && (on != m_survival_on || t != m_survival_values[0] || c != m_survival_values[1] ||
                                h != m_survival_values[2] || e != m_survival_values[3])) {
            m_survival = 1; // a changed value posts type 35 again
        }
        m_survival_on = on;
        m_survival_values = {t, c, h, e};
    }

    // HUD data type 35: ShowSurvivalElements(level == 1, {aTemperatureLevel, aColdPenaltyPercent,
    // aHungerPenaltyPercent, aExhaustionPenaltyPercent}, changed); survival mode off: level 0, no penalties.
    void show_survival() {
        m_survival = 2;
        {
            auto& vm = player()->vm;
            Object* data = vm.make_object();
            const char* names[4] = {"aTemperatureLevel", "aColdPenaltyPercent", "aHungerPenaltyPercent",
                                    "aExhaustionPenaltyPercent"};
            for (size_t i = 0; i < 4; ++i) {
                vm.set(Value::object(data), names[i], Value::number(m_survival_values[i]));
            }
            call("ShowSurvivalElements", {Value::boolean(m_survival_on), Value::object(data), Value::boolean(true)});
        }
    }

    // A HUD data message to the menu (HUDMenu::ProcessMessage).
    void message(int type, const std::string& text_value = {}, bool flag_value = false, int value = 0) {
        if (!player() || !m_root.is_object()) {
            return;
        }
        auto& vm = player()->vm;
        switch (type) {
        case hud_message:
            call("ShowMessage", {Value::string(text_value)});
            break;
        case hud_load_door:
            call("SetLoadDoorInfo", {Value::boolean(flag_value), Value::string(text_value)});
            break;
        case hud_subtitle:
            call("ShowSubtitle", {Value::string(text_value)});
            break;
        case hud_subtitle_hide:
            call("HideSubtitle", {});
            break;
        case hud_subtitles_enabled:
            call("SetSubtitlesEnabled", {Value::boolean(!flag_value)});
            break;
        case hud_mode:
            call("ShowElements", {Value::string(text_value), Value::boolean(flag_value)});
            break;
        case hud_location_name:
            call("SetLocationName", {Value::string(text_value)});
            break;
        case hud_tutorial_hint:
            call("ShowTutorialHintText", {Value::string(text_value), Value::boolean(flag_value)});
            break;
        case hud_crosshair_enabled:
            call("SetCrosshairEnabled", {Value::boolean(m_crosshair_enabled)});
            break;
        case hud_opacity:
            vm.set(m_root, "_alpha", Value::number(m_opacity * 100.0));
            break;
        case hud_refresh_activate_art:
            refresh_activate_art();
            break;
        case hud_crosshair_alert:
            vm.call_method(m_root, "gotoAndStop", {Value::string(flag_value ? "Normal" : "Alert")});
            call("ValidateCrosshair", {});
            break;
        case hud_reset:
            reset();
            break;
        case hud_refresh_meters:
            // ActorValueMeter::ProcessMessage: type 13 forces the three actor-value meters (not the shout
            // meter; measured when Sleep/Wait closes).
            for (Meter& m : m_meters) {
                if (m.actor_value) {
                    set_meter(m, true);
                }
            }
            break;
        case hud_meter_blink:
            // The meter whose actor value the message carries blinks; any other blink flashes the shout
            // meter.
            if (value == 25 || value == 26) {
                call(value == 25 ? "StartMagickaBlinking" : "StartStaminaBlinking", {});
            } else {
                call("FlashShoutMeter", {});
            }
            break;
        default:
            break;
        }
    }

    // A HUD mode on or off (HUD data type 23).
    void set_mode(const std::string& mode, bool on) { message(hud_mode, mode, on); }
    void stealth(bool sneaking, double level) { set_stealth(sneaking, level); }
    // SetCrosshairTarget's ten values for the target the player points at.
    void crosshair(const CrosshairTarget& t) {
        const auto number = [](const std::optional<double>& v) { return v ? Value::number(*v) : Value{}; };
        call("SetCrosshairTarget",
             {Value::boolean(!t.hostile && !t.text.empty()), Value::string(t.hostile ? std::string() : t.text),
              Value::boolean(!t.hostile && !t.text.empty()),
              Value::boolean(false), Value::boolean(false), Value::boolean(true), number(t.weight), number(t.value),
              number(t.field_value), t.field_text.empty() ? Value{} : Value::string(t.field_text)});
    }

    // HUD data type 20 from the game: sSkillIncreased "%s Increased to %i" with
    // the skill's name and new level. HUDNotifications queues it for iObjectivesWaitTime (250 ms) and shows
    // it as type 1 with the player's level and the level meter from the remembered XP to the current one over
    // the level's threshold (start 0 when the remembered XP is the larger), then remembers the current XP.
    void post_skill_increase(const std::string& skill, int new_level, int player_level, double xp) {
        if (!player()) {
            return;
        }
        // The threshold: level x fXPLevelUpMult + fXPLevelUpBase, Skyrim.esm's 25 and 75 (the game's
        // defaults are 30 and 170; the live values measured with the game's settings dump).
        const double threshold = player_level * 25.0 + 75.0;
        const double start = m_xp_remembered > xp ? 0.0 : m_xp_remembered / threshold;
        m_xp_remembered = xp;
        std::string text_value;
        const std::string format = m_ui.game_setting("sSkillIncreased");
        for (size_t i = 0; i < format.size(); ++i) {
            if (format[i] == '%' && i + 1 < format.size()) {
                const char c = format[++i];
                text_value += c == 's'               ? skill
                              : c == 'i' || c == 'd' ? std::to_string(new_level)
                                                     : std::string(1, c);
            } else {
                text_value += format[i];
            }
        }
        m_notifications.push_back(
            {m_time + 0.25,
             {Value::string(text_value), Value::string(""), Value::string("UISkillIncrease"),
              Value::number(0), Value::number(1), Value::number(player_level), Value::number(start),
              Value::number(xp / threshold), Value{}}});
    }

    // The game: "%s %s" of the name and sDiscoveredText; HUDNotifications picks the
    // music by the marker's type; only types 20 and 21 fill in the rest of the nine values. Queued for iObjectivesWaitTime.
    void post_location_discovered(const std::string& name, int type) {
        if (!player()) {
            return;
        }
        const char* sound = type == 1                                                       ? "MUSDiscoveryCity"
                            : type == 2                                                     ? "MUSDiscoveryTown"
                            : type == 4 || type == 7 || type == 8 || type == 12 || type == 15 ? "MUSDiscoveryDungeon"
                                                                                              : "MUSDiscoveryGeneric";
        m_notifications.push_back({m_time + 0.25,
                                   {Value::string(name + " " + m_ui.game_setting("sDiscoveredText")), Value::string(""),
                                    Value::string(sound), Value::number(0), Value{}, Value{}, Value{}, Value{},
                                    Value{}}});
    }
    // The game: identical texts within 5 s are dropped, then HUD data type 1.
    void post_notification(const std::string& text) {
        if (text == m_last_notification && m_time < m_last_notification_time + 5.0) {
            return;
        }
        m_last_notification = text;
        m_last_notification_time = m_time;
        message(hud_message, text);
    }
    void post_location_name(const std::string& name) { message(hud_location_name, name); }
    // HUDNotifications::ProcessMessage, types 14 to 19.
    void post_quest_notice(int type, uint32_t quest, const std::string& text) {
        if (!player()) {
            return;
        }
        if (type <= hud_quest_failed) {
            const char* status = type == hud_quest_started ? "sHUDStarted" : type == hud_quest_completed ? "sHUDCompleted" : "sHUDFailed";
            const char* sound = type == hud_quest_started ? "UIQuestNew" : type == hud_quest_completed ? "UIQuestComplete" : "UIQuestUpdate";
            Queued q{m_time + 0.25,
                     {Value::string(text), Value::string(m_ui.game_setting(status)), Value::string(sound), Value::number(0), Value{}, Value{},
                      Value{}, Value{}, Value{}}};
            q.quest = quest;
            m_notifications.push_back(std::move(q));
            return;
        }
        const char* status = type == hud_objective ? nullptr : type == hud_objective_completed ? "sHUDCompleted" : "sHUDFailed";
        const std::pair<std::string, std::string> objective{text, status ? m_ui.game_setting(status) : std::string()};
        // Onto a queued line of the same quest, else any queued objective line, else one of its own.
        for (Queued& q : m_notifications) {
            if (q.quest == quest) {
                q.objectives.push_back(objective);
                return;
            }
        }
        for (Queued& q : m_notifications) {
            if (q.objective_line) {
                q.objectives.push_back(objective);
                return;
            }
        }
        Queued q{m_time + 0.25,
                 {Value::string(""), Value::string(""), Value::string(""), Value::number(0), Value{}, Value{}, Value{}, Value{}, Value{}}};
        q.objective_line = true;
        q.objectives.push_back(objective);
        m_notifications.push_back(std::move(q));
    }
    void post_tutorial_hint(const std::string& text) { message(hud_tutorial_hint, text, !text.empty()); }

    // The XP HUDNotifications remembers: read on reset; the level meter of the next skill
    // notification starts there.
    void remember_xp(double xp) { m_xp_remembered = xp; }

  private:
    struct Meter {
        int actor_value;     // 24 health, 25 magicka, 26 stamina; 0 the shout meter
        const char* method;  // the meter's Set<...>MeterPercent
        double percent;      // what the game system reports (GetPercent)
        double last = 100.0; // the last percent sent (HUDMeter)
    };

    void register_handlers() override {
        handle("PlaySound", [this](Call& c) {
            if (c.args.size() == 1) {
                m_ui.host().ui_play_sound(*player()->vm.to_string(c.arg(0)));
            }
        });
        handle("RegisterHUDComponents", [this](Call& c) {
            // The game: value 0 is the root every engine call goes to; the
            // objects keep the others.
            m_root = c.arg(0);
            m_hud_elements = c.arg(1);
            m_quest_update = c.arg(2);
            m_enemy_meter = c.arg(3);
            m_stealth = c.arg(4);
            m_sneak_anim = c.arg(5);
            m_brackets = c.arg(6);
            m_rollover_name = c.arg(7);
            m_sneak_text_holder = c.arg(8);
            m_sneak_text = c.arg(9);
            register_components();
        });
        handle("GetButtonFromUserEvent", [this](Call& c) {
            // The game: responds with the button bound to the user event
            // (keyboard, then mouse, without a gamepad), or the event's own name when none is.
            const std::string event = *player()->vm.to_string(c.arg(0));
            const std::string button = m_ui.button_name(event);
            c.respond({Value::string(button.empty() ? event : button)});
        });
        handle("myLog", [](Call&) {});
    }

    void on_loaded() override {
        // The constructor's steps after the movie loaded: RegisterComponents (answered at once by
        // RegisterHUDComponents), the floating quest markers' clips, then HUD data type 31 to itself.
        invoke("HUDMovieBaseInstance.RegisterComponents");
        init_floating_markers();
        const json::Value* cap = m_ui.hud_recording() ? m_ui.capture("hud") : nullptr;
        if (m_ui.hud_recording() && !cap) {
            log::info("ui: HUD Menu: no capture 'hud' in the cache; the HUD shows no game data");
        }
        m_capture.load(cap);
        message(hud_crosshair_enabled);
    }

    void register_components() {
        auto& vm = player()->vm;
        // Compass: the marker data array and the six marker frame numbers the movie measured.
        m_compass_data = vm.get(m_root, "CompassTargetDataA");
        m_frame_quest = vm.to_number(vm.get(m_root, "CompassMarkerQuest"));
        m_frame_quest_door = vm.to_number(vm.get(m_root, "CompassMarkerQuestDoor"));
        m_frame_player_set = vm.to_number(vm.get(m_root, "CompassMarkerPlayerSet"));
        m_frame_locations = vm.to_number(vm.get(m_root, "CompassMarkerLocations"));
        m_frame_undiscovered = vm.to_number(vm.get(m_root, "CompassMarkerUndiscovered"));
        // HUDNotifications: the objectives array, sized to 40.
        if (m_quest_update.is_object()) {
            const Value objectives = vm.get(m_quest_update, "ObjectivesA");
            if (objectives.is_object()) {
                objectives.o->elements.resize(40);
            }
        }
    }

    // FloatingQuestMarker::Init: 48 clips "target0". "target47" of "quest target_HUD" under
    // FloatingQuestMarkerInstance, each made invisible (alpha 0).
    void init_floating_markers() {
        auto& vm = player()->vm;
        const Value holder = vm.get(m_root, "FloatingQuestMarkerInstance");
        if (!holder.is_object()) {
            return;
        }
        for (int i = 0; i < 48; ++i) {
            const Value depth = vm.call_method(holder, "getNextHighestDepth", {});
            const Value clip = vm.call_method(
                holder, "attachMovie",
                {Value::string("quest target_HUD"), Value::string(std::format("target{}", i)), depth});
            if (clip.is_object()) {
                vm.set(clip, "_alpha", Value::number(0));
            }
        }
    }

    // The full reset (HUD data type 34): messages, hint, subtitle, modes back to "All", meters
    // forced, the activate art, crosshair and subtitles enabled.
    void reset() {
        auto& vm = player()->vm;
        const Value messages = vm.get(m_root, "MessagesInstance");
        if (messages.is_object()) {
            const Value queued = vm.get(messages, "MessageArray");
            if (queued.is_object()) {
                queued.o->elements.clear();
            }
            const Value shown = vm.get(messages, "ShownMessageArray");
            if (shown.is_object()) {
                for (const Value& e : shown.o->elements) {
                    if (e.is_object()) {
                        vm.set(e, "_visible", Value::boolean(false));
                    }
                }
            }
        }
        // In the order measured in the game (run hud-explore1, load.trace.json, the frame after a load): what
        // the reset calls itself, then the messages it posted (modes, the activate art, favor mode off).
        call("HideSubtitle", {}); // the subtitle manager, on the same load
        call("ShowTutorialHintText", {Value::string(""), Value::boolean(false)});
        set_stealth(false, 0); // HUD data type 24: not sneaking (no movie call while not sneaking)
        call("ShowSubtitle", {Value::string(" ")});
        const Value modes = vm.get(m_root, "HUDModes");
        if (modes.is_object()) {
            modes.o->elements.clear();
        }
        for (Meter& m : m_meters) {
            set_meter(m, true);
        }
        // HUDNotifications' reset (slot 4): the queue emptied, SetVariable of the letters' quest
        // names on these paths, the XP re-read (spud has no player XP: whoever knows it hands it over,
        // remember_xp).
        m_notifications.clear();
        if (player()->has_path("QuestNotification.AnimLetter")) {
            player()->set_path("QuestNotification.AnimLetter.QuestName", Value::string(""));
            player()->set_path("QuestNotification.ShoutLetter.QuestName", Value::string(""));
        }
        call("SetCrosshairEnabled", {Value::boolean(m_crosshair_enabled)});
        call("SetSubtitlesEnabled", {Value::boolean(true)});
        set_mode("All", true);
        refresh_activate_art();
        call("ShowElements", {Value::string("Favor"), Value::boolean(false)}); // The game(false)
    }

    // RefreshActivateButtonArt(the button bound to "Activate", undefined if none).
    void refresh_activate_art() {
        const std::string button = m_ui.button_name("Activate");
        call("RefreshActivateButtonArt", {button.empty() ? Value{} : Value::string(button)});
    }

    // HUDMeter::SetPercent: sent when the percent changed, when forced, and every frame below
    // 100.
    void set_meter(Meter& m, bool force) {
        if (m.actor_value) {
            m_ui.host().ui_meter_percent(m.actor_value, m.percent);
        }
        const double p = m.percent;
        if (p != m.last || force || p < 100.0) {
            m.last = p;
            m_args.clear();
            m_args.push_back(Value::number(p));
            m_args.push_back(Value::boolean(force));
            player()->vm.call_method(m_root, m.method, m_args);
        }
    }

    // StealthMeter (HUD data type 24): fades in or out on a change of sneaking; the level is
    // followed in Update.
    void set_stealth(bool sneaking, double level) {
        auto& vm = player()->vm;
        if (sneaking != m_sneaking && m_stealth.is_object()) {
            vm.call_method(m_stealth, "gotoAndPlay", {Value::string(sneaking ? "FadeIn" : "FadeOut")});
            set_mode("StealthMode", sneaking);
            if (sneaking) {
                m_stealth_shown = level;
            }
        }
        m_sneaking = sneaking;
        m_stealth_target = level;
    }

    // The per-frame update (the type-0 message without data, and the objects' Update).
    void update() {
        auto& vm = player()->vm;
        // The meters: ActorValueMeter / ShoutMeter Update = SetPercent(false).
        for (Meter& m : m_meters) {
            set_meter(m, false);
        }
        update_enemy();
        update_compass();
        // The game posts type 24. Level 0: spud has no detection yet,
        // so nothing ever sees the player.
        const bool sneaking = m_ui.host().ui_player_sneaking();
        if (m_capture.empty() && sneaking != m_sneaking) {
            set_stealth(sneaking, 0);
        }
        // HUDNotifications::Update: when the movie can show one, the first notification whose
        // time has come (at most one per frame).
        if (m_quest_update.is_object()) {
            const bool can = vm.to_boolean(vm.call_method(m_quest_update, "CanShowNotification", m_no_args));
            if (can) {
                // Due once its time has passed (strictly: measured, a notification posted 250 ms ahead showed
                // on the 16th frame at 1/60 s, not the 15th; the epsilon absorbs the summed frame times'
                // rounding).
                for (auto it = m_notifications.begin(); it != m_notifications.end(); ++it) {
                    if (m_time > it->due + 1e-6) {
                        if (!it->objectives.empty()) {
                            // ObjectivesA: text and status in turn, at most 20 objectives.
                            const Value a = vm.get(m_quest_update, "ObjectivesA");
                            const size_t count = std::min<size_t>(it->objectives.size(), 20);
                            if (a.is_object()) {
                                for (size_t i = 0; i < count; ++i) {
                                    a.o->elements[i * 2] = Value::string(it->objectives[i].first);
                                    a.o->elements[i * 2 + 1] = Value::string(it->objectives[i].second);
                                }
                            }
                            it->args[3] = Value::number(double(count));
                        }
                        vm.call_method(m_quest_update, "ShowNotification", it->args);
                        m_notifications.erase(it);
                        break;
                    }
                }
            }
        }
        // StealthMeter::Update: the shown level follows the target by 0.3 of the difference per
        // frame (or jumps when that is below 1); HIDDEN at 0 and DETECTED at 100 when it arrives there.
        if (m_sneaking && m_sneak_anim.is_object()) {
            const double step = std::trunc((m_stealth_shown - m_stealth_target) * 0.3);
            const double before = m_stealth_shown;
            m_stealth_shown = std::abs(step) < 1.0 ? m_stealth_target : m_stealth_shown - step;
            if (m_stealth_shown != before && (m_stealth_shown == 0.0 || m_stealth_shown == 100.0)) {
                vm.call_method(m_sneak_text, "SetText",
                               {Value::string(m_stealth_shown == 0.0 ? "$HIDDEN" : "$DETECTED")});
                vm.call_method(m_sneak_text_holder, "gotoAndPlay", {Value::string("Start")});
            }
            vm.call_method(m_sneak_anim, "gotoAndStop", {Value::number(std::trunc(m_stealth_shown * 1.01))});
        }
    }

    // EnemyHealth::Update: a new target sets the meter, shows the brackets sized to its name and
    // the name; afterwards the meter follows changes; no target hides it (percent 0, brackets alpha 0).
    void update_enemy() {
        auto& vm = player()->vm;
        if (!m_enemy_meter.is_object()) {
            return;
        }
        if (!m_enemy.present) {
            // EnemyHealth::Hide runs every frame without a target (measured: SetPercent(0) and
            // the brackets' alpha 0 on each frame of the recording).
            m_enemy.shown = false;
            m_args.clear();
            m_args.push_back(Value::number(0));
            vm.call_method(m_enemy_meter, "SetPercent", m_args);
            vm.set(m_brackets, "_alpha", Value::number(0));
            return;
        }
        const double p = std::clamp(m_enemy.percent, 1.0, 100.0);
        if (!m_enemy.shown || m_enemy.changed_target) {
            m_enemy.shown = true;
            m_enemy.changed_target = false;
            m_enemy.last = p;
            vm.call_method(m_enemy_meter, "SetPercent", {Value::number(p)});
            vm.set(m_brackets, "_alpha", Value::number(100));
            const double len = double(std::min<size_t>(m_enemy.name.size(), 50));
            vm.call_method(m_brackets, "gotoAndStop", {Value::number((len + 5.0) * 3.5)});
            vm.call_method(m_rollover_name, "SetText", {Value::string(m_enemy.name)});
        } else if (p != m_enemy.last) {
            m_enemy.last = p;
            vm.call_method(m_enemy_meter, "SetTargetPercent", {Value::number(p)});
        }
    }

    // Compass::Update: the markers into CompassTargetDataA (heading, alpha, frame, scale per
    // marker), SetCompassMarkers(), then SetCompassAngle(camera yaw, yaw + the cell's north rotation,
    // bShowCompass).
    void update_compass() {
        auto& vm = player()->vm;
        float pos[3] = {float(m_player[0]), float(m_player[1]), float(m_player[2])};
        float yaw = float(m_yaw_deg / k_rad_to_deg);
        if (m_ui.host().ui_player_pose(pos, yaw)) {
            m_player[0] = pos[0];
            m_player[1] = pos[1];
            m_player[2] = pos[2];
            m_yaw_deg = double(yaw) * k_rad_to_deg;
            m_north_deg = double(m_ui.host().ui_cell_north()) * k_rad_to_deg;
        }
        // The game: the frame by discovery, then the marker's type.
        m_live_markers.clear();
        if (m_ui.host().ui_compass_markers(m_live_markers)) {
            m_markers.clear();
            for (const CompassEntry& e : m_live_markers) {
                m_markers.push_back({{e.position[0], e.position[1], e.position[2]},
                                     e.quest_door ? m_frame_quest_door
                                     : e.quest    ? m_frame_quest
                                     : e.player_set
                                         ? m_frame_player_set
                                         : (e.can_travel ? m_frame_locations : m_frame_undiscovered) + e.type - 1.0});
            }
        }
        if (m_compass_data.is_object()) {
            std::vector<Value>& data = m_compass_data.o->elements;
            data.resize(m_markers.size() * 4);
            for (size_t i = 0; i < m_markers.size(); ++i) {
                const CompassMarker& mk = m_markers[i];
                double v[4];
                marker_values(mk, v);
                for (int k = 0; k < 4; ++k) {
                    data[i * 4 + size_t(k)] = Value::number(v[k]);
                }
            }
            vm.call_method(m_root, "SetCompassMarkers", m_no_args);
        }
        const double yaw_deg = wrap_degrees(m_yaw_deg);
        const double compass_deg = wrap_degrees(m_yaw_deg + m_north_deg);
        m_args.clear();
        m_args.push_back(Value::number(yaw_deg));
        m_args.push_back(Value::number(compass_deg));
        m_args.push_back(Value::boolean(true)); // bShowCompass:Interface
        vm.call_method(m_root, "SetCompassAngle", m_args);
    }

    // The game: heading clockwise from north in degrees; alpha and scale fall
    // linearly in the squared distance up to iMapMarkerFadeStartDistance, with the quest or the map-marker
    // limits.
    void marker_values(const CompassMarker& m, double out[4]) const {
        const double dx = m.position[0] - m_player[0], dy = m.position[1] - m_player[1],
                     dz = m.position[2] - m_player[2];
        double h;
        if (dy == 0.0) {
            h = dx > 0 ? std::numbers::pi / 2 : dx < 0 ? 3 * std::numbers::pi / 2 : 0.0;
        } else {
            h = std::atan(dx / dy);
            if (dy < 0) {
                h += std::numbers::pi;
            }
        }
        if (h < 0) {
            h += 2 * std::numbers::pi;
        }
        // iMapMarkerFadeStartDistance: 7500 in the game, 10000 from Skyrim.esm (the live value,
        // measured with the game's settings dump; the cache holds no numeric game settings).
        const double r2 = 10000.0 * 10000.0;
        const double d2 = dx * dx + dy * dy + dz * dz;
        const double t = (std::min(d2, r2) - r2) / r2;
        const bool quest = m.frame == m_frame_quest || m.frame == m_frame_quest_door;
        // fMapQuestMarkerMinFadeAlpha / MinPercentSize / MaxPercentSize, fMapMarkerMinFadeAlpha / ...
        const double a = quest ? 90.0 : 40.0, s0 = quest ? 100.0 : 90.0, s1 = quest ? 150.0 : 130.0;
        out[0] = float(h) * float(k_rad_to_deg);
        out[1] = a + (a - 100.0) * t;
        out[2] = m.frame;
        out[3] = s0 - (s1 - s0) * t;
    }

    // One recorded entry.
    void apply(const json::Value& e) {
        auto& vm = player()->vm;
        if (const json::Value* p = member(&e, "pose")) {
            for (int i = 0; i < 3 && i < int(p->items.size()); ++i) {
                m_player[i] = p->items[size_t(i)].n;
            }
        }
        if (const json::Value* y = member(&e, "yaw")) {
            m_yaw_deg = y->n;
        }
        if (const json::Value* n = member(&e, "north")) {
            m_north_deg = n->n;
        }
        if (const json::Value* meters = member(&e, "meters")) {
            for (Meter& m : m_meters) {
                if (const json::Value* v =
                        member(meters, m.actor_value ? std::to_string(m.actor_value) : "shout")) {
                    m.percent = v->n;
                }
            }
        }
        if (const json::Value* markers = member(&e, "markers")) {
            m_markers.clear();
            for (const json::Value& mk : markers->items) {
                CompassMarker c{};
                const json::Value* p = member(&mk, "pos");
                for (int i = 0; p && i < 3 && i < int(p->items.size()); ++i) {
                    c.position[i] = p->items[size_t(i)].n;
                }
                c.frame = number(member(&mk, "frame"));
                m_markers.push_back(c);
            }
        }
        if (const json::Value* s = member(&e, "stealth")) {
            set_stealth(flag(member(s, "sneaking")), number(member(s, "level")));
        }
        if (const json::Value* en = member(&e, "enemy")) {
            // The enemy health target (the player's combat target or HUD data type 8): name and health
            // percent.
            const bool present = en->kind == json::Value::object;
            const std::string name = text(member(en, "name"));
            m_enemy.changed_target = present && (!m_enemy.present || name != m_enemy.name);
            m_enemy.present = present;
            m_enemy.name = name;
            m_enemy.percent = number(member(en, "percent"), 100);
        }
        if (const json::Value* ch = member(&e, "charge")) {
            // HUDChargeMeter::Update: SetChargeMeterPercent(percent, force, left hand, shown).
            call("SetChargeMeterPercent",
                 {Value::number(number(member(ch, "percent"))), Value::boolean(flag(member(ch, "force"))),
                  Value::boolean(flag(member(ch, "left"))), Value::boolean(flag(member(ch, "show"), true))});
        }
        if (const json::Value* h = member(&e, "hud")) {
            const int type = int(h->n);
            if (type == hud_crosshair || type == hud_crosshair_text) {
                // The ten values the menu computed from the target (activate, name, show button, text only,
                // favor mode, show crosshair, weight, value, field text, field value).
                std::vector<Value> args;
                if (const json::Value* a = member(&e, "args")) {
                    for (const json::Value& v : a->items) {
                        args.push_back(json_value(vm, v));
                    }
                }
                vm.call_method(m_root, "SetCrosshairTarget", args);
            } else if (type == hud_arrows) {
                call("ShowArrowCount",
                     {Value::number(number(member(&e, "value"))), Value::boolean(flag(member(&e, "flag"))),
                      Value::string(text(member(&e, "text")))});
            } else {
                message(type, text(member(&e, "text")), flag(member(&e, "flag")),
                        int(number(member(&e, "value"))));
            }
        }
        if (const json::Value* n = member(&e, "notification")) {
            // HUDNotifications::Update: the objectives, then ShowNotification with nine values.
            if (const json::Value* objectives = member(&e, "objectives")) {
                const Value a = vm.get(m_quest_update, "ObjectivesA");
                if (a.is_object()) {
                    for (size_t i = 0; i < objectives->items.size() && i < a.o->elements.size(); ++i) {
                        a.o->elements[i] = json_value(vm, objectives->items[i]);
                    }
                }
            }
            std::vector<Value> args;
            for (const json::Value& v : n->items) {
                args.push_back(json_value(vm, v));
            }
            // Recorded when the game showed it, so due at once: shown by the next update that may show one.
            m_notifications.push_back({m_time, std::move(args)});
        }
    }

    void call(std::string_view method, std::vector<Value> args) {
        player()->vm.call_method(m_root, method, args);
    }

    Value m_root, m_hud_elements, m_quest_update, m_enemy_meter, m_stealth, m_sneak_anim, m_brackets,
        m_rollover_name, m_sneak_text_holder, m_sneak_text, m_compass_data;
    double m_frame_quest = -1, m_frame_quest_door = -1, m_frame_locations = -1, m_frame_undiscovered = -1,
           m_frame_player_set = -1;
    std::vector<CompassEntry> m_live_markers;
    std::string m_last_notification;
    double m_last_notification_time = 0;
    Meter m_meters[4] = {{24, "SetHealthMeterPercent", 100.0},
                         {25, "SetMagickaMeterPercent", 100.0},
                         {26, "SetStaminaMeterPercent", 100.0},
                         {0, "SetShoutMeterPercent", 100.0}};
    std::vector<CompassMarker> m_markers;
    struct Queued {
        double due;              // seconds of HUD time when it may show (posting time + iObjectivesWaitTime)
        std::vector<Value> args; // ShowNotification's nine values
        uint32_t quest = 0;      // a line of types 14 to 16
        bool objective_line = false;
        std::vector<std::pair<std::string, std::string>> objectives; // text, status
    };
    std::vector<Queued> m_notifications;
    double m_time = 0; // the HUD's frame times summed, seconds
    double m_xp_remembered = 0;
    double m_player[3] = {0, 0, 0};
    double m_yaw_deg = 0, m_north_deg = 0;
    bool m_crosshair_enabled = true; // bCrosshairEnabled
    double m_opacity = 1.0;          // fHUDOpacity
    bool m_sneaking = false;
    double m_stealth_shown = 0, m_stealth_target = 0;
    struct Enemy {
        bool present = false, shown = false, changed_target = false;
        std::string name;
        double percent = 100, last = 0;
    } m_enemy;
    Timeline m_capture;
    uint64_t m_frame = 0;
    int m_survival = 0;        // 0 nothing posted, 1 posted, 2 shown
    bool m_survival_on = false;
    std::array<double, 4> m_survival_values{}; // temperature, cold, hunger, exhaustion
    std::vector<Value> m_args; // reused each frame (no allocation once grown)
    const std::vector<Value> m_no_args;
};

// A notification on the HUD (HUD data type 1), with its sound.
void notify(Ui& ui, const std::string& text_value, const std::string& sound = {}) {
    if (!sound.empty()) {
        ui.host().ui_play_sound(sound);
    }
    if (Menu* m = ui.find_menu("HUD Menu"); m && !text_value.empty()) {
        static_cast<HudMenu*>(m)->message(hud_message, text_value);
    }
}

// ===========================================================================================================
// Sleep/Wait Menu (SleepWaitMenu, movie SleepWaitMenu)
// ===========================================================================================================

class SleepWaitMenu final : public Menu {
  public:
    // Constructor: ExactFit, flags 0x20901 plus 0x4 (uses cursor) without a gamepad, input context
    // 1, depth 4.
    explicit SleepWaitMenu(Ui& ui)
        : Menu(ui, {"Sleep/Wait Menu", "SleepWaitMenu", ScaleMode::exact_fit, 4,
                    0x20901u | (ui.gamepad_active() ? 0u : 0x4u), 0, 1}),
          m_blur(ui) {}
    ~SleepWaitMenu() override { hud_set_mode(m_ui, "SleepWaitMode", false); }

    void on_show() override {
        // Show: the sleeping flag of the show data (none: wait), SetSleeping(flag).
        call("SetSleeping", {Value::boolean(m_sleeping)});
        // The constructor posts the HUD's SleepWaitMode as a HUD data message, so the HUD movie gets it after
        // SetSleeping (measured, run hud-explore1 sw.trace: SetCurrentTime, SetSleeping, ShowElements in one
        // frame).
        hud_set_mode(m_ui, "SleepWaitMode", true);
    }

    bool on_hide() override {
        // Hide: a wait in progress is interrupted (UIMenuCancel); after a sleep the black fade lifts again;
        // the HUD refreshes its meters (HUD data type 13).
        if (m_waiting) {
            m_waiting = false;
            m_ui.host().ui_play_sound("UIMenuCancel");
        }
        if (m_sleeping && m_ui.is_open("Fader Menu")) {
            fade(false);
        }
        if (Menu* hud = m_ui.find_menu("HUD Menu")) {
            static_cast<HudMenu*>(hud)->message(hud_refresh_meters);
        }
        return true;
    }

    void advance(double dt, double engine_ms) override {
        // The update message every frame (flag 0x100): while time passes, one step of
        // iSecondsToSleepPerUpdate (900 game seconds) whenever a second of frame time has gathered, then the
        // slider and the time text per hour.
        if (m_waiting) {
            // A float sum of the frame times, as the game keeps it: at 1/60 s it reaches 1.0 on the
            // 61st frame (measured: an hour every 64 frames, 61 to gather plus 3 more steps).
            m_accumulated += float(dt);
            if (m_accumulated >= 1.0f) {
                tick();
                const int shown = int(player()->vm.to_number(call("getSliderValue", {})));
                const int left = int((m_seconds_left + 3599.0) / 3600.0);
                if (left <= 0) {
                    m_accumulated = 0;
                    m_waiting = false;
                    m_ui.host().ui_rest_ended(m_sleeping, false);
                    m_ui.close(name());
                } else if (shown > left) {
                    m_accumulated = 0;
                    call("modifySliderValue", {Value::number(-1.0)});
                    update_current_time();
                }
            }
        }
        Menu::advance(dt, engine_ms);
    }

  private:
    BlurHold m_blur;
    void register_handlers() override {
        handle("OK", [this](Call& c) {
            // UIMenuOK, then StartSleep / StartWait for the hours (a sleep first fades to black
            // over fSleepFaderTime, 0.7 s, and stays black).
            if (m_waiting) {
                return;
            }
            m_ui.host().ui_play_sound("UIMenuOK");
            const int hours = int(player()->vm.to_number(c.arg(0)));
            if (m_sleeping) {
                fade(true);
            }
            m_seconds_left = std::max(0.0, 3600.0 * hours);
            m_ui.host().ui_autosave(m_sleeping ? "rest" : "wait");
            m_ui.host().ui_rest_started(m_sleeping, hours);
            m_waiting = true;
            m_accumulated = 0;
        });
        handle("Cancel", [this](Call&) { m_ui.close(name()); });
        handle("PlaySound", [this](Call& c) {
            if (c.args.size() == 1) {
                m_ui.host().ui_play_sound(*player()->vm.to_string(c.arg(0)));
            }
        });
    }

    void on_loaded() override {
        // After loading: SleepWaitMenu_mc, SetCurrentTime(the date without the year), the UI blur, the HUD
        // mode.
        m_obj = player()->get_path("SleepWaitMenu_mc");
        update_current_time();
    }

    // The game: the calendar moves on by iSecondsToSleepPerUpdate (900)
    // game seconds; with no more than one step left the sleep or wait ends.
    void tick() {
        constexpr double k_step = 900.0; // iSecondsToSleepPerUpdate
        m_ui.host().ui_pass_time(k_step / 3600.0);
        if (m_seconds_left <= k_step) {
            m_seconds_left = 0;
        } else {
            m_seconds_left -= k_step;
        }
    }

    void update_current_time() {
        const Calendar c = m_ui.calendar(m_ui.host().ui_player_info());
        call("SetCurrentTime", {Value::string(m_ui.date_text(c, false))});
    }

    // initFade(from opaque, black, fSleepFaderTime, 0); to black on OK, staying up while the
    // player sleeps (stay true), and back on close (stay false).
    void fade(bool to_black) { m_ui.request_fade({!to_black, true, 0.7, 0.0, to_black}); }

    Value call(std::string_view method, std::vector<Value> args) {
        return m_obj.is_object() ? player()->vm.call_method(m_obj, method, args) : Value{};
    }

    Value m_obj;
    bool m_sleeping = m_ui.take_sleep_request(); // the show data's byte; spud opens it from the Wait key only (no beds yet)
    bool m_waiting = false;
    float m_accumulated = 0;
    double m_seconds_left = 0;
};

// ===========================================================================================================
// Training Menu (TrainingMenu, movie TrainingMenu)
// ===========================================================================================================

// What the trainer and the player bring: the trainer's class (the skill it teaches and how high), the
// player's skill, gold and trainings this level. Recorded in the game (capture `training`) until spud has
// actors and skills.
struct TrainingState {
    bool valid = false;
    std::string skill_name; // the actor value's name ("One-Handed")
    int max_level = 0;
    double base = 0, permanent = 0;
    double progress = 0;   // the skill's XP / threshold x 100
    std::string gold_name; // the gold form's name, for the "removed" notice
    double gold = 0;
    int times_trained = 0; // player
    int player_level = 1;
    double xp = 0; // the player's XP towards the next level (the game level-up XP)
};

class TrainingMenu final : public Menu {
  public:
    // Constructor: ExactFit, flags 0x409 plus 0x4 without a gamepad, depth 3, input context not
    // set.
    explicit TrainingMenu(Ui& ui)
        : Menu(ui, {"Training Menu", "TrainingMenu", ScaleMode::exact_fit, 3,
                    0x409u | (ui.gamepad_active() ? 0u : 0x4u), 0, 19}) {}
    ~TrainingMenu() override { hud_set_mode(m_ui, "BarterMode", false); }

    void on_show() override {
        // The game: the movie's text fields, then UpdateText and the skill meter.
        load_state();
        auto& vm = player()->vm;
        m_obj = player()->get_path("TrainingMenuObj");
        for (auto [name, out] :
             {std::pair{"SkillName", &m_skill_name}, std::pair{"TrainerSkill", &m_trainer_skill},
              std::pair{"SkillMeter", &m_skill_meter}, std::pair{"TimesTrained", &m_times},
              std::pair{"TrainCost", &m_cost}, std::pair{"CurrentGold", &m_gold}}) {
            *out = m_obj.is_object() ? vm.get(m_obj, name) : Value{};
        }
        update_text();
        if (m_skill_meter.is_object() && m_state.valid) {
            vm.call_method(m_skill_meter, "SetPercent", {Value::number(progress())});
        }
        if (Menu* hud = m_ui.find_menu("HUD Menu"); hud && m_state.valid) {
            // The HUD last read the player's XP when it reset or showed a skill notification: the XP now.
            static_cast<HudMenu*>(hud)->remember_xp(m_state.xp);
        }
    }

    bool on_user_event(const std::string& event) override {
        // ProcessMessage type 7: Accept trains, Cancel closes.
        if (event == "Accept") {
            train();
            return true;
        }
        if (event == "Cancel") {
            m_ui.close(name());
            return true;
        }
        return false;
    }

  private:
    void register_handlers() override {
        handle("Train", [this](Call&) { train(); });
        handle("Exit", [this](Call&) { m_ui.close(name()); });
    }

    void on_loaded() override { hud_set_mode(m_ui, "BarterMode", true); }

    void load_state() {
        if (TrainingCard card; m_ui.host().ui_training_card(card)) {
            m_live = m_state.valid = true;
            m_state.skill_name = card.skill_name;
            m_state.gold_name = card.gold_name;
            m_state.max_level = card.max_level;
            m_state.times_trained = card.times_trained;
            m_state.player_level = card.player_level;
            m_state.base = card.base;
            m_state.permanent = card.permanent;
            m_state.progress = card.progress;
            m_state.gold = card.gold;
            m_state.xp = card.xp;
            return;
        }
        const json::Value* c = m_ui.capture("training");
        if (!c) {
            log::info("ui: Training Menu: no capture 'training' in the cache; the card stays empty");
            return;
        }
        m_state.valid = true;
        m_state.skill_name = text(member(c, "skill_name"));
        m_state.max_level = int(number(member(c, "max_level")));
        m_state.base = number(member(c, "base"));
        m_state.permanent = number(member(c, "permanent"), m_state.base);
        m_state.progress = number(member(c, "progress"));
        m_state.gold_name = text(member(c, "gold_name"));
        m_state.gold = number(member(c, "gold"));
        m_state.times_trained = int(number(member(c, "times_trained")));
        m_state.player_level = int(number(member(c, "player_level"), 1));
        m_state.xp = number(member(c, "xp"));
    }

    // GetTrainingCost: multiplier x fTrainingMultCost x L + fTrainingBaseCost, the multiplier by
    // iTrainingJourneyman/Expert/MasterSkill (50, 75, 90) -> iTraining...Cost (1, 3, 5), else 1.
    // fTrainingMultCost is Skyrim.esm's 10.0 (GMST 0x10AFA8; the game's default is 2.0).
    static double cost(int level) {
        const double mult = level <= 50 ? 1 : level <= 75 ? 3 : level <= 90 ? 5 : 1;
        return mult * 10.0 * level + 50.0;
    }

    double progress() const { return m_state.progress; }

    // The game: each field's text, in this order.
    void update_text() {
        if (!m_state.valid) {
            return;
        }
        auto& vm = player()->vm;
        auto set = [&](const Value& field, Value v) {
            if (field.is_object()) {
                vm.set(field, "text", std::move(v));
            }
        };
        set(m_skill_name, Value::string(std::format("{} {:.0f}", m_state.skill_name, m_state.base)));
        const char* rank = m_state.max_level <= 50   ? "sJourneyman"
                           : m_state.max_level <= 75 ? "sExpert"
                                                     : "sMaster";
        set(m_trainer_skill, Value::string(m_ui.game_setting(rank)));
        set(m_times,
            Value::string(std::format("{}/{}", m_state.times_trained, 5))); // iTrainingNumAllowedPerLevel
        set(m_cost, Value::number(cost(int(m_state.permanent))));
        set(m_gold, Value::number(m_state.gold));
    }

    // TrainingMenu::Train: the three refusals, then the gold, the skill and the count.
    void train() {
        if (!m_state.valid) {
            return;
        }
        const int level = int(m_state.base);
        std::string refusal;
        if (m_state.times_trained >= 5) {
            refusal = "sCanNotTrainAnymore";
        } else if (m_state.max_level <= level) {
            refusal = "sCanNotTrainHigher";
        } else if (m_state.gold < cost(level)) {
            refusal = "sInsufficientGoldToTrain";
        }
        if (!refusal.empty()) {
            notify(m_ui, m_ui.game_setting(refusal));
            return;
        }
        // The game with the player's notice: measured "Gold (260) Removed" (the item's
        // name, the count, sRemoveItemfromInventory). The notice reaches the HUD through the UI message
        // queue, after the card's own calls (measured, run hud-explore1 tr.trace: SetTargetPercent(100), then
        // ShowMessage, in one frame).
        const std::string removed = std::format("{} ({}) {}", m_state.gold_name, int(cost(level)),
                                                m_ui.game_setting("sRemoveItemfromInventory"));
        if (m_live) {
            // The game's own skill notice follows from the skill going up.
            m_ui.host().ui_train(int(cost(level)));
            load_state();
            update_text();
            if (m_skill_meter.is_object()) {
                player()->vm.call_method(m_skill_meter, "SetTargetPercent", {Value::number(100.0)});
            }
            notify(m_ui, removed);
            return;
        }
        m_state.gold -= cost(level);
        m_state.base += 1;
        m_state.permanent += 1;
        ++m_state.times_trained;
        update_text();
        if (m_skill_meter.is_object()) {
            player()->vm.call_method(m_skill_meter, "SetTargetPercent", {Value::number(100.0)});
        }
        notify(m_ui, removed);
        // The skill's level-up: the player gains the new level x fXPPerSkillRank
        // (1.0) XP and the HUD posts the skill notification. Measured: One-handed 21 -> 22 at player level 1
        // showed the level meter from 0.21 to 0.43, 16 frames (250 ms queue time) after the training.
        m_state.xp += m_state.base * 1.0;
        if (Menu* hud = m_ui.find_menu("HUD Menu")) {
            static_cast<HudMenu*>(hud)->post_skill_increase(m_state.skill_name, int(m_state.base),
                                                            m_state.player_level, m_state.xp);
        }
    }

    TrainingState m_state;
    bool m_live = false; // the card comes from the game
    Value m_obj, m_skill_name, m_trainer_skill, m_skill_meter, m_times, m_cost, m_gold;
};

// ===========================================================================================================
// Lockpicking Menu (LockpickingMenu, movie LockpickingMenu)
// ===========================================================================================================

// The player's skill and picks and the lock: recorded in the game (capture `lockpicking`) until spud has
// skills, inventories and locks. The sweet spot's centre is a random draw in the game; the capture carries
// the drawn one.
struct LockState {
    bool valid = false;
    double skill = 15, xp_percent = 0;
    bool next_is_blank = false; // the skill's XP equals its threshold: the next level shows " "
    int level = 0;              // 0 Novice .. 4 Master, 5 Requires Key
    int picks = 0;
    double centre = 0, size = 0, partial = 0, break_seconds = 1;
    double health = 100; // g_LockpickingPickHealth: carried over between locks
    double start_pick = 0;
    bool unbreakable = false, skeleton_key = false;
};

class LockpickingMenu final : public Menu {
  public:
    // Constructor: ExactFit, flags 0x181, input context 15 (Lockpicking), depth not set (3).
    explicit LockpickingMenu(Ui& ui)
        : Menu(ui, {"Lockpicking Menu", "LockpickingMenu", ScaleMode::exact_fit, 3, 0x181, 0, 15}), m_blur(ui),
          m_lock3d(ui) {}
    ~LockpickingMenu() override {
        stop_tension();
        hud_set_mode(m_ui, "InventoryMode", false);
    }

    void on_show() override {
        // OnShow: the sweet spot, the start pick angle and the break time, then UpdateSweetSpot.
        load_state();
        m_pick_angle = m_sound_angle = m_state.start_pick;
        m_lock3d.open(m_state.skeleton_key);
        send("UpdateSweetSpot",
             {Value::number(m_state.centre), Value::number(m_state.size), Value::number(m_state.partial)});
    }

    // PostDisplay: slot 7 first, then the movie.
    bool ui_scene(UiScene& out) override { return m_lock3d.scene(out); }

    void advance(double dt, double engine_ms) override {
        // The update (flag 0x100): the first one sets the models up and sends SetLockInfo; the Intros block
        // input (lock 1.333 s, pick 1.0 s, then one more update); the lock springs back when not turned;
        // unlocking finishes after fUnlockDoorDelay (1.5 s).
        m_dt = dt;
        if (!m_first_update_done) {
            if (!m_lock3d.settled()) { // the game has them from the session's start: here, a few frames
                Menu::advance(dt, engine_ms);
                return;
            }
            m_first_update_done = true;
            m_lock3d.start();
            m_ui.host().ui_play_sound("UILockpickingEnter"); // InsertPick, from ProcessMessage too
            m_lock3d.pose_pick(m_pick_angle, m_lock_angle);
            m_lock3d.pose_damage(m_state.health);
            send_lock_info();
            m_intro_left = 1.333;
        } else if (m_intro_left > 0) {
            m_intro_left -= dt;
            if (m_intro_left <= 0) {
                m_hold_input = true; // one more update before input
            }
        } else if (m_hold_input) {
            m_hold_input = false;
        }
        pick_sound(dt);
        const Lock3D::Ended ended = m_lock3d.update(dt);
        if (ended.pick_intro || ended.lock_intro) {
            m_lock3d.pose_damage(m_state.health);
            m_lock3d.pose_lock(m_lock_angle);
        }
        if (m_breaking > 0 && ((m_breaking -= dt) <= 0 || ended.pick_break)) {
            m_breaking = 0;
            insert_pick();
        }
        if (m_unlocked) {
            m_unlock_wait += dt;
            if (m_unlock_wait >= 1.5) {
                m_ui.close(name());
                m_ui.host().ui_lock_event("unlocked", m_state.health);
            }
        } else if (!m_turning && m_lock_angle != 0.0) {
            rotate_lock(-2.0 * 80.0 * dt); // fLockRotationSpeed, twice as fast back
        }
        if (m_turning) {
            rotate_lock(m_turn_value * 80.0 * dt);
        }
        Menu::advance(dt, engine_ms);
    }

    bool on_user_event(const std::string& event) override {
        if (event == "Cancel") {
            m_ui.close(name()); // user event "Cancel" (message type 7): the menu posts its own hide
            return true;
        }
        return false;
    }

    bool input_event(std::string_view event, float value) override {
        // RotateLock: the movement keys (controlmap.txt, Lockpicking: the bindings of Forward, Back, Strafe
        // Left, Strafe Right).
        if (event == "RotateLock" || event == "Forward" || event == "Back" || event == "Strafe Left" ||
            event == "Strafe Right") {
            if (input_allowed()) {
                turn_lock_input(value);
            }
            return true;
        }
        return false;
    }

    // The mouse turns the pick: no cursor here (flags without 0x4), and the OS pointer stays hidden with it.
    bool holds_cursor() const override { return true; }

    void mouse_delta(float dx, float) override {
        // Mouse: RotatePick(dx x fPickMouseRotationSpeed (15) x the frame time), not while turning. The
        // game's frame is a 60th of a second; a mouse's counts don't grow with the frame rate, so here the
        // same 60th, or the pick would slow down as frames get shorter.
        if (input_allowed() && !m_turning) {
            const double delta = double(dx) * 15.0 / 60.0;
            rotate_pick(delta);
            m_frame_delta += delta;
            m_frame_moved = true;
        }
    }

  private:
    BlurHold m_blur;
    Lock3D m_lock3d;
    void register_handlers() override {} // the movie calls nothing (Accept registers no callbacks)

    void on_loaded() override {
        // Constructor: HUD InventoryMode, the blur, then SetPickMinMax(-fPickMaxAngle, fPickMaxAngle).
        hud_set_mode(m_ui, "InventoryMode", true);
        send("SetPickMinMax", {Value::number(-90.0), Value::number(90.0)});
    }

    void load_state() {
        if (LockInfo lock; m_ui.host().ui_lock(lock) && lock.present) {
            m_state.valid = true;
            m_state.skill = lock.skill;
            m_state.xp_percent = lock.xp_percent;
            m_state.next_is_blank = lock.next_is_blank;
            m_state.level = lock.level;
            m_state.picks = lock.picks;
            m_state.centre = lock.centre;
            m_state.size = lock.size;
            m_state.partial = lock.partial;
            m_state.break_seconds = lock.break_seconds;
            m_state.health = lock.health;
            m_state.start_pick = lock.start_pick;
            m_state.unbreakable = lock.unbreakable;
            m_state.skeleton_key = lock.skeleton_key;
            return;
        }
        const json::Value* c = m_ui.capture("lockpicking");
        if (!c) {
            log::info("ui: Lockpicking Menu: no capture 'lockpicking' in the cache; no lock data");
            return;
        }
        m_state.valid = true;
        m_state.skill = number(member(c, "skill"), 15);
        m_state.xp_percent = number(member(c, "xp_percent"));
        m_state.next_is_blank = flag(member(c, "next_is_blank"));
        m_state.level = int(number(member(c, "level")));
        m_state.picks = int(number(member(c, "picks")));
        m_state.centre = number(member(c, "centre"));
        m_state.size = number(member(c, "size"));
        m_state.partial = number(member(c, "partial"));
        m_state.break_seconds = number(member(c, "break_seconds"), 1);
        m_state.health = number(member(c, "health"), 100);
    }

    bool input_allowed() const {
        return m_first_update_done && m_intro_left <= 0 && !m_hold_input && !m_unlocked && m_breaking <= 0;
    }

    // SendLockInfo: skill, the next level (or " "), the XP percent, the lock level's name, the
    // picks.
    void send_lock_info() {
        static constexpr const char* k_names[6] = {"sLockLevelNameVeryEasy", "sLockLevelNameEasy",
                                                   "sLockLevelNameAverage",  "sLockLevelNameHard",
                                                   "sLockLevelNameVeryHard", "sLockLevelNameImpossible"};
        const int level = std::clamp(m_state.level, 0, 5);
        send("SetLockInfo", {Value::number(m_state.skill),
                             m_state.next_is_blank ? Value::string(" ") : Value::number(m_state.skill + 1),
                             Value::number(m_state.xp_percent),
                             Value::string(m_ui.game_setting(k_names[level])), Value::number(m_state.picks)});
    }

    void stop_tension() {
        m_ui.host().ui_stop_sound(m_tension);
        m_tension = 0;
    }

    // A small generator of the menu's own: the jitter and the movement sound's chance are for show.
    double random_unit() {
        m_random = m_random * 1664525u + 1013904223u;
        return double(m_random >> 8) / double(1u << 24);
    }

    // RotatePick: the pick angle, clamped to fPickMaxAngle (90); PickDamage from the health, the pick
    // placed, then UpdatePickAngle.
    void rotate_pick(double delta) {
        const double before = m_pick_angle;
        m_pick_angle = std::clamp(m_pick_angle + delta, -90.0, 90.0);
        m_lock3d.pose_damage(m_state.health);
        m_lock3d.pose_pick(m_pick_angle, m_lock_angle);
        if (m_pick_angle != before) {
            send("UpdatePickAngle", {Value::number(m_pick_angle)});
        }
    }

    // RotatePick's sound: past each 18 degrees one time in ten, and on starting to move (a delta after a delta of 0).
    // The game has one mouse event a frame, at 60 Hz. A mouse's own events come hundreds of times a second, many with
    // no sideways motion, and each would be a start: so this goes by what a 60th of a second moved.
    void pick_sound(double dt) {
        m_sound_wait -= dt;
        if (m_sound_wait > 0.0) {
            return;
        }
        m_sound_wait = std::max(m_sound_wait + 1.0 / 60.0, 0.0);
        if (!m_frame_moved) {
            return;
        }
        constexpr double k_step = 90.0 * 0.2;
        const double delta = m_frame_delta, step_before = std::fmod(m_sound_angle, k_step);
        const double step_after = std::fmod(m_pick_angle, k_step);
        const bool crossed = delta > 0.0 ? step_before > step_after : delta < 0.0 && step_after > step_before;
        if ((crossed && random_unit() < 0.1) || (delta != 0.0 && m_last_pick_delta == 0.0)) {
            m_ui.host().ui_play_sound("UILockpickingPickMovement");
        }
        m_last_pick_delta = delta;
        m_sound_angle = m_pick_angle;
        m_frame_delta = 0;
        m_frame_moved = false;
    }

    // TurnLockInput: the limit of this turn is fixed on its first frame from the pick's distance
    // to the sweet spot; released, the turn ends.
    void turn_lock_input(float value) {
        if (value == 0.0f) {
            stop_tension();
            m_turning = false;
            return;
        }
        if (!m_turning) {
            const double d = std::abs(m_pick_angle - m_state.centre), half = m_state.size * 0.5;
            m_limit = d <= half                     ? 90.0
                      : d <= half + m_state.partial ? (1.0 - (d - half) / m_state.partial) * 90.0
                                                    : 0.0;
            m_ui.host().ui_play_sound("UILockpickingCylinderTurn");
        }
        m_turning = true;
        m_turn_value = std::abs(value);
    }

    // RotateLock: the lock angle in [0, the turn's limit]; a full turn unlocks; held at the limit
    // the pick takes damage.
    void rotate_lock(double delta) {
        if (m_unlocked) {
            return;
        }
        const double limit = m_turning ? m_limit : 90.0;
        const double before = m_lock_angle;
        m_lock_angle = std::clamp(m_lock_angle + delta, 0.0, std::max(limit, 0.0));
        if (m_lock_angle >= 90.0) {
            m_unlocked = true;
            m_ui.host().ui_play_sound("UILockpickingUnlock");
        } else if (m_turning && m_lock_angle == limit) {
            if (m_lock_angle != before) {
                m_ui.host().ui_play_sound("UILockpickingCylinderStop");
            }
            if (!m_tension) {
                m_tension = m_ui.host().ui_start_sound("UILockpickingPickTensionLPM");
            }
            damage_pick();
        } else {
            m_lock3d.pose_lock(m_lock_angle);
            m_lock3d.pose_pick(m_pick_angle, m_lock_angle);
        }
        if ((before < 30.0) != (m_lock_angle < 30.0)) {
            m_ui.host().ui_play_sound("UILockpickingCylinderSqueakA");
        } else if ((before < 60.0) != (m_lock_angle < 60.0)) {
            m_ui.host().ui_play_sound("UILockpickingCylinderSqueakB");
        }
        send("UpdateLockAngle",
             {Value::number(m_lock_angle)}); // every call, also when held at the limit (measured)
    }

    // DamagePick: health falls by 100 / break time per second, unless EP 65 says otherwise. Held there
    // the lock and pick shake. At 0 the pick breaks: one pick less, and PickBreak plays before the next goes in.
    void damage_pick() {
        if (!m_state.unbreakable) {
            const double health =
                m_state.break_seconds > 0 ? m_state.health - 100.0 / m_state.break_seconds * m_dt : 0.0;
            m_state.health = std::clamp(health, 0.0, 100.0);
        }
        send("UpdatePickHealth", {Value::number(m_state.health)});
        if (m_state.health > 0.0) {
            m_lock3d.pose_pick(m_pick_angle + (random_unit() * 4.0 - 2.0), m_lock_angle);
            m_lock3d.pose_lock(m_lock_angle == 0.0 ? random_unit() * 8.0 : m_lock_angle + random_unit() - 0.5);
            return;
        }
        stop_tension();
        m_ui.host().ui_play_sound("UILockpickingPickBreak");
        m_lock3d.break_pick();
        m_turning = false;
        m_pick_angle = m_sound_angle = 0;
        m_state.health = 100;
        m_state.picks = std::max(0, m_state.picks - 1);
        m_breaking = 1.7; // the sequence's length, for a menu with no model to end it
        m_ui.host().ui_lock_event("broken", 100);
    }

    // OnSequenceEnd and InsertPick: the next pick, or out of them and out.
    void insert_pick() {
        m_turning = false;
        if (m_state.picks == 0 && !m_state.skeleton_key) {
            notify(m_ui, m_ui.game_setting("sOutOfLockpicks"));
            m_ui.close(name());
            return;
        }
        m_lock3d.insert_pick();
        m_lock3d.pose_pick(m_pick_angle, m_lock_angle);
        m_ui.host().ui_play_sound("UILockpickingEnter");
        send_lock_info();
    }

    LockState m_state;
    bool m_first_update_done = false, m_hold_input = false, m_turning = false, m_unlocked = false;
    double m_intro_left = 0, m_unlock_wait = 0, m_dt = 1.0 / 60.0, m_breaking = 0;
    uint32_t m_random = 1, m_tension = 0;
    double m_frame_delta = 0, m_sound_wait = 0, m_sound_angle = 0; // what the last 60th of a second moved
    bool m_frame_moved = false;
    double m_pick_angle = 0, m_lock_angle = 0, m_limit = 90, m_turn_value = 0, m_last_pick_delta = 0;
};

} // namespace

std::unique_ptr<Menu> create_hud_menu(Ui& ui, const std::string& name) {
    if (name == "HUD Menu")
        return std::make_unique<HudMenu>(ui);
    if (name == "Sleep/Wait Menu")
        return std::make_unique<SleepWaitMenu>(ui);
    if (name == "Training Menu")
        return std::make_unique<TrainingMenu>(ui);
    if (name == "Lockpicking Menu")
        return std::make_unique<LockpickingMenu>(ui);
    return nullptr;
}

void hud_set_mode(Ui& ui, const std::string& mode, bool on) {
    if (Menu* m = ui.find_menu("HUD Menu")) {
        static_cast<HudMenu*>(m)->set_mode(mode, on);
    }
}

void Ui::hud_corner_message(const std::string& text) {
    if (Menu* m = find_menu("HUD Menu")) {
        static_cast<HudMenu*>(m)->post_notification(text);
    }
}

void Ui::hud_location_discovered(const std::string& name, int type) {
    if (Menu* m = find_menu("HUD Menu")) {
        static_cast<HudMenu*>(m)->post_location_discovered(name, type);
    }
}

void Ui::hud_quest_notice(int type, uint32_t quest, const std::string& text) {
    if (Menu* m = find_menu("HUD Menu")) {
        static_cast<HudMenu*>(m)->post_quest_notice(type, quest, text);
    }
}

void Ui::hud_tutorial_hint(const std::string& text) {
    if (Menu* m = find_menu("HUD Menu")) {
        static_cast<HudMenu*>(m)->post_tutorial_hint(text);
    }
}

void Ui::hud_location_name(const std::string& name) {
    if (Menu* m = find_menu("HUD Menu")) {
        static_cast<HudMenu*>(m)->post_location_name(name);
    }
}

void hud_crosshair(Ui& ui, const CrosshairTarget& target) {
    if (Menu* m = ui.find_menu("HUD Menu")) {
        static_cast<HudMenu*>(m)->crosshair(target);
    }
}

void hud_notify(Ui& ui, const std::string& text, const std::string& sound) { notify(ui, text, sound); }

void hud_stealth(Ui& ui, bool sneaking, double level) {
    if (Menu* m = ui.find_menu("HUD Menu")) {
        static_cast<HudMenu*>(m)->stealth(sneaking, level);
    }
}

void hud_skill_increase(Ui& ui, const std::string& skill, int level, int player_level, double xp) {
    if (Menu* m = ui.find_menu("HUD Menu")) {
        static_cast<HudMenu*>(m)->post_skill_increase(skill, level, player_level, xp);
    }
}

} // namespace spud::ui
